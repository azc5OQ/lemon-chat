package com.lemonchat.server;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.annotation.SuppressLint;
import android.content.Intent;
import android.net.wifi.WifiManager;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import org.json.JSONObject;

/** One foreground session owns exactly one native server process. */
public class ServerService extends Service
{
    public static final String ACTION_STOP = "com.lemonchat.server.STOP";
    private static final String CHANNEL = "hosting";
    // Accessed on the main thread by the Activity and Service.
    public static boolean active;
    public static boolean hosting;
    public static String status = "Stopped";
    public static String funnelAddress = "";
    public static String funnelLogin = "";
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final ServerProcess runner = new ServerProcess();
    private final FunnelProcess funnel = new FunnelProcess();
    private PowerManager.WakeLock wakeLock;
    private WifiManager.WifiLock wifiLock;
    private boolean stopping;
    private int generation;
    private String funnelFailure = "";

    @Override public IBinder onBind(Intent intent)
    {
        return null;
    }

    // Hosting is an ongoing foreground session; every stop/failure/destroy path releases the locks.
    @SuppressLint("WakelockTimeout")
    @Override public int onStartCommand(Intent intent, int flags, int startId)
    {
        if (intent != null && ACTION_STOP.equals(intent.getAction()))
        {
            if (active)
            {
                stopping = true;
                hosting = false;
                status = "Stopping…";
                UpdateNotification();
                funnel.Stop();
                runner.Stop();
            }
            else
            {
                stopSelf();
            }
            return START_NOT_STICKY;
        }
        if (active)
        {
            return START_NOT_STICKY;
        }
        active = true;
        hosting = false;
        stopping = false;
        funnelAddress = "";
        funnelLogin = "";
        funnelFailure = "";
        status = "Starting…";
        final int session = ++generation;
        try
        {
            startForeground(1, BuildNotification());
            ServerSettings.Read(getFilesDir());
            File executable = new File(getApplicationInfo().nativeLibraryDir, "libchat-server.so");
            if (!executable.canExecute())
            {
                throw new IllegalStateException("Server binary is missing. Reinstall the APK for this phone.");
            }
            // Each start gets a fresh, bounded log. Never include passwords here.
            try (FileOutputStream log = new FileOutputStream(new File(getFilesDir(), "server.log")))
            {
                log.write("Starting Lemon Chat Server…\n".getBytes(StandardCharsets.UTF_8));
            }
            wakeLock =
                getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "lemonchat:hosting");
            wakeLock.acquire();
            WifiManager wifi = (WifiManager)getApplicationContext().getSystemService(WIFI_SERVICE);
            if (wifi != null)
            {
                wifiLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "lemonchat:hosting");
                wifiLock.acquire();
            }
            PrepareServer(session, executable);
        }
        catch (Exception error)
        {
            AppendError(error.toString());
            Finish("Could not start. Check Settings and View log.");
        }
        return START_NOT_STICKY;
    }

    /** Copies the web client and writes startup settings away from the UI thread. */
    private void PrepareServer(int session, File executable)
    {
        new Thread(() ->
        {
            try
            {
                JSONObject settings = FunnelSettings.Prepare(this);
                handler.post(() ->
                {
                    if (session != generation || !active)
                    {
                        return;
                    }
                    if (stopping)
                    {
                        Finish("Stopped");
                        return;
                    }
                    LaunchNativeServer(session, executable, settings);
                });
            }
            catch (Exception error)
            {
                handler.post(() ->
                {
                    if (session == generation && active)
                    {
                        Finish("Could not prepare hosting: " + error.getMessage());
                    }
                });
            }
        }, "lemon-hosting-setup").start();
    }

    private void LaunchNativeServer(int session, File executable, JSONObject settings)
    {
        runner.Start(Collections.singletonList(executable.getAbsolutePath()), getFilesDir(),
            new File(getFilesDir(), "server.log"), new ServerProcess.Listener()
            {
                @Override public void Ready()
                {
                    handler.post(() ->
                    {
                        if (session != generation || stopping || !active)
                        {
                            return;
                        }
                        hosting = true;
                        status = "Hosting";
                        if (FunnelSettings.IsEnabled(ServerService.this))
                        {
                            try
                            {
                                StartFunnel(session, settings);
                            }
                            catch (Exception error)
                            {
                                funnelFailure = "Could not start Funnel: " + error.getMessage();
                                runner.Stop();
                            }
                        }
                        UpdateNotification();
                    });
                }

                @Override public void Finished(int code, String error)
                {
                    handler.post(() ->
                    {
                        if (session != generation)
                        {
                            return;
                        }
                        Finish(stopping ? "Stopped" : !funnelFailure.isEmpty() ? funnelFailure
                            : "Server stopped unexpectedly. Open View log for details.");
                        if (error != null)
                        {
                            AppendError(error);
                        }
                    });
                }
            });
    }

    private void StartFunnel(int session, JSONObject settings)
    {
        status = "Connecting Tailscale";
        funnel.Start(new File(getApplicationInfo().nativeLibraryDir, "liblemon-funnel.so"), getFilesDir(),
            settings.optInt("http_port", 8080), settings.optInt("websocket_port", 1111), new FunnelProcess.Listener()
            {
                @Override public void Update(String type, String value)
                {
                    handler.post(() ->
                    {
                        if (session != generation || stopping || !active)
                        {
                            return;
                        }
                        if (type.equals("login"))
                        {
                            funnelLogin = value;
                            status = "Sign in to Tailscale";
                        }
                        else if (type.equals("ready"))
                        {
                            funnelLogin = "";
                            funnelAddress = value;
                            status = "Hosting publicly";
                        }
                        else if (type.equals("status"))
                        {
                            status = value;
                        }
                        else if (type.equals("error"))
                        {
                            funnelFailure = "Funnel: " + value;
                            funnelAddress = "";
                            funnel.Stop();
                            runner.Stop();
                        }
                        UpdateNotification();
                    });
                }

                @Override public void Finished()
                {
                    handler.post(() ->
                    {
                        if (session == generation && active && !stopping)
                        {
                            funnelAddress = "";
                            if (funnelFailure.isEmpty())
                            {
                                funnelFailure = "Tailscale stopped. Start hosting again to reconnect.";
                            }
                            runner.Stop();
                        }
                    });
                }
            });
    }

    private void AppendError(String message)
    {
        try (FileOutputStream log = new FileOutputStream(new File(getFilesDir(), "server.log"), true))
        {
            log.write(("\n" + message + "\n").getBytes(StandardCharsets.UTF_8));
        }
        catch (Exception ignored)
        {
        }
    }

    private void Finish(String message)
    {
        funnel.Stop();
        funnelAddress = "";
        funnelLogin = "";
        status = message;
        active = false;
        hosting = false;
        ReleaseLocks();
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    private void ReleaseLocks()
    {
        if (wifiLock != null && wifiLock.isHeld())
        {
            wifiLock.release();
        }
        if (wakeLock != null && wakeLock.isHeld())
        {
            wakeLock.release();
        }
        wifiLock = null;
        wakeLock = null;
    }

    private Notification BuildNotification()
    {
        NotificationManager manager = getSystemService(NotificationManager.class);
        manager.createNotificationChannel(
            new NotificationChannel(CHANNEL, "Chat server", NotificationManager.IMPORTANCE_LOW));
        PendingIntent open =
            PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class),
                                      PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        PendingIntent stop =
            PendingIntent.getService(this, 1, new Intent(this, ServerService.class).setAction(ACTION_STOP),
                                     PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        return new Notification.Builder(this, CHANNEL)
            .setContentTitle("Lemon Chat Server")
            .setContentText(status)
            .setSmallIcon(android.R.drawable.stat_notify_sync)
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .addAction(new Notification.Action.Builder(null, "Stop server", stop).build())
            .build();
    }

    private void UpdateNotification()
    {
        getSystemService(NotificationManager.class).notify(1, BuildNotification());
    }

    @Override public void onDestroy()
    {
        boolean wasActive = active;
        ++generation;
        funnel.Stop();
        runner.Stop();
        funnelAddress = "";
        funnelLogin = "";
        ReleaseLocks();
        active = false;
        hosting = false;
        if (wasActive || stopping)
        {
            status = "Stopped";
        }
        handler.removeCallbacksAndMessages(null);
        super.onDestroy();
    }
}
