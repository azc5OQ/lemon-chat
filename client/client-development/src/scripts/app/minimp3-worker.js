
            var mp3_to_pcm_decoder = null;
            var mp3_to_pcm_wasm = null;


            function to_mono_pcm(pcm, numChannels)
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


            async function minimp3_worker_onmessage(e)
            {
                if (e.data.type == "decode")
                {
                    var data = new Uint8Array(e.data.value);
                    let decoded = mp3_to_pcm_decoder.decode(data);
                    let result = to_mono_pcm(decoded.pcm, decoded.numChannels);

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