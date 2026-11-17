package com.lemonchat.server;

import android.app.Instrumentation;
import android.content.Context;
import java.io.File;
import java.io.InputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Comparator;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.stream.Stream;

/** checks the bundled helper before account authorization */
final class FunnelStartupChecks
{
    private FunnelStartupChecks()
    {
    }

    static int Verify(Instrumentation instrumentation) throws Exception
    {
        Context context = instrumentation.getTargetContext();
        Map<String, ?> preferences = new HashMap<>(context.getSharedPreferences("hosting", 0).getAll());
        instrumentation.runOnMainSync(() -> Require(!ServerService.active, "helper fixture requires a stopped server"));
        File executable = new File(context.getApplicationInfo().nativeLibraryDir, "liblemon-funnel.so");
        Require(executable.canExecute(), "installed helper is executable");
        for (String name : new String[] {"TS_AUTHKEY", "TS_AUTH_KEY"})
        {
            Require(System.getenv(name) == null || System.getenv(name).isEmpty(), "helper fixture must not inherit an authentication key");
        }
        Path cache = context.getCacheDir().getCanonicalFile().toPath();
        Path directory = Files.createTempDirectory(cache, "funnel-startup-");
        Require(directory.getParent().equals(cache) && !Files.exists(directory.resolve("tailscale-state")), "helper starts with fresh private state");
        FunnelProcess helper = new FunnelProcess();
        CountDownLatch outcome = new CountDownLatch(1);
        CountDownLatch finished = new CountDownLatch(1);
        AtomicReference<String> type = new AtomicReference<>("");
        AtomicReference<String> detail = new AtomicReference<>("");
        AtomicInteger statusCount = new AtomicInteger();
        boolean started = false;
        try (LoopbackListener http = new LoopbackListener(true); LoopbackListener websocket = new LoopbackListener(false))
        {
            helper.Start(executable, directory.toFile(), http.port(), websocket.port(), new FunnelProcess.Listener()
            {
                @Override
                public void Update(String event, String value)
                {
                    if ("status".equals(event))
                    {
                        statusCount.incrementAndGet();
                    }
                    if ("login".equals(event) || "error".equals(event) || "ready".equals(event))
                    {
                        if (type.compareAndSet("", event))
                        {
                            detail.set("login".equals(event) ? (FunnelGuideState.LoginAddress(value) ? "validated login.tailscale.com address" : "invalid login address") : Sanitize(value));
                            outcome.countDown();
                        }
                    }
                }

                @Override
                public void Finished()
                {
                    finished.countDown();
                    outcome.countDown();
                }
            });
            started = true;
            Require(outcome.await(90, TimeUnit.SECONDS), "helper did not produce sign in instructions within 90 seconds");
            Require(http.accepted.get() > 0 && websocket.accepted.get() > 0, "helper checked both loopback destinations");
            Require(statusCount.get() > 0, "helper emitted actual startup status");
            Require("login".equals(type.get()) && "validated login.tailscale.com address".equals(detail.get()), "helper stopped before sign in: " + type.get() + " " + detail.get());
            helper.Stop();
            Require(finished.await(12, TimeUnit.SECONDS), "helper stops after closing its control input");
            Require(preferences.equals(context.getSharedPreferences("hosting", 0).getAll()), "helper test preserves the hosting choice");
            instrumentation.runOnMainSync(() -> Require(!ServerService.active, "helper test leaves the server stopped"));
            return 8;
        }
        finally
        {
            helper.Stop();
            boolean stopped = !started || finished.await(12, TimeUnit.SECONDS);
            if (stopped)
            {
                RemoveFixture(cache, directory);
            }
            else
            {
                throw new AssertionError("Helper shutdown was not confirmed. Private fixture state was kept.");
            }
        }
    }

    private static String Sanitize(String value)
    {
        if (value == null)
        {
            return "";
        }
        String clean = value.replaceAll("https://login\\.tailscale\\.com/\\S+", "[sign in address]");
        return clean.substring(0, Math.min(clean.length(), 500));
    }

    private static void RemoveFixture(Path cache, Path directory) throws Exception
    {
        if (!directory.getParent().equals(cache) || !directory.getFileName().toString().startsWith("funnel-startup-"))
        {
            throw new IllegalStateException("Unexpected helper fixture path");
        }
        try (Stream<Path> paths = Files.walk(directory))
        {
            Path[] owned = paths.sorted(Comparator.reverseOrder()).toArray(Path[]::new);
            for (Path path : owned)
            {
                if (!path.normalize().startsWith(directory))
                {
                    throw new IllegalStateException("Helper fixture path escaped its directory");
                }
                Files.deleteIfExists(path);
            }
        }
    }

    private static void Require(boolean condition, String message)
    {
        if (!condition)
        {
            throw new AssertionError(message);
        }
    }

    private static final class LoopbackListener implements AutoCloseable
    {
        private final ServerSocket server;
        private final Thread worker;
        private final AtomicInteger accepted = new AtomicInteger();

        private LoopbackListener(boolean http) throws Exception
        {
            server = new ServerSocket(0, 4, InetAddress.getByName("127.0.0.1"));
            worker = new Thread(() ->
            {
                while (!server.isClosed())
                {
                    try (Socket client = server.accept())
                    {
                        accepted.incrementAndGet();
                        client.setSoTimeout(2000);
                        if (http)
                        {
                            InputStream input = client.getInputStream();
                            int ending = 0;
                            for (int count = 0; count < 4096; count++)
                            {
                                int value = input.read();
                                if (value < 0)
                                {
                                    break;
                                }
                                ending = (ending << 8) | value;
                                if (ending == 0x0d0a0d0a)
                                {
                                    break;
                                }
                            }
                            client.getOutputStream().write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK".getBytes(StandardCharsets.US_ASCII));
                            client.getOutputStream().flush();
                        }
                    }
                    catch (Exception error)
                    {
                        if (!server.isClosed())
                        {
                            return;
                        }
                    }
                }
            }, "lemon-funnel-loopback-fixture");
            worker.start();
        }

        private int port()
        {
            return server.getLocalPort();
        }

        @Override
        public void close() throws Exception
        {
            server.close();
            worker.join(3000);
        }
    }
}
