package com.lemonchat.ui;

import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.Drawable;
import android.os.Handler;
import android.os.Looper;
import com.lemonchat.model.ServerIcon;
import java.util.Base64;
import java.util.HashMap;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/** decodes bounded server images away from the main thread */
final class ServerIconCache implements AutoCloseable
{
    private final Resources resources;
    private final Runnable changed;
    private final int encodedLimit;
    private final int pixelLimit;
    private final int cacheLimit;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Map<Long, String> catalog = new HashMap<>();
    private final Map<Long, String> pending = new HashMap<>();
    private final LinkedHashMap<Long, Entry> entries = new LinkedHashMap<>(16, 0.75f, true);
    private ThreadPoolExecutor worker;
    private long generation;
    private int decodedBytes;
    private boolean keepDecoded;

    ServerIconCache(Resources resources, Runnable changed)
    {
        this(resources, changed, 40959, 64, 2 * 1024 * 1024);
    }

    ServerIconCache(Resources resources, Runnable changed, int encodedLimit, int pixelLimit, int cacheLimit)
    {
        this.resources = resources;
        this.changed = changed;
        this.encodedLimit = encodedLimit;
        this.pixelLimit = pixelLimit;
        this.cacheLimit = cacheLimit;
    }

    void update(List<ServerIcon> icons)
    {
        update(icons, false);
    }

    void update(List<ServerIcon> icons, boolean keepDecoded)
    {
        this.keepDecoded = keepDecoded;
        catalog.clear();
        for (ServerIcon icon : icons)
        {
            catalog.put(icon.id, icon.data);
        }
        Iterator<Map.Entry<Long, Entry>> iterator = entries.entrySet().iterator();
        while (iterator.hasNext())
        {
            Map.Entry<Long, Entry> cached = iterator.next();
            String current = catalog.get(cached.getKey());
            if (current == null || !(keepDecoded && current.isEmpty()) && !cached.getValue().data.equals(current))
            {
                decodedBytes -= cached.getValue().bytes;
                iterator.remove();
            }
        }
    }

    Drawable drawable(long id)
    {
        String data = catalog.get(id);
        if (data == null)
        {
            return null;
        }
        Entry cached = entries.get(id);
        if (cached != null && (cached.data.equals(data) || keepDecoded && data.isEmpty()))
        {
            return cached.bitmap == null ? null : new BitmapDrawable(resources, cached.bitmap);
        }
        if (data.equals(pending.get(id)) || !supported(data, encodedLimit))
        {
            return null;
        }
        if (worker == null)
        {
            worker = new ThreadPoolExecutor(1, 1, 10, TimeUnit.SECONDS, new ArrayBlockingQueue<>(32), runnable -> new Thread(runnable, "server-icons"));
            worker.allowCoreThreadTimeOut(true);
        }
        long origin = generation;
        pending.put(id, data);
        try
        {
            worker.execute(() ->
            {
                Bitmap bitmap = decode(data, encodedLimit, pixelLimit);
                main.post(() ->
                {
                    if (origin != generation || !data.equals(pending.get(id)))
                    {
                        return;
                    }
                    pending.remove(id);
                    String current = catalog.get(id);
                    if (data.equals(current) || keepDecoded && current != null && current.isEmpty())
                    {
                        store(id, new Entry(data, bitmap));
                        changed.run();
                    }
                });
            });
        }
        catch (RejectedExecutionException unavailable)
        {
            pending.remove(id);
        }
        return null;
    }

    static boolean supported(String data)
    {
        return supported(data, 40959);
    }

    private static boolean supported(String data, int encodedLimit)
    {
        if (data == null || data.length() > encodedLimit)
        {
            return false;
        }
        int separator = data.indexOf(',');
        if (separator < 0 || separator > 30)
        {
            return false;
        }
        String header = data.substring(0, separator).toLowerCase(Locale.ROOT);
        return header.equals("data:image/png;base64") || header.equals("data:image/jpeg;base64") || header.equals("data:image/gif;base64") || header.equals("data:image/webp;base64");
    }

    private static Bitmap decode(String data)
    {
        return decode(data, 40959, 64);
    }

    private static Bitmap decode(String data, int encodedLimit, int pixelLimit)
    {
        if (!supported(data, encodedLimit))
        {
            return null;
        }
        try
        {
            byte[] encoded = Base64.getDecoder().decode(data.substring(data.indexOf(',') + 1));
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            BitmapFactory.decodeByteArray(encoded, 0, encoded.length, bounds);
            if (bounds.outWidth <= 0 || bounds.outHeight <= 0 || bounds.outWidth > 1024 || bounds.outHeight > 1024)
            {
                return null;
            }
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inPreferredConfig = Bitmap.Config.ARGB_8888;
            options.inSampleSize = 1;
            while (Math.max(bounds.outWidth, bounds.outHeight) / options.inSampleSize > pixelLimit)
            {
                options.inSampleSize *= 2;
            }
            Bitmap bitmap = BitmapFactory.decodeByteArray(encoded, 0, encoded.length, options);
            if (bitmap != null && (bitmap.getWidth() > pixelLimit || bitmap.getHeight() > pixelLimit || bitmap.getAllocationByteCount() > pixelLimit * pixelLimit * 4))
            {
                bitmap.recycle();
                return null;
            }
            return bitmap;
        }
        catch (IllegalArgumentException invalid)
        {
            return null;
        }
    }

    private void store(long id, Entry entry)
    {
        Entry previous = entries.put(id, entry);
        decodedBytes += entry.bytes - (previous == null ? 0 : previous.bytes);
        Iterator<Entry> iterator = entries.values().iterator();
        while (entries.size() > 128 || decodedBytes > cacheLimit)
        {
            decodedBytes -= iterator.next().bytes;
            iterator.remove();
        }
    }

    @Override
    public void close()
    {
        generation++;
        if (worker != null)
        {
            worker.shutdownNow();
            worker = null;
        }
        pending.clear();
        entries.clear();
        catalog.clear();
        decodedBytes = 0;
    }

    private static final class Entry
    {
        final String data;
        final Bitmap bitmap;
        final int bytes;

        Entry(String data, Bitmap bitmap)
        {
            this.data = data;
            this.bitmap = bitmap;
            this.bytes = bitmap == null ? 0 : bitmap.getAllocationByteCount();
        }
    }
}
