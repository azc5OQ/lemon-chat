
        var samplesPerCallback = 2048;			//Has to be between 2048 and 4096 (If over, then samples are ignored, if under then silence is added).
        var outputConvert = null;


        //Audio API Event Handler:
        var audioContextHandle = null;
        var audioNode = null;
        var audioSource = null;
        var launchedContext = false;
        var audioContextSampleBuffer = [];
        var resampled = [];
        var webAudioMinBufferSize = 15000;
        var webAudioMaxBufferSize = 25000;
        var webAudioActualSampleRate = 44100;
        var XAudioJSSampleRate = 0;
        var webAudioMono = false;
        var defaultNeutralValue = 0;
        var resampleControl = null;
        var audioBufferSize = 0;
        var resampleBufferStart = 0;
        var resampleBufferEnd = 0;
        var resampleBufferSize = 2;

        function audioOutputEvent(event)
        {		//Web Audio API callback...
            var index = 0;
            var buffer1 = event.outputBuffer.getChannelData(0);
            var buffer2 = event.outputBuffer.getChannelData(1);
            resampleRefill();
            if (!webAudioMono)
            {
                //STEREO:
                while (index < samplesPerCallback && resampleBufferStart != resampleBufferEnd)
                {
                    buffer1[index] = resampled[resampleBufferStart++];
                    buffer2[index++] = resampled[resampleBufferStart++];
                    if (resampleBufferStart == resampleBufferSize)
                    {
                        resampleBufferStart = 0;
                    }
                }
            }
            else
            {
                //MONO:
                while (index < samplesPerCallback && resampleBufferStart != resampleBufferEnd)
                {
                    buffer2[index] = buffer1[index] = resampled[resampleBufferStart++];
                    ++index;
                    if (resampleBufferStart == resampleBufferSize)
                    {
                        resampleBufferStart = 0;
                    }
                }
            }
            //Pad with silence if we're underrunning:
            while (index < samplesPerCallback)
            {
                buffer2[index] = buffer1[index] = defaultNeutralValue;
                ++index;
            }
        }
        function resampleRefill()
        {
            if (audioBufferSize > 0)
            {
                //Resample a chunk of audio:
                var resampleLength = resampleControl.resampler(getBufferSamples());
                var resampledResult = resampleControl.outputBuffer;
                for (var index2 = 0; index2 < resampleLength; ++index2)
                {
                    resampled[resampleBufferEnd++] = resampledResult[index2];
                    if (resampleBufferEnd == resampleBufferSize)
                    {
                        resampleBufferEnd = 0;
                    }
                    if (resampleBufferStart == resampleBufferEnd)
                    {
                        ++resampleBufferStart;
                        if (resampleBufferStart == resampleBufferSize)
                        {
                            resampleBufferStart = 0;
                        }
                    }
                }
                audioBufferSize = 0;
            }
        }
        function resampledSamplesLeft()
        {
            return ((resampleBufferStart <= resampleBufferEnd) ? 0 : resampleBufferSize) + resampleBufferEnd - resampleBufferStart;
        }
        function getBufferSamples()
        {
            //Typed array and normal array buffer section referencing:
            try
            {
                return audioContextSampleBuffer.subarray(0, audioBufferSize);
            }
            catch (error)
            {
                try
                {
                    //Regular array pass:
                    audioContextSampleBuffer.length = audioBufferSize;
                    return audioContextSampleBuffer;
                }
                catch (error)
                {
                    //Nightly Firefox 4 used to have the subarray function named as slice:
                    return audioContextSampleBuffer.slice(0, audioBufferSize);
                }
            }
        }

        function initializeWebkitAudio()
        {
            if (!launchedContext)
            {
                try
                {
                    console.log("audioContextHandle = new AudioContext();");
                    audioContextHandle = new AudioContext();							//Create a system audio context.
                }
                catch (error)
                {
                    try
                    {
                        audioContextHandle = new AudioContext();								//Create a system audio context.
                    }
                    catch (error)
                    {
                        return;
                    }
                }
                try
                {
                    audioSource = audioContextHandle.createBufferSource();						//We need to create a false input to get the chain started.
                    audioSource.loop = false;	//Keep this alive forever (Event handler will know when to ouput.)
                    XAudioJSSampleRate = webAudioActualSampleRate = audioContextHandle.sampleRate;
                    audioSource.buffer = audioContextHandle.createBuffer(1, 1, webAudioActualSampleRate);	//Create a zero'd input buffer for the input to be valid.
                    audioNode = audioContextHandle.createJavaScriptNode(samplesPerCallback, 1, 2);			//Create 2 outputs and ignore the input buffer (Just copy buffer 1 over if mono)
                    audioNode.onaudioprocess = audioOutputEvent;								//Connect the audio processing event to a handling function so we can manipulate output
                    audioSource.connect(audioNode);												//Send and chain the input to the audio manipulation.
                    audioNode.connect(audioContextHandle.destination);							//Send and chain the output of the audio manipulation to the system audio output.
                    audioSource.noteOn(0);														//Start the loop!
                }
                catch (error)
                {
                    return;
                }
                launchedContext = true;
            }
        }
