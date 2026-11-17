package com.lemonchat.service;

import android.content.Context;
import android.util.AtomicFile;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.Executor;

/** records fixed event names without messages, addresses or identity data */
final class ConnectionLog
{
    enum Event
    {
        STARTED,
        CONNECTING,
        AUTHENTICATING,
        CONNECTED,
        DISCONNECTED,
        FAILED,
        RECONNECT_SCHEDULED,
        SETTINGS_SAVED,
        INCOMING_CALL,
        CALL_ACCEPTED,
        CALL_DECLINED,
        STORAGE_FAILED,
        STOPPED
    }

    private final ArrayDeque<String> entries = new ArrayDeque<>();
    private final Executor storage;
    private final AtomicFile file;
    private boolean enabled;
    private boolean queued;

    ConnectionLog(Context context, Executor storage)
    {
        this.storage = storage;
        file = new AtomicFile(new File(context.getFilesDir(), "connection.log"));
    }

    synchronized void setFileEnabled(boolean value)
    {
        enabled = value;
        schedule();
    }

    synchronized void add(Event event)
    {
        String timestamp = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT).format(new Date());
        entries.addLast(timestamp + "  " + event.name().toLowerCase(Locale.ROOT).replace('_', ' '));
        while (entries.size() > 256)
        {
            entries.removeFirst();
        }
        schedule();
    }

    synchronized String text()
    {
        return String.join("\n", entries);
    }

    private void schedule()
    {
        if (!enabled || queued)
        {
            return;
        }
        queued = true;
        storage.execute(() ->
        {
            byte[] content;
            synchronized (ConnectionLog.this)
            {
                queued = false;
                if (!enabled)
                {
                    return;
                }
                content = text().getBytes(StandardCharsets.UTF_8);
            }
            FileOutputStream stream = null;
            try
            {
                stream = file.startWrite();
                stream.write(content);
                file.finishWrite(stream);
            }
            catch (Exception error)
            {
                file.failWrite(stream);
                android.util.Log.w("LemonNative", "The connection log could not be saved");
            }
        });
    }
}
