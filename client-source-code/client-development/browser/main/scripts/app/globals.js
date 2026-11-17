// stores shared client state inside the common application closure

// connection and session

var g_are_server_details_predefined = false;

// controls connection startup before the browser receives a user gesture

var g_is_autoconnect_without_user_action_active = false;

// an imported identity is waiting for key generation
var g_is_identity_import_pending = false;

// the last connection attempt failed
var g_last_connect_attempt_failed = false;

// predefined server details bypass the connection form

var g_autoconnect_details = {
    host: "192.168.1.106",
    port: 1111,
    keys: [
        "test",
        // "test2"
    ]
};

// requested username, or empty to use the name assigned by the server
var g_chosen_username = "";

var g_host = "";

var g_is_websocket_connected = false;

var g_is_authenticated = false;

var g_should_connection_check_be_running = false;

// each browser connection gets a new pair of ports between the existing workers
var g_worker_connection_generation = 0;
var g_worker_connection_active = false;
var g_worker_connection_ended = false;

// send regular heartbeats and declare the connection lost after the reply deadline
var g_connection_check = {
    interval_ms: 10 * 1000,
    lost_threshold_ms: 35 * 1000,
    sleep_resolve: null,        // wakes the check loop early
    last_response_timestamp: 0  // set on every heartbeat reply, first when the client connects
};

// tracks session traffic and heartbeat latency for the connection information card
var g_session = {
    bytes_sent: 0,
    bytes_received: 0,
    connected_at: 0,
    ping_sent_at: 0,
    last_ping_ms: -1
};

// retains the current session while bounded recovery attempts run
var g_fast_reconnect = {
    in_progress: false,   // recovery is running without wiping the chat or restarting its deadline
    resumed: false,       // the server said fast_reconnect_ok: the lists that follow are a refresh
    deadline_timer: null, // the classic "connection lost" path when the attempt stalls
    pending_lists: null   // the refreshed lists, applied together so the page repaints once
};

// public key and identity seed for connection startup
// the private key stays in the worker
var g_identity_slot = null;

// login page connection status, supplied by Node on Android
var g_connection_status = {
    state: "idle",          // idle | connecting | waiting_retry | connected
    reason: "",             // why the last attempt failed, human readable
    next_retry_at: 0,       // ms timestamp of the next automatic attempt, 0 = none
    last_connected_at: 0    // ms timestamp the server connection last existed, 0 = never
};

// connection status listeners for the local page and native bridge
var g_connection_status_listeners = [];

// filled by the close and error handlers, read by the connect loop for the retry status
var g_last_disconnect_reason = "";

// native network availability, or null before the first update
var g_device_has_network = null;

// identity and crypto

var g_metadata_keys = [];

// on android the node runtime hands the session and configured server keys to the webview over loopback
var g_media_keys_from_host = null;
var g_media_server_key_layers_from_host = null;

var g_dh_generator = null;

var g_dh_modulus = null;

var g_dh_secret_exponent = null;

if (typeof window != 'undefined')
{
    g_dh_generator = 2n;

    var dh_modulus_bits = 8192; // 2048, 4096 or 8192, matching the server's DH_MODULUS_BITS

    // safe prime selected by dh_modulus_bits
    var dh_modulus_string = "32317006071311007300338913926423828248817941241140239112842009751400741706634354222619689417363569347117901737909704191754605873209195028853758986185622153212175412514901774520270235796078236248884246189477587641105928646099411723245426622522193230540919037680524235519125679715870117001058055877651038861847280257976054903569732561526167081339361799541336476559160368317896729073178384589680639671900977202194168647225871031411336429319536193471636533209717077448227988588565369208645296636077250268955505928362751121174096972998068410554359584866583291642136218231078990999448652468262416972035911852507045361090559";
    if (dh_modulus_bits == 4096)
    {
        dh_modulus_string = "769693417275193209984647063932271739387855846059952565355802298991172654607712104048642837327086393649061117273977479029847880929901816490502575106445708811728815104699538212859676255621694933541780065300216380119365448477045659714142752962409351060465337847941705392356465059912091910379610354725312649190019796723866880686790102505810145302961022375682955537024852712153016097337874982984026217981644194741246064934907045623310252540105014478602030042625050790892256552738501094150544017503366521405695110938208299693781667383898463231081051406284286841973557391837014717399840317430691522097547152517168661381236591027075489125945391080953462725086945640553263655450552529331021143283920078415126554651532264427032676910742519456229972244566183184460134372157613705601578581124341629241972089511142281008551119184446873409929566545851290281361166571221433352162292794386037934251491816926283501321267998170847037246436312385384549128374516008246102779333275418845691810684079267893733515705735729967088709311436111220883412133997678612136656076019025878523079903588858263406471350679442414909683284164773663260965983542100471510190345089294782440003918912451456992077790689703598681468439291465843547007915368517110174489101683183";
    }
    if (dh_modulus_bits == 8192)
    {
        dh_modulus_string = "977457999394373613160803436413990067824664325329752783398860665650891758153546599425963979290921371512554424155404999662026528515231003619059250227581073193191817667145872254566150016152267378041043707227533090523130854162674719502912605093407840683480360272745764870153876137077404098306192221010043540729677343845507367074906936225679715947791388837030145881441948294883090889231339114529926218527174080089614380520453541140942641189135120655392817995558672348259240314664441211284094553472715483266674338226096876439738570920830431990068192898823624154982561509367679529622217472548774982858792731946532808614927934739139866206407985368007801168974233995065956999264060271473667589912911635282735847769997114239537087675283088650912126339354407150753898672557625721020468123121985217987055904104587919799098757089865856148151372159009557757252554523938389147793317088678845894492183206966490686288450161789777284440531583751707235355985011244929317700461604659491747569397015417605015844114189684757052804077400345793511780894375616367276781058142309055525279814951138171200246013005920763485637476273362810215876618783686490089700542042236756450450875748150296041125307287408385472961923570457489504445415040799421139916299426300456654408986639074321942096027448333024943556858265321864649169731360525833923693176347415312865652669889035367188555225154697353012485541899891405325448203641838378457181625265662866012641101851261909549715296062722220871699753970653198277244417978954211900339094323007747108915608099803996978341421956304178871205193962176652206358214977516703892527582137534788148271002406853630936928238671372686645287319965234542895792366832783193938302132219463877916744337606350077802640502896551808414614146119887726456058723288826440787605655349724793408478985959688481296784606909713086104258354166909655924759369179237403392373490546567686597579050582406536565868808879640217167277372359442106490085619712603267716148318470568791898988932303644832977117895522999512850187807810874398403568491329765776005361335497620431318887438033026603280081068652656086250926318691324234636743583950635209943952384403217947022081052893713058850302983931039183796265186758214153198152532323300955155280353467780525511888234737731346004632762960546857790075686663681335897303755575277977768637069626833995439307976899886428471454602498498262648098952772543021280017770216979020055000596511525631600903871686087096720903178326987700681798435674674029499323";
    }
    g_dh_modulus = BigInt(dh_modulus_string);
}

var g_my_rsa_key_object = null;

var g_rsa_public_key_string = "";

var g_identity_string = "";

// tracks the selected identity separately from the next identity's size preference
var g_identity_revision = 0;
var g_identity_rsa_key_bits = 0;
var g_identity_import_expected_revision = 0;

var g_is_rsa_key_generated = false;

var g_keys_init_status = false;

var g_is_identity_switch_in_progress = false; // identity import is replacing the key pair and connection

// last key size requested by the server, to avoid repeated prompts
var g_rsa_key_too_weak_prompted_for_bits = 0;

// server policy

// server policy from login and later updates, with defaults for missing fields
var g_server_policy = {
    is_permission_system_enabled: true,
    is_stored_client_list_allowed: false,
    is_blocking_direct_chat_pictures_active: false,
    is_blocking_direct_chat_files_active: false,
    is_blocking_channel_chat_pictures_active: false,
    is_blocking_channel_chat_files_active: false,
    is_connection_check_enforced: true,
    is_channel_key_reuse_enabled: true,
    is_channel_key_reuse_on_join_allowed: true,
    is_channel_key_reuse_on_leave_allowed: true,
    channel_key_reuse_join_min_clients: 50,
    channel_key_reuse_leave_min_clients: 100,
    is_fast_reconnect_allowed: false,          // a lost socket may resume its session instead of starting over
    is_music_bot_marquee_visible_to_everyone: false, // a streaming bot's marquee also for people outside its channel
    is_typing_indicator_allowed: false,             // "x is typing ..." is sent and shown
    is_client_rename_allowed: true,                // allows renaming, with an exception for admins when disabled
    is_link_preview_allowed: false, // allows URL lookup when enabled by the server
    is_avatar_upload_allowed: false,                      // image avatars on identities
    avatar_max_size: 51200,                    // largest raw avatar image in bytes
    is_file_upload_allowed: false,                 // file sending is disabled until the server permits it
    file_upload_max_size: 10 * 1024 * 1024,    // largest raw chat file in bytes
    is_chat_picture_allowed: true,                 // inline pictures are enabled unless the server disables them
    chat_picture_max_size: 4 * 1024 * 1024,    // maximum inline picture size in bytes
    icon_max_size: 5000,                       // largest raw group/channel icon in bytes (the server settings tab upload)
    is_video_streaming_allowed: false,         // enable channel screen and video file streaming controls
    is_audio_over_websocket_allowed: false,    // allow voice over the /audio websocket
    is_audio_over_webrtc_allowed: true,        // voice may use the webrtc datachannel
    is_voice_chat_active: true,                // allow relaying client voice
    is_music_bot_audio_active: true            // the server relays music bot audio
};

// preferred audio transport, subject to server policy: webrtc, websocket or none
var g_audio_connection_preference = "webrtc";

// who is here

var g_local_username = "";

var g_local_client_id = 0;

// true once the server granted this session the admin group
var g_is_local_client_admin = false;

var g_client_list = [];

var g_channel_list = [];

var g_icons = [];

var g_groups = [];

var g_map_client_id_to_array_index = new Map();

var g_offline_client_list = []; // might be supported by the server, might not

var g_is_client_list_retrieved = false;

var g_is_channel_list_retrieved = false;

var g_selected_channel_id;

var g_selected_client_id;

var g_current_channel_id = 0;

// root level lookup marker, separate from a channel's is_root_channel flag
const g_ROOT_LEVEL_PARENT_MARKER = -1;

var g_chat_message_author_public_keys = {}; // message authors' public keys, used to validate edits and deletions

// channel keys

var g_current_channel_keys = null;

// previous channel key sets, each with an expiry time ten minutes after replacement
var g_historic_keys_of_current_channel = [];

// chat context and composer

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

// selected offline identity, or empty for channels and connected recipients
var g_offline_chat_recipient_identity_hash = "";

var g_selected_server_chat_message_id = null;

var g_local_message_id = 0;

var g_selected_font = "custom-font-usage-default";

var g_selected_font_color = "#ffffff";

var g_selected_font_size = 12;

var g_base64_picture_string_to_send = "" ;

var g_file_send_intent = "";

var g_file_send_intent_extra_data = {};

var g_is_file_being_uploaded = false;

// platform

var g_is_client_running_under_touch_device = false;  // for touch devices

var g_is_running_in_android_webview = false;

// prefetch connected users' avatars after joining
var g_android_app_mode = "";

// distinguishes initial native settings from live updates
var g_have_received_android_settings = false;

var g_is_microphone_enabled_on_touch_device = false;  // for touch devices

// excludes the headless service's compatibility document
var G_HAS_DOM = (typeof window !== "undefined" && typeof document !== "undefined" && document.lemonchat_headless !== true);

// workers

var g_opus_encoder_worker = null;

var g_opus_decoder_worker = null;

var g_data_processing_worker = null;

var g_minimp3_worker = null;

var g_websocket_worker = null;

// ui flags

var g_textarea_log = null;

var g_is_chat_hidden = false;

var g_show_hide_toggle = false;

// optional theme features: flat channels and a live member list
var g_is_channel_list_flattened = false;

var g_alert_push_to_talk_key_shown_once = false;

var g_alert_streaming_music_shown_once = false;

var g_are_sound_effects_enabled = true;

// allows a themed cursor, saved locally as lemon_custom_cursor
var g_is_custom_cursor_enabled = true;

var g_stop_song_stream_message_received = false;

var g_selected_song_name = null;

// connect page hold

// hides the connect page while the initial Android attachment is pending
var g_is_holding_back_connect_page = true;

// deadline for showing the connect screen
var g_connect_holdback_deadline = 0;

var g_connect_holdback_started = 0;

// idle

// background idle keeps only the websocket and slow heartbeat
var g_is_deep_idle = false;

var g_is_deep_idle_pending = false;

// typing, unread, avatars, layout

// tracks typing notices by conversation, client and expiry time
var g_typing = {
    state: {},          // chat context id -> { client_id: expiry_timestamp_ms }
    last_sent_at: 0,
    render_timer: null
};

// unread count by channel ID, shared with Node notifications
var g_channel_unread_counts = {};

// stores avatar content, loading requests and the selected profile avatar
var g_profile_avatar_client_id = -1;

// avatar state for the member list and profile panel
var g_avatar = {
    grid_visible: false,   // the avatar grid member list (strip themes) is shown
    load_queue: [],        // client ids whose avatar is wanted, loaded in chunks
    load_scheduled: false,
    prefetch_queue: [],    // everyone connected, asked one at a time after joining
    prefetch_timer: null
};

// stores the desktop panel layout and column sizes
var g_layout = {
    grid_active: false,
    edit_active: false,
    panels: null,        // name -> panel element, filled at init
    drag: null,          // active column width drag
    edit_dragged: null,  // panel name being moved in edit mode
    state: {             // what persists in localStorage under "lemon_layout"
        order: ["channels", "chat", "info"], // column order from left to right
        input_col: "chat",                   // column containing the message input
        input_pos: "bottom",                 // "top" or "bottom" inside that column
        toolbar_col: "full",                 // full width, or attached to one column
        toolbar_pos: "top",
        col_channels: "15%",                 // channels column width (becomes px after a drag)
        col_info: "13%"                      // info column width
    }
};

// chat files

// receive buffers indexed by file ID
var received_files = []; // incoming file chunks

// incoming transfers by file ID. encrypted_size determines progress
var g_chat_file_transfers = {};

// decrypted file data by message ID or local echo key, kept outside DOM attributes
var g_chat_files_by_message_id = {};

// the file the user attached but has not sent yet, as { name, size, mime, base64 }
var g_pending_chat_file = null;

// server settings tab

var g_channel_properties_edit_channel_id = null; // channel being edited, or null when creating one

var g_icon_upload_queue = [];             // base64 icons waiting to be uploaded one at a time

var g_icon_upload_in_flight_base64 = null; // the icon whose server reply we are currently waiting for

// draft country block list for server settings

// preserve the country draft while edits are unsaved
var g_blocked_countries_draft = [];

// local settings (read from localStorage by node-runtime.js at load)

// the RSA size and passphrase together determine the generated identity
var G_ALLOWED_RSA_KEY_BITS = [2048, 3072, 4096, 6144, 8192];

var g_rsa_key_bits = 2048;

// local preference for avatars beside messages
var g_show_message_avatars = false;

// display message text in this theme's font, size and color without changing what was sent
var g_use_theme_chat_formatting = false;
var g_attachment_receive_preferences =
{
    block_direct_chat_pictures: false,
    block_direct_chat_files: false,
    block_channel_chat_pictures: false,
    block_channel_chat_files: false
};

// separate preferences for displaying and sending read receipts
var g_show_seen_indicator = true;

var g_send_seen_receipts = true;

// scroll to new messages unless the user disabled it
var g_auto_scroll_chat_to_end = true;

// show the touch microphone control unless hidden in local settings
var g_hide_microphone_button = false;

// separate microphone visibility preference for the Mobile theme
var g_hide_mobile_microphone_button = false;

// preferred microphone, or empty for the browser default
var g_selected_microphone_device_id = "";

// toggle microphone transmission instead of requiring a held button
var g_is_continuous_mic_mode = false;

// continuous microphone transmission is active
var g_is_continuous_transmission_active = false;

// talk key code, defaulting to 81 for Q
var g_push_to_talk_key_code = 81;

var g_push_to_talk_key_label = "Q";

// keep typing in the chat box from opening the microphone
var g_is_push_to_talk_disabled_while_typing = true;

// copy of the native debug logging preference
var g_is_file_logging_enabled = false;

// node runtime (the headless android service)

// local Node connection protected by a token, disabled when the port is zero
var g_loopback_port = 0;

var g_loopback_token = "";

// authentication frame retained for page replay
var g_node_cached_auth_frame = null;

// false closes the connection and pauses retries until the host allows them
var g_node_connection_wanted = true;

// receives decrypted server frames for page replay
var g_node_frame_listener = null;

// page visibility for Node idle policy
// read commands control unread counts
var g_node_has_attached_ui = false;

// native callback for incoming calls
var g_node_incoming_call_listener = null;

// listeners called after shared message dispatch
var g_node_message_listeners = [];

// receives the total unread count for the launcher badge
var g_node_unread_listener = null;

// sounds

// interface sound clips loaded by sounds.js
var g_sound_effects = null;

// audio and voice

var g_audio_transport =
{
    mode: "webrtc",
    url: "",
    socket: null,
    ready: false,              // the websocket is attached and authenticated
    pending_until: 0,          // no new session request before this time
    request_attempts: 0,       // failed attachment count used to calculate the next retry delay
    preferred_down_since: 0,   // when the preferred connection was last up, or when the attempt began
    last_received: 0,
    datachannel_last_received: 0, // the last frame of any kind from the data channel
    datachannel_last_probe: 0,
    frame_counter: null,       // outgoing frame encryption counter
    timer: null
};

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

// video streaming (video.js, video-worker.js)

var g_video_datachannel = null;                 // the second, reliable datachannel from the server (label "video")

var g_is_video_datachannel_connected = false;

var g_video_worker = null;                      // video worker for encoding, encryption, decoding and display

// the one stream of the current channel as this client sees it
var g_video_stream = {
    is_active: false,               // somebody (maybe us) streams in the current channel
    streamer_client_id: -1,
    source: "",                     // "screen" or "file"
    codec: "",
    width: 0,
    height: 0,
    fps: 0,
    role: "none",                   // "streamer", "viewer" or "none"
    is_start_pending: false,        // we asked the server to start and wait for its video_stream_state
    is_offered: false,              // the server sent us the offer, we may press connect
    is_watching: false,             // we pressed connect and the server relays to us
    is_decoder_supported: false,    // the announced codec decodes here
    is_popup_open: false,
    viewer_count: 0,                // streamer side
    pending_viewer_requests: [],    // viewers waiting for the streamer's approval
    is_sound_included: false,       // the stream carries sound packets next to the picture packets (video.js)
    watching_client_ids: [],        // viewers announced to the streamer and permitted members
    epoch: 0                        // bumped per stream we start, so viewers never mix two streams
};

// saved stream playback volume and local monitoring preference
var g_video_stream_sound_volume = 1;
var g_is_video_stream_monitor_enabled = true;

var g_is_webrtc_datachannel_check_running = false;

// wait for the server's datachannel cooldown before retrying
var g_webrtc_datachannel_cooldown_until_ms = 0;

var g_datachannel_retry_sleep_resolve = null; // a login resolves it early, the server then starts counting afresh

var g_is_voice_chat_allowed_by_server = false;   // audio subsystem is active for client voice or music bots

var g_is_client_microphone_allowed_by_server = false;   // client voice transmission is permitted

var g_local_audio_stream = null;

var g_is_microphone_enabled = false;

var g_is_microphone_active = false;

// the active audio connection permits sending
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

var g_client_volume_by_id = {};  // local playback gain by client ID, defaulting to 1.0

// outgoing audio sequence by sender, wrapping at 65536
var g_voice_send_sequence_number = 0;
