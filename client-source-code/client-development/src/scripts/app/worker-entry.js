// worker-entry.js is embedded in template.html along with the other client files
// it is the entry point of every web worker: the whole page is re-loaded inside each worker, and the
// THREAD_NAME that audio_opus_glue__create_new_webworker_in_same_file patches in says which onmessage handler this one runs
// it is included early, so a load error in a later file can never leave a worker without a handler

// worker boot diagnostics: flip to true to trace patching, dispatch and the keypair flow
var DBG_WORKER_BOOT_LOG = false;

// this variable value is hardcoded in audio_opus_glue__create_new_webworker_in_same_file function
var THREAD_NAME = "                                                   ";

if (IS_CURRENT_THREAD_WORKER)
{
    THREAD_NAME = THREAD_NAME.trim();

    if (DBG_WORKER_BOOT_LOG) { console.log("[dispatch] THREAD_NAME=" + JSON.stringify(THREAD_NAME)); }


    if (THREAD_NAME == "data_processing_worker")
    {
        global.onmessage = workers__data_processing_worker_onmessage;
    }
    else if (THREAD_NAME == "websocket_worker")
    {
        global.onmessage = workers__websocket_worker_onmessage;
    }
    else if (THREAD_NAME == "opus_encoder_worker")
    {
        global.onmessage = audio_opus_glue__opus_encoder_worker_onmessage;
        asm = create_opus_webassembly_instance();
    }
    else if (THREAD_NAME == "opus_decoder_worker")
    {
        global.onmessage = audio_opus_glue__opus_decoder_worker_onmessage;
        asm = create_opus_webassembly_instance();
    }
    else if (THREAD_NAME == "minimp3_worker")
    {
        global.onmessage = minimp3_worker__minimp3_worker_onmessage;
    }

}
