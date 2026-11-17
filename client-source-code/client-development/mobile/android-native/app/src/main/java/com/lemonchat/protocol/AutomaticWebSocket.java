package com.lemonchat.protocol;

import java.io.EOFException;
import java.net.SocketException;
import java.net.SocketTimeoutException;
import java.net.URI;
import java.security.cert.CertificateException;
import java.security.cert.CertPathValidatorException;
import java.util.Locale;
import javax.net.ssl.SSLException;
import javax.net.ssl.SSLPeerUnverifiedException;

/** selects the listener transport before exposing a connected WebSocket */
public final class AutomaticWebSocket implements AutoCloseable
{
    private final URI savedEndpoint;
    private final int messageLimit;
    private final BoundedWebSocket.Listener listener;
    private BoundedWebSocket socket;
    private URI selected;
    private long attempt;
    private boolean started;
    private boolean stopped;
    private boolean opened;

    /**
     * prepares transport selection without sending login data
     *
     * @param endpoint saved address with its existing port and request path
     * @param messageLimit maximum received message size
     * @param listener callbacks for the selected connection
     */
    public AutomaticWebSocket(URI endpoint, int messageLimit, BoundedWebSocket.Listener listener)
    {
        if (endpoint == null || listener == null)
        {
            throw new IllegalArgumentException("Missing connection settings");
        }
        savedEndpoint = endpoint;
        this.messageLimit = messageLimit;
        this.listener = listener;
    }

    /** starts with a certificate checked TLS connection */
    public void connect()
    {
        synchronized (this)
        {
            if (started || stopped)
            {
                return;
            }
            started = true;
        }
        start(true);
    }

    /** @return the transport selected for chat and media */
    public synchronized URI endpoint()
    {
        return selected;
    }

    /**
     * sends only after the selected WebSocket has opened
     *
     * @param text complete encrypted message
     * @return whether the bounded socket accepted it
     */
    public boolean sendText(String text)
    {
        BoundedWebSocket current;
        synchronized (this)
        {
            if (stopped || !opened || socket == null)
            {
                return false;
            }
            current = socket;
        }
        return current.sendText(text);
    }

    private void start(boolean secure)
    {
        long current;
        URI address;
        synchronized (this)
        {
            if (stopped)
            {
                return;
            }
            current = ++attempt;
            address = transportEndpoint(savedEndpoint, secure);
        }
        BoundedWebSocket next = new BoundedWebSocket(address, messageLimit, new BoundedWebSocket.Listener()
        {
            @Override
            public void onOpen()
            {
                synchronized (AutomaticWebSocket.this)
                {
                    if (!active(current))
                    {
                        return;
                    }
                    opened = true;
                }
                listener.onOpen();
            }

            @Override
            public void onText(String text)
            {
                synchronized (AutomaticWebSocket.this)
                {
                    if (!active(current) || !opened)
                    {
                        return;
                    }
                }
                listener.onText(text);
            }

            @Override
            public void onBinary(byte[] bytes)
            {
                synchronized (AutomaticWebSocket.this)
                {
                    if (!active(current) || !opened)
                    {
                        return;
                    }
                }
                listener.onBinary(bytes);
            }

            @Override
            public void onClosed(int code, String reason)
            {
                synchronized (AutomaticWebSocket.this)
                {
                    if (!active(current))
                    {
                        return;
                    }
                    stopped = true;
                }
                listener.onClosed(code, reason);
            }

            @Override
            public void onError(Exception error)
            {
                BoundedWebSocket previous;
                boolean tryPlain;
                synchronized (AutomaticWebSocket.this)
                {
                    if (!active(current))
                    {
                        return;
                    }
                    tryPlain = secure && !opened && canTryPlain(error);
                    previous = socket;
                    socket = null;
                    attempt++;
                    stopped = !tryPlain;
                }
                previous.close();
                if (tryPlain)
                {
                    start(false);
                }
                else
                {
                    listener.onError(error);
                }
            }
        });
        boolean accepted;
        synchronized (this)
        {
            accepted = active(current);
            if (accepted)
            {
                socket = next;
                selected = address;
            }
        }
        if (accepted)
        {
            next.connect();
        }
        else
        {
            next.close();
        }
    }

    private boolean active(long current)
    {
        return !stopped && current == attempt;
    }

    /**
     * keeps the saved port when trying the other transport
     *
     * @param endpoint saved WebSocket URL
     * @param secure whether to use TLS
     * @return transport URL without changing escaped path or query values
     */
    static URI transportEndpoint(URI endpoint, boolean secure)
    {
        int port = endpoint.getPort() >= 0 ? endpoint.getPort() : "wss".equals(endpoint.getScheme()) ? 443 : 80;
        String host = endpoint.getHost();
        if (host.contains(":") && !host.startsWith("["))
        {
            host = "[" + host + "]";
        }
        String path = endpoint.getRawPath();
        String query = endpoint.getRawQuery();
        return URI.create((secure ? "wss://" : "ws://") + host + ":" + port + (path == null || path.isEmpty() ? "/" : path) + (query == null ? "" : "?" + query));
    }

    /**
     * recognizes a non TLS listener without ignoring certificate failures
     *
     * @param error failure before the WebSocket opened
     * @return whether a plain WebSocket upgrade may be attempted
     */
    static boolean canTryPlain(Throwable error)
    {
        if (certificateFailure(error) || (error instanceof BoundedWebSocket.TlsFailure && ((BoundedWebSocket.TlsFailure)error).handshakeComplete))
        {
            return false;
        }
        boolean tlsFailure = false;
        boolean mismatch = false;
        Throwable cause = error;
        for (int count = 0; cause != null && count < 16; count++, cause = cause.getCause())
        {
            tlsFailure |= cause instanceof SSLException;
            String message = cause.getMessage() == null ? "" : cause.getMessage().toLowerCase(Locale.ROOT);
            mismatch |= cause instanceof EOFException || cause instanceof SocketTimeoutException || message.contains("unrecognized ssl message") || message.contains("plaintext connection") || message.contains("wrong version number") || message.contains("wrong_version_number") || message.contains("remote host terminated the handshake") || message.contains("peer shut down incorrectly") || message.contains("connection closed by peer");
            mismatch |= cause instanceof SocketException && message.contains("connection reset");
        }
        return tlsFailure && mismatch;
    }

    /**
     * identifies trust and hostname errors for the connection message
     *
     * @param error transport failure and its causes
     * @return whether TLS identity verification failed
     */
    public static boolean certificateFailure(Throwable error)
    {
        Throwable cause = error;
        for (int count = 0; cause != null && count < 16; count++, cause = cause.getCause())
        {
            if (cause instanceof CertificateException || cause instanceof CertPathValidatorException || cause instanceof SSLPeerUnverifiedException)
            {
                return true;
            }
            String message = cause.getMessage() == null ? "" : cause.getMessage().toLowerCase(Locale.ROOT);
            if (cause instanceof SSLException && (message.contains("certificate") || message.contains("certpath") || message.contains("trust anchor") || message.contains("hostname") || message.contains("subject alternative") || message.contains("no name matching")))
            {
                return true;
            }
        }
        return false;
    }

    /** retires both selection callbacks and the selected socket */
    @Override
    public void close()
    {
        BoundedWebSocket previous;
        synchronized (this)
        {
            stopped = true;
            attempt++;
            previous = socket;
            socket = null;
        }
        if (previous != null)
        {
            previous.close();
        }
    }
}
