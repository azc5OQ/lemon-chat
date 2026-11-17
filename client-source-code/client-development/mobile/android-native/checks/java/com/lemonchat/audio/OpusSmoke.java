package com.lemonchat.audio;

/**
 * exercises the real JNI codec with deterministic audio and invalid buffers
 */
public final class OpusSmoke
{
    /**
     * runs the native codec compatibility checks
     *
     * @param arguments unused command line arguments
     */
    public static void main(String[] arguments)
    {
        for (int rate : new int[] {48000, 16000, 8000})
        {
            verifyRate(rate);
        }
        try (OpusCodec decoder = new OpusCodec(false, 48000, 2))
        {
            require(decoder.decode(new byte[0]) == null, "empty packet was accepted");
            require(decoder.decode(new byte[3839]) == null, "oversized packet was accepted");
        }
        OpusCodec closed = new OpusCodec(true, 48000, 1);
        closed.close();
        closed.close();
        boolean refused = false;
        try
        {
            closed.encode(new short[960], 960);
        }
        catch (IllegalStateException expected)
        {
            refused = true;
        }
        require(refused, "closed encoder accepted audio");
        System.out.println("PASS: real Opus JNI at 48/16/8 kHz, stereo decode, malformed bounds and codec closure");
    }

    /**
     * checks frame timing and decoded signal for one capture rate
     *
     * @param rate microphone sample rate
     */
    private static void verifyRate(int rate)
    {
        try (OpusCodec encoder = new OpusCodec(true, rate, 1); OpusCodec decoder = new OpusCodec(false, 48000, 2))
        {
            int samples = rate / 50;
            short[] input = new short[samples];
            long energy = 0;
            for (int frame = 0; frame < 10; frame++)
            {
                for (int index = 0; index < samples; index++)
                {
                    input[index] = (short)(Math.sin(2 * Math.PI * 440 * (frame * samples + index) / rate) * 10000);
                }
                byte[] packet = encoder.encode(input, samples);
                require(packet != null && packet.length > 0 && packet.length <= 1275, "invalid encoded packet");
                short[] output = decoder.decode(packet);
                require(output != null && output.length == 1920, "wrong playback frame length");
                for (int index = 0; index < output.length; index += 2)
                {
                    require(output[index] == output[index + 1], "mono voice changed stereo balance");
                    energy += (long)output[index] * output[index];
                }
            }
            require(energy > 100000000, "decoded voice was silent");
            require(encoder.encode(new short[10], samples) == null, "short PCM buffer was accepted");
            require(encoder.encode(new short[1000], 1000) == null, "oversized PCM frame was accepted");
        }
    }

    /**
     * fails the smoke run when an invariant does not hold
     *
     * @param condition expected result
     * @param message failure description
     */
    private static void require(boolean condition, String message)
    {
        if (!condition)
        {
            throw new AssertionError(message);
        }
    }
}
