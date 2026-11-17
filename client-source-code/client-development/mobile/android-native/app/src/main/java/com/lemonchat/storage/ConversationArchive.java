package com.lemonchat.storage;

import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteOpenHelper;
import com.lemonchat.crypto.Identity;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.model.Attachment;
import com.lemonchat.model.MessageBudget;
import com.lemonchat.model.Reaction;
import com.lemonchat.model.ReplyQuote;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import org.json.JSONArray;
import org.json.JSONObject;

/** keeps recent private conversations encrypted on the storage worker */
public final class ConversationArchive extends SQLiteOpenHelper
{
    public static final int MAX_MESSAGES = MessageBudget.MAX_MESSAGES;
    public static final int MAX_TEXT_BYTES = MessageBudget.MAX_METADATA_BYTES;
    public static final int MAX_ATTACHMENT_BYTES = MessageBudget.MAX_ATTACHMENT_BYTES;
    private static final int MAX_STORED_MESSAGES = 2000;
    private static final int MAX_STORED_BYTES = 8 * 1024 * 1024;
    private static final int MAX_STORED_ATTACHMENT_BYTES = 64 * 1024 * 1024;
    private static final int MAX_RECORD_BYTES = 768 * 1024;
    private static final int CHUNK_BYTES = 256 * 1024;
    private final ArchiveVault vault;

    public static final class Loaded
    {
        public final List<ChatMessage> messages = new ArrayList<>();
        public final Map<String, String> titles = new LinkedHashMap<>();
        public final Map<Long, String> recordIds = new LinkedHashMap<>();
    }

    public ConversationArchive(Context context, ArchiveVault vault)
    {
        this(context, vault, HistoryFiles.CURRENT);
    }

    ConversationArchive(Context context, ArchiveVault vault, String databaseName)
    {
        this(context, vault, databaseName, 3);
    }

    private ConversationArchive(Context context, ArchiveVault vault, String databaseName, int version)
    {
        super(context, databaseName, null, version);
        this.vault = vault;
    }

    static ConversationArchive legacy(Context context, IdentityVault vault)
    {
        return new ConversationArchive(context, ArchiveVault.legacy(vault), HistoryFiles.LEGACY, 2);
    }

    @Override
    public void onConfigure(SQLiteDatabase database)
    {
        database.setForeignKeyConstraintsEnabled(true);
        database.setMaximumSize(96 * 1024 * 1024);
    }

    @Override
    public void onCreate(SQLiteDatabase database)
    {
        database.execSQL("CREATE TABLE messages (sequence INTEGER PRIMARY KEY AUTOINCREMENT, scope TEXT NOT NULL, record_id TEXT NOT NULL, cipher TEXT NOT NULL, bytes INTEGER NOT NULL, received_at INTEGER NOT NULL, UNIQUE(scope, record_id))");
        database.execSQL("CREATE INDEX messages_scope ON messages(scope, sequence)");
        database.execSQL("CREATE INDEX messages_received ON messages(received_at)");
        database.execSQL("CREATE TABLE archive_metadata (name TEXT PRIMARY KEY, value TEXT NOT NULL)");
        createAttachments(database);
    }

    @Override
    public void onUpgrade(SQLiteDatabase database, int oldVersion, int newVersion)
    {
        if (oldVersion == 1)
        {
            createAttachments(database);
            oldVersion = 2;
        }
        if (oldVersion == 2 && newVersion == 3)
        {
            database.execSQL("ALTER TABLE messages ADD COLUMN received_at INTEGER NOT NULL DEFAULT 0");
            database.execSQL("CREATE INDEX messages_received ON messages(received_at)");
            database.execSQL("CREATE TABLE archive_metadata (name TEXT PRIMARY KEY, value TEXT NOT NULL)");
            oldVersion = 3;
        }
        if (oldVersion != newVersion)
        {
            throw new IllegalStateException("Unsupported conversation archive version");
        }
    }

    private static void createAttachments(SQLiteDatabase database)
    {
        database.execSQL("CREATE TABLE attachment_chunks (message_sequence INTEGER NOT NULL, position INTEGER NOT NULL, cipher BLOB NOT NULL, bytes INTEGER NOT NULL, PRIMARY KEY(message_sequence, position), FOREIGN KEY(message_sequence) REFERENCES messages(sequence) ON DELETE CASCADE)");
    }

    /**
     * separates saved conversations by server and local identity
     *
     * @param endpoint server address used for the connection
     * @param identity local identity
     * @return storage scope without the identity secret
     * @throws Exception when SHA256 is unavailable
     */
    public static String scope(String endpoint, Identity identity) throws Exception
    {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        byte[] seed = identity.seedDigest();
        try
        {
            digest.update(seed);
            digest.update((byte)(identity.rsaKeyBits >>> 8));
            digest.update((byte)identity.rsaKeyBits);
            digest.update(endpoint.trim().getBytes(StandardCharsets.UTF_8));
            StringBuilder value = new StringBuilder(64);
            for (byte item : digest.digest())
            {
                value.append(Character.forDigit((item >>> 4) & 15, 16));
                value.append(Character.forDigit(item & 15, 16));
            }
            return value.toString();
        }
        finally
        {
            Arrays.fill(seed, (byte)0);
        }
    }

    /**
     * restores the newest messages within the memory limits
     *
     * @param scope server and identity scope
     * @return chronological messages and saved conversation names
     * @throws Exception when stored content cannot be authenticated
     */
    public Loaded load(String scope) throws Exception
    {
        validateScope(scope);
        Loaded result = new Loaded();
        long metadataBytes = 0;
        long attachmentBytes = 0;
        try (Cursor rows = getReadableDatabase().query("messages", new String[]{"sequence", "record_id", "cipher"}, "scope = ?", new String[]{scope}, null, null, "sequence DESC", Integer.toString(MAX_MESSAGES)))
        {
            while (rows.moveToNext())
            {
                long sequence = rows.getLong(0);
                String recordId = rows.getString(1);
                JSONObject data = openMetadata(rows.getString(2), purpose(scope, recordId));
                ChatMessage message = readMessage(data, sequence);
                long messageBytes = MessageBudget.metadataBytes(message);
                JSONObject file = data.optJSONObject("attachment");
                long fileBytes = file == null ? 0 : attachmentSize(file);
                if (metadataBytes + messageBytes > MAX_TEXT_BYTES || attachmentBytes + fileBytes > MAX_ATTACHMENT_BYTES)
                {
                    break;
                }
                if (file != null)
                {
                    message = message.withAttachment(readAttachment(scope, recordId, sequence, file));
                    messageBytes = MessageBudget.metadataBytes(message);
                    if (metadataBytes + messageBytes > MAX_TEXT_BYTES)
                    {
                        break;
                    }
                }
                metadataBytes += messageBytes;
                attachmentBytes += fileBytes;
                result.messages.add(message);
                result.recordIds.put(message.id, recordId);
                if (!result.titles.containsKey(message.conversationId))
                {
                    result.titles.put(message.conversationId, text(data, "title", 512, "Saved chat"));
                }
            }
        }
        Collections.reverse(result.messages);
        return result;
    }

    /**
     * commits one private message before it is shown as saved
     *
     * @param scope server and identity scope
     * @param recordId stable identifier for message updates
     * @param message message or delivery update
     * @param title other person's display name
     * @throws Exception when encryption or persistence fails
     */
    public void save(String scope, String recordId, ChatMessage message, String title) throws Exception
    {
        save(scope, recordId, message, title, System.currentTimeMillis());
    }

    /**
     * keeps the first local receipt time when an existing message changes
     *
     * @param scope server and identity scope
     * @param recordId stable identifier for message updates
     * @param message message or delivery update
     * @param title other person's display name
     * @param receivedAt local receipt time in milliseconds
     * @throws Exception when encryption or persistence fails
     */
    public void save(String scope, String recordId, ChatMessage message, String title, long receivedAt) throws Exception
    {
        save(scope, recordId, message, title, receivedAt, true);
    }

    void save(String scope, String recordId, ChatMessage message, String title, long receivedAt, boolean allowInsert) throws Exception
    {
        validateScope(scope);
        if (recordId == null || recordId.isEmpty() || recordId.length() > 256 || !message.conversationId.matches("user:[0-9a-f]{64}") || message.text.length() > 65536 || MessageBudget.metadataBytes(message) > 512 * 1024)
        {
            throw new IllegalArgumentException("Invalid private message for storage");
        }
        SQLiteDatabase database = getWritableDatabase();
        try (Cursor rows = database.rawQuery("SELECT 1 FROM messages LIMIT 1", null))
        {
            if (rows.moveToFirst())
            {
                vault.requireExistingKey();
            }
        }
        String version = UUID.randomUUID().toString();
        JSONObject data = writeMessage(message, title, version);
        byte[] bytes = data.toString().getBytes(StandardCharsets.UTF_8);
        String encrypted;
        try
        {
            if (bytes.length > MAX_RECORD_BYTES)
            {
                throw new IOException("The message metadata is too large to save");
            }
            encrypted = vault.seal(bytes, purpose(scope, recordId));
        }
        finally
        {
            Arrays.fill(bytes, (byte)0);
        }
        database.beginTransaction();
        try
        {
            ContentValues values = new ContentValues();
            values.put("cipher", encrypted);
            values.put("bytes", encrypted.length());
            int updated = database.update("messages", values, "scope = ? AND record_id = ?", new String[]{scope, recordId});
            if (updated == 0)
            {
                if (!allowInsert)
                {
                    database.setTransactionSuccessful();
                    return;
                }
                values.put("scope", scope);
                values.put("record_id", recordId);
                values.put("received_at", Math.max(1, receivedAt));
                database.insertOrThrow("messages", null, values);
            }
            long sequence;
            try (Cursor row = database.query("messages", new String[]{"sequence"}, "scope = ? AND record_id = ?", new String[]{scope, recordId}, null, null, null))
            {
                if (!row.moveToFirst())
                {
                    throw new IOException("The message could not be saved");
                }
                sequence = row.getLong(0);
            }
            database.delete("attachment_chunks", "message_sequence = ?", new String[]{Long.toString(sequence)});
            if (message.attachment != null)
            {
                writeAttachment(database, scope, recordId, sequence, version, message.attachment);
            }
            prune(database);
            database.setTransactionSuccessful();
        }
        finally
        {
            database.endTransaction();
        }
    }

    /**
     * @brief deletes one saved message. Its attachment chunks are deleted with it, because the attachment table
     *        refers to the message row with ON DELETE CASCADE.
     *
     * @param scope server and identity scope
     * @param recordId identifier the message was saved with
     */
    public void delete(String scope, String recordId)
    {
        // check the scope
        validateScope(scope);

        // delete the row
        getWritableDatabase().delete("messages", "scope = ? AND record_id = ?", new String[]{scope, recordId});
    }

    boolean legacyImported()
    {
        try (Cursor rows = getReadableDatabase().query("archive_metadata", new String[]{"value"}, "name = ?", new String[]{"legacy_imported"}, null, null, null))
        {
            return rows.moveToFirst() && "1".equals(rows.getString(0));
        }
    }

    /**
     * copies the old archive atomically after the user enables saving
     *
     * @param legacy old archive opened with its original cipher
     * @param receivedAt local migration time for records without a receipt time
     * @throws Exception when any old record cannot be recovered
     */
    void migrate(ConversationArchive legacy, long receivedAt) throws Exception
    {
        SQLiteDatabase database = getWritableDatabase();
        database.beginTransaction();
        try
        {
            try (Cursor rows = legacy.getReadableDatabase().query("messages", new String[]{"sequence", "scope", "record_id", "cipher"}, null, null, null, null, "sequence ASC"))
            {
                while (rows.moveToNext())
                {
                    long sequence = rows.getLong(0);
                    String scope = rows.getString(1);
                    String recordId = rows.getString(2);
                    JSONObject data = legacy.openMetadata(rows.getString(3), purpose(scope, recordId));
                    ChatMessage message = readMessage(data, sequence);
                    JSONObject file = data.optJSONObject("attachment");
                    if (file != null)
                    {
                        message = message.withAttachment(legacy.readAttachment(scope, recordId, sequence, file));
                    }
                    save(scope, recordId, message, text(data, "title", 512, "Saved chat"), receivedAt);
                }
            }
            ContentValues complete = new ContentValues();
            complete.put("name", "legacy_imported");
            complete.put("value", "1");
            if (database.insertWithOnConflict("archive_metadata", null, complete, SQLiteDatabase.CONFLICT_REPLACE) < 0)
            {
                throw new IOException("The history migration could not be committed");
            }
            database.setTransactionSuccessful();
        }
        finally
        {
            database.endTransaction();
        }
    }

    /**
     * removes expired records and their attachment chunks
     *
     * @param policy current history policy
     * @param now local wall clock in milliseconds
     * @return delay until the next bounded maintenance check
     */
    public long expire(HistoryPolicy policy, long now)
    {
        long maximumDelay = 3600000;
        if (!policy.enabled() || policy.retentionDays == 0)
        {
            return maximumDelay;
        }
        SQLiteDatabase database = getWritableDatabase();
        database.delete("messages", "received_at <= ?", new String[]{Long.toString(policy.cutoff(now))});
        try (Cursor row = database.rawQuery("SELECT MIN(received_at) FROM messages", null))
        {
            if (row.moveToFirst() && !row.isNull(0))
            {
                long deadline = row.getLong(0) + policy.retentionDays * 86400000L;
                return Math.max(1000, Math.min(maximumDelay, deadline - now));
            }
        }
        return maximumDelay;
    }

    private static String purpose(String scope, String recordId)
    {
        return "message:" + scope + ":" + recordId;
    }

    private static String chunkPurpose(String scope, String recordId, String version, int position, long size)
    {
        return "attachment:" + scope + ":" + recordId + ":" + version + ":" + position + ":" + size;
    }

    private JSONObject openMetadata(String cipher, String purpose) throws Exception
    {
        if (cipher.length() > MAX_RECORD_BYTES * 4 / 3 + 128)
        {
            throw new IOException("The saved message metadata is too large");
        }
        byte[] bytes = vault.open(cipher, purpose);
        try
        {
            if (bytes.length > MAX_RECORD_BYTES)
            {
                throw new IOException("The saved message metadata is too large");
            }
            return new JSONObject(new String(bytes, StandardCharsets.UTF_8));
        }
        finally
        {
            Arrays.fill(bytes, (byte)0);
        }
    }

    private static JSONObject writeMessage(ChatMessage message, String title, String version) throws Exception
    {
        JSONObject data = new JSONObject();
        data.put("conversation", message.conversationId);
        data.put("sender", message.senderName);
        data.put("text", message.text);
        data.put("outgoing", message.outgoing);
        data.put("timestamp", message.timestamp);
        data.put("delivery", message.delivery);
        data.put("author", message.authorPublicKey);
        data.put("title", title);
        data.put("reaction_id", message.reactionId);
        data.put("seen", message.seen);
        data.put("font", message.fontFamily);
        data.put("font_size", message.fontSize);
        data.put("font_color", message.fontColor);
        if (message.reply != null)
        {
            JSONObject reply = new JSONObject();
            reply.put("id", message.reply.messageId);
            reply.put("author", message.reply.authorPublicKey);
            reply.put("name", message.reply.authorName);
            reply.put("text", message.reply.text);
            data.put("reply", reply);
        }
        JSONArray reactions = new JSONArray();
        for (Reaction reaction : message.reactions)
        {
            JSONObject value = new JSONObject();
            value.put("id", reaction.emojiId);
            value.put("name", reaction.emojiName);
            value.put("mine", reaction.mine);
            value.put("authors", new JSONArray(reaction.authorKeys));
            reactions.put(value);
        }
        data.put("reactions", reactions);
        if (message.attachment != null)
        {
            Attachment attachment = message.attachment;
            JSONObject file = new JSONObject();
            file.put("version", version);
            file.put("id", attachment.id);
            file.put("name", attachment.name);
            file.put("mime", attachment.mime);
            file.put("size", attachment.size);
            file.put("picture", attachment.picture);
            data.put("attachment", file);
            attachmentSize(file);
            text(file, "id", 256, "");
            text(file, "name", 1024, "attachment");
            text(file, "mime", 256, "application/octet-stream");
        }
        text(data, "title", 512, "Saved chat");
        readMessage(data, 1);
        return data;
    }

    private static ChatMessage readMessage(JSONObject data, long sequence) throws Exception
    {
        String conversation = text(data, "conversation", 69, "");
        if (!conversation.matches("user:[0-9a-f]{64}"))
        {
            throw new IOException("The saved conversation is damaged");
        }
        String reactionId = text(data, "reaction_id", 32, "");
        if (!reactionId.isEmpty() && !reactionId.matches("[0-9a-f]{32}"))
        {
            throw new IOException("The saved message identifier is damaged");
        }
        ReplyQuote quote = null;
        JSONObject reply = data.optJSONObject("reply");
        if (reply != null)
        {
            String id = text(reply, "id", 32, "");
            if (!id.matches("[0-9a-f]{32}"))
            {
                throw new IOException("The saved reply is damaged");
            }
            quote = new ReplyQuote(id, text(reply, "author", 16384, ""), text(reply, "name", 128, ""), text(reply, "text", 320, ""));
        }
        List<Reaction> reactions = new ArrayList<>();
        JSONArray items = data.optJSONArray("reactions");
        if (items != null)
        {
            if (items.length() > 32)
            {
                throw new IOException("The saved message has too many reactions");
            }
            for (int index = 0; index < items.length(); index++)
            {
                JSONObject item = items.getJSONObject(index);
                JSONArray authors = item.getJSONArray("authors");
                if (authors.length() > 128)
                {
                    throw new IOException("The saved reaction has too many participants");
                }
                List<String> keys = new ArrayList<>();
                for (int author = 0; author < authors.length(); author++)
                {
                    String key = authors.getString(author);
                    if (key.length() > 16384)
                    {
                        throw new IOException("The saved reaction is damaged");
                    }
                    keys.add(key);
                }
                reactions.add(new Reaction(text(item, "id", 128, ""), text(item, "name", 128, ""), keys, item.optBoolean("mine")));
            }
        }
        String delivery = text(data, "delivery", 256, "");
        if (delivery.equals("Sending"))
        {
            delivery = "Unconfirmed";
        }
        return new ChatMessage(-sequence, conversation, -1, text(data, "sender", 512, ""), text(data, "text", 65536, ""), data.getBoolean("outgoing"), data.getLong("timestamp"), delivery, text(data, "author", 16384, ""), -1, -1, reactionId, quote, reactions, null, data.optBoolean("seen"), text(data, "font", 100, "Arial"), data.optInt("font_size", 12), text(data, "font_color", 7, "#000000"));
    }

    private void writeAttachment(SQLiteDatabase database, String scope, String recordId, long sequence, String version, Attachment attachment) throws Exception
    {
        byte[] content = attachment.data();
        try
        {
            int count = Math.max(1, (content.length + CHUNK_BYTES - 1) / CHUNK_BYTES);
            for (int position = 0; position < count; position++)
            {
                int start = position * CHUNK_BYTES;
                byte[] part = Arrays.copyOfRange(content, start, Math.min(content.length, start + CHUNK_BYTES));
                byte[] cipher;
                try
                {
                    cipher = vault.sealBytes(part, chunkPurpose(scope, recordId, version, position, content.length));
                }
                finally
                {
                    Arrays.fill(part, (byte)0);
                }
                ContentValues values = new ContentValues();
                values.put("message_sequence", sequence);
                values.put("position", position);
                values.put("cipher", cipher);
                values.put("bytes", cipher.length);
                database.insertOrThrow("attachment_chunks", null, values);
            }
        }
        finally
        {
            Arrays.fill(content, (byte)0);
        }
    }

    private Attachment readAttachment(String scope, String recordId, long sequence, JSONObject file) throws Exception
    {
        int size = attachmentSize(file);
        String version = text(file, "version", 36, "");
        if (!version.matches("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"))
        {
            throw new IOException("The saved attachment is damaged");
        }
        byte[] content = new byte[size];
        int count = Math.max(1, (size + CHUNK_BYTES - 1) / CHUNK_BYTES);
        int position = 0;
        try (Cursor chunks = getReadableDatabase().query("attachment_chunks", new String[]{"position", "cipher"}, "message_sequence = ?", new String[]{Long.toString(sequence)}, null, null, "position ASC"))
        {
            while (chunks.moveToNext())
            {
                if (position >= count || chunks.getInt(0) != position)
                {
                    throw new IOException("The saved attachment is incomplete");
                }
                byte[] cipher = chunks.getBlob(1);
                if (cipher.length > CHUNK_BYTES + 29)
                {
                    throw new IOException("The saved attachment chunk is too large");
                }
                byte[] part = vault.openBytes(cipher, chunkPurpose(scope, recordId, version, position, size));
                try
                {
                    int start = position * CHUNK_BYTES;
                    if (part.length != Math.min(CHUNK_BYTES, size - start))
                    {
                        throw new IOException("The saved attachment is incomplete");
                    }
                    System.arraycopy(part, 0, content, start, part.length);
                }
                finally
                {
                    Arrays.fill(part, (byte)0);
                }
                position++;
            }
            if (position != count)
            {
                throw new IOException("The saved attachment is incomplete");
            }
            return new Attachment(text(file, "id", 256, ""), text(file, "name", 1024, "attachment"), text(file, "mime", 256, "application/octet-stream"), content, file.getBoolean("picture"));
        }
        finally
        {
            Arrays.fill(content, (byte)0);
        }
    }

    private static int attachmentSize(JSONObject file) throws Exception
    {
        long size = file.getLong("size");
        if (size < 0 || size > Attachment.MAX_BYTES)
        {
            throw new IOException("The saved attachment is too large");
        }
        return (int)size;
    }

    private static String text(JSONObject data, String name, int maximum, String fallback) throws IOException
    {
        Object raw = data.opt(name);
        if (raw == null)
        {
            return fallback;
        }
        if (!(raw instanceof String) || ((String)raw).length() > maximum)
        {
            throw new IOException("The saved message is damaged");
        }
        return (String)raw;
    }

    private static void validateScope(String scope)
    {
        if (scope == null || !scope.matches("[0-9a-f]{64}"))
        {
            throw new IllegalArgumentException("Invalid conversation storage scope");
        }
    }

    private static void prune(SQLiteDatabase database)
    {
        long count;
        long bytes;
        long attachmentBytes;
        try (Cursor totals = database.rawQuery("SELECT COUNT(*), COALESCE(SUM(bytes), 0) FROM messages", null))
        {
            totals.moveToFirst();
            count = totals.getLong(0);
            bytes = totals.getLong(1);
        }
        try (Cursor totals = database.rawQuery("SELECT COALESCE(SUM(bytes), 0) FROM attachment_chunks", null))
        {
            totals.moveToFirst();
            attachmentBytes = totals.getLong(0);
        }
        long removeThrough = -1;
        if (count > MAX_STORED_MESSAGES || bytes > MAX_STORED_BYTES || attachmentBytes > MAX_STORED_ATTACHMENT_BYTES)
        {
            try (Cursor oldest = database.rawQuery("SELECT messages.sequence, messages.bytes, COALESCE((SELECT SUM(bytes) FROM attachment_chunks WHERE message_sequence = messages.sequence), 0) FROM messages ORDER BY messages.sequence ASC", null))
            {
                while ((count > MAX_STORED_MESSAGES || bytes > MAX_STORED_BYTES || attachmentBytes > MAX_STORED_ATTACHMENT_BYTES) && oldest.moveToNext())
                {
                    removeThrough = oldest.getLong(0);
                    bytes -= oldest.getLong(1);
                    attachmentBytes -= oldest.getLong(2);
                    count--;
                }
            }
        }
        if (removeThrough >= 0)
        {
            database.delete("messages", "sequence <= ?", new String[]{Long.toString(removeThrough)});
        }
    }
}
