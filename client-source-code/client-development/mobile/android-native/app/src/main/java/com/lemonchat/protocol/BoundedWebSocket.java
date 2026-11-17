package com.lemonchat.protocol;

import java.net.URI;
import java.net.Socket;
import java.net.SocketException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.concurrent.atomic.AtomicBoolean;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLException;
import javax.net.ssl.SSLSocket;
import org.java_websocket.WebSocket;
import org.java_websocket.WebSocketImpl;
import org.java_websocket.client.WebSocketClient;
import org.java_websocket.drafts.Draft;
import org.java_websocket.drafts.Draft_6455;
import org.java_websocket.exceptions.InvalidDataException;
import org.java_websocket.exceptions.InvalidHandshakeException;
import org.java_websocket.exceptions.LimitExceededException;
import org.java_websocket.handshake.Handshakedata;
import org.java_websocket.handshake.ServerHandshake;
import org.java_websocket.framing.Framedata;

/** transports messages with bounded frames, handshake and send queue */
public final class BoundedWebSocket implements AutoCloseable
{
    public interface Listener
    {
        void onOpen();
        void onText(String text);
        void onBinary(byte[] bytes);
        void onClosed(int code, String reason);
        void onError(Exception error);
    }

    private static final int HANDSHAKE_LIMIT = 16 * 1024;
    private static final int SEND_LIMIT = 2 * 1024 * 1024;
    private final WebSocketClient client;
    private final int messageLimit;
    private final int sendLimit;
    private final AtomicBoolean stopped = new AtomicBoolean();
    private final AtomicBoolean tlsStarted = new AtomicBoolean();
    private final AtomicBoolean tlsComplete = new AtomicBoolean();

    /**
     * creates one socket without connecting it
     *
     * @param uri explicit ws or wss endpoint
     * @param maxMessageBytes maximum received frame and assembled message size
     * @param listener callbacks delivered on socket threads
     */
    public BoundedWebSocket(URI uri, int maxMessageBytes, Listener listener)
    {
        this(uri, maxMessageBytes, SEND_LIMIT, listener);
    }

    /**
     * creates one socket with a separate send queue budget
     *
     * @param uri explicit ws or wss endpoint
     * @param maxMessageBytes maximum received message size
     * @param maxQueuedBytes maximum queued outgoing bytes
     * @param listener callbacks delivered on socket threads
     */
    public BoundedWebSocket(URI uri, int maxMessageBytes, int maxQueuedBytes, Listener listener)
    {
        if (uri == null || listener == null || maxMessageBytes < 1024 || maxMessageBytes > 16 * 1024 * 1024 || maxQueuedBytes < 1024 || maxQueuedBytes > SEND_LIMIT)
        {
            throw new IllegalArgumentException("Invalid WebSocket settings");
        }
        messageLimit = maxMessageBytes;
        sendLimit = maxQueuedBytes;
        client = new WebSocketClient(uri, new BoundedDraft(maxMessageBytes), Collections.emptyMap(), 15000)
        {
            @Override
            public void onOpen(ServerHandshake handshake)
            {
                if (stopped.get())
                {
                    closeConnection(1000, "Closed");
                    return;
                }
                try
                {
                    getSocket().setSoTimeout(0);
                }
                catch (SocketException error)
                {
                    listener.onError(error);
                    closeConnection(1000, "Closed");
                    return;
                }
                listener.onOpen();
            }

            @Override
            public void onMessage(String text)
            {
                if (!stopped.get())
                {
                    listener.onText(text);
                }
            }

            @Override
            public void onMessage(ByteBuffer buffer)
            {
                if (!stopped.get())
                {
                    byte[] bytes = new byte[buffer.remaining()];
                    buffer.get(bytes);
                    listener.onBinary(bytes);
                }
            }

            @Override
            public void onClose(int code, String reason, boolean remote)
            {
                stopped.set(true);
                listener.onClosed(code, reason);
            }

            @Override
            public void onError(Exception error)
            {
                if (!stopped.get())
                {
                    listener.onError(tlsStarted.get() ? new TlsFailure(error, tlsComplete.get()) : error);
                }
            }

            @Override
            public void onWebsocketPing(WebSocket connection, Framedata frame)
            {
                synchronized (BoundedWebSocket.this)
                {
                    if (!canSend(frame.getPayloadData().remaining()))
                    {
                        closeConnection(1009, "Send queue is full");
                        return;
                    }
                    super.onWebsocketPing(connection, frame);
                }
            }

            @Override
            protected void onSetSSLParameters(SSLParameters parameters)
            {
                parameters.setEndpointIdentificationAlgorithm("HTTPS");
                try
                {
                    SSLSocket tls = (SSLSocket)getSocket();
                    if (stopped.get())
                    {
                        tls.close();
                        throw new java.io.IOException("Connection closed");
                    }
                    tls.setSSLParameters(parameters);
                    tls.setSoTimeout(5000);
                    tlsStarted.set(true);
                    tls.startHandshake();
                    tlsComplete.set(true);
                    tls.setSoTimeout(15000);
                }
                catch (java.io.IOException error)
                {
                    throw new IllegalStateException("TLS handshake failed", error);
                }
            }
        };
        client.setConnectionLostTimeout(20);
        client.setTcpNoDelay(true);
    }

    /** starts the socket once */
    public void connect()
    {
        if (!stopped.get())
        {
            client.connect();
        }
    }

    /**
     * queues one text message within the send budget
     *
     * @param text complete message
     * @return whether the message was queued
     */
    public synchronized boolean sendText(String text)
    {
        if (text == null || text.isEmpty() || !canSend(text.getBytes(StandardCharsets.UTF_8).length))
        {
            return false;
        }
        try
        {
            client.send(text);
            return true;
        }
        catch (RuntimeException error)
        {
            return false;
        }
    }

    /**
     * queues one binary message within the send budget
     *
     * @param bytes complete message
     * @return whether the message was queued
     */
    public synchronized boolean sendBinary(byte[] bytes)
    {
        if (bytes == null || !canSend(bytes.length))
        {
            return false;
        }
        try
        {
            client.send(bytes.clone());
            return true;
        }
        catch (RuntimeException error)
        {
            return false;
        }
    }

    private boolean canSend(int length)
    {
        if (stopped.get() || !client.isOpen() || length < 0 || length > messageLimit || length > sendLimit)
        {
            return false;
        }
        long queued = length + 14L;
        WebSocketImpl connection = (WebSocketImpl) client.getConnection();
        for (ByteBuffer buffer : connection.outQueue)
        {
            queued += buffer.remaining();
            if (queued > sendLimit)
            {
                return false;
            }
        }
        return queued <= sendLimit;
    }

    /** @return whether the socket accepts messages */
    public boolean isOpen()
    {
        return !stopped.get() && client.isOpen();
    }

    /** closes this socket without waiting on its reader thread */
    @Override
    public void close()
    {
        if (stopped.compareAndSet(false, true))
        {
            client.closeConnection(1000, "Closed");
            Socket socket = client.getSocket();
            if (socket != null)
            {
                try
                {
                    socket.close();
                }
                catch (java.io.IOException ignored)
                {
                }
            }
        }
    }

    static final class BoundedDraft extends Draft_6455
    {
        private final int limit;

        BoundedDraft(int limit)
        {
            super(Collections.emptyList(), limit);
            this.limit = limit;
        }

        @Override
        public int checkAlloc(int count) throws InvalidDataException
        {
            if (count > limit + 14)
            {
                throw new LimitExceededException("Frame exceeds the receive limit", limit);
            }
            return super.checkAlloc(count);
        }

        @Override
        public Handshakedata translateHandshake(ByteBuffer buffer) throws InvalidHandshakeException
        {
            int start = buffer.position();
            int end = Math.min(buffer.limit(), start + HANDSHAKE_LIMIT);
            boolean complete = false;
            for (int i = start; i + 3 < end; i++)
            {
                if (buffer.get(i) == '\r' && buffer.get(i + 1) == '\n' && buffer.get(i + 2) == '\r' && buffer.get(i + 3) == '\n')
                {
                    complete = true;
                    break;
                }
            }
            if (!complete && buffer.remaining() > HANDSHAKE_LIMIT)
            {
                throw new InvalidHandshakeException("Handshake exceeds the receive limit");
            }
            return super.translateHandshake(buffer);
        }

        @Override
        public Draft copyInstance()
        {
            return new BoundedDraft(limit);
        }
    }

    static final class TlsFailure extends SSLException
    {
        final boolean handshakeComplete;

        TlsFailure(Exception cause, boolean handshakeComplete)
        {
            super(cause.getMessage(), cause);
            this.handshakeComplete = handshakeComplete;
        }
    }
}
