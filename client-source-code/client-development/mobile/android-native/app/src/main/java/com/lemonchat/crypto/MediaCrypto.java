package com.lemonchat.crypto;

import java.nio.ByteBuffer;
import java.util.Arrays;

/** owns media keys independently of the chat encryption context */
public final class MediaCrypto implements AutoCloseable
{
    private static final int CONTENT_LIMIT = 3840;
    private static final int FRAME_LIMIT = CONTENT_LIMIT + 29;
    private long handle;

    private MediaCrypto(long handle)
    {
        this.handle = handle;
    }

    static MediaCrypto adopt(long handle)
    {
        if (handle == 0)
        {
            throw new CryptoException("Media keys are unavailable");
        }
        try
        {
            return new MediaCrypto(handle);
        }
        catch (RuntimeException | Error failure)
        {
            nativeDestroy(handle);
            throw failure;
        }
    }

    /**
     * protects one outgoing audio packet without locking chat encryption
     *
     * @param sender authenticated local client ID
     * @param counter unique unsigned frame counter
     * @param content channel encrypted audio
     * @return authenticated media frame
     */
    public synchronized byte[] sealMedia(int sender, long counter, byte[] content)
    {
        requireOpen();
        if (sender < 0 || content == null || content.length < 3 || content.length > CONTENT_LIMIT)
        {
            throw new CryptoException("Invalid outgoing media content");
        }
        byte[] frame = nativeSeal(handle, sender, counter, content);
        if (frame == null)
        {
            throw new CryptoException("Media encryption failed");
        }
        return frame;
    }

    /**
     * authenticates an incoming frame using this connection's copied keys
     *
     * @param frame complete received media frame
     * @return authenticated sender, counters and channel ciphertext
     */
    public synchronized CryptoSession.MediaPacket openMedia(byte[] frame)
    {
        requireOpen();
        if (frame == null || frame.length < 32 || frame.length > FRAME_LIMIT)
        {
            throw new CryptoException("Invalid incoming media frame");
        }
        byte[] result = nativeOpen(handle, frame);
        if (result == null || result.length < 23)
        {
            throw new CryptoException("Media authentication failed");
        }
        try
        {
            ByteBuffer bytes = ByteBuffer.wrap(result);
            int sender = bytes.getInt();
            long senderCounter = bytes.getLong();
            long frameCounter = bytes.getLong();
            byte[] content = new byte[bytes.remaining()];
            bytes.get(content);
            return new CryptoSession.MediaPacket(sender, senderCounter, frameCounter, content);
        }
        finally
        {
            Arrays.fill(result, (byte)0);
        }
    }

    /** clears copied keys after the current media operation finishes */
    @Override
    public synchronized void close()
    {
        if (handle != 0)
        {
            nativeDestroy(handle);
            handle = 0;
        }
    }

    private void requireOpen()
    {
        if (handle == 0)
        {
            throw new CryptoException("Media encryption is closed");
        }
    }

    private static native void nativeDestroy(long handle);
    private static native byte[] nativeSeal(long handle, int sender, long counter, byte[] content);
    private static native byte[] nativeOpen(long handle, byte[] frame);
}
