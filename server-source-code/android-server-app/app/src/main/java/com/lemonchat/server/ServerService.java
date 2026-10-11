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

/** One foreground session owns exactly one native server process. */
public class ServerService extends Service
{
    public static final String ACTION_STOP = "com.lemonchat.server.STOP";
    private static final String CHANNEL = "hosting";
    // Accessed on the main thread by the Activity and Service.
    public static boolean active;
    public static boolean hosting;
    public static String status = "Stopped";
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final ServerProcess runner = new ServerProcess();
    private PowerManager.WakeLock wakeLock;
    private WifiManager.WifiLock wifiLock;
    private boolean stopping;
    private int generation;

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
                                     Finish(stopping ? "Stopped"
                                                     : "Server stopped unexpectedly. Open View log for details.");
                                     if (error != null)
                                     {
                                         AppendError(error);
                                     }
                                 });
                             }
                         });
        }
        catch (Exception error)
        {
            AppendError(error.toString());
            Finish("Could not start. Check Settings and View log.");
        }
        return START_NOT_STICKY;
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
            new NotificationChannel(CHANNEL, "LAN server", NotificationManager.IMPORTANCE_LOW));
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
        ++generation;
        runner.Stop();
        ReleaseLocks();
        active = false;
        hosting = false;
        if ("Hosting".equals(status) || "Starting…".equals(status) || stopping)
        {
            status = "Stopped";
        }
        handler.removeCallbacksAndMessages(null);
        super.onDestroy();
    }
}
