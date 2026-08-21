// The loopback websocket the ui (client.html) connects to instead of the real server. Node holds
// the one real connection; when a ui attaches, node replays the login sequence from held state, in
// the same wire format the server uses - so client.html receives what it already knows how to read.
//
// "BURST" = that replayed login sequence. A real server greets a fresh login with a fixed run of
// frames: authentication_status, channel_list, client_list, channel_maintainer_id per channel,
// tag_list, icon_list, then the channel keys arrive as a direct message. The ui here logs in long
// after node did, so node rebuilds that exact run from its own state and sends it in one burst the
// moment a ui attaches (or, if node is not logged in yet, as soon as its own login state is
// complete). To the ui a burst is indistinguishable from a real server login.
//
// Plaintext on purpose: this never leaves the device. The token is required as the first frame,
// because 127.0.0.1 is reachable by every app on the device.

let crypto = require("crypto");
let WebSocketServer = require("./mini-ws.js").WebSocketServer;

function start(bundle, on_ready)
{
    let token = crypto.randomBytes(16).toString("hex");
    let ui_socket = null;

    // the server's auth frame, cached at login and replayed verbatim - it drives the ui's
    // entire "you're in" transition
    let cached_auth_frame = null;

    // same idea for the channel keys: they can arrive before the ui attaches
    let cached_keys_frame = null;
    let ui_needs_burst = false;

    // frames that arrive while the burst is still pending - the maintainer frame landed in that
    // window and was neither in the replayed state nor live-forwarded, so keys got rejected
    let pre_burst_frames = [];

    let server = new WebSocketServer({ host: "127.0.0.1", port: 0 });

    function send_burst(socket)
    {
        socket.send(cached_auth_frame || bundle.get_auth_frame());
        build_login_burst().forEach(function(frame) { socket.send(JSON.stringify(frame)); });

        if (cached_keys_frame != null)
        {
            socket.send(cached_keys_frame);
        }

        // flush what arrived during the wait, minus the kinds the burst itself just replayed
        for (let i = 0; i < pre_burst_frames.length; i++)
        {
            try
            {
                let queued = JSON.parse(pre_burst_frames[i]);
                let queued_type = (queued.message && queued.message.type) ? queued.message.type : "";

                if (queued_type === "authentication_status" || queued_type === "channel_list" || queued_type === "client_list")
                {
                    continue;
                }
            }
            catch (e) { }

            socket.send(pre_burst_frames[i]);
        }

        pre_burst_frames = [];
        ui_needs_burst = false;
        console.log("loopback ui: login burst sent");
    }

    // when node loses the server, drop the attached ui: the webview then runs its own
    // lost-connection path (clean state) and reattaches for a fresh burst once node is back in
    bundle.set_on_message_processed(function(message_type)
    {
        if (message_type === "websocket_worker_onclose" || message_type === "websocket_worker_onerror")
        {
            // the session is gone, so everything cached from it is a lie - a stale auth frame
            // led a burst that showed "connected" with no channels
            cached_auth_frame = null;
            cached_keys_frame = null;
            pre_burst_frames = [];

            if (ui_socket != null)
            {
                console.log("loopback ui: server connection lost, detaching ui");
                try { ui_socket.close(); } catch (e) { }
                ui_socket = null;

                if (typeof bundle.node_set_ui_attached === "function")
                {
                    bundle.node_set_ui_attached(false);
                }
            }
        }

        // a deferred burst fires once node's login is COMPLETE (client_list processed),
        // so the state it is rebuilt from is whole
        if (ui_socket != null && ui_needs_burst === true
            && (cached_auth_frame != null || bundle.get_auth_frame() != null)
            && bundle.read_state().g_is_client_list_retrieved === true)
        {
            send_burst(ui_socket);
        }
    });

    // node's connection status feeds the ui's login page - meta traffic, so it bypasses the
    // burst gating and flows even while the ui is unauthenticated
    function send_status_to_ui(status)
    {
        if (ui_socket != null)
        {
            try { ui_socket.send(JSON.stringify({ message: { type: "loopback_status", value: status } })); }
            catch (e) { }
        }
    }

    if (typeof bundle.set_connection_status_listener === "function")
    {
        bundle.set_connection_status_listener(send_status_to_ui);
    }

    bundle.set_frame_listener(function(json_string)
    {
        try
        {
            let frame = JSON.parse(json_string);

            // node forwards direct messages already decrypted, so the marker is readable here.
            // keys nearly always arrive before a ui attaches, hence the cache
            if (frame.message && frame.message.type === "direct_chat_message"
                && frame.message.some_json
                && frame.message.some_json.type === "channel_keys_from_maintainer")
            {
                cached_keys_frame = json_string;
            }
            if (frame.message && frame.message.type === "authentication_status")
            {
                cached_auth_frame = json_string;
            }

        }
        catch (e) { }

        // live traffic goes straight through to an attached ui; while the burst is pending,
        // hold the frames instead of dropping them - send_burst flushes the queue
        if (ui_socket != null)
        {
            if (ui_needs_burst === false)
            {
                ui_socket.send(json_string);
            }
            else
            {
                pre_burst_frames.push(json_string);
            }
        }
    });

    // the login burst: what the server sends after authentication, rebuilt from state. the state
    // arrays hold the wire objects themselves (the handlers push them in unchanged), except g_icons,
    // which is stored remapped and has to be mapped back
    function build_login_burst()
    {
        let state = bundle.read_state();
        let frames = [];

        frames.push({ message: { type: "channel_list", channels: state.g_channel_list } });

        frames.push({ message: {
            type: "client_list",
            clients: state.g_client_list,
            local_username: state.g_local_username
        } });

        // maintainer_id / has_maintainer are NOT part of channel_list on the wire - only this
        // message carries them, and the keys handler silently drops keys without them
        state.g_channel_list.forEach(function(channel)
        {
            if (channel.has_maintainer === true)
            {
                frames.push({ message: {
                    type: "channel_maintainer_id",
                    channel_id: channel.channel_id,
                    maintainer_id: channel.maintainer_id,
                    has_maintainer: true
                } });
            }
        });

        if (state.g_tags.length > 0)
        {
            frames.push({ message: { type: "tag_list", tags: state.g_tags } });
        }

        if (state.g_icons.length > 0)
        {
            frames.push({ message: { type: "icon_list", icons: state.g_icons.map(function(icon)
            {
                return { icon_id: icon.id, base64_icon: icon.base64_icon };
            }) } });
        }

        return frames;
    }

    server.on("connection", function(socket)
    {
        let authed = false;

        socket.on("message", function(data)
        {
            let text = data.toString("utf8");

            if (authed === false)
            {
                if (text.trim() !== token)
                {
                    console.error("loopback ui: wrong token, dropping connection");
                    socket.close();
                    return;
                }

                authed = true;

                // one ui at a time: a reattach replaces the previous socket
                if (ui_socket != null && ui_socket !== socket)
                {
                    try { ui_socket.close(); } catch (e) { }
                }

                ui_socket = socket;
                console.log("loopback ui: ui attached");

                // somebody is looking now: node stops counting unread and drops the icon badge
                if (typeof bundle.node_set_ui_attached === "function")
                {
                    bundle.node_set_ui_attached(true);
                }

                // a fresh ui shows node's current status right away, not on the next change
                if (typeof bundle.get_connection_status === "function")
                {
                    send_status_to_ui(bundle.get_connection_status());
                }

                // an attached ui wants to be online - tells node to dial if it has not
                if (typeof bundle.node_connect_intent === "function")
                {
                    bundle.node_connect_intent();
                }

                // send only when node's login is COMPLETE (client_list processed) - an auth
                // frame alone means the lists are not in yet, the burst would replay empty ones
                let is_login_state_complete = false;
                try { is_login_state_complete = (bundle.read_state().g_is_client_list_retrieved === true); } catch (e) { }

                if (is_login_state_complete === true && (cached_auth_frame != null || bundle.get_auth_frame() != null))
                {
                    send_burst(socket);
                }
                else
                {
                    ui_needs_burst = true;
                    console.log("loopback ui: node login not complete yet, burst deferred");
                }

                return;
            }

            // plaintext request from the ui: encrypt and forward to the real server
            bundle.node_forward_raw_request(text);
        });

        socket.on("close", function()
        {
            if (ui_socket === socket)
            {
                ui_socket = null;

                // a burst owed to this ui is owed to nobody now; the next attach re-decides
                ui_needs_burst = false;
                pre_burst_frames = [];

                console.log("loopback ui: ui detached");

                // nobody is looking any more: node counts unread again, for the icon badge
                if (typeof bundle.node_set_ui_attached === "function")
                {
                    bundle.node_set_ui_attached(false);
                }
            }
        });

        socket.on("error", function() {});
    });

    server.on("listening", function()
    {
        let port = server.address().port;
        console.log("loopback ui: listening on 127.0.0.1:" + port);

        if (typeof on_ready === "function")
        {
            on_ready(port, token);
        }
    });

    return {
        // slice B: node forwards each decrypted server frame here so an attached ui stays live
        forward_to_ui: function(json_string)
        {
            if (ui_socket != null)
            {
                ui_socket.send(json_string);
            }
        }
    };
}

module.exports = { start: start };
