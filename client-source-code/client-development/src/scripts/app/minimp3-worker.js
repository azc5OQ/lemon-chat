// minimp3-worker.js is embedded in template.html along with the other client files
// it is the mp3 decoder worker for the music bot: it turns an uploaded mp3 into 48 kHz pcm that
// voice.js streams to the server; dispatch.js receives the decoded chunks

var mp3_to_pcm_decoder = null;
var mp3_to_pcm_wasm = null;


/**
 * @brief keeps the first channel of interleaved pcm
 *
 * @param Int16Array pcm -> the decoded samples, interleaved when there is more than one channel
 * @param number numChannels -> how many channels the samples interleave
 *
 * @return Int16Array the first channel only; the input itself when it was mono already
 */
function minimp3_worker__to_mono_pcm(pcm, numChannels)
{
    if (numChannels === 1)
    {
        return pcm;
    }
    const length = Math.floor(pcm.length / numChannels);
    const mono = new Int16Array(length);
    for (let i = 0, j = 0; i < length; i += 1, j += numChannels)
    {
        mono[i] = pcm[j];
    }
    return mono;
}


/**
 * @brief the mp3 decoder worker's message handler: "init" builds the decoder, "decode" turns an mp3 into mono pcm and posts it back
 *
 * @param object e -> the worker message event; e.data.type picks the branch
 *
 * @return promise resolves when the message is handled
 */
async function minimp3_worker__minimp3_worker_onmessage(e)
{
    if (e.data.type == "decode")
    {
        var data = new Uint8Array(e.data.value);
        let decoded = mp3_to_pcm_decoder.decode(data);
        let result = minimp3_worker__to_mono_pcm(decoded.pcm, decoded.numChannels);

        global.postMessage({
            type: "minimp3_worker__decode_result",
            value: result,
            mp3_sample_rate: decoded.samplingRate
        });
    }
    else if (e.data.type == "init")
    {
        let mp3_decoder_webassembly_base64 = "@@WASM:wasm/mp3_decoder.wasm@@";
        const buffer = decode(mp3_decoder_webassembly_base64);
        mp3_to_pcm_wasm = await WebAssembly.instantiate(buffer, {});
        mp3_to_pcm_decoder = new Decoder(mp3_to_pcm_wasm.instance.exports);
    }
}