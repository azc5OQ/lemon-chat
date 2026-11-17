// selects the worker handler using the name injected into this copy of the page
// loaded before the worker implementations so onmessage is installed early

// enable worker startup logging
var DBG_WORKER_BOOT_LOG = false;

// audio_opus_glue__create_new_webworker_in_same_file replaces this padded name
var THREAD_NAME = "                                                   ";

if (IS_CURRENT_THREAD_WORKER)
{
    THREAD_NAME = THREAD_NAME.trim();

    if (DBG_WORKER_BOOT_LOG)
    {
        console.log("[dispatch] THREAD_NAME=" + JSON.stringify(THREAD_NAME));
    }


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
    else if (THREAD_NAME == "video_worker")
    {
        global.onmessage = video_worker__onmessage;
    }

}
