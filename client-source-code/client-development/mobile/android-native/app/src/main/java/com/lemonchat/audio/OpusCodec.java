package com.lemonchat.audio;

/**
 * owns one native Opus encoder or decoder
 */
final class OpusCodec implements AutoCloseable
{
    static
    {
        System.loadLibrary("lemonnative");
    }

    private long handle;
    private final boolean encoder;
    private final int channels;

    /**
     * creates a codec with the requested sample format
     *
     * @param encoder true for microphone encoding
     * @param sampleRate samples per second
     * @param channels one for capture or two for playback
     */
    OpusCodec(boolean encoder, int sampleRate, int channels)
    {
        this.encoder = encoder;
        this.channels = channels;
        handle = nativeCreate(encoder, sampleRate, channels);
        if (handle == 0)
        {
            throw new IllegalStateException("Cannot initialize Opus");
        }
    }

    /**
     * encodes one complete microphone frame
     *
     * @param pcm mono samples
     * @param samples samples in the frame
     * @return encoded packet
     */
    synchronized byte[] encode(short[] pcm, int samples)
    {
        if (handle == 0 || !encoder)
        {
            throw new IllegalStateException("Opus encoder is closed");
        }
        return nativeEncode(handle, pcm, samples, channels);
    }

    /**
     * decodes one packet to stereo at 48 kHz
     *
     * @param packet encoded Opus bytes
     * @return interleaved stereo samples or null for an invalid packet
     */
    synchronized short[] decode(byte[] packet)
    {
        if (handle == 0 || encoder)
        {
            return null;
        }
        return nativeDecode(handle, packet);
    }

    /**
     * releases native codec storage once
     */
    @Override
    public synchronized void close()
    {
        if (handle != 0)
        {
            nativeDestroy(handle, encoder);
            handle = 0;
        }
    }

    private static native long nativeCreate(boolean encoder, int sampleRate, int channels);
    private static native byte[] nativeEncode(long handle, short[] pcm, int samples, int channels);
    private static native short[] nativeDecode(long handle, byte[] packet);
    private static native void nativeDestroy(long handle, boolean encoder);
}

