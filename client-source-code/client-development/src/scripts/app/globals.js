// globals.js is embedded in template.html (and the node bundle) along with the other client files
// it holds every piece of state that more than one file uses, one group per topic
// every top-level var and function between aes-js.js and the end of main.js lives in one closure, so
// all of them are visible everywhere; the prefix is the convention: g_ = app-wide state, G_ = app-wide
// constant, no prefix = private to the file that declares it

// ---- connection and session ----

var g_are_server_details_predefined = false;

// autoconnect without a click: browsers only start audio after a user gesture, so a page opened
// straight from disk may refuse the AudioContext; set this to false if that bites (a click on
// connect is enough). not an issue in the android webview or on a site the user already clicked

var g_is_autoconnect_without_user_action_active = false;

// whether the last connect attempt actually failed (so the login shows
// "connection failed, retrying in Xs" - not before)
var g_last_connect_attempt_failed = false;

// the server details used when g_are_server_details_predefined is true (no prompt for host, port
// and keys); with autoconnect on top the page needs no click at all, at the cost of the sound rule above

var g_autoconnect_details = {
    host: "192.168.1.106",
    port: 1111,
    keys: [
        "test",
        // "test2"
    ]
};

// the username this client asks for while connecting; the server uses it instead of the
// assigned user0/user1/... name, provided nobody connected is using it. leave "" for the
// assigned name. a registered identity's admin-set name still wins over this
var g_chosen_username = "";

var g_host = "";

var g_is_websocket_connected = false;

var g_is_authenticated = false;

var g_should_connection_check_be_running = false;

// the heartbeat: one every interval_ms, the link counts as lost after lost_threshold_ms without
// a reply (three missed 10 s heartbeats plus slack, so a dead network shows within a minute)
var g_connection_check = {
    interval_ms: 10 * 1000,
    lost_threshold_ms: 35 * 1000,
    sleep_resolve: null,        // wakes the check loop early
    last_response_timestamp: 0  // set on every heartbeat reply, first when the client connects
};

// session statistics for the strip themes' session-info card. bytes are counted at
// the two choke points every message passes through (encrypted base64 lengths, so
// "wire-ish" numbers); ping is the heartbeat round trip.
var g_session = {
    bytes_sent: 0,
    bytes_received: 0,
    connected_at: 0,
    ping_sent_at: 0,
    last_ping_ms: -1
};

// fast reconnect (server setting): a lost socket keeps the page exactly as it is and re-dials with
// the same identity; the server adopts the still-open session. only a failed attempt shows
// "connection lost" and wipes the page
var g_fast_reconnect = {
    in_progress: false,   // an attempt is running: no wipe, no toast, one attempt only
    resumed: false,       // the server said fast_reconnect_ok: the lists that follow are a refresh
    deadline_timer: null, // the classic "connection lost" path when the attempt stalls
    pending_lists: null   // the refreshed lists, applied together so the page repaints once
};

// the local keypair as { public_key_string, identity_string }, awaited by the driver and filled by
// dispatch.js when the worker made it; connection.js builds the slot at load. the private key stays in the worker
var g_identity_slot = null;

// the live status for the login page. the local driver writes it; in the webview,
// node's reports arriving over the loopback overwrite it, because node owns the real connection
var g_connection_status = {
    state: "idle",          // idle | connecting | waiting_retry | connected
    reason: "",             // why the last attempt failed, human readable
    next_retry_at: 0,       // ms timestamp of the next automatic attempt, 0 = none
    last_connected_at: 0    // ms timestamp the server connection last existed, 0 = never
};

// set through the export seam. every listener receives every status change; under node
// two exist, the loopback ui and the java bridge
var g_connection_status_listeners = [];

// filled by the close and error handlers, consumed by the driver for the retry status
var g_last_disconnect_reason = "";

// android reports the device's real network state over the bridge; null means never told
// a browser has navigator.onLine, but the android webview lies about it, so this is the
// only trustworthy answer to whether the phone has any network
var g_device_has_network = null;

// ---- identity and crypto ----

var g_metadata_keys = [];

var g_dh_generator = null;

var g_dh_modulus = null;

var g_dh_secret_exponent = null;

if (typeof window != 'undefined')
{
    g_dh_generator = 2n;

    var dh_modulus_bits = 8192; // 2048, 4096, or 8192 - MUST match the server's DH_MODULUS_BITS in dh_primes.h

    // safe prime; the size is chosen by dh_modulus_bits above
    var dh_modulus_string = "32317006071311007300338913926423828248817941241140239112842009751400741706634354222619689417363569347117901737909704191754605873209195028853758986185622153212175412514901774520270235796078236248884246189477587641105928646099411723245426622522193230540919037680524235519125679715870117001058055877651038861847280257976054903569732561526167081339361799541336476559160368317896729073178384589680639671900977202194168647225871031411336429319536193471636533209717077448227988588565369208645296636077250268955505928362751121174096972998068410554359584866583291642136218231078990999448652468262416972035911852507045361090559";
    if (dh_modulus_bits == 4096) { dh_modulus_string = "769693417275193209984647063932271739387855846059952565355802298991172654607712104048642837327086393649061117273977479029847880929901816490502575106445708811728815104699538212859676255621694933541780065300216380119365448477045659714142752962409351060465337847941705392356465059912091910379610354725312649190019796723866880686790102505810145302961022375682955537024852712153016097337874982984026217981644194741246064934907045623310252540105014478602030042625050790892256552738501094150544017503366521405695110938208299693781667383898463231081051406284286841973557391837014717399840317430691522097547152517168661381236591027075489125945391080953462725086945640553263655450552529331021143283920078415126554651532264427032676910742519456229972244566183184460134372157613705601578581124341629241972089511142281008551119184446873409929566545851290281361166571221433352162292794386037934251491816926283501321267998170847037246436312385384549128374516008246102779333275418845691810684079267893733515705735729967088709311436111220883412133997678612136656076019025878523079903588858263406471350679442414909683284164773663260965983542100471510190345089294782440003918912451456992077790689703598681468439291465843547007915368517110174489101683183"; }
    if (dh_modulus_bits == 8192) { dh_modulus_string = "977457999394373613160803436413990067824664325329752783398860665650891758153546599425963979290921371512554424155404999662026528515231003619059250227581073193191817667145872254566150016152267378041043707227533090523130854162674719502912605093407840683480360272745764870153876137077404098306192221010043540729677343845507367074906936225679715947791388837030145881441948294883090889231339114529926218527174080089614380520453541140942641189135120655392817995558672348259240314664441211284094553472715483266674338226096876439738570920830431990068192898823624154982561509367679529622217472548774982858792731946532808614927934739139866206407985368007801168974233995065956999264060271473667589912911635282735847769997114239537087675283088650912126339354407150753898672557625721020468123121985217987055904104587919799098757089865856148151372159009557757252554523938389147793317088678845894492183206966490686288450161789777284440531583751707235355985011244929317700461604659491747569397015417605015844114189684757052804077400345793511780894375616367276781058142309055525279814951138171200246013005920763485637476273362810215876618783686490089700542042236756450450875748150296041125307287408385472961923570457489504445415040799421139916299426300456654408986639074321942096027448333024943556858265321864649169731360525833923693176347415312865652669889035367188555225154697353012485541899891405325448203641838378457181625265662866012641101851261909549715296062722220871699753970653198277244417978954211900339094323007747108915608099803996978341421956304178871205193962176652206358214977516703892527582137534788148271002406853630936928238671372686645287319965234542895792366832783193938302132219463877916744337606350077802640502896551808414614146119887726456058723288826440787605655349724793408478985959688481296784606909713086104258354166909655924759369179237403392373490546567686597579050582406536565868808879640217167277372359442106490085619712603267716148318470568791898988932303644832977117895522999512850187807810874398403568491329765776005361335497620431318887438033026603280081068652656086250926318691324234636743583950635209943952384403217947022081052893713058850302983931039183796265186758214153198152532323300955155280353467780525511888234737731346004632762960546857790075686663681335897303755575277977768637069626833995439307976899886428471454602498498262648098952772543021280017770216979020055000596511525631600903871686087096720903178326987700681798435674674029499323"; }
    g_dh_modulus = BigInt(dh_modulus_string);
}

var g_my_rsa_key_object = null;

var g_rsa_public_key_string = "";

var g_identity_string = "";

var g_is_rsa_key_generated = false;

var g_keys_init_status = false;

var g_is_identity_switch_in_progress = false; // deliberate disconnect->new-keypair->reconnect cycle (top bar "identity" button)

// the size this server last asked for, so the offer is not repeated on every redial. the
// connection driver retries forever, and the rejection repeats on every attempt
var g_rsa_key_too_weak_prompted_for_bits = 0;

// ---- server policy ----

// server-side policy this client only obeys, announced at login (authentication_status) and on every
// admin save (server_policy); absent fields on an older server keep these defaults. the keys are the
// wire names, so server_settings_tab__apply_server_policy_fields copies them generically (a number only when positive)
var g_server_policy = {
    is_fast_reconnect_allowed: false,          // a lost socket may resume its session instead of starting over
    show_music_bot_marquee_to_everyone: false, // a streaming bot's marquee also for people outside its channel
    is_alias_registration_allowed: false,      // admins may register aliases (display names) on identities
    allow_typing_indicator: false,             // "x is typing ..." is sent and shown
    allow_client_renames: true,                // users may rename themselves (admins always may); on unless the server says otherwise
    allow_avatars: false,                      // image avatars on identities
    avatar_max_size: 51200,                    // largest raw avatar image in bytes
    allow_file_uploads: false,                 // any file may be sent in chat; off until the server says so
    file_upload_max_size: 10 * 1024 * 1024,    // largest raw chat file in bytes
    allow_chat_pictures: true,                 // inline pictures in chat; on unless the server says otherwise
    chat_picture_max_size: 4 * 1024 * 1024,    // largest raw inline picture in bytes; bigger images still travel as files
    icon_max_size: 5000                        // largest raw tag/channel icon in bytes (the server settings tab upload)
};

// ---- who is here ----

var g_local_username = "";

var g_local_client_id = 0;

// true once the server granted this session the admin tag
var g_is_local_client_admin = false;

var g_client_list = [];

var g_channel_list = [];

var g_icons = [];

var g_tags = [];

var g_map_client_id_to_array_index = new Map();

var g_offline_client_list = []; // might be supported by the server, might not

var g_is_client_list_retrieved = false;

var g_is_channel_list_retrieved = false;

var g_selected_channel_id;

var g_selected_client_id;

var g_current_channel_id = 0;

// internal sentinel meaning "the root level" while walking the channel tree.
// root channels are identified by their is_root_channel flag, never by this value
// (server ids are uint64 and can no longer carry -1).
const g_ROOT_LEVEL_PARENT_SENTINEL = -1;

var g_chat_message_author_public_keys = {}; // server chat message id -> author's public key; used to decide whether to honour an incoming delete/edit

// ---- channel keys ----

var g_current_channel_keys = null;

// the previous channel keys: keys change when somebody joins, so a message encrypted with the old ones
// can still arrive; the data-processing worker tries these too, and drops them on a channel switch

var g_historic_keys_of_current_channel = [];

// ---- chat context and composer ----

var g_chat_context_array = [
    {
        type: "channel",
        chat_context_id: "chat-context-channel-0",
        last_known_message_sender_username: ""
    }
];

var g_current_chat_context_id = "chat-context-channel-0";

var g_chat_message_receiver_type = "channel";

var g_chat_message_receiver_id = "main";

// which offline person the chat input is currently addressing (set when their circle is
// tapped). empty whenever the open conversation is a channel or a connected client.
var g_offline_chat_recipient_alias = "";

var g_selected_server_chat_message_id = null;

var g_local_message_id = 0;

var g_selected_font = "custom-font-usage-default";

var g_selected_font_color = "#ffffff";

var g_selected_font_size = 12;

var g_base64_picture_string_to_send = "" ;

var g_file_send_intent = "";

var g_file_send_intent_extra_data = {};

var g_is_file_being_uploaded = false;

// ---- platform ----

var g_is_client_running_under_touch_device = false;  // for touch devices

var g_is_running_in_android_webview = false;

// AVATAR PREFETCH (option). on by default: after joining, quietly ask for every connected
// person's avatar one at a time so faces are there before anybody is clicked.
var g_android_app_mode = "";

// first push is the app handing over saved settings, later ones mean a switch moved while running
var g_have_received_android_settings = false;

var g_is_microphone_enabled_on_touch_device = false;  // for touch devices

// true on the page, false inside a web worker and in the node runtime, where document does not exist
var G_HAS_DOM = (typeof window !== "undefined" && typeof document !== "undefined");

// ---- workers ----

var g_opus_encoder_worker = null;

var g_opus_decoder_worker = null;

var g_data_processing_worker = null;

var g_minimp3_worker = null;

var g_websocket_worker = null;

// ---- ui flags ----

var g_textarea_log = null;

var g_is_chat_hidden = false;

var g_show_hide_toggle = false;

// optional per-theme extras: flat channel list + a live right-pane member list
var g_is_channel_list_flattened = false;

var g_alert_push_to_talk_key_shown_once = false;

var g_alert_streaming_music_shown_once = false;

var g_are_sound_effects_enabled = true;

var g_stop_song_stream_message_received = false;

var g_selected_song_name = null;

// ---- connect page hold ----

// launching the webview onto a node that is already logged in used to show the connect
// page for a moment before the burst arrived. the page STARTS held back (the spinner is
// the first page); it is revealed only when connecting turns out to fail or stall
var g_is_holding_back_connect_page = true;

// the spinner's reveal deadline; node's "still connecting" reports push it out
var g_connect_holdback_deadline = 0;

var g_connect_holdback_started = 0;

// ---- idle ----

// deep idle state (android background mode): only the websocket + slow heartbeat stay alive
var g_is_deep_idle = false;

var g_is_deep_idle_pending = false;

// ---- typing, unread, avatars, layout ----

// typing indicator (only alive when the server policy allows it). g_typing.state maps a chat
// context id to { client_id: expiry_timestamp_ms } - an entry that is not refreshed just
// expires, so a sender that disappears mid-sentence never leaves "x is typing" hanging
var g_typing = {
    state: {},          // chat context id -> { client_id: expiry_timestamp_ms }
    last_sent_at: 0,
    render_timer: null
};

// unread message count per channel, keyed by channel id; the channel being looked at is always zero.
// kept as state, not only in the badge markup, so node can raise a notification without a dom
var g_channel_unread_counts = {};

// avatars (server opt-in, g_server_policy.allow_avatars). cache maps client_id -> base64
// data-url; the queue drives chunked lazy loading; g_profile_avatar_client_id is the client whose
// avatar belongs in the big right-pane #current-client-avatar.
var g_profile_avatar_client_id = -1;

// the avatar state; grid_visible gates painting faces into the small circles (in other themes that
// circle is the mic-state icon), the big right-pane #current-client-avatar works in every theme
var g_avatar = {
    grid_visible: false,   // the avatar grid member list (strip themes) is shown
    load_queue: [],        // client ids whose avatar is wanted, loaded in chunks
    load_scheduled: false,
    prefetch_queue: [],    // everyone connected, asked one at a time after joining
    prefetch_timer: null
};

// the desktop grid layout: the three columns (channels, chat, info) and the chat-input row in one css
// grid; order, the input row's place and column widths are editable and persist under "lemon_layout".
// touch devices keep the legacy layout, so grid_active stays false there
var g_layout = {
    grid_active: false,
    edit_active: false,
    panels: null,        // name -> panel element, filled at init
    drag: null,          // active column-width drag
    edit_dragged: null,  // panel name being moved in edit mode
    state: {             // what persists in localStorage under "lemon_layout"
        order: ["channels", "chat", "info"], // left-to-right column order
        input_col: "chat",                   // which column holds the chat-input row
        input_pos: "bottom",                 // "top" or "bottom" inside that column
        col_channels: "15%",                 // channels column width (becomes px after a drag)
        col_info: "13%"                      // info column width
    }
};

// ---- chat files ----

// transfers that are currently being received, stored by file id. encrypted_size is the total
// the progress ring divides by
var g_chat_file_transfers = {};

// decrypted files stored by card key, which is the server message id, or "local-N" for our own echo
// they live here because the download button should not carry megabytes in a dom attribute
var g_chat_files_by_message_id = {};

// the file the user attached but has not sent yet, as { name, size, mime, base64 }
var g_pending_chat_file = null;

// ---- server settings tab ----

var g_channel_properties_edit_channel_id = null; // the channel currently open in the edit form (null while creating); the form's icon box targets it

var g_icon_upload_queue = [];             // base64 icons waiting to be uploaded one at a time

var g_icon_upload_in_flight_base64 = null; // the icon whose server reply we are currently waiting for

// the admin's country block list as edited in the settings tab; the selectable countries come from
// the flag stylesheet the client ships, the display names from the browser's Intl

// the admin's unsaved block list, replaced whenever server_settings_values arrives
var g_blocked_countries_draft = [];

// ---- local settings (read from localStorage by node-runtime.js at load) ----

// size of the rsa identity keypair this device creates. it is part of the identity function:
// the same passphrase at a different size is a DIFFERENT keypair, so changing this makes the
// server see a new person. only the sizes the wasm and the server both accept are allowed
var G_ALLOWED_RSA_KEY_BITS = [2048, 3072, 4096, 6144, 8192];

var g_rsa_key_bits = 2048;

// local-only preference for the avatar circles next to chat messages
var g_show_message_avatars = false;

// read receipts have two halves that are set separately: whether we draw the eye others send
// us, and whether we send one back. both are on unless turned off
var g_show_seen_indicator = true;

var g_send_seen_receipts = true;

// a received message scrolls the chat to the end; off keeps the place of somebody reading older messages
var g_auto_scroll_chat_to_end = true;

// the user can hide the mic button outright; it stays visible by default
var g_hide_microphone_button = false;

// local-only preference for which microphone to capture from; "" means the browser's default
var g_selected_microphone_device_id = "";

// local-only preference that makes the mic button toggle transmission instead of push-to-talk
var g_is_continuous_mic_mode = false;

// true while continuous-mode transmission is running: a tap started it and no tap has stopped it yet
var g_is_continuous_transmission_active = false;

// which key pushes to talk. 81 is Q, the long-standing default
var g_push_to_talk_key_code = 81;

var g_push_to_talk_key_label = "Q";

// just a copy so the checkbox can show the right state, because the real flag lives in java
var g_is_file_logging_enabled = false;

// ---- node runtime (the headless android service) ----

// loopback mode means connecting to the node runtime on-device, plaintext and token-gated
// the port stays zero on the desktop and website, which never receive these settings fields
var g_loopback_port = 0;

var g_loopback_token = "";

// the server's auth frame, kept from the moment it arrives, because the ui replay leads with it
var g_node_cached_auth_frame = null;

// false parks the connection: the socket is closed and the reconnect ticker idles. it is
// always true in the browser; only the node host flips it, for the webview handover
var g_node_connection_wanted = true;

// set through the export seam. it receives every decrypted server frame raw, for the ui replay
var g_node_frame_listener = null;

// node only: true while a ui is attached to the loopback. node runs the same client
// code as the page, so without this it would assume somebody is reading its "current"
// channel and never count those messages as unread
var g_node_has_attached_ui = false;

// the bridge host's hook for incoming calls. it stays null everywhere but android
var g_node_incoming_call_listener = null;

// set through the export seam. every listener is called after every dispatched message
var g_node_message_listeners = [];

// set through the export seam. it receives the total unread count for the launcher icon badge
var g_node_unread_listener = null;

// ---- sounds ----

// the 14 ui sound clips; sounds.js fills it at load, main.js, ui.js and dispatch.js play them
var g_sound_effects = null;

// ---- audio and voice ----

const G_AUDIO_STATE = {
    PUSH_TO_TALK_ACTIVE: 1,
    PUSH_TO_TALK_ENABLED: 2,
    PUSH_TO_TALK_DISABLED_BUT_CAN_RECEIVE_AUDIO_FROM_OTHERS: 3,
    AUDIO_COMPLETELY_DISABLED: 4
};

// for voice chat

var g_peer_connection_with_server = null;

var g_iceconfig = null;

var g_datachannel = null;

var g_is_webrtc_datachannel_connected = false;

var g_is_webrtc_datachannel_check_running = false;

// server-side datachannel cooldown: after 10 attempts that never connected the server refuses to
// build peers for a while and says for how long; the retry loop sleeps that long instead of 10 s
var g_webrtc_datachannel_cooldown_until_ms = 0;

var g_datachannel_retry_sleep_resolve = null; // a login resolves it early, the server then starts counting afresh

var g_is_voice_chat_allowed_by_server = false;   // audio subsystem active (datachannel kept up); true when client voice OR music-bot audio is on

var g_is_client_microphone_allowed_by_server = false;   // may this client transmit its own mic; true only when client voice is on

var g_local_audio_stream = null;

var g_is_microphone_enabled = false;

var g_is_microphone_active = false;

// whether audio can actually be sent right now. false means the datachannel is gone or
// the server disabled audio for us
var g_is_microphone_available = false;

var g_is_microphone_always_on = false;

var g_last_sent_value_microphone_usage = false;

var g_audio_config = {
    codec: {
        bufferSize: 16384 / 2
    }
};

var g_opus_decoding_sampler = null;

var g_silence = null;

var g_audio_context = null;

var g_microphone_recorder = null;

var g_audio_player_gain_node = null;

var g_audio_recorder_gain_node = null;

var g_audio_input = null;

var g_client_volume_by_id = {};  // local per-client playback volume (client_id -> gain, 1.0 = default); worklet mode only, never sent to the server

// per-sender frame sequence for outgoing voice/song audio; 16 bits, wraps at 65536
var g_voice_send_sequence_number = 0;
