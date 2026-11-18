package com.lemonchat.server;

import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.SystemClock;
import java.io.File;
import java.util.concurrent.Callable;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONObject;

/** checks the actual server service inside the isolated verification app */
public final class ServerChecks extends Instrumentation
{
    private Bundle arguments;
    private Context target;
    private int checks;
    private boolean ownsServer;

    @Override
    public void onCreate(Bundle arguments)
    {
        super.onCreate(arguments);
        this.arguments = arguments == null ? new Bundle() : arguments;
        start();
    }

    @Override
    public void onStart()
    {
        Bundle result = new Bundle();
        int code = -1;
        try
        {
            target = getTargetContext();
            if (!target.getPackageName().equals("com.lemonchat.server.checks"))
            {
                throw new AssertionError("Server checks require the isolated verification app");
            }
            if ("guide".equals(arguments.getString("suite")))
            {
                checks = FunnelUiChecks.Verify(this);
                result.putString("stream", "PASS " + checks + " Funnel guide checks without publication\n");
                finish(-1, result);
                return;
            }
            if ("funnel-startup".equals(arguments.getString("suite")))
            {
                checks = FunnelStartupChecks.Verify(this);
                result.putString("stream", "PASS " + checks + " bundled Funnel startup checks. Sign in URL received from login.tailscale.com. No account authorized or public listener opened.\n");
                finish(-1, result);
                return;
            }
            check(!onMain(() -> ServerService.active), "no server was already running");
            JSONObject settings = ServerSettings.Read(target.getFilesDir());
            check(settings.optJSONArray("channels") != null, "copied channel settings are present");
            check(!HostingSettings.IsFunnelEnabled(target), "public access remains disabled");
            MemoryInfo.Load(new File(target.getApplicationInfo().nativeLibraryDir, "libchat-server.so"));
            check(MemoryInfo.slotBytes > 0 && MemoryInfo.channelBytes > 0, "installed native memory query works");
            long reserved = MemoryInfo.Estimate(settings.getInt("server_slots"), settings.optInt("max_channel_count", MemoryInfo.defaultChannels));
            progress("Reserved array estimate: " + reserved + " bytes");
            progress("WebSocket port: " + settings.getInt("websocket_port"));
            startServer();
            await(() -> onMain(() -> ServerService.hosting), "foreground service starts the native server");
            stopServer();
            startServer();
            await(() -> onMain(() -> ServerService.hosting), "service restarts and restores the native server");
            progress("ANDROID_SERVER_READY");
            int seconds = Math.max(30, Math.min(600, Integer.parseInt(arguments.getString("hold_seconds", "180"))));
            long deadline = SystemClock.elapsedRealtime() + seconds * 1000L;
            while (SystemClock.elapsedRealtime() < deadline)
            {
                if (!onMain(() -> ServerService.hosting))
                {
                    throw new AssertionError("Server stopped during the external restoration probe: " + onMain(() -> ServerService.status));
                }
                SystemClock.sleep(250);
            }
            stopServer();
            result.putString("stream", "PASS " + checks + " Android server checks\n");
        }
        catch (Throwable error)
        {
            code = 0;
            result.putString("stream", "FAIL " + (error instanceof AssertionError ? error.getMessage() : error.getClass().getSimpleName()) + "\n");
        }
        finally
        {
            if (ownsServer)
            {
                try
                {
                    stopServer();
                }
                catch (Exception ignored)
                {
                    code = 0;
                    result.putString("cleanup", "Server shutdown was not confirmed");
                }
            }
        }
        finish(code, result);
    }

    private void startServer() throws Exception
    {
        onMain(() -> {
            target.startForegroundService(new Intent(target, ServerService.class));
            return null;
        });
        ownsServer = true;
    }

    private void stopServer() throws Exception
    {
        onMain(() -> {
            target.startService(new Intent(target, ServerService.class).setAction(ServerService.ACTION_STOP));
            return null;
        });
        await(() -> onMain(() -> !ServerService.active && !ServerService.hosting), "service stops its native child");
        ownsServer = false;
    }

    private void await(Callable<Boolean> condition, String description) throws Exception
    {
        long deadline = SystemClock.elapsedRealtime() + 60000;
        while (!condition.call() && SystemClock.elapsedRealtime() < deadline)
        {
            SystemClock.sleep(50);
        }
        check(condition.call(), description);
    }

    private void check(boolean passed, String description)
    {
        if (!passed)
        {
            throw new AssertionError(description);
        }
        checks++;
        progress("PASS " + description);
    }

    private void progress(String message)
    {
        Bundle status = new Bundle();
        status.putString("stream", message + "\n");
        sendStatus(0, status);
    }

    private <T> T onMain(Callable<T> action) throws Exception
    {
        AtomicReference<T> value = new AtomicReference<>();
        AtomicReference<Exception> failure = new AtomicReference<>();
        runOnMainSync(() -> {
            try
            {
                value.set(action.call());
            }
            catch (Exception error)
            {
                failure.set(error);
            }
        });
        if (failure.get() != null)
        {
            throw failure.get();
        }
        return value.get();
    }
}
