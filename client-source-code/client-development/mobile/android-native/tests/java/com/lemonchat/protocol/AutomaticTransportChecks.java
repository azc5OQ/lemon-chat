package com.lemonchat.protocol;

import java.io.EOFException;
import java.io.InputStream;
import java.lang.reflect.Field;
import java.net.InetSocketAddress;
import java.net.URI;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.channels.SelectionKey;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.KeyStore;
import java.security.cert.CertificateException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import javax.net.ssl.KeyManagerFactory;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLHandshakeException;
import javax.net.ssl.SSLException;
import javax.net.ssl.TrustManagerFactory;
import org.java_websocket.WebSocket;
import org.java_websocket.handshake.ClientHandshake;
import org.java_websocket.handshake.ServerHandshakeBuilder;
import org.java_websocket.drafts.Draft;
import org.java_websocket.exceptions.InvalidDataException;
import org.java_websocket.server.DefaultSSLWebSocketServerFactory;
import org.java_websocket.server.WebSocketServer;

/** checks transport selection against actual plain and TLS listeners */
public final class AutomaticTransportChecks
{
    private static int checks;

    public static void main(String[] arguments) throws Exception
    {
        SSLContext previous = SSLContext.getDefault();
        SSLContext trusted = fixtureContext(Path.of(arguments[0]));
        try
        {
            policyChecks();
            SSLContext.setDefault(trusted);
            success(false, "wss", "127.0.0.1");
            success(true, "ws", "localhost");
            refusal(trusted, "127.0.0.1", "hostname mismatch never tries plain WebSocket");
            upgradeRefusal();
            cancellation();
            concurrentPingClose();
            SSLContext.setDefault(previous);
            refusal(trusted, "localhost", "untrusted certificate never tries plain WebSocket");
            System.out.println("PASS " + checks + " automatic transport checks");
        }
        finally
        {
            SSLContext.setDefault(previous);
        }
    }

    private static void policyChecks() throws Exception
    {
        check(AutomaticWebSocket.canTryPlain(new SSLException("Unsupported or unrecognized SSL message")), "plain HTTP response permits a WebSocket upgrade attempt");
        check(AutomaticWebSocket.canTryPlain(new SSLException("error: WRONG_VERSION_NUMBER")), "Android protocol mismatch permits a WebSocket upgrade attempt");
        SSLHandshakeException closed = new SSLHandshakeException("Remote host terminated the handshake");
        closed.initCause(new EOFException());
        check(AutomaticWebSocket.canTryPlain(closed), "listener closure before TLS can be checked with a plain upgrade");
        SSLHandshakeException certificate = new SSLHandshakeException("Remote host terminated the handshake");
        certificate.initCause(new CertificateException("untrusted certificate"));
        check(!AutomaticWebSocket.canTryPlain(certificate), "certificate cause overrides an apparent early close");
        check(!AutomaticWebSocket.canTryPlain(new SSLHandshakeException("Received fatal alert: protocol_version")), "unsupported TLS version is not treated as a plain listener");
        check(AutomaticWebSocket.canTryPlain(new BoundedWebSocket.TlsFailure(new java.net.SocketTimeoutException("Read timed out"), false)), "silent TLS listener requires a successful plain upgrade before selection");
        check(!AutomaticWebSocket.canTryPlain(new java.net.SocketTimeoutException("Timed out")), "TCP connection timeout does not change transport");
        check(!AutomaticWebSocket.canTryPlain(new BoundedWebSocket.TlsFailure(new EOFException(), true)), "verified TLS listener cannot fall back after WebSocket refusal");
        check(!AutomaticWebSocket.canTryPlain(new SSLHandshakeException("Received fatal alert: handshake_failure")), "TLS handshake rejection does not change transport");
        URI legacy = URI.create("ws://localhost/socket%2Fpath?q=a%2Fb");
        check(AutomaticWebSocket.transportEndpoint(legacy, true).toString().equals("wss://localhost:80/socket%2Fpath?q=a%2Fb"), "TLS selection retains legacy default port and escaped path");
        check(AutomaticWebSocket.transportEndpoint(URI.create("wss://[::1]/"), false).toString().equals("ws://[::1]:443/"), "plain probe retains the secure saved port and IPv6");
    }

    private static void success(boolean tls, String storedScheme, String hostname) throws Exception
    {
        try (Server server = new Server(tls ? SSLContext.getDefault() : null))
        {
            URI endpoint = URI.create(storedScheme + "://" + hostname + ":" + server.getPort() + "/");
            CountDownLatch opened = new CountDownLatch(1);
            CountDownLatch echoes = new CountDownLatch(2);
            AtomicReference<Exception> failure = new AtomicReference<>();
            AtomicInteger opens = new AtomicInteger();
            try (AutomaticWebSocket socket = new AutomaticWebSocket(endpoint, 4096, new BoundedWebSocket.Listener()
            {
                @Override
                public void onOpen()
                {
                    opens.incrementAndGet();
                    opened.countDown();
                }

                @Override
                public void onText(String text)
                {
                    if (text.equals("transport fixture"))
                    {
                        echoes.countDown();
                    }
                }

                @Override
                public void onBinary(byte[] bytes)
                {
                    failure.set(new IllegalStateException("Unexpected binary fixture"));
                }

                @Override
                public void onClosed(int code, String reason)
                {
                }

                @Override
                public void onError(Exception error)
                {
                    failure.set(error);
                    opened.countDown();
                }
            }))
            {
                check(!socket.sendText("must not be sent"), "selection refuses application data before connection");
                socket.connect();
                check(opened.await(20, TimeUnit.SECONDS) && failure.get() == null && opens.get() == 1, (tls ? "TLS" : "plain") + " listener selected once: " + failure.get());
                check(socket.endpoint().getScheme().equals(tls ? "wss" : "ws"), "media receives the selected transport");
                check(socket.sendText("transport fixture"), "selected connection accepts application data");
                Thread.sleep(150);
                check(socket.sendText("transport fixture") && echoes.await(5, TimeUnit.SECONDS), "retired selection callbacks do not close the replacement socket");
                check(server.messages.get() == 2 && server.opens.get() == 1, "only the selected WebSocket receives application data");
                check(server.accepts.get() == (tls ? 1 : 2), "selection uses one TLS attempt and at most one plain attempt");
            }
        }
    }

    private static void refusal(SSLContext serverContext, String hostname, String description) throws Exception
    {
        try (Server server = new Server(serverContext))
        {
            CountDownLatch failed = new CountDownLatch(1);
            AtomicReference<Exception> failure = new AtomicReference<>();
            AtomicInteger opened = new AtomicInteger();
            try (AutomaticWebSocket socket = new AutomaticWebSocket(URI.create("ws://" + hostname + ":" + server.getPort() + "/"), 4096, new BoundedWebSocket.Listener()
            {
                @Override
                public void onOpen()
                {
                    opened.incrementAndGet();
                }

                @Override
                public void onText(String text)
                {
                }

                @Override
                public void onBinary(byte[] bytes)
                {
                }

                @Override
                public void onClosed(int code, String reason)
                {
                }

                @Override
                public void onError(Exception error)
                {
                    failure.set(error);
                    failed.countDown();
                }
            }))
            {
                socket.connect();
                check(failed.await(15, TimeUnit.SECONDS) && AutomaticWebSocket.certificateFailure(failure.get()), "certificate failure is reported accurately");
                Thread.sleep(200);
                check(opened.get() == 0 && server.accepts.get() == 1 && server.messages.get() == 0, description);
            }
        }
    }

    private static SSLContext fixtureContext(Path file) throws Exception
    {
        KeyStore store = KeyStore.getInstance("PKCS12");
        try (InputStream input = Files.newInputStream(file))
        {
            store.load(input, "transport-fixture".toCharArray());
        }
        KeyManagerFactory keys = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
        keys.init(store, "transport-fixture".toCharArray());
        TrustManagerFactory trust = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
        trust.init(store);
        SSLContext context = SSLContext.getInstance("TLS");
        context.init(keys.getKeyManagers(), trust.getTrustManagers(), null);
        return context;
    }

    private static void upgradeRefusal() throws Exception
    {
        try (Server server = new Server(SSLContext.getDefault(), true))
        {
            CountDownLatch finished = new CountDownLatch(1);
            AtomicInteger opens = new AtomicInteger();
            try (AutomaticWebSocket socket = new AutomaticWebSocket(URI.create("ws://localhost:" + server.getPort() + "/"), 4096, quiet(opens, finished)))
            {
                socket.connect();
                check(finished.await(10, TimeUnit.SECONDS), "verified TLS listener can reject the WebSocket upgrade");
                Thread.sleep(150);
                check(server.accepts.get() == 1 && opens.get() == 0, "TLS upgrade refusal does not try plain WebSocket");
            }
        }
    }

    private static void cancellation() throws Exception
    {
        CountDownLatch accepted = new CountDownLatch(1);
        CountDownLatch released = new CountDownLatch(1);
        AtomicReference<Socket> peer = new AtomicReference<>();
        AtomicInteger opens = new AtomicInteger();
        try (ServerSocket server = new ServerSocket(0, 1, java.net.InetAddress.getByName("127.0.0.1")))
        {
            Thread reader = new Thread(() ->
            {
                try (Socket connection = server.accept())
                {
                    peer.set(connection);
                    connection.setSoTimeout(8000);
                    accepted.countDown();
                    while (connection.getInputStream().read() != -1)
                    {
                    }
                    released.countDown();
                }
                catch (Exception ignored)
                {
                }
            });
            reader.setDaemon(true);
            reader.start();
            try (AutomaticWebSocket socket = new AutomaticWebSocket(URI.create("ws://127.0.0.1:" + server.getLocalPort() + "/"), 4096, quiet(opens, new CountDownLatch(1))))
            {
                socket.connect();
                check(accepted.await(5, TimeUnit.SECONDS), "silent TLS probe starts");
                socket.close();
                check(released.await(3, TimeUnit.SECONDS) && opens.get() == 0, "cancelling TLS setup promptly closes its socket without opening plain transport");
            }
            finally
            {
                if (peer.get() != null)
                {
                    peer.get().close();
                }
            }
        }
    }

    private static BoundedWebSocket.Listener quiet(AtomicInteger opens, CountDownLatch finished)
    {
        return new BoundedWebSocket.Listener()
        {
            @Override
            public void onOpen()
            {
                opens.incrementAndGet();
            }

            @Override
            public void onText(String text)
            {
            }

            @Override
            public void onBinary(byte[] bytes)
            {
            }

            @Override
            public void onClosed(int code, String reason)
            {
                finished.countDown();
            }

            @Override
            public void onError(Exception error)
            {
                finished.countDown();
            }
        };
    }

    private static void concurrentPingClose() throws Exception
    {
        try (Server server = new Server(SSLContext.getDefault()))
        {
            AtomicInteger opens = new AtomicInteger();
            CountDownLatch closed = new CountDownLatch(1);
            try (AutomaticWebSocket socket = new AutomaticWebSocket(URI.create("ws://localhost:" + server.getPort() + "/"), 4096, quiet(opens, closed)))
            {
                socket.connect();
                long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
                while (opens.get() == 0 && System.nanoTime() < deadline)
                {
                    Thread.sleep(10);
                }
                check(opens.get() == 1, "concurrent close fixture connects over TLS");
                BoundedWebSocket bounded = (BoundedWebSocket)field(socket, "socket");
                org.java_websocket.client.WebSocketClient client = (org.java_websocket.client.WebSocketClient)field(bounded, "client");
                Field budget = BoundedWebSocket.class.getDeclaredField("sendLimit");
                budget.setAccessible(true);
                budget.setInt(bounded, 0);
                CountDownLatch held = new CountDownLatch(1);
                CountDownLatch proceed = new CountDownLatch(1);
                CountDownLatch pingDone = new CountDownLatch(1);
                CountDownLatch sendDone = new CountDownLatch(1);
                Thread ping = new Thread(() ->
                {
                    synchronized (bounded)
                    {
                        held.countDown();
                        try
                        {
                            proceed.await();
                            client.onWebsocketPing(client.getConnection(), new org.java_websocket.framing.PingFrame());
                        }
                        catch (InterruptedException error)
                        {
                            Thread.currentThread().interrupt();
                        }
                    }
                    pingDone.countDown();
                });
                Thread sender = new Thread(() ->
                {
                    socket.sendText("budget exhausted");
                    sendDone.countDown();
                });
                ping.setDaemon(true);
                sender.setDaemon(true);
                ping.start();
                check(held.await(2, TimeUnit.SECONDS), "ping callback holds the bounded socket monitor");
                sender.start();
                deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
                while (sender.getState() != Thread.State.BLOCKED && System.nanoTime() < deadline)
                {
                    Thread.sleep(1);
                }
                check(sender.getState() == Thread.State.BLOCKED, "application send waits for the bounded socket monitor");
                proceed.countDown();
                check(pingDone.await(3, TimeUnit.SECONDS) && sendDone.await(3, TimeUnit.SECONDS) && closed.await(3, TimeUnit.SECONDS), "ping close and application send finish without lock inversion");
            }
        }
    }

    private static Object field(Object owner, String name) throws Exception
    {
        Field field = owner.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(owner);
    }

    private static void check(boolean value, String description)
    {
        if (!value)
        {
            throw new AssertionError(description);
        }
        checks++;
    }

    private static final class Server extends WebSocketServer implements AutoCloseable
    {
        final AtomicInteger accepts = new AtomicInteger();
        final AtomicInteger opens = new AtomicInteger();
        final AtomicInteger messages = new AtomicInteger();
        final CountDownLatch ready = new CountDownLatch(1);
        private final boolean refuseUpgrade;

        Server(SSLContext context) throws Exception
        {
            this(context, false);
        }

        Server(SSLContext context, boolean refuseUpgrade) throws Exception
        {
            super(new InetSocketAddress("127.0.0.1", 0), 1);
            this.refuseUpgrade = refuseUpgrade;
            if (context != null)
            {
                setWebSocketFactory(new DefaultSSLWebSocketServerFactory(context));
            }
            start();
            check(ready.await(5, TimeUnit.SECONDS), "fixture listener starts");
        }

        @Override
        protected boolean onConnect(SelectionKey key)
        {
            accepts.incrementAndGet();
            return true;
        }

        @Override
        public ServerHandshakeBuilder onWebsocketHandshakeReceivedAsServer(WebSocket connection, Draft draft, ClientHandshake request) throws InvalidDataException
        {
            if (refuseUpgrade)
            {
                throw new InvalidDataException(1002, "Fixture refuses WebSocket upgrade");
            }
            return super.onWebsocketHandshakeReceivedAsServer(connection, draft, request);
        }

        @Override
        public void onOpen(WebSocket connection, ClientHandshake handshake)
        {
            opens.incrementAndGet();
        }

        @Override
        public void onMessage(WebSocket connection, String message)
        {
            messages.incrementAndGet();
            connection.send(message);
        }

        @Override
        public void onClose(WebSocket connection, int code, String reason, boolean remote)
        {
        }

        @Override
        public void onError(WebSocket connection, Exception error)
        {
        }

        @Override
        public void onStart()
        {
            ready.countDown();
        }

        @Override
        public void close() throws Exception
        {
            stop(3000);
        }
    }
}
