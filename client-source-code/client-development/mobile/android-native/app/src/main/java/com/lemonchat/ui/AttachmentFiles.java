package com.lemonchat.ui;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import com.lemonchat.model.Attachment;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.ref.WeakReference;
import java.util.Base64;
import java.util.UUID;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/** retains one attachment draft and file operation across activity recreation */
final class AttachmentFiles implements AutoCloseable
{
    interface Listener
    {
        void onAttachmentChanged();
    }

    static final class Ticket
    {
        final String scope;
        final String conversation;
        final int limit;
        final boolean picture;
        final boolean avatar;

        Ticket(String scope, String conversation, int limit, boolean picture, boolean avatar)
        {
            this.scope = scope;
            this.conversation = conversation;
            this.limit = Math.min(Attachment.MAX_BYTES, Math.max(0, limit));
            this.picture = picture;
            this.avatar = avatar;
        }
    }

    final ThreadPoolExecutor worker = new ThreadPoolExecutor(1, 1, 10, TimeUnit.SECONDS, new ArrayBlockingQueue<>(2), runnable -> new Thread(runnable, "attachment-files"));
    private final Handler main = new Handler(Looper.getMainLooper());
    private WeakReference<Listener> listener = new WeakReference<>(null);
    Ticket picker;
    Ticket owner;
    Ticket avatarOwner;
    Attachment draft;
    Attachment saving;
    String avatarData;
    Uri openUri;
    String openMime;
    private long openGeneration;
    Uri musicUri;
    String musicName;
    String musicScope;
    boolean sending;
    long sentAfterId;
    boolean busy;
    private boolean closed;
    private String notice;

    AttachmentFiles()
    {
        worker.allowCoreThreadTimeOut(true);
    }

    void attach(Listener value)
    {
        listener = new WeakReference<>(value);
        changed();
    }

    void detach(Listener value)
    {
        if (listener.get() == value)
        {
            listener.clear();
        }
    }

    String takeNotice()
    {
        String value = notice;
        notice = null;
        return value;
    }

    boolean matches(String scope, String conversation)
    {
        return draft != null && owner != null && owner.scope.equals(scope) && owner.conversation.equals(conversation);
    }

    void clearDraft()
    {
        draft = null;
        owner = null;
        sending = false;
        changed();
    }

    boolean read(ContentResolver resolver, Uri uri)
    {
        Ticket ticket = picker;
        picker = null;
        if (closed || busy || ticket == null || ticket.limit <= 0)
        {
            return false;
        }
        busy = true;
        worker.execute(() ->
        {
            Attachment result = null;
            String avatar = null;
            String error = null;
            try
            {
                String name = displayName(resolver, uri);
                String mime = resolver.getType(uri);
                if (mime == null || mime.length() > 100)
                {
                    mime = "application/octet-stream";
                }
                byte[] bytes = readBytes(resolver, uri, ticket.avatar ? Attachment.MAX_BYTES : ticket.limit);
                if (ticket.avatar)
                {
                    avatar = avatar(bytes, ticket.limit);
                }
                else
                {
                    if (ticket.picture)
                    {
                        Bitmap test = AttachmentImages.decode(bytes);
                        if (test == null)
                        {
                            throw new IOException("This picture cannot be previewed. Select it as a file instead.");
                        }
                        test.recycle();
                    }
                    result = new Attachment(UUID.randomUUID().toString(), name, mime, bytes, ticket.picture);
                }
            }
            catch (Exception failure)
            {
                error = failure.getMessage() == null ? "The selected file could not be opened" : failure.getMessage();
            }
            Attachment selected = result;
            String selectedAvatar = avatar;
            String problem = error;
            main.post(() ->
            {
                if (closed)
                {
                    return;
                }
                busy = false;
                if (problem == null)
                {
                    if (ticket.avatar)
                    {
                        avatarOwner = ticket;
                        avatarData = selectedAvatar;
                    }
                    else
                    {
                        owner = ticket;
                        draft = selected;
                        sending = false;
                    }
                }
                else
                {
                    notice = problem;
                }
                changed();
            });
        });
        changed();
        return true;
    }

    void music(ContentResolver resolver, Uri uri, String scope)
    {
        if (closed || busy)
        {
            return;
        }
        busy = true;
        worker.execute(() ->
        {
            String name = displayName(resolver, uri);
            main.post(() ->
            {
                if (!closed)
                {
                    busy = false;
                    musicUri = uri;
                    musicName = name;
                    musicScope = scope;
                    changed();
                }
            });
        });
    }

    boolean save(ContentResolver resolver, Uri uri)
    {
        Attachment attachment = saving;
        saving = null;
        if (closed || busy || attachment == null)
        {
            return false;
        }
        busy = true;
        worker.execute(() ->
        {
            String result;
            try (OutputStream output = resolver.openOutputStream(uri, "wt"))
            {
                if (output == null)
                {
                    throw new IOException("The destination could not be opened");
                }
                output.write(attachment.data());
                output.flush();
                result = "File saved";
            }
            catch (Exception failure)
            {
                result = "The file could not be saved";
            }
            complete(result);
        });
        changed();
        return true;
    }

    boolean open(Context context, Attachment attachment)
    {
        if (closed || busy)
        {
            return false;
        }
        busy = true;
        long generation = AttachmentProvider.generation();
        worker.execute(() ->
        {
            try
            {
                Uri uri = AttachmentProvider.publish(context, attachment, generation);
                main.post(() ->
                {
                    if (!closed)
                    {
                        busy = false;
                        if (AttachmentProvider.current(generation))
                        {
                            openUri = uri;
                            openMime = attachment.mime;
                            openGeneration = generation;
                        }
                        else
                        {
                            notice = "The attachment request was cancelled because history was cleared";
                        }
                        changed();
                    }
                });
            }
            catch (Exception failure)
            {
                complete("The file could not be opened");
            }
        });
        changed();
        return true;
    }

    private static byte[] readBytes(ContentResolver resolver, Uri uri, int limit) throws IOException
    {
        try (InputStream input = resolver.openInputStream(uri); ByteArrayOutputStream output = new ByteArrayOutputStream())
        {
            if (input == null)
            {
                throw new IOException("The file could not be opened");
            }
            byte[] block = new byte[8192];
            int read;
            while ((read = input.read(block)) != -1)
            {
                if (read == 0)
                {
                    throw new IOException("The file provider stopped returning data");
                }
                if (read > limit - output.size())
                {
                    throw new IOException("This file exceeds the allowed " + limit + " byte limit");
                }
                output.write(block, 0, read);
            }
            return output.toByteArray();
        }
    }

    private static String displayName(ContentResolver resolver, Uri uri)
    {
        try (Cursor cursor = resolver.query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null))
        {
            if (cursor != null && cursor.moveToFirst())
            {
                String name = cursor.getString(0);
                if (name != null && !name.isBlank())
                {
                    return name.replaceAll("[\\p{Cntrl}/\\\\]", "_").substring(0, Math.min(name.length(), 180));
                }
            }
        }
        catch (RuntimeException unavailable)
        {
            // use a neutral name when the provider does not expose one
        }
        return "attachment";
    }

    private static String avatar(byte[] bytes, int limit) throws IOException
    {
        Bitmap source = AttachmentImages.decode(bytes);
        if (source == null)
        {
            throw new IOException("Choose a PNG, JPEG, GIF or WebP picture");
        }
        int side = Math.min(source.getWidth(), source.getHeight());
        Bitmap square = Bitmap.createBitmap(source, (source.getWidth() - side) / 2, (source.getHeight() - side) / 2, side, side);
        Bitmap image = Bitmap.createScaledBitmap(square, 256, 256, true);
        if (square != source && square != image)
        {
            square.recycle();
        }
        if (source != image)
        {
            source.recycle();
        }
        try
        {
            for (int size = 256; size >= 32; size /= 2)
            {
                Bitmap scaled = Bitmap.createScaledBitmap(image, size, size, true);
                try
                {
                    for (int quality = 90; quality >= 30; quality -= 20)
                    {
                        ByteArrayOutputStream output = new ByteArrayOutputStream();
                        scaled.compress(Bitmap.CompressFormat.JPEG, quality, output);
                        if (output.size() <= limit)
                        {
                            return "data:image/jpeg;base64," + Base64.getEncoder().encodeToString(output.toByteArray());
                        }
                    }
                }
                finally
                {
                    if (scaled != image)
                    {
                        scaled.recycle();
                    }
                }
            }
            throw new IOException("The server's avatar size limit is too small for this picture");
        }
        finally
        {
            image.recycle();
        }
    }

    private void complete(String result)
    {
        main.post(() ->
        {
            if (!closed)
            {
                busy = false;
                notice = result;
                changed();
            }
        });
    }

    private void changed()
    {
        if (openUri != null && !AttachmentProvider.current(openGeneration))
        {
            openUri = null;
            openMime = null;
            notice = "The attachment request was cancelled because history was cleared";
        }
        Listener value = listener.get();
        if (value != null)
        {
            value.onAttachmentChanged();
        }
    }

    @Override
    public void close()
    {
        closed = true;
        listener.clear();
        worker.shutdownNow();
        draft = null;
        saving = null;
        avatarData = null;
        picker = null;
        owner = null;
        avatarOwner = null;
    }
}
