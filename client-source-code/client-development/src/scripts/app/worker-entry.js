// ENTRY POINT OF EVERY WEB WORKER. the whole page is re-loaded inside each worker, so
// this file answers the only question a worker has on startup: "which one am I, and
// which onmessage handler do I run?". THREAD_NAME is what says which - it is patched
// into the copy of the source handed to each worker (create_new_webworker_in_same_file).
//
// it lives in its own file, included EARLY, so that every include after it runs AFTER
// onmessage is assigned: a later module-level error can then no longer leave a worker
// with no handler at all. the handler functions themselves live in main.js and are
// reachable here because function declarations hoist to the enclosing scope.

// worker boot diagnostics: flip to true to trace patching, dispatch and the keypair flow
var DBG_WORKER_BOOT_LOG = false;

// this variable value is hardcoded in create_new_webworker_in_same_file function
var THREAD_NAME = "                                                   ";

if (IS_CURRENT_THREAD_WORKER)
{
    THREAD_NAME = THREAD_NAME.trim();

    if (DBG_WORKER_BOOT_LOG) { console.log("[dispatch] THREAD_NAME=" + JSON.stringify(THREAD_NAME)); }


    if (THREAD_NAME == "data_processing_worker")
    {
        global.onmessage = data_processing_worker_onmessage;
    }
    else if (THREAD_NAME == "websocket_worker")
    {
        global.onmessage = websocket_worker_onmessage;
    }
    else if (THREAD_NAME == "opus_encoder_worker")
    {
        global.onmessage = opus_encoder_worker_onmessage;
        asm = create_opus_webassembly_instance();
    }
    else if (THREAD_NAME == "opus_decoder_worker")
    {
        global.onmessage = opus_decoder_worker_onmessage;
        asm = create_opus_webassembly_instance();
    }
    else if (THREAD_NAME == "minimp3_worker")
    {
        global.onmessage = minimp3_worker_onmessage;
    }

}
