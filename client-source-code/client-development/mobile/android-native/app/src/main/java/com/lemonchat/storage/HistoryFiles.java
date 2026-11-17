package com.lemonchat.storage;

import android.content.Context;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.security.SecureRandom;
import java.util.Arrays;

/** overwrites only the app's known history databases and sidecars */
final class HistoryFiles
{
    static final String CURRENT = "private-history.db";
    static final String LEGACY = "private-conversations.db";
    private static final String[] SUFFIXES = {"", "-wal", "-shm", "-journal"};
    private final Context context;
    private final String current;
    private final String legacy;

    HistoryFiles(Context context)
    {
        this(context, false);
    }

    HistoryFiles(Context context, boolean test)
    {
        this.context = context;
        current = test ? "private-history-test-clear.db" : CURRENT;
        legacy = test ? "private-conversations-test-clear.db" : LEGACY;
    }

    boolean legacyExists()
    {
        return context.getDatabasePath(legacy).exists();
    }

    boolean currentExists()
    {
        return context.getDatabasePath(current).exists();
    }

    void erase(boolean includeCurrent) throws IOException
    {
        IOException failure = null;
        for (String name : includeCurrent ? new String[]{current, legacy} : new String[]{legacy})
        {
            for (String suffix : SUFFIXES)
            {
                try
                {
                    File file = checkedFile(name + suffix);
                    if (!file.exists())
                    {
                        continue;
                    }
                    overwrite(file);
                    if (!file.delete() && file.exists())
                    {
                        throw new IOException("A history file could not be removed");
                    }
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

    private File checkedFile(String name) throws IOException
    {
        File directory = context.getDatabasePath(current).getParentFile().getCanonicalFile();
        if (!directory.toPath().startsWith(context.getDataDir().getCanonicalFile().toPath()))
        {
            throw new IOException("The history directory is outside app storage");
        }
        File expected = new File(directory, name);
        if (!expected.getCanonicalFile().equals(expected))
        {
            throw new IOException("The history file points outside its storage location");
        }
        if (expected.exists() && !expected.isFile())
        {
            throw new IOException("The history storage path is not a file");
        }
        return expected;
    }

    private static void overwrite(File file) throws IOException
    {
        byte[] bytes = new byte[65536];
        SecureRandom random = new SecureRandom();
        try (RandomAccessFile stream = new RandomAccessFile(file, "rw"))
        {
            long remaining = stream.length();
            while (remaining > 0)
            {
                random.nextBytes(bytes);
                int size = (int)Math.min(bytes.length, remaining);
                stream.write(bytes, 0, size);
                remaining -= size;
            }
            stream.getFD().sync();
        }
        finally
        {
            Arrays.fill(bytes, (byte)0);
        }
    }
}
