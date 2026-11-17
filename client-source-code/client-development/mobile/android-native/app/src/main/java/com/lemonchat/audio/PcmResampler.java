package com.lemonchat.audio;

/** converts a continuous mono or stereo stream to stereo at 48 kHz */
final class PcmResampler
{
    interface Sink
    {
        void sample(short left, short right) throws Exception;
    }

    private final int rate;
    private final int channels;
    private long sourceFrame;
    private long nextOutput;
    private short previousLeft;
    private short previousRight;
    private boolean previous;

    PcmResampler(int rate, int channels)
    {
        if (rate < 8000 || rate > 192000 || channels < 1 || channels > 2)
        {
            throw new IllegalArgumentException("Unsupported music sample format");
        }
        this.rate = rate;
        this.channels = channels;
    }

    void accept(short[] samples, int count, Sink sink) throws Exception
    {
        if (count < 0 || count > samples.length || count % channels != 0)
        {
            throw new IllegalArgumentException("Incomplete music sample frame");
        }
        for (int offset = 0; offset < count; offset += channels)
        {
            short left = samples[offset];
            short right = channels == 2 ? samples[offset + 1] : left;
            if (!previous)
            {
                previousLeft = left;
                previousRight = right;
                previous = true;
                sink.sample(left, right);
                nextOutput = rate;
                continue;
            }
            sourceFrame++;
            long boundary = sourceFrame * 48000L;
            while (nextOutput <= boundary)
            {
                double fraction = (nextOutput - (sourceFrame - 1) * 48000L) / 48000.0;
                sink.sample((short)Math.round(previousLeft + (left - previousLeft) * fraction), (short)Math.round(previousRight + (right - previousRight) * fraction));
                nextOutput += rate;
            }
            previousLeft = left;
            previousRight = right;
        }
    }
}
