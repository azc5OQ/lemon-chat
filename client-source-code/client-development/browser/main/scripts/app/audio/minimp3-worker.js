// decodes MP3 data into mono PCM and reports its original sample rate

var mp3_to_pcm_decoder = null;
var mp3_to_pcm_wasm = null;


/**
 * @brief keeps the first channel of interleaved pcm
 *
 * @param Int16Array pcm -> the decoded samples, interleaved when there is more than one channel
 * @param number numChannels -> how many channels the samples interleave
 *
 * @return Int16Array the first channel only. the input itself when it was mono already
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


// seconds decoded per call. The decoder keeps the whole MP3 file and the PCM of one call in its WebAssembly
// memory. Decoding a whole song in one call needs about 50 MB for 4 minutes of stereo sound; some phones
// (32 bit Android WebView) cannot grow WebAssembly memory beyond about 32 MB and the decoder then stops with
// "unreachable". 20 seconds of stereo PCM at 48 kHz are 3.7 MB
var MINIMP3_SEGMENT_SECONDS = 20;


/**
 * @brief decodes MP3 data segment by segment into the first channel as 16 bit samples
 *
 * @param Uint8Array data -> the whole MP3 file
 *
 * @return object { pcm: Int16Array, sample_rate: number }
 */
function minimp3_worker__decode_mono_in_segments(data)
{
    let wasm = mp3_to_pcm_decoder.wasm;

    // copy the file
    wasm.decoder_set_mp3_data_size(data.byteLength);
    new Uint8Array(wasm.memory.buffer, wasm.decoder_mp3_data_offset(), wasm.decoder_mp3_data_size()).set(data);

    // measure the song
    wasm.decoder_seek(-1);
    let channels = Math.max(1, wasm.decode_results_num_channels());
    let sample_rate = wasm.decode_results_sampling_rate();
    let pcm = new Int16Array(Math.floor(wasm.decode_results_num_samples() / channels));
    wasm.decoder_seek(0);

    // decode the segments
    let written = 0;
    while (written < pcm.length)
    {
        wasm.decoder_decode(MINIMP3_SEGMENT_SECONDS);
        let segment_bytes = wasm.decoder_pcm_data_size();
        if (segment_bytes == 0)
        {
            break;
        }

        // keep first channel
        let segment = new Int16Array(wasm.memory.buffer, wasm.decoder_pcm_data_offset(), segment_bytes / 2);
        let mono = minimp3_worker__to_mono_pcm(segment, channels);
        let count = Math.min(mono.length, pcm.length - written);
        pcm.set(mono.subarray(0, count), written);
        written += count;
    }
    return { pcm: written == pcm.length ? pcm : pcm.slice(0, written), sample_rate: sample_rate };
}


/**
 * @brief initializes the MP3 decoder or decodes a file into mono PCM
 *
 * @param object e -> the worker message event. E.data.type picks the branch
 *
 * @return void
 */
function minimp3_worker__minimp3_worker_onmessage(e)
{
    if (e.data.type == "decode")
    {
        let decoded = minimp3_worker__decode_mono_in_segments(new Uint8Array(e.data.value));

        // hand over without a copy
        global.postMessage({
            type: "minimp3_worker__decode_result",
            value: decoded.pcm,
            mp3_sample_rate: decoded.sample_rate
        }, [decoded.pcm.buffer]);
    }
    else if (e.data.type == "init")
    {
        let mp3_decoder_webassembly_base64 = "@@WASM:wasm/mp3_decoder.wasm@@";
        const buffer = Uint8Array.from(atob(mp3_decoder_webassembly_base64), character => character.charCodeAt(0));
        mp3_to_pcm_wasm = new WebAssembly.Instance(new WebAssembly.Module(buffer), {});
        mp3_to_pcm_decoder = new Decoder(mp3_to_pcm_wasm.exports);
    }
}