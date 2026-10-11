package com.lemonchat.server;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.nio.charset.StandardCharsets;

/** Owns one child. Stop and failure never schedule an implicit restart. */
final class ServerProcess
{
    interface Listener
    {
        default void Ready()
        {
        }
        void Finished(int exitCode, String error);
    }
    private final Object lock = new Object();
    private Thread worker;
    private Process process;
    private boolean stopping;
    boolean IsActive()
    {
        synchronized (lock)
        {
            return worker != null;
        }
    }

    boolean Start(List<String> command, File directory, File log, Listener listener)
    {
        synchronized (lock)
        {
            if (worker != null)
            {
                return false;
            }
            stopping = false;
            worker = new Thread(() -> Run(command, directory, log, listener), "lemon-server-output");
            worker.start();
            return true;
        }
    }

    private void Run(List<String> command, File directory, File log, Listener listener)
    {
        int exitCode = -1;
        String failure = null;
        Process child = null;
        try
        {
            synchronized (lock)
            {
                if (!stopping)
                {
                    process = new ProcessBuilder(command).directory(directory).redirectErrorStream(true).start();
                    child = process;
                }
            }
            if (child != null)
            {
                child.getOutputStream().close();
                // Drain bytes even without newlines; keep at most about 2 MiB of log.
                try (InputStream input = child.getInputStream())
                {
                    byte[] buffer = new byte[8192];
                    int count;
                    String tail = "";
                    boolean announced = false;
                    while ((count = input.read(buffer)) != -1)
                    {
                        try (FileOutputStream output = new FileOutputStream(log, log.length() < 2 * 1024 * 1024))
                        {
                            output.write(buffer, 0, count);
                        }
                        if (!announced)
                        {
                            tail += new String(buffer, 0, count, StandardCharsets.UTF_8);
                            if (tail.contains("LEMON_ANDROID_READY"))
                            {
                                announced = true;
                                listener.Ready();
                            }
                            tail = tail.substring(Math.max(0, tail.length() - 64));
                        }
                    }
                }
                exitCode = child.waitFor();
            }
        }
        catch (Exception error)
        {
            failure = error.toString();
        }
        finally
        {
            if (child != null && child.isAlive())
            {
                child.destroyForcibly();
                try
                {
                    child.waitFor();
                }
                catch (InterruptedException ignored)
                {
                    Thread.currentThread().interrupt();
                }
            }
            synchronized (lock)
            {
                process = null;
                worker = null;
            }
            listener.Finished(exitCode, failure);
        }
    }

    void Stop()
    {
        synchronized (lock)
        {
            if (stopping)
            {
                return;
            }
            stopping = true;
            final Process child = process;
            if (child == null)
            {
                return;
            }
            child.destroy();
            new Thread(() ->
            {
                try
                {
                    if (!child.waitFor(2, TimeUnit.SECONDS))
                    {
                        child.destroyForcibly();
                    }
                }
                catch (InterruptedException error)
                {
                    child.destroyForcibly();
                    Thread.currentThread().interrupt();
                }
            }, "lemon-server-stop").start();
        }
    }
}
