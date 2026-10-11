// video-stubs.js - the video streaming functions that the shared client code calls, as no-ops
// for the headless node runtime. video.js itself needs WebCodecs and the DOM, so the node bundle
// leaves it out; without these stubs the authentication, client list and channel key handlers
// in dispatch.js, messages.js and server-settings-tab.js throw a ReferenceError in node.

function video__apply_client_list(clients) { }
function video__apply_policy_to_ui() { }
function video__init_ui() { }
function video__on_datachannel_received(channel) { }
function video__on_keyframe_request() { }
function video__on_local_channel_changed() { }
function video__on_offer(message) { }
function video__on_refused(message) { }
function video__on_state(message) { }
function video__on_viewer_request(message) { }
function video__on_viewer_state(message) { }
function video__on_worker_message(data) { }
function video__post_channel_keys_to_worker() { }
function video__refresh_controls() { }
function video__reset_on_disconnect() { }
function video__on_media_frame(frame) { }
function video__post_transport_keys_to_worker() { }
