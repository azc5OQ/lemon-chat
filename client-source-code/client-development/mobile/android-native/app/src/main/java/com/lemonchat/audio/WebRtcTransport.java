package com.lemonchat.audio;

import java.nio.charset.StandardCharsets;

/** owns one bounded native data channel peer */
public final class WebRtcTransport implements AutoCloseable
{
    public static final int DESCRIPTION = 1;
    public static final int CANDIDATE = 2;
    public static final int OPEN = 3;
    public static final int FRAME = 4;
    public static final int CLOSED = 5;

    public static final class Event
    {
        public final int type;
        public final byte[] payload;
        public final String detail;

        private Event(byte[][] values)
        {
            type = values[0][0];
            payload = values[1];
            detail = new String(values[2], StandardCharsets.UTF_8);
        }

        public String text()
        {
            return new String(payload, StandardCharsets.UTF_8);
        }
    }

    static
    {
        System.loadLibrary("lemonnative");
    }

    private long handle;

    public WebRtcTransport()
    {
        this("", 0);
    }

    /**
     * uses the selected server for optional STUN discovery
     *
     * @param host selected chat server host
     * @param port announced STUN port or zero to disable discovery
     */
    public WebRtcTransport(String host, int port)
    {
        if (port < 0 || port > 65535 || (port != 0 && (host == null || host.isEmpty() || host.length() > 255)))
        {
            throw new IllegalArgumentException("Invalid STUN endpoint");
        }
        String server = port == 0 ? "" : host;
        if (server.startsWith("[") && server.endsWith("]"))
        {
            server = server.substring(1, server.length() - 1);
        }
        handle = nativeCreate(server, port);
        if (handle == 0)
        {
            throw new IllegalStateException("WebRTC could not start");
        }
    }

    public synchronized boolean offer(String sdp)
    {
        return handle != 0 && sdp != null && sdp.length() <= 65536 && nativeOffer(handle, sdp);
    }

    public synchronized boolean candidate(String value, String mid)
    {
        return handle != 0 && value != null && value.length() <= 4096 && mid != null && mid.length() <= 128 && nativeCandidate(handle, value, mid);
    }

    public synchronized boolean send(byte[] frame)
    {
        return handle != 0 && frame != null && frame.length <= 3869 && nativeSend(handle, frame);
    }

    public synchronized Event poll()
    {
        byte[][] value = handle != 0 ? nativePoll(handle) : null;
        return value == null ? null : new Event(value);
    }

    @Override
    public synchronized void close()
    {
        if (handle != 0)
        {
            long previous = handle;
            handle = 0;
            nativeClose(previous);
        }
    }

    private static native long nativeCreate(String host, int port);
    private static native boolean nativeOffer(long handle, String sdp);
    private static native boolean nativeCandidate(long handle, String candidate, String mid);
    private static native byte[][] nativePoll(long handle);
    private static native boolean nativeSend(long handle, byte[] frame);
    private static native void nativeClose(long handle);
}
