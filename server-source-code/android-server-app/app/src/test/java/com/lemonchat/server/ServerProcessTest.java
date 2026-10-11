package com.lemonchat.server;

import org.junit.Test;
import java.io.File;
import java.nio.file.Files;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import static org.junit.Assert.*;

public class ServerProcessTest
{
    private static class Result implements ServerProcess.Listener
    {
        final CountDownLatch ready = new CountDownLatch(1);
        final CountDownLatch done = new CountDownLatch(1);
        final AtomicInteger calls = new AtomicInteger();
        final AtomicInteger exit = new AtomicInteger(-1);
        final AtomicReference<String> error = new AtomicReference<>();

        @Override public void Ready()
        {
            ready.countDown();
        }

        @Override public void Finished(int code, String failure)
        {
            exit.set(code);
            error.set(failure);
            calls.incrementAndGet();
            done.countDown();
        }
    }

    private List<String> Command(String mode) throws Exception
    {
        File java = new File(System.getProperty("java.home"), "bin/java");
        if (!java.exists())
        {
            java = new File(System.getProperty("java.home"), "bin/java.exe");
        }
        String classes = new File(Child.class.getProtectionDomain().getCodeSource().getLocation().toURI()).getPath();
        return Arrays.asList(java.getPath(), "-cp", classes, Child.class.getName(), mode);
    }

    @Test public void DuplicateStartIsRejectedAndStopReapsChild() throws Exception
    {
        File directory = Files.createTempDirectory("lemon-process-test").toFile();
        File log = new File(directory, "server.log");
        ServerProcess runner = new ServerProcess();
        Result result = new Result();
        try
        {
            assertTrue(runner.Start(Command("wait"), directory, log, result));
            assertFalse(runner.Start(Command("wait"), directory, log, result));
            assertTrue("split readiness marker was not detected", result.ready.await(10, TimeUnit.SECONDS));
            assertTrue(new String(Files.readAllBytes(log.toPath()), StandardCharsets.UTF_8).contains("stdin closed"));
            runner.Stop();
            runner.Stop();
            assertTrue("child did not stop", result.done.await(10, TimeUnit.SECONDS));
            assertFalse(runner.IsActive());
            assertEquals(1, result.calls.get());
        }
        finally
        {
            runner.Stop();
            result.done.await(10, TimeUnit.SECONDS);
            Files.deleteIfExists(log.toPath());
            Files.deleteIfExists(directory.toPath());
        }
    }

    @Test public void EarlyStopNeverLeavesAChildRunning() throws Exception
    {
        File directory = Files.createTempDirectory("lemon-early-stop").toFile();
        File log = new File(directory, "server.log");
        try
        {
            for (int attempt = 0; attempt < 10; attempt++)
            {
                ServerProcess runner = new ServerProcess();
                Result result = new Result();
                assertTrue(runner.Start(Command("wait"), directory, log, result));
                runner.Stop();
                assertTrue(result.done.await(10, TimeUnit.SECONDS));
                assertFalse(runner.IsActive());
                assertEquals(1, result.calls.get());
            }
        }
        finally
        {
            Files.deleteIfExists(log.toPath());
            Files.deleteIfExists(directory.toPath());
        }
    }

    @Test public void LargeOutputIsDrainedAndLogRemainsBounded() throws Exception
    {
        File directory = Files.createTempDirectory("lemon-log-test").toFile();
        File log = new File(directory, "server.log");
        ServerProcess runner = new ServerProcess();
        Result result = new Result();
        try
        {
            assertTrue(runner.Start(Command("output"), directory, log, result));
            assertTrue(result.done.await(15, TimeUnit.SECONDS));
            assertEquals(7, result.exit.get());
            assertNull(result.error.get());
            assertTrue(log.length() > 0);
            assertTrue(log.length() <= 2 * 1024 * 1024 + 8192);
            assertFalse(runner.IsActive());
        }
        finally
        {
            runner.Stop();
            result.done.await(10, TimeUnit.SECONDS);
            Files.deleteIfExists(log.toPath());
            Files.deleteIfExists(directory.toPath());
        }
    }

    @Test public void MissingExecutableReportsFailure() throws Exception
    {
        File directory = Files.createTempDirectory("lemon-missing-test").toFile();
        ServerProcess runner = new ServerProcess();
        Result result = new Result();
        assertTrue(runner.Start(Collections.singletonList(new File(directory, "missing-binary").getPath()), directory,
                                new File(directory, "server.log"), result));
        assertTrue(result.done.await(10, TimeUnit.SECONDS));
        assertNotNull(result.error.get());
        assertFalse(runner.IsActive());
        Files.deleteIfExists(directory.toPath());
    }

    public static class Child
    {
        public static void main(String[] arguments) throws Exception
        {
            if ("output".equals(arguments[0]))
            {
                byte[] buffer = new byte[8192];
                Arrays.fill(buffer, (byte)'x');
                for (int index = 0; index < 513; index++)
                {
                    System.out.write(buffer);
                }
                System.out.flush();
                System.exit(7);
            }
            if (System.in.read() == -1)
            {
                System.out.println("stdin closed");
            }
            System.out.print("LEMON_ANDROID_");
            System.out.flush();
            Thread.sleep(100);
            System.out.println("READY");
            System.out.flush();
            Thread.sleep(60000);
        }
    }
}
