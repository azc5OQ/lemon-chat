package com.lemonchat.ui;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import com.lemonchat.model.Attachment;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.security.SecureRandom;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicLong;

/** grants read access only to attachments the user explicitly opened */
public final class AttachmentProvider extends ContentProvider
{
    private static final Map<String, Entry> OPENED = new LinkedHashMap<>();
    private static final long LIFETIME = 60 * 60 * 1000L;
    private static final AtomicLong GENERATION = new AtomicLong();
    private static volatile boolean clearing;

    static Uri publish(Context context, Attachment attachment) throws IOException
    {
        return publish(context, attachment, generation());
    }

    static long generation()
    {
        return GENERATION.get();
    }

    static boolean current(long generation)
    {
        return !clearing && GENERATION.get() == generation;
    }

    static Uri publish(Context context, Attachment attachment, long generation) throws IOException
    {
        File directory = directory(context);
        if (!directory.isDirectory() && !directory.mkdirs())
        {
            throw new IOException("The attachment cache is unavailable");
        }
        synchronized (OPENED)
        {
            if (!current(generation))
            {
                throw new IOException("This attachment request was cancelled when history was cleared");
            }
            prune();
            while (OPENED.size() >= 8)
            {
                Iterator<Entry> entries = OPENED.values().iterator();
                entries.next().file.delete();
                entries.remove();
            }
            String token = UUID.randomUUID().toString();
            File file = new File(directory, token + ".bin");
            if (!file.createNewFile())
            {
                throw new IOException("The attachment could not be opened");
            }
            try (FileOutputStream output = new FileOutputStream(file))
            {
                output.write(attachment.data());
            }
            catch (IOException error)
            {
                file.delete();
                throw error;
            }
            if (!current(generation))
            {
                throw new IOException("This attachment request was cancelled when history was cleared");
            }
            String mime = attachment.mime.matches("[A-Za-z0-9.+-]+/[A-Za-z0-9.+-]+") ? attachment.mime : "application/octet-stream";
            OPENED.put(token, new Entry(file, attachment.name, mime, android.os.SystemClock.elapsedRealtime()));
            return new Uri.Builder().scheme("content").authority(context.getPackageName() + ".attachments").appendPath(token).build();
        }
    }

    /**
     * invalidates queued opens and removes the app's temporary attachment copies
     *
     * @param context application context
     * @throws IOException when a copy cannot be overwritten or deleted
     */
    public static synchronized void clearOpenedCopies(Context context) throws IOException
    {
        clearing = true;
        GENERATION.incrementAndGet();
        try
        {
            synchronized (OPENED)
            {
                for (String token : OPENED.keySet())
                {
                    Uri uri = new Uri.Builder().scheme("content").authority(context.getPackageName() + ".attachments").appendPath(token).build();
                    context.revokeUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                }
                OPENED.clear();
                File directory = directory(context).getAbsoluteFile();
                if (!directory.exists())
                {
                    return;
                }
                File expected = new File(context.getCacheDir().getCanonicalFile(), "opened-attachments");
                if (!directory.getCanonicalFile().equals(expected))
                {
                    throw new IOException("The attachment cache path is invalid");
                }
                File[] files = directory.listFiles();
                if (files == null)
                {
                    throw new IOException("The attachment cache cannot be read");
                }
                IOException failure = null;
                SecureRandom random = new SecureRandom();
                byte[] block = new byte[32768];
                for (File file : files)
                {
                    if (file.getName().matches("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.bin"))
                    {
                        try
                        {
                            erase(file, expected, random, block);
                        }
                        catch (IOException error)
                        {
                            if (failure == null)
                            {
                                failure = error;
                            }
                            else
                            {
                                failure.addSuppressed(error);
                            }
                        }
                    }
                }
                if (failure != null)
                {
                    throw failure;
                }
            }
        }
        finally
        {
            clearing = false;
        }
    }

    private static void erase(File file, File directory, SecureRandom random, byte[] block) throws IOException
    {
        if (!file.isFile() || !file.getCanonicalFile().equals(new File(directory, file.getName())))
        {
            throw new IOException("An attachment cache file has an invalid path");
        }
        try (RandomAccessFile output = new RandomAccessFile(file, "rw"))
        {
            long length = output.length();
            if (length > Attachment.MAX_BYTES)
            {
                throw new IOException("An attachment cache file exceeds the size limit");
            }
            for (long offset = 0; offset < length; offset += block.length)
            {
                random.nextBytes(block);
                output.write(block, 0, (int)Math.min(block.length, length - offset));
            }
            output.getFD().sync();
        }
        if (!file.delete() && file.exists())
        {
            throw new IOException("An attachment cache file could not be deleted");
        }
    }

    @Override
    public boolean onCreate()
    {
        File[] previous = directory(getContext()).listFiles();
        if (previous != null)
        {
            for (File file : previous)
            {
                if (file.isFile() && file.getName().matches("[0-9a-f-]{36}\\.bin"))
                {
                    file.delete();
                }
            }
        }
        return true;
    }

    @Override
    public String getType(Uri uri)
    {
        synchronized (OPENED)
        {
            Entry entry = lookup(uri);
            return entry == null ? "application/octet-stream" : entry.mime;
        }
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] arguments, String sortOrder)
    {
        String[] columns = projection == null ? new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE} : projection;
        MatrixCursor result = new MatrixCursor(columns);
        synchronized (OPENED)
        {
            Entry entry = lookup(uri);
            if (entry == null)
            {
                return result;
            }
            Object[] values = new Object[columns.length];
            for (int index = 0; index < columns.length; index++)
            {
                if (OpenableColumns.DISPLAY_NAME.equals(columns[index]))
                {
                    values[index] = entry.name;
                }
                else if (OpenableColumns.SIZE.equals(columns[index]))
                {
                    values[index] = entry.file.length();
                }
            }
            result.addRow(values);
        }
        return result;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException
    {
        if (!"r".equals(mode))
        {
            throw new FileNotFoundException("Attachments are read only");
        }
        synchronized (OPENED)
        {
            Entry entry = lookup(uri);
            if (entry == null)
            {
                throw new FileNotFoundException("This attachment link expired");
            }
            return ParcelFileDescriptor.open(entry.file, ParcelFileDescriptor.MODE_READ_ONLY);
        }
    }

    @Override
    public Uri insert(Uri uri, ContentValues values)
    {
        throw new UnsupportedOperationException("Attachments are read only");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] arguments)
    {
        throw new UnsupportedOperationException("Attachments are read only");
    }

    @Override
    public int delete(Uri uri, String selection, String[] arguments)
    {
        throw new UnsupportedOperationException("Attachments are read only");
    }

    private Entry lookup(Uri uri)
    {
        prune();
        if (!"content".equals(uri.getScheme()) || !(getContext().getPackageName() + ".attachments").equals(uri.getAuthority()) || uri.getPathSegments().size() != 1)
        {
            return null;
        }
        return OPENED.get(uri.getLastPathSegment());
    }

    private static void prune()
    {
        long now = android.os.SystemClock.elapsedRealtime();
        Iterator<Entry> iterator = OPENED.values().iterator();
        while (iterator.hasNext())
        {
            Entry entry = iterator.next();
            if (now - entry.created >= LIFETIME)
            {
                entry.file.delete();
                iterator.remove();
            }
        }
    }

    private static File directory(Context context)
    {
        return new File(context.getCacheDir(), "opened-attachments");
    }

    private static final class Entry
    {
        final File file;
        final String name;
        final String mime;
        final long created;

        Entry(File file, String name, String mime, long created)
        {
            this.file = file;
            this.name = name;
            this.mime = mime;
            this.created = created;
        }
    }
}
