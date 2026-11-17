package com.lemonchat.storage;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.os.Bundle;
import com.lemonchat.crypto.Identity;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.model.Attachment;
import com.lemonchat.model.MessageBudget;
import com.lemonchat.model.Reaction;
import com.lemonchat.model.ReplyQuote;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.UUID;
import org.json.JSONObject;

/** checks the real Keystore and archive without changing the app's saved identity */
public final class StorageInstrumentation extends Instrumentation
{
    private int checks;
    private String endpoint;
    private String suite;

    @Override
    public void onCreate(Bundle arguments)
    {
        super.onCreate(arguments);
        endpoint = arguments != null ? arguments.getString("endpoint") : null;
        suite = arguments != null ? arguments.getString("suite") : null;
        start();
    }

    @Override
    public void onStart()
    {
        Bundle result = new Bundle();
        try
        {
            if ("themes".equals(suite))
            {
                int themeChecks = com.lemonchat.ui.ThemeChecks.verify(this);
                result.putString("stream", "PASS: " + themeChecks + " theme checks\n");
                finish(Activity.RESULT_OK, result);
                return;
            }
            if (endpoint != null)
            {
                int liveChecks = com.lemonchat.service.LiveServiceChecks.verify(this, endpoint);
                result.putString("stream", "PASS: " + liveChecks + " live Android service checks\n");
                finish(Activity.RESULT_OK, result);
                return;
            }
            verifyStorage();
            verifyFeatureStorage();
            int historyChecks = HistoryChecks.verify(getTargetContext());
            int historyPolicyChecks = HistoryPolicyChecks.verify(getTargetContext());
            int historyServiceChecks = com.lemonchat.service.HistoryServiceChecks.verify(this);
            if ("storage".equals(suite))
            {
                result.putString("stream", "PASS: " + checks + " Keystore and archive checks\nPASS: " + historyChecks + " history consent and erasure checks\nPASS: " + historyPolicyChecks + " history policy commit checks\nPASS: " + historyServiceChecks + " history service ordering checks\n");
                finish(Activity.RESULT_OK, result);
                return;
            }
            int iconChecks = com.lemonchat.ui.IconChecks.verify();
            int audioChecks = com.lemonchat.audio.AudioChecks.verify(getTargetContext());
            int settingsChecks = com.lemonchat.settings.SettingsChecks.verify(getTargetContext());
            int chatUiChecks = com.lemonchat.ui.ChatUiChecks.verify(this);
            int musicTitleChecks = com.lemonchat.ui.MusicTitleChecks.verify(this);
            int messageRowChecks = com.lemonchat.ui.MessageRowChecks.verify(this);
            int connectionWizardChecks = com.lemonchat.ui.ConnectionWizardChecks.verify(this);
            int emojiImageChecks = com.lemonchat.ui.EmojiImageChecks.verify(this);
            int backgroundChecks = com.lemonchat.service.BackgroundRuntimeChecks.verify(getTargetContext());
            int callNavigationChecks = com.lemonchat.ui.CallNavigationChecks.verify(getTargetContext());
            int themeChecks = com.lemonchat.ui.ThemeChecks.verify(this);
            result.putString("stream", "PASS: " + checks + " Keystore and archive checks\nPASS: " + historyChecks + " history consent and erasure checks\nPASS: " + historyPolicyChecks + " history policy commit checks\nPASS: " + historyServiceChecks + " history service ordering checks\nPASS: " + iconChecks + " Android image checks\nPASS: " + audioChecks + " Android audio checks\nPASS: " + settingsChecks + " local settings checks\nPASS: " + chatUiChecks + " native chat UI checks\nPASS: " + backgroundChecks + " background runtime checks\nPASS: " + callNavigationChecks + " call task configuration checks\nPASS: " + themeChecks + " theme checks\n");
            result.putString("stream", result.getString("stream") + "PASS: " + musicTitleChecks + " music title checks\n");
            result.putString("stream", result.getString("stream") + "PASS: " + messageRowChecks + " message row checks\n");
            result.putString("stream", result.getString("stream") + "PASS: " + connectionWizardChecks + " connection wizard checks\n");
            result.putString("stream", result.getString("stream") + "PASS: " + emojiImageChecks + " emoji image checks\n");
            finish(Activity.RESULT_OK, result);
        }
        catch (Throwable error)
        {
            result.putString("stream", "FAIL: " + android.util.Log.getStackTraceString(error));
            finish(Activity.RESULT_CANCELED, result);
        }
    }

    private void verifyStorage() throws Exception
    {
        Context context = getTargetContext();
        String name = "archive-test-" + UUID.randomUUID();
        String databaseName = name + ".db";
        IdentityVault vault = new IdentityVault(context, name);
        ArchiveVault archiveVault = new ArchiveVault("lemon-native-history-test-" + UUID.randomUUID());
        ConversationArchive archive = new ConversationArchive(context, archiveVault, databaseName);
        try
        {
            boolean rejected = false;
            try
            {
                vault.loadExisting();
            }
            catch (Exception expected)
            {
                rejected = true;
            }
            check(rejected && !context.getSharedPreferences(name, Context.MODE_PRIVATE).contains("identity"), "background restore cannot create a replacement identity");
            rejected = false;
            try
            {
                vault.loadExistingServerKeys();
            }
            catch (Exception expected)
            {
                rejected = true;
            }
            check(rejected && !context.getSharedPreferences(name, Context.MODE_PRIVATE).contains("server-keys"), "missing saved keys cannot become an empty key list");
            Identity identity = new Identity("a".repeat(199), 3072);
            vault.save(identity);
            Identity restored = vault.load();
            check(restored.passphrase.equals(identity.passphrase) && restored.rsaKeyBits == 3072, "identity round trip");
            restored = vault.loadExisting();
            check(restored.passphrase.equals(identity.passphrase) && restored.rsaKeyBits == 3072, "background restore preserves the exact identity and key size");
            vault.saveServerKeys("");
            check(vault.loadExistingServerKeys().isEmpty(), "an explicitly saved empty key list remains valid");
            vault.saveServerKeys("private server configuration");
            check(vault.loadServerKeys().equals("private server configuration"), "server keys round trip");
            check(vault.loadExistingServerKeys().equals("private server configuration"), "background restore preserves server keys");
            android.content.SharedPreferences saved = context.getSharedPreferences(name, Context.MODE_PRIVATE);
            String encryptedKeys = saved.getString("server-keys", "");
            saved.edit().putString("server-keys", "damaged").commit();
            rejected = false;
            try
            {
                vault.loadExistingServerKeys();
            }
            catch (Exception expected)
            {
                rejected = true;
            }
            check(rejected && saved.getString("server-keys", "").equals("damaged"), "damaged server keys are rejected without replacing them");
            saved.edit().putString("server-keys", encryptedKeys).commit();
            String encryptedIdentity = saved.getString("identity", "");
            saved.edit().putString("identity", "damaged").commit();
            rejected = false;
            try
            {
                vault.loadExisting();
            }
            catch (Exception expected)
            {
                rejected = true;
            }
            check(rejected && saved.getString("identity", "").equals("damaged"), "damaged saved identity is rejected without replacing it");
            saved.edit().putString("identity", encryptedIdentity).commit();
            String sealed = vault.seal("private text".getBytes(StandardCharsets.UTF_8), "original");
            check(new String(vault.open(sealed, "original"), StandardCharsets.UTF_8).equals("private text"), "authenticated record round trip");
            rejected = false;
            try
            {
                vault.open(sealed, "replacement");
            }
            catch (Exception expected)
            {
                rejected = true;
            }
            check(rejected, "record ownership authentication");
            check(!context.getSharedPreferences(name, Context.MODE_PRIVATE).getString("identity", "").contains(identity.passphrase), "identity is encrypted at rest");

            String scope = ConversationArchive.scope("wss://one.example/", identity);
            String otherServer = ConversationArchive.scope("wss://two.example/", identity);
            String otherIdentity = ConversationArchive.scope("wss://one.example/", new Identity("b".repeat(199), 3072));
            String otherSize = ConversationArchive.scope("wss://one.example/", new Identity(identity.passphrase, 2048));
            check(!scope.equals(otherServer) && !scope.equals(otherIdentity) && !scope.equals(otherSize), "server and identity separation");
            String conversation = "user:" + "c".repeat(64);
            ChatMessage message = message(1, conversation, "private message sentinel", "Sending");
            archive.save(scope, "run:1", message, "Maya");
            archive.save(scope, "run:1", message.withDelivery("Delivered", 42), "Maya");
            ConversationArchive.Loaded loaded = archive.load(scope);
            check(loaded.messages.size() == 1, "delivery update does not duplicate a message");
            check(loaded.messages.get(0).text.equals(message.text) && loaded.messages.get(0).delivery.equals("Delivered"), "private text and delivery restore");
            check(loaded.messages.get(0).serverId == -1 && loaded.messages.get(0).id < 0, "old server message ids are retired");
            check(loaded.titles.get(conversation).equals("Maya"), "offline conversation title restore");
            check(archive.load(otherServer).messages.isEmpty() && archive.load(otherIdentity).messages.isEmpty(), "other scopes cannot display this history");
            try (Cursor rows = archive.getReadableDatabase().rawQuery("SELECT cipher FROM messages", null))
            {
                rows.moveToFirst();
                check(!rows.getString(0).contains(message.text) && !rows.getString(0).contains("Maya"), "message and name are encrypted at rest");
            }
            archive.save(scope, "run:2", message(2, conversation, "not confirmed", "Sending"), "Maya");
            check(archive.load(scope).messages.get(1).delivery.equals("Unconfirmed"), "pending send is not falsely marked delivered");
            archive.close();
            archive = new ConversationArchive(context, archiveVault, databaseName);
            check(archive.load(scope).messages.size() == 2, "history survives closing and reopening the database");

            String body = "x".repeat(60000);
            for (int index = 3; index < 123; index++)
            {
                archive.save(scope, "run:" + index, message(index, conversation, body, ""), "Maya");
            }
            try (Cursor totals = archive.getReadableDatabase().rawQuery("SELECT COUNT(*), SUM(bytes) FROM messages", null))
            {
                totals.moveToFirst();
                check(totals.getLong(0) <= 2000 && totals.getLong(1) <= 8 * 1024 * 1024, "archive ciphertext limit");
            }
            loaded = archive.load(scope);
            long textBytes = 0;
            for (ChatMessage item : loaded.messages)
            {
                textBytes += item.text.length() * 2L;
            }
            check(loaded.messages.size() <= 500 && textBytes <= 2 * 1024 * 1024, "restored history memory limits");
            check(loaded.messages.get(loaded.messages.size() - 1).timestamp == 122, "newest messages are retained");
            check(context.getDatabasePath(databaseName).length() <= 96 * 1024 * 1024, "database file size limit");

            archive.getWritableDatabase().execSQL("UPDATE messages SET scope = ? WHERE scope = ?", new Object[]{otherServer, scope});
            rejected = false;
            try
            {
                archive.load(otherServer);
            }
            catch (Exception expected)
            {
                rejected = true;
            }
            check(rejected, "moving ciphertext between owners is rejected");
        }
        finally
        {
            archive.close();
            archiveVault.revoke();
            context.deleteDatabase(databaseName);
            context.deleteSharedPreferences(name);
        }
    }

    private static ChatMessage message(long id, String conversation, String text, String delivery)
    {
        return new ChatMessage(id, conversation, 5, "Maya", text, true, id, delivery, "public key", -1);
    }

    private void verifyFeatureStorage() throws Exception
    {
        Context context = getTargetContext();
        String name = "feature-archive-test-" + UUID.randomUUID();
        String databaseName = name + ".db";
        IdentityVault vault = new IdentityVault(context, name);
        ArchiveVault archiveVault = new ArchiveVault("lemon-native-history-test-" + UUID.randomUUID());
        ConversationArchive archive = new ConversationArchive(context, archiveVault, databaseName);
        try
        {
            String scope = ConversationArchive.scope("wss://feature.example/", new Identity("f".repeat(199), 2048));
            String conversation = "user:" + "d".repeat(64);
            byte[] content = new byte[2 * 1024 * 1024 + 31];
            for (int index = 0; index < content.length; index++)
            {
                content[index] = (byte)(index % 251);
            }
            ReplyQuote reply = new ReplyQuote("a".repeat(32), "original author", "Sam", "quoted private text");
            Reaction reaction = new Reaction("server:" + "c".repeat(32), "wave", Arrays.asList("local key", "peer key"), "local key");
            Attachment attachment = new Attachment("private-file", "private-feature.bin", "application/octet-stream", content, false);
            ChatMessage feature = new ChatMessage(77, conversation, 5, "Maya", "feature private text", true, 77, "Delivered", "public key", 123, 99, "b".repeat(32), reply, Collections.singletonList(reaction), attachment, true, "Georgia", 19, "#123456");
            archive.save(scope, "feature:1", feature, "Maya");
            archive.close();
            archive = new ConversationArchive(context, archiveVault, databaseName);
            ChatMessage restored = archive.load(scope).messages.get(0);
            check(restored.attachment != null && Arrays.equals(restored.attachment.data(), content), "an attachment larger than CursorWindow survives database reopening");
            check(restored.attachment.name.equals("private-feature.bin") && restored.attachment.mime.equals("application/octet-stream") && !restored.attachment.picture, "attachment metadata survives reopening");
            check(restored.reply != null && restored.reply.messageId.equals(reply.messageId) && restored.reply.authorPublicKey.equals(reply.authorPublicKey) && restored.reply.authorName.equals(reply.authorName) && restored.reply.text.equals(reply.text), "encrypted quoted reply restores exactly");
            check(restored.reactions.size() == 1 && restored.reactions.get(0).mine && restored.reactions.get(0).authorKeys.equals(reaction.authorKeys) && restored.reactions.get(0).emojiId.equals(reaction.emojiId), "reaction authors and local selection survive reopening");
            check(restored.seen && restored.fontFamily.equals("Georgia") && restored.fontSize == 19 && restored.fontColor.equals("#123456"), "receipt state and message formatting survive reopening");
            check(restored.connectionId == -1 && restored.serverId == -1 && restored.senderId == -1 && restored.id < 0, "restored features cannot reuse live connection identifiers");
            check(MessageBudget.attachmentBytes(feature) == content.length && MessageBudget.messageBytes(feature) == MessageBudget.metadataBytes(feature) + content.length, "shared queue budget includes attachment bytes and metadata");
            long firstSequence;
            try (Cursor rows = archive.getReadableDatabase().rawQuery("SELECT sequence, cipher FROM messages WHERE record_id = 'feature:1'", null))
            {
                rows.moveToFirst();
                firstSequence = rows.getLong(0);
                String encrypted = rows.getString(1);
                check(!encrypted.contains("private-feature.bin") && !encrypted.contains("quoted private text") && !encrypted.contains("Georgia"), "attachment names and rich metadata stay encrypted at rest");
            }
            byte[] original = chunk(archive, firstSequence, 0);
            byte[] damaged = original.clone();
            damaged[20] ^= 1;
            replaceChunk(archive, firstSequence, 0, damaged);
            check(loadRejected(archive, scope), "tampered attachment ciphertext is rejected before display");
            replaceChunk(archive, firstSequence, 0, original);
            byte[] second = chunk(archive, firstSequence, 1);
            replaceChunk(archive, firstSequence, 0, second);
            check(loadRejected(archive, scope), "attachment chunk reordering is rejected");
            replaceChunk(archive, firstSequence, 0, original);
            archive.save(scope, "feature:2", feature, "Maya");
            long secondSequence;
            try (Cursor rows = archive.getReadableDatabase().rawQuery("SELECT sequence FROM messages WHERE record_id = 'feature:2'", null))
            {
                rows.moveToFirst();
                secondSequence = rows.getLong(0);
            }
            replaceChunk(archive, secondSequence, 0, original);
            check(loadRejected(archive, scope), "attachment chunks cannot move between message records");
            archive.save(scope, "feature:2", feature, "Maya");
            archive.getWritableDatabase().delete("attachment_chunks", "message_sequence = ? AND position = 1", new String[]{Long.toString(secondSequence)});
            check(loadRejected(archive, scope), "a missing authenticated chunk cannot produce a partial attachment");
            archive.save(scope, "feature:2", feature, "Maya");
            archive.getWritableDatabase().execSQL("CREATE TRIGGER fail_payload BEFORE INSERT ON attachment_chunks WHEN NEW.position = 1 BEGIN SELECT RAISE(ABORT, 'test write failure'); END");
            boolean rejected = false;
            try
            {
                archive.save(scope, "feature:1", feature.withText("must not replace the saved message"), "Maya");
            }
            catch (Exception expected)
            {
                rejected = true;
            }
            finally
            {
                archive.getWritableDatabase().execSQL("DROP TRIGGER fail_payload");
            }
            restored = archive.load(scope).messages.get(0);
            check(rejected && restored.text.equals(feature.text) && Arrays.equals(restored.attachment.data(), content), "failed attachment replacement rolls back metadata and payload together");
            byte[] binary = vault.sealBytes(new byte[]{1, 2, 3}, "attachment:test-scope");
            rejected = false;
            try
            {
                vault.openBytes(binary, "attachment:another-scope");
            }
            catch (Exception expected)
            {
                rejected = true;
            }
            check(rejected, "binary attachment authentication binds its storage scope");
            archive.save(scope, "feature:3", feature, "Maya");
            archive.save(scope, "feature:4", feature, "Maya");
            ConversationArchive.Loaded loaded = archive.load(scope);
            long payloadBytes = 0;
            long metadataBytes = 0;
            for (ChatMessage item : loaded.messages)
            {
                payloadBytes += MessageBudget.attachmentBytes(item);
                metadataBytes += MessageBudget.metadataBytes(item);
            }
            check(loaded.messages.size() == 3 && payloadBytes <= MessageBudget.MAX_ATTACHMENT_BYTES && metadataBytes <= MessageBudget.MAX_METADATA_BYTES, "restoration applies separate attachment and metadata budgets");
            archive.getWritableDatabase().execSQL("UPDATE attachment_chunks SET bytes = 67108864 WHERE message_sequence = ? AND position = 0", new Object[]{firstSequence});
            archive.save(scope, "budget-trigger", message(100, conversation, "newest", "Delivered"), "Maya");
            try (Cursor rows = archive.getReadableDatabase().rawQuery("SELECT COUNT(*) FROM messages WHERE sequence = ?", new String[]{Long.toString(firstSequence)}))
            {
                rows.moveToFirst();
                check(rows.getLong(0) == 0, "global payload accounting prunes the oldest message");
            }
            try (Cursor rows = archive.getReadableDatabase().rawQuery("SELECT COUNT(*) FROM attachment_chunks WHERE message_sequence = ?", new String[]{Long.toString(firstSequence)}))
            {
                rows.moveToFirst();
                check(rows.getLong(0) == 0, "pruning a message removes its encrypted payload chunks");
            }
            verifyArchiveUpgrade(context, archiveVault, name + "-legacy.db", scope, conversation);
        }
        finally
        {
            archive.close();
            archiveVault.revoke();
            context.deleteDatabase(databaseName);
            context.deleteSharedPreferences(name);
        }
    }

    private void verifyArchiveUpgrade(Context context, ArchiveVault vault, String databaseName, String scope, String conversation) throws Exception
    {
        ConversationArchive upgraded = null;
        try
        {
            JSONObject data = new JSONObject().put("conversation", conversation).put("sender", "Maya").put("text", "old saved message").put("outgoing", false).put("timestamp", 1).put("delivery", "").put("author", "public key").put("title", "Maya");
            String cipher = vault.seal(data.toString().getBytes(StandardCharsets.UTF_8), "message:" + scope + ":legacy:1");
            try (SQLiteDatabase legacy = context.openOrCreateDatabase(databaseName, Context.MODE_PRIVATE, null))
            {
                legacy.execSQL("CREATE TABLE messages (sequence INTEGER PRIMARY KEY AUTOINCREMENT, scope TEXT NOT NULL, record_id TEXT NOT NULL, cipher TEXT NOT NULL, bytes INTEGER NOT NULL, UNIQUE(scope, record_id))");
                legacy.execSQL("CREATE INDEX messages_scope ON messages(scope, sequence)");
                legacy.execSQL("INSERT INTO messages(scope, record_id, cipher, bytes) VALUES(?, ?, ?, ?)", new Object[]{scope, "legacy:1", cipher, cipher.length()});
                legacy.setVersion(1);
            }
            upgraded = new ConversationArchive(context, vault, databaseName);
            ChatMessage restored = upgraded.load(scope).messages.get(0);
            check(restored.text.equals("old saved message") && restored.reply == null && restored.reactions.isEmpty() && restored.attachment == null && !restored.seen, "archive upgrade preserves original private messages");
            Attachment picture = new Attachment("picture", "image.png", "image/png", new byte[]{1, 2, 3}, true);
            upgraded.save(scope, "new:1", message(2, conversation, "", "Delivered").withAttachment(picture), "Maya");
            check(upgraded.load(scope).messages.get(1).attachment.picture, "upgraded archives can persist new picture attachments");
        }
        finally
        {
            if (upgraded != null)
            {
                upgraded.close();
            }
            context.deleteDatabase(databaseName);
        }
    }

    private static byte[] chunk(ConversationArchive archive, long sequence, int position)
    {
        try (Cursor rows = archive.getReadableDatabase().query("attachment_chunks", new String[]{"cipher"}, "message_sequence = ? AND position = ?", new String[]{Long.toString(sequence), Integer.toString(position)}, null, null, null))
        {
            if (!rows.moveToFirst())
            {
                throw new AssertionError("The test attachment chunk is missing");
            }
            return rows.getBlob(0);
        }
    }

    private static void replaceChunk(ConversationArchive archive, long sequence, int position, byte[] cipher)
    {
        ContentValues values = new ContentValues();
        values.put("cipher", cipher);
        archive.getWritableDatabase().update("attachment_chunks", values, "message_sequence = ? AND position = ?", new String[]{Long.toString(sequence), Integer.toString(position)});
    }

    private static boolean loadRejected(ConversationArchive archive, String scope)
    {
        try
        {
            archive.load(scope);
            return false;
        }
        catch (Exception expected)
        {
            return true;
        }
    }

    private void check(boolean value, String description)
    {
        if (!value)
        {
            throw new AssertionError(description);
        }
        checks++;
    }
}
