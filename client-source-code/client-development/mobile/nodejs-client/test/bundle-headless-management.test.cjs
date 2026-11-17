const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const vm = require("node:vm");
const crypto = require("node:crypto");
const {spawnSync} = require("node:child_process");

const directory = fs.mkdtempSync(path.join(os.tmpdir(), "lemon-headless-management-"));
const output = path.join(directory, "bundle.js");
let source;
try
{
    const built = spawnSync("python", ["-c", [
        "import importlib.util,sys",
        "spec=importlib.util.spec_from_file_location('builder',sys.argv[1])",
        "builder=importlib.util.module_from_spec(spec)",
        "spec.loader.exec_module(builder)",
        "builder.OUT=sys.argv[2]",
        "sys.exit(builder.main())"
    ].join("\n"), path.resolve(__dirname, "../build-node.py"), output], {encoding: "utf8"});
    assert.equal(built.status, 0, built.stderr || built.stdout);
    source = fs.readFileSync(output, "utf8");
}
finally
{
    fs.rmSync(directory, {recursive: true, force: true});
}
const anchor = "node_forward_upload_request: function(request)";
assert.ok(source.includes(anchor));
source = source.replace(anchor, `management_test_setup: function()
{
    g_is_authenticated = true;
    g_node_connection_wanted = true;
    g_worker_connection_ended = false;
    g_connection_status = {state: "connected", reason: ""};
    g_local_client_id = 0;
    g_current_channel_id = 1;
    g_current_chat_context_id = "chat-context-channel-1";
    g_client_list = [
        {client_id: 0, username: "Me", public_key: "me", channel_id: 1, group_ids: [], is_music_bot: false},
        {client_id: 7, username: "Maya", public_key: "maya", channel_id: 1, group_ids: [], is_music_bot: false},
        {client_id: 8, username: "Radio", public_key: "radio", channel_id: 1, group_ids: [], is_music_bot: true}
    ];
    g_map_client_id_to_array_index = new Map(g_client_list.map((client, index) => [client.client_id, index]));
    g_channel_list = [{channel_id: 1, name: "General", unread_count: 0}];
    g_metadata_keys = [];
    return {
        receive: function(message)
        {
            const frame = keys__encrypt_all_message_data_and_convert_to_base64(JSON.stringify({message: message}));
            android_host__dispatch_safely(workers__process_received_frame, {value: frame}, "remote_test");
        },
        state: function()
        {
            return {authenticated: g_is_authenticated, wanted: g_node_connection_wanted,
                file_limit: g_server_policy.file_upload_max_size, picture_limit: g_server_policy.chat_picture_max_size,
                channel_blocking: g_server_policy.is_blocking_channel_chat_files_active,
                name: g_client_list[1].username, country: g_client_list[1].country_iso_code,
                avatar: g_client_list[1].base64_avatar, groups: g_client_list[1].group_ids.slice(),
                discard: String(g_client_list[2].discard_audio_through_counter || "")};
        }
    };
},
` + anchor);

function fixture()
{
    const errors = [];
    const timers = [];
    const context =
    {
        process: {}, crypto: crypto.webcrypto, Buffer, TextDecoder, TextEncoder, performance, atob, btoa,
        console:
        {
            log: function()
            {
            },
            warn: function()
            {
            },
            error: function(message)
            {
                errors.push(String(message));
            }
        },
        setTimeout: function(callback, delay)
        {
            timers.push({callback, delay});
            return timers.length;
        },
        clearTimeout: function()
        {
        },
        setInterval: function()
        {
        },
        clearInterval: function()
        {
        }
    };
    context.global = context;
    vm.createContext(context);
    vm.runInContext(source, context, {timeout: 10000});
    const bundle = context.what;
    bundle.init_node_runtime();
    function flush()
    {
        let count = 0;
        while (timers.some(timer => timer.delay === 0))
        {
            assert.ok(++count < 100, "message dispatch must finish without a rendering loop");
            timers.splice(timers.findIndex(timer => timer.delay === 0), 1)[0].callback();
        }
    }
    flush();
    const controls = bundle.management_test_setup();
    const pending = new Map();
    const forwarded = [];
    let next = 0;
    bundle.set_frame_listener(function(frame, options)
    {
        if (options && options.defer_delivery)
        {
            const id = "frame-" + (++next);
            pending.set(id, JSON.parse(frame));
            return id;
        }
        if (options && options.release_delivery)
        {
            forwarded.push(pending.get(options.release_delivery));
            pending.delete(options.release_delivery);
            return;
        }
        forwarded.push(JSON.parse(frame));
    });
    return {errors, forwarded, controls, receive: function(message)
    {
        controls.receive(message);
        flush();
    }};
}

for (const memoryOnly of [false, true])
{
    test("headless admin " + (memoryOnly ? "RAM" : "full settings") + " replies forward without rendering or disconnecting", function()
    {
        const f = fixture();
        const message = {type: "server_settings_values", memory_only: memoryOnly, uptime_seconds: 20,
            used_slots: 2, max_slots: 16, ram_bytes: 10485760, connection_password_count: 2, bans: [], blocked_countries: []};
        f.receive(message);
        assert.deepEqual(f.errors, []);
        assert.equal(f.controls.state().authenticated, true);
        assert.equal(f.controls.state().wanted, true);
        assert.deepEqual(JSON.parse(JSON.stringify(f.forwarded)), [{message}]);
    });
}

test("headless server policy still updates file limits and receive permissions", function()
{
    const f = fixture();
    f.receive({type: "server_policy", file_upload_max_size: 123456, chat_picture_max_size: 654321,
        is_blocking_channel_chat_files_active: true});
    assert.deepEqual(f.errors, []);
    assert.equal(f.controls.state().file_limit, 123456);
    assert.equal(f.controls.state().picture_limit, 654321);
    assert.equal(f.controls.state().channel_blocking, true);
    assert.equal(f.controls.state().authenticated, true);
    assert.equal(f.controls.state().wanted, true);
});

test("headless media and management events retain model state without browser callbacks", function()
{
    const f = fixture();
    const avatar = "data:image/png;base64,c3ludGhldGlj";
    const messages = [
        {type: "group_list", groups: [{group_id: 2, name: "Members", has_icon: false}], channel_groups: [], access: {}},
        {type: "group_permissions_changed", group_id: 2, permissions: {actions: {send_direct_messages: "grant"}}},
        {type: "group_add_to_client", client_id: 7, group_id: 2},
        {type: "icon_list", icons: [{icon_id: 4, base64_icon: avatar}]},
        {type: "group_icon_changed", group_id: 2, has_icon: true, group_linked_icon_id: 4},
        {type: "client_rename", client_id: 7, new_username: "Maya renamed"},
        {type: "client_country_code_changed", client_id: 7, country_iso_code: "SK"},
        {type: "client_avatar", client_id: 7, base64_avatar: avatar},
        {type: "start_song_stream", client_id: 8, song_name: "Synthetic song"},
        {type: "stop_song_stream", client_id: 8, discard_audio_through_counter: "123"},
        {type: "music_bot_song_list", client_id: 8, songs: [{id: 1, name: "Synthetic song", duration_seconds: 20}]},
        {type: "video_stream_state", client_id: 7, channel_id: 1, active: true},
        {type: "video_stream_offer", client_id: 7, channel_id: 1},
        {type: "video_stream_viewer_request", client_id: 7},
        {type: "video_stream_viewer_state", client_id: 7, active: false},
        {type: "video_stream_keyframe_request", client_id: 7},
        {type: "video_stream_refused", reason: "Synthetic refusal"},
        {type: "sdp_offer", value: JSON.stringify({type: "offer", sdp: "synthetic"})},
        {type: "ice_candidate", value: {candidate: "synthetic"}},
        {type: "poke", client_id: 7, poke_message: "Synthetic poke"},
        {type: "access_denied", permission: "send_pokes", reason: "Synthetic refusal"},
        {type: "admin_log", lines: ["Synthetic entry"]},
        {type: "server_info", visibility: {}},
        {type: "server_banner_result", success: false},
        {type: "server_settings_saved", success: true}
    ];
    for (const message of messages)
    {
        f.receive(message);
        assert.deepEqual(f.errors, [], message.type);
        assert.equal(f.controls.state().authenticated, true, message.type);
        assert.equal(f.controls.state().wanted, true, message.type);
    }
    assert.equal(f.controls.state().name, "Maya renamed");
    assert.equal(f.controls.state().country, "SK");
    assert.equal(f.controls.state().avatar, avatar);
    assert.deepEqual(Array.from(f.controls.state().groups), [2]);
    assert.equal(f.controls.state().discard, "123");
    assert.deepEqual(f.forwarded.map(frame => frame.message.type), messages.map(message => message.type));
});
