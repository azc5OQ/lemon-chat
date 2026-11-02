package com.lemonchat.server;

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

/** Owns the Funnel helper and its private status pipe. */
final class FunnelProcess
{
    interface Listener
    {
        void Update(String type, String value);
        void Finished();
    }

    private Process process;
    private boolean stopping;
    private Thread worker;
    private static final java.util.concurrent.atomic.AtomicBoolean running =
        new java.util.concurrent.atomic.AtomicBoolean();

    synchronized void Start(File executable, File directory, int httpPort, int websocketPort, Listener listener)
    {
        if (worker != null || !running.compareAndSet(false, true))
        {
            throw new IllegalStateException("The previous Funnel process is still stopping.");
        }
        stopping = false;
        worker = new Thread(() -> Run(executable, directory, httpPort, websocketPort, listener), "lemon-funnel");
        try
        {
            worker.start();
        }
        catch (RuntimeException error)
        {
            worker = null;
            running.set(false);
            throw error;
        }
    }

    private void Run(File executable, File directory, int httpPort, int websocketPort, Listener listener)
    {
        Process child = null;
        try
        {
            synchronized (this)
            {
                if (!stopping)
                {
                    child = new ProcessBuilder(executable.getAbsolutePath(), "--state",
                        new File(directory, "tailscale-state").getAbsolutePath(), "--http-port", String.valueOf(httpPort),
                        "--websocket-port", String.valueOf(websocketPort))
                        .directory(directory).redirectErrorStream(true).start();
                    process = child;
                }
            }
            if (child != null)
            {
                try (BufferedReader reader = new BufferedReader(new InputStreamReader(child.getInputStream(), StandardCharsets.UTF_8)))
                {
                    String line;
                    while ((line = reader.readLine()) != null)
                    {
                        try
                        {
                            JSONObject update = new JSONObject(line);
                            listener.Update(update.getString("type"), update.getString("value"));
                        }
                        catch (org.json.JSONException ignored)
                        {
                        }
                    }
                }
                child.waitFor();
            }
        }
        catch (Exception error)
        {
            listener.Update("error", "Could not run Tailscale: " + error.getMessage());
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
                catch (InterruptedException error)
                {
                    Thread.currentThread().interrupt();
                }
            }
            synchronized (this)
            {
                process = null;
                worker = null;
            }
            running.set(false);
            listener.Finished();
        }
    }

    synchronized void Stop()
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
        new Thread(() ->
        {
            try
            {
                child.getOutputStream().close();
                if (!child.waitFor(5, TimeUnit.SECONDS))
                {
                    child.destroyForcibly();
                }
            }
            catch (Exception error)
            {
                child.destroyForcibly();
            }
        }, "lemon-funnel-stop").start();
    }
}
