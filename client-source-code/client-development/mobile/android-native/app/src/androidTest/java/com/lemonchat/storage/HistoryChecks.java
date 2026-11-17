package com.lemonchat.storage;

import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.content.ContentValues;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.database.DatabaseErrorHandler;
import android.database.sqlite.SQLiteDatabase;
import com.lemonchat.crypto.Identity;
import com.lemonchat.model.Attachment;
import com.lemonchat.model.ChatMessage;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.KeyStore;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.UUID;
import org.json.JSONObject;

/** exercises consent, migration and erasure in isolated app storage */
public final class HistoryChecks
{
    private static final String SCOPE = "a".repeat(64);
    private static final String OTHER_SCOPE = "b".repeat(64);
    private static final String CONVERSATION = "user:" + "c".repeat(64);
    private int checks;

    public static int verify(Context context) throws Exception
    {
        HistoryChecks suite = new HistoryChecks();
        suite.consentAndKeys(context);
        suite.expiry(context);
        suite.missingKey(context);
        suite.migration(context, false);
        suite.migration(context, true);
        suite.overwrite(context);
        suite.maintenance(context);
        return suite.checks;
    }

    private void consentAndKeys(Context base) throws Exception
    {
        try (FixtureContext context = new FixtureContext(base))
        {
            String alias = "lemon-native-history-test-" + UUID.randomUUID();
            ArchiveVault cipher = new ArchiveVault(alias);
            IdentityVault identity = new IdentityVault(context);
            HistoryStore store = new HistoryStore(context, identity, cipher);
            try
            {
                HistoryPolicy policy = HistoryPolicy.load(context);
                check(!policy.configured && !policy.enabled(), "no decision refuses history");
                store.save(SCOPE, "unsaved", message(1, 1), "Maya", System.currentTimeMillis(), true, policy);
                check(store.load(SCOPE, policy).messages.isEmpty() && !context.getDatabasePath(HistoryFiles.CURRENT).exists(), "unconfigured access does not create a database");
                check(!hasKey(alias), "unconfigured access does not generate a history key");
                File legacy = context.getDatabasePath(HistoryFiles.LEGACY);
                legacy.getParentFile().mkdirs();
                byte[] original = "existing prototype bytes".getBytes(StandardCharsets.UTF_8);
                Files.write(legacy.toPath(), original);
                store.load(SCOPE, policy);
                store.maintain(policy);
                check(Arrays.equals(original, Files.readAllBytes(legacy.toPath())), "unconfigured history leaves old files unopened");
                Files.delete(legacy.toPath());
                for (int days : new int[]{-1, 0, 1, 7, 30, 90})
                {
                    policy = HistoryPolicy.save(context, days, false);
                    check(HistoryPolicy.load(context).retentionDays == days && policy.enabled() == (days >= 0), "explicit history duration " + days);
                }
                boolean rejected = false;
                try
                {
                    HistoryPolicy.save(context, 2, false);
                }
                catch (IllegalArgumentException expected)
                {
                    rejected = true;
                }
                check(rejected && HistoryPolicy.load(context).retentionDays == 90, "unsupported duration cannot replace consent");
                policy = HistoryPolicy.save(context, 7, true);
                check(policy.configured && !policy.enabled() && HistoryPolicy.load(context).erasePending, "pending erasure denies disk access durably");
                context.getSharedPreferences(HistoryPolicy.PREFERENCES, Context.MODE_PRIVATE).edit().putString("retention_days", "7").commit();
                check(!HistoryPolicy.load(context).configured && HistoryPolicy.load(context).erasePending, "invalid stored type never grants consent");

                Identity expectedIdentity = new Identity("s".repeat(199), 2048);
                identity.save(expectedIdentity);
                identity.saveServerKeys("saved server keys");
                byte[] plain = "private archive data".getBytes(StandardCharsets.UTF_8);
                String first = cipher.seal(plain, "fixture");
                String second = cipher.seal(plain, "fixture");
                check(!first.equals(second) && Arrays.equals(plain, cipher.open(first, "fixture")), "each history encryption gets a fresh IV");
                cipher.revoke();
                check(!hasKey(alias), "deletion revokes the separate history key");
                rejected = false;
                try
                {
                    cipher.open(first, "fixture");
                }
                catch (Exception expected)
                {
                    rejected = true;
                }
                check(rejected && !hasKey(alias), "reading erased history cannot silently create a new key");
                cipher.seal(plain, "fixture");
                rejected = false;
                try
                {
                    cipher.open(first, "fixture");
                }
                catch (Exception expected)
                {
                    rejected = true;
                }
                check(rejected, "replacement history key cannot open old ciphertext");
                check(identity.loadExisting().passphrase.equals(expectedIdentity.passphrase) && identity.loadExistingServerKeys().equals("saved server keys"), "history key deletion preserves identity and server keys");
            }
            finally
            {
                store.close();
                cipher.revoke();
            }
        }
    }

    private void expiry(Context base) throws Exception
    {
        try (FixtureContext context = new FixtureContext(base))
        {
            ArchiveVault cipher = new ArchiveVault("lemon-native-history-test-" + UUID.randomUUID());
            try (ConversationArchive archive = new ConversationArchive(context, cipher))
            {
                long now = System.currentTimeMillis();
                long old = now - 2 * 86400000L;
                HistoryPolicy policy = HistoryPolicy.save(context, 1, false);
                Attachment attachment = new Attachment("file", "test.bin", "application/octet-stream", new byte[]{1, 2, 3}, false);
                archive.save(SCOPE, "old", message(1, Long.MAX_VALUE).withAttachment(attachment), "Maya", old);
                archive.save(SCOPE, "recent", message(2, 1), "Maya", now);
                archive.save(SCOPE, "old", message(1, Long.MAX_VALUE).withText("edited").withAttachment(attachment), "Maya", now);
                check(scalar(archive.getReadableDatabase(), "SELECT received_at FROM messages WHERE record_id='old'") == old, "editing cannot extend the first receipt time");
                long next = archive.expire(policy, now);
                ConversationArchive.Loaded loaded = archive.load(SCOPE);
                check(loaded.messages.size() == 1 && loaded.messages.get(0).timestamp == 1, "expiry follows local receipt instead of remote timestamp");
                check(scalar(archive.getReadableDatabase(), "SELECT COUNT(*) FROM attachment_chunks") == 0, "expiry removes attachment chunks");
                check(next >= 1000 && next <= 3600000, "maintenance delay is bounded");
                archive.save(SCOPE, "old", message(1, now), "Maya", now, false);
                check(archive.load(SCOPE).messages.size() == 1, "a late update cannot recreate an expired record");
                archive.save(OTHER_SCOPE, "other", message(3, now), "Sam", old);
                archive.expire(policy, now);
                check(archive.load(OTHER_SCOPE).messages.isEmpty(), "expiry applies to every server and identity scope");
                archive.save(SCOPE, "keep", message(4, now), "Maya", old);
                archive.expire(HistoryPolicy.save(context, 0, false), now);
                check(archive.load(SCOPE).messages.size() == 2, "until deleted keeps old local receipt times");
                check(archive.load(SCOPE).recordIds.size() == 2, "loaded records retain stable merge identifiers");
            }
            finally
            {
                cipher.revoke();
            }
        }
    }

    private void migration(Context base, boolean damaged) throws Exception
    {
        try (FixtureContext context = new FixtureContext(base))
        {
            ArchiveVault cipher = new ArchiveVault("lemon-native-history-test-" + UUID.randomUUID());
            IdentityVault identity = new IdentityVault(context);
            HistoryStore store = new HistoryStore(context, identity, cipher);
            try
            {
                createLegacy(context, identity, damaged);
                File oldFile = context.getDatabasePath(HistoryFiles.LEGACY);
                byte[] original = Files.readAllBytes(oldFile.toPath());
                HistoryPolicy policy = HistoryPolicy.save(context, 7, false);
                long started = System.currentTimeMillis();
                if (damaged)
                {
                    boolean refused = false;
                    try
                    {
                        store.load(SCOPE, policy);
                    }
                    catch (Exception expected)
                    {
                        refused = true;
                    }
                    check(refused && Arrays.equals(original, Files.readAllBytes(oldFile.toPath())), "failed migration preserves the old database");
                    try (ConversationArchive current = new ConversationArchive(context, cipher))
                    {
                        check(current.load(SCOPE).messages.isEmpty() && !current.legacyImported(), "failed migration rolls back earlier rows and completion marker");
                    }
                    return;
                }
                try (RandomAccessFile retained = new RandomAccessFile(oldFile, "r"))
                {
                    ConversationArchive.Loaded loaded = store.load(SCOPE, policy);
                    check(loaded.messages.size() == 1 && loaded.messages.get(0).text.equals("message 1"), "migration preserves private message text");
                    check(Arrays.equals(loaded.messages.get(0).attachment.data(), new byte[]{5, 6, 7}), "migration preserves encrypted attachment contents");
                    check(store.load(OTHER_SCOPE, policy).messages.size() == 1, "migration includes other saved server scopes");
                    check(!oldFile.exists(), "committed migration removes the accessible old database");
                    byte[] overwritten = new byte[original.length];
                    retained.readFully(overwritten);
                    check(!Arrays.equals(original, overwritten), "migration overwrites the old database before unlinking it");
                }
                try (ConversationArchive current = new ConversationArchive(context, cipher))
                {
                    check(current.legacyImported(), "migration completion is committed with the records");
                    check(scalar(current.getReadableDatabase(), "SELECT MIN(received_at) FROM messages") >= started, "legacy retention starts at local migration time");
                }
                store.close();
                Files.write(oldFile.toPath(), original);
                check(store.load(SCOPE, policy).messages.size() == 1 && !oldFile.exists(), "retry only erases an already imported legacy file without duplicating messages");
            }
            finally
            {
                store.close();
                cipher.revoke();
            }
        }
    }

    private void missingKey(Context base) throws Exception
    {
        try (FixtureContext context = new FixtureContext(base))
        {
            String alias = "lemon-native-history-test-" + UUID.randomUUID();
            ArchiveVault cipher = new ArchiveVault(alias);
            HistoryStore store = new HistoryStore(context, new IdentityVault(context), cipher);
            try
            {
                HistoryPolicy policy = HistoryPolicy.save(context, 0, false);
                store.save(SCOPE, "original", message(1, 1), "Maya", System.currentTimeMillis(), true, policy);
                cipher.revoke();
                boolean rejected = false;
                try
                {
                    store.save(SCOPE, "replacement", message(2, 2), "Maya", System.currentTimeMillis(), true, policy);
                }
                catch (Exception expected)
                {
                    rejected = true;
                }
                check(rejected && !hasKey(alias), "populated history refuses a missing key without creating a replacement");
                store.close();
                try (SQLiteDatabase database = SQLiteDatabase.openDatabase(context.getDatabasePath(HistoryFiles.CURRENT).getPath(), null, SQLiteDatabase.OPEN_READONLY))
                {
                    check(scalar(database, "SELECT COUNT(*) FROM messages") == 1, "missing key failure preserves the existing archive rows");
                }
                store.erase();
                store.save(SCOPE, "after-clear", message(3, 3), "Maya", System.currentTimeMillis(), true, policy);
                check(hasKey(alias) && store.load(SCOPE, policy).messages.size() == 1, "explicit clearing permits a new independent history key");
            }
            finally
            {
                store.close();
                cipher.revoke();
            }
        }
    }

    private static void createLegacy(Context context, IdentityVault identity, boolean damaged) throws Exception
    {
        File file = context.getDatabasePath(HistoryFiles.LEGACY);
        file.getParentFile().mkdirs();
        try (SQLiteDatabase database = SQLiteDatabase.openOrCreateDatabase(file, null))
        {
            database.execSQL("CREATE TABLE messages (sequence INTEGER PRIMARY KEY AUTOINCREMENT, scope TEXT NOT NULL, record_id TEXT NOT NULL, cipher TEXT NOT NULL, bytes INTEGER NOT NULL, UNIQUE(scope,record_id))");
            database.execSQL("CREATE TABLE attachment_chunks (message_sequence INTEGER NOT NULL, position INTEGER NOT NULL, cipher BLOB NOT NULL, bytes INTEGER NOT NULL, PRIMARY KEY(message_sequence,position), FOREIGN KEY(message_sequence) REFERENCES messages(sequence) ON DELETE CASCADE)");
            database.setVersion(2);
            for (int index = 1; index <= 2; index++)
            {
                String scope = index == 1 ? SCOPE : OTHER_SCOPE;
                String record = "legacy-" + index;
                String version = UUID.randomUUID().toString();
                JSONObject data = new JSONObject().put("conversation", CONVERSATION).put("sender", "Maya").put("text", "message " + index).put("outgoing", false).put("timestamp", 1).put("delivery", "Received").put("author", "").put("title", "Maya");
                if (index == 1)
                {
                    data.put("attachment", new JSONObject().put("version", version).put("id", "file").put("name", "test.bin").put("mime", "application/octet-stream").put("size", 3).put("picture", false));
                }
                String cipher = identity.seal(data.toString().getBytes(StandardCharsets.UTF_8), "message:" + scope + ":" + record);
                ContentValues row = new ContentValues();
                row.put("scope", scope);
                row.put("record_id", record);
                row.put("cipher", damaged && index == 2 ? "damaged" : cipher);
                row.put("bytes", cipher.length());
                long sequence = database.insertOrThrow("messages", null, row);
                if (index == 1)
                {
                    byte[] chunk = identity.sealBytes(new byte[]{5, 6, 7}, "attachment:" + scope + ":" + record + ":" + version + ":0:3");
                    ContentValues part = new ContentValues();
                    part.put("message_sequence", sequence);
                    part.put("position", 0);
                    part.put("cipher", chunk);
                    part.put("bytes", chunk.length);
                    database.insertOrThrow("attachment_chunks", null, part);
                }
            }
        }
    }

    private void overwrite(Context base) throws Exception
    {
        try (FixtureContext context = new FixtureContext(base))
        {
            List<RandomAccessFile> retained = new ArrayList<>();
            List<File> files = new ArrayList<>();
            byte[] original = new byte[140000];
            Arrays.fill(original, (byte)0x5a);
            try
            {
                for (String name : new String[]{HistoryFiles.CURRENT, HistoryFiles.LEGACY})
                {
                    for (String suffix : new String[]{"", "-wal", "-shm", "-journal"})
                    {
                        File file = context.getDatabasePath(name + suffix);
                        file.getParentFile().mkdirs();
                        Files.write(file.toPath(), original);
                        files.add(file);
                        retained.add(new RandomAccessFile(file, "r"));
                    }
                }
                new HistoryFiles(context).erase(true);
                for (int index = 0; index < files.size(); index++)
                {
                    byte[] changed = new byte[original.length];
                    retained.get(index).readFully(changed);
                    boolean startChanged = !Arrays.equals(Arrays.copyOfRange(original, 0, 65536), Arrays.copyOfRange(changed, 0, 65536));
                    boolean endChanged = !Arrays.equals(Arrays.copyOfRange(original, 131072, original.length), Arrays.copyOfRange(changed, 131072, changed.length));
                    check(!files.get(index).exists() && startChanged && endChanged, "whole file overwrite and removal " + files.get(index).getName());
                }
            }
            finally
            {
                for (RandomAccessFile file : retained)
                {
                    file.close();
                }
            }
            File outside = new File(context.getCacheDir(), "unrelated.bin");
            Files.write(outside.toPath(), original);
            File link = context.getDatabasePath(HistoryFiles.CURRENT);
            android.system.Os.symlink(outside.getAbsolutePath(), link.getAbsolutePath());
            boolean refused = false;
            try
            {
                new HistoryFiles(context).erase(true);
            }
            catch (IOException expected)
            {
                refused = true;
            }
            check(refused && Arrays.equals(original, Files.readAllBytes(outside.toPath())), "erasure refuses a symlink and preserves its unrelated target");
            Files.delete(link.toPath());
            new HistoryFiles(context).erase(true);
            check(!link.exists(), "erasure can safely retry after a rejected path is removed");
        }
    }

    private void maintenance(Context base) throws Exception
    {
        HistoryPolicy original = HistoryPolicy.load(base);
        try (FixtureContext context = new FixtureContext(base))
        {
            JobScheduler scheduler = base.getSystemService(JobScheduler.class);
            int id = 0x4c4849;
            HistoryMaintenanceJob.schedule(base, HistoryPolicy.load(context));
            check(scheduler.getPendingJob(id) == null, "unconfigured policy schedules no maintenance");
            HistoryMaintenanceJob.schedule(base, HistoryPolicy.save(context, 0, false));
            check(scheduler.getPendingJob(id) == null, "until deleted requires no expiry job");
            HistoryMaintenanceJob.schedule(base, HistoryPolicy.save(context, 7, false));
            JobInfo job = scheduler.getPendingJob(id);
            check(job != null && job.isPeriodic() && job.isPersisted() && job.getNetworkType() == JobInfo.NETWORK_TYPE_NONE, "finite retention schedules persisted maintenance without a network requirement");
            android.content.pm.ServiceInfo info = base.getPackageManager().getServiceInfo(job.getService(), 0);
            check(!info.exported && "android.permission.BIND_JOB_SERVICE".equals(info.permission), "installed maintenance service requires the system job permission");
            HistoryMaintenanceJob.schedule(base, HistoryPolicy.save(context, 7, true));
            check(scheduler.getPendingJob(id) == null, "pending erasure cancels expiry maintenance");
        }
        finally
        {
            HistoryMaintenanceJob.schedule(base, original);
        }
    }

    private static ChatMessage message(long id, long timestamp)
    {
        return new ChatMessage(id, CONVERSATION, 2, "Maya", "message " + id, false, timestamp, "Received", "", -1);
    }

    private static boolean hasKey(String alias) throws Exception
    {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        return store.containsAlias(alias);
    }

    private static long scalar(SQLiteDatabase database, String sql)
    {
        try (Cursor rows = database.rawQuery(sql, null))
        {
            rows.moveToFirst();
            return rows.getLong(0);
        }
    }

    private void check(boolean value, String message)
    {
        if (!value)
        {
            throw new AssertionError(message);
        }
        checks++;
    }

    public static final class FixtureContext extends ContextWrapper implements AutoCloseable
    {
        private final String prefix = "history-check-" + UUID.randomUUID();
        private final File directory;
        private final List<String> preferences = new ArrayList<>();

        public FixtureContext(Context base)
        {
            super(base);
            directory = new File(base.getCacheDir(), prefix);
            directory.mkdirs();
            new File(directory, "databases").mkdirs();
        }

        @Override
        public Context getApplicationContext()
        {
            return this;
        }

        @Override
        public File getDataDir()
        {
            return directory;
        }

        @Override
        public File getDatabasePath(String name)
        {
            return new File(new File(directory, "databases"), name);
        }

        @Override
        public File getCacheDir()
        {
            File cache = new File(directory, "cache");
            cache.mkdirs();
            return cache;
        }

        @Override
        public SharedPreferences getSharedPreferences(String name, int mode)
        {
            String actual = prefix + "-" + name;
            if (!preferences.contains(actual))
            {
                preferences.add(actual);
            }
            return getBaseContext().getSharedPreferences(actual, mode);
        }

        @Override
        public Object getSystemService(String name)
        {
            if (Context.JOB_SCHEDULER_SERVICE.equals(name) || Context.CONNECTIVITY_SERVICE.equals(name))
            {
                return null;
            }
            return super.getSystemService(name);
        }

        @Override
        public SQLiteDatabase openOrCreateDatabase(String name, int mode, SQLiteDatabase.CursorFactory factory)
        {
            return openOrCreateDatabase(name, mode, factory, null);
        }

        @Override
        public SQLiteDatabase openOrCreateDatabase(String name, int mode, SQLiteDatabase.CursorFactory factory, DatabaseErrorHandler handler)
        {
            File file = getDatabasePath(name);
            file.getParentFile().mkdirs();
            return SQLiteDatabase.openOrCreateDatabase(file.getPath(), factory, handler);
        }

        @Override
        public boolean deleteDatabase(String name)
        {
            return SQLiteDatabase.deleteDatabase(getDatabasePath(name));
        }

        @Override
        public void close() throws IOException
        {
            for (String name : preferences)
            {
                getBaseContext().deleteSharedPreferences(name);
            }
            try (java.util.stream.Stream<java.nio.file.Path> paths = Files.walk(directory.toPath()))
            {
                for (java.nio.file.Path path : (Iterable<java.nio.file.Path>)paths.sorted(java.util.Comparator.reverseOrder())::iterator)
                {
                    Files.deleteIfExists(path);
                }
            }
        }
    }
}
