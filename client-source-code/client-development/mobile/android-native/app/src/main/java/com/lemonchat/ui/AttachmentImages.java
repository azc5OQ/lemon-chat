package com.lemonchat.ui;

import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.drawable.Drawable;
import android.os.Handler;
import android.os.Looper;
import android.util.LruCache;
import com.lemonchat.model.Attachment;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.Set;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/** creates bounded image previews without keeping another copy of attachment bytes */
final class AttachmentImages implements AutoCloseable
{
    private final Runnable changed;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Set<String> pending = new HashSet<>();
    private final Set<String> invalid = new LinkedHashSet<>();
    private final LruCache<String, Bitmap> images = new LruCache<>(8 * 1024 * 1024)
    {
        @Override
        protected int sizeOf(String key, Bitmap value)
        {
            return value.getAllocationByteCount();
        }
    };
    private ThreadPoolExecutor worker;
    private long generation;

    AttachmentImages(Resources resources, Runnable changed)
    {
        this.changed = changed;
    }

    Drawable drawable(Attachment attachment)
    {
        if (!attachment.picture || invalid.contains(attachment.id))
        {
            return null;
        }
        Bitmap image = images.get(attachment.id);
        return new Preview(attachment, image == null ? 320 : image.getWidth(), image == null ? 200 : image.getHeight());
    }

    private void request(Attachment attachment)
    {
        if (pending.contains(attachment.id) || invalid.contains(attachment.id))
        {
            return;
        }
        if (worker == null)
        {
            worker = new ThreadPoolExecutor(1, 1, 10, TimeUnit.SECONDS, new ArrayBlockingQueue<>(8), runnable -> new Thread(runnable, "attachment-images"));
            worker.allowCoreThreadTimeOut(true);
        }
        long origin = generation;
        pending.add(attachment.id);
        try
        {
            worker.execute(() ->
            {
                Bitmap decoded = decode(attachment.data());
                main.post(() ->
                {
                    if (generation != origin)
                    {
                        return;
                    }
                    pending.remove(attachment.id);
                    if (decoded == null)
                    {
                        invalid.add(attachment.id);
                        while (invalid.size() > 128)
                        {
                            invalid.remove(invalid.iterator().next());
                        }
                    }
                    else
                    {
                        images.put(attachment.id, decoded);
                    }
                    changed.run();
                });
            });
        }
        catch (RejectedExecutionException unavailable)
        {
            pending.remove(attachment.id);
        }
    }

    private final class Preview extends Drawable
    {
        private final Attachment attachment;
        private final int width;
        private final int height;
        private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);

        Preview(Attachment attachment, int width, int height)
        {
            this.attachment = attachment;
            this.width = width;
            this.height = height;
        }

        @Override
        public void draw(Canvas canvas)
        {
            if (getCallback() instanceof android.view.View && !((android.view.View) getCallback()).getGlobalVisibleRect(new android.graphics.Rect()))
            {
                return;
            }
            Bitmap bitmap = images.get(attachment.id);
            if (bitmap == null)
            {
                request(attachment);
            }
            else
            {
                canvas.drawBitmap(bitmap, null, getBounds(), paint);
            }
        }

        @Override
        public int getIntrinsicWidth()
        {
            return width;
        }

        @Override
        public int getIntrinsicHeight()
        {
            return height;
        }

        @Override
        public void setAlpha(int alpha)
        {
            paint.setAlpha(alpha);
        }

        @Override
        public void setColorFilter(ColorFilter filter)
        {
            paint.setColorFilter(filter);
        }

        @Override
        public int getOpacity()
        {
            return PixelFormat.TRANSLUCENT;
        }
    }

    static Bitmap decode(byte[] bytes)
    {
        if (bytes.length > Attachment.MAX_BYTES)
        {
            return null;
        }
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(bytes, 0, bytes.length, bounds);
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0 || bounds.outWidth > 8192 || bounds.outHeight > 8192 || (long) bounds.outWidth * bounds.outHeight > 16777216)
        {
            return null;
        }
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inPreferredConfig = Bitmap.Config.ARGB_8888;
        options.inSampleSize = 1;
        while (Math.max(bounds.outWidth, bounds.outHeight) / options.inSampleSize > 1024)
        {
            options.inSampleSize *= 2;
        }
        try
        {
            Bitmap image = BitmapFactory.decodeByteArray(bytes, 0, bytes.length, options);
            if (image != null && image.getAllocationByteCount() > 4 * 1024 * 1024)
            {
                image.recycle();
                return null;
            }
            return image;
        }
        catch (IllegalArgumentException invalid)
        {
            return null;
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
        invalid.clear();
        images.evictAll();
    }
}
