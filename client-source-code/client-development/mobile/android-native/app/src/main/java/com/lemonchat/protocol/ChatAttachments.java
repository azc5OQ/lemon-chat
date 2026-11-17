package com.lemonchat.protocol;

import com.lemonchat.crypto.CryptoSession;
import com.lemonchat.model.*;
import org.json.JSONException;
import org.json.JSONObject;
import java.util.ArrayList;
import java.util.Base64;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.UUID;
import static com.lemonchat.protocol.ChatFeatures.*;

/** assembles and authenticates bounded attachment transfers */
final class ChatAttachments
{
    static final int WIRE_LIMIT = 12 * 1024 * 1024;
    private final ChatSession session;
    private final Map<Long, Transfer> incoming = new LinkedHashMap<>();
    private Upload upload;
    private long lastError;

    ChatAttachments(ChatSession session)
    {
        this.session = session;
    }

    void reset()
    {
        incoming.clear();
        upload = null;
    }

    void delivered(ChatMessage message)
    {
        if (upload != null && upload.message.id == message.id && !"Sending".equals(message.delivery))
        {
            upload = null;
        }
    }

    boolean control(String type, JSONObject message) throws JSONException
    {
        if (type.endsWith("_chat_picture_metadata") || type.endsWith("_chat_file_metadata"))
        {
            receiveMetadata(type, message);
            return true;
        }
        if (type.equals("file_receive_chunk"))
        {
            Transfer transfer = incoming.get(message.optLong("server_chat_message_id", -1));
            if (transfer != null)
            {
                String chunk = message.optString("value", "");
                if (number(message, "sender_id") != transfer.sender.id || chunk.isEmpty() || chunk.length() > 65536 || transfer.content.length() + chunk.length() > transfer.expected)
                {
                    discard(transfer, "The attachment transfer was invalid");
                }
                else
                {
                    transfer.content.append(chunk);
                    transfer.deadline = now() + 30000;
                }
            }
            return true;
        }
        if (type.equals("file_receive_completed"))
        {
            complete(message);
            return true;
        }
        if (type.equals("file_send_success"))
        {
            if (upload != null && upload.cursor == upload.wire.length() && !upload.completed && uploadCurrent())
            {
                upload.completed = true;
                JSONObject request = body("file_send_completed");
                put(request, "file_send_intent", upload.intent);
                put(request, "file_send_intent_extra_data", upload.extra);
                session.featureSend(request);
                upload.deadline = now() + 30000;
            }
            return true;
        }
        if (type.equals("file_send_error"))
        {
            if (upload != null)
            {
                String reason = message.optString("reason", "upload failed").replace('_', ' ');
                session.featureEmit(upload.message.withDelivery("Not sent", -1));
                upload = null;
                session.featureError("Attachment was not sent: " + reason);
            }
            return true;
        }
        return false;
    }

    void sendAttachment(SessionSnapshot selected, Member peer, String name, String mime, byte[] data, boolean picture)
    {
        if (upload != null)
        {
            session.featureError("Wait for the current attachment to finish");
            return;
        }
        SessionSnapshot current = session.featureState();
        if (!current.connected || current.idle || selected.connectionId != current.connectionId || (peer == null && (selected.channelId != current.channelId || session.featureKeys() == null)) || (peer != null && !session.featureCurrent(peer)))
        {
            session.featureError("The attachment's conversation changed. Select it again before sending");
            return;
        }
        ClientCapabilities capabilities = current.capabilities;
        if (!(picture ? capabilities.pictureUpload : capabilities.fileUpload))
        {
            session.featureError(picture ? "This server does not allow pictures" : "This server does not allow files");
            return;
        }
        if (data.length > Attachment.MAX_BYTES || data.length > (picture ? capabilities.pictureMaxBytes : capabilities.fileMaxBytes))
        {
            session.featureError("The attachment exceeds the server limit or the native client's 4 MiB limit");
            return;
        }
        try
        {
            String safeName = cleanName(name);
            String safeMime = cleanMime(mime);
            String base64 = Base64.getEncoder().encodeToString(data);
            String payload;
            String header = "";
            if (picture)
            {
                payload = "data:" + safeMime + ";base64," + base64;
                if (!imageUri(payload, Attachment.MAX_BYTES))
                {
                    throw new IllegalArgumentException("Choose a PNG, JPEG, GIF or WebP picture");
                }
            }
            else
            {
                JSONObject file = new JSONObject();
                put(file, "name", safeName);
                put(file, "size", data.length);
                put(file, "mime", safeMime);
                header = file.toString();
                put(file, "data", base64);
                payload = file.toString();
            }
            CryptoSession crypto = session.featureCrypto();
            String wire = peer == null ? crypto.encryptChannelAttachment(session.featureKeys(), payload, !picture) : crypto.encryptDirectAttachment(peer.publicKey, payload, !picture);
            if (wire.length() > WIRE_LIMIT)
            {
                throw new IllegalArgumentException("The encrypted attachment exceeds the transfer limit");
            }
            long id = session.featureNextId();
            JSONObject extra = new JSONObject();
            put(extra, "receiver_id", peer == null ? current.channelId : peer.id);
            put(extra, "local_message_id", id);
            if (!picture)
            {
                String encryptedHeader = peer == null ? crypto.encryptChannelAttachment(session.featureKeys(), header, true) : crypto.encryptDirectAttachment(peer.publicKey, header, true);
                if (encryptedHeader.length() > 8192)
                {
                    throw new IllegalArgumentException("The encrypted filename is too long");
                }
                put(extra, "file_header", encryptedHeader);
            }
            String conversation = peer == null ? "channel:" + current.channelId : peer.conversationId;
            Attachment attachment = new Attachment(UUID.randomUUID().toString(), safeName, safeMime, data, picture);
            ChatMessage chat = new ChatMessage(id, conversation, current.clientId, current.username, safeName, true, System.currentTimeMillis(), "Sending", session.featureKey(), -1, current.connectionId, "", null, Collections.emptyList(), attachment, false);
            upload = new Upload(wire, (peer == null ? "channel" : "direct") + (picture ? "_chat_picture_file" : "_chat_file"), extra, chat, peer, current.channelId);
            session.featurePending(chat);
            session.featureEmit(chat);
        }
        catch (RuntimeException error)
        {
            session.featureError(error.getMessage() != null ? error.getMessage() : "The attachment could not be encrypted");
        }
    }

    private void receiveMetadata(String type, JSONObject message) throws JSONException
    {
        boolean picture = type.contains("_picture_");
        boolean direct = type.startsWith("direct");
        Member sender = session.featureMember(number(message, "sender_id"));
        long id = ProtocolJson.integer(message, picture ? "picture_id" : "file_id", 0, 9007199254740991L);
        long length = ProtocolJson.integer(message, picture ? "size" : "encrypted_size", 1, 9007199254740991L);
        if (sender == null || sender.musicBot || sender.id == session.featureState().clientId || incoming.containsKey(id))
        {
            return;
        }
        int channel = direct ? -1 : number(message, "channel_id");
        if (!direct && (channel != session.featureState().channelId || session.featureKeys() == null))
        {
            return;
        }
        long total = length;
        for (Transfer active : incoming.values())
        {
            total += active.expected;
        }
        if (length > WIRE_LIMIT || incoming.size() >= 2 || total > WIRE_LIMIT)
        {
            limitedError("An attachment exceeds this client's transfer limit. Ask for files no larger than 4 MiB");
            return;
        }
        String header = picture ? "" : ProtocolJson.string(message, "file_header", 8192);
        incoming.put(id, new Transfer(id, sender, channel, direct, picture, (int)length, header, session.featureKeys(), session.featurePreviousKeys()));
    }

    private void complete(JSONObject message) throws JSONException
    {
        long id = ProtocolJson.integer(message, "server_chat_message_id", 0, 9007199254740991L);
        Transfer transfer = incoming.remove(id);
        if (transfer == null)
        {
            return;
        }
        Member current = session.featureMember(transfer.sender.id);
        String expectedType = (transfer.direct ? "direct" : "channel") + (transfer.picture ? "_chat_picture" : "_chat_file");
        if (current == null || !current.publicKey.equals(transfer.sender.publicKey) || number(message, "sender_id") != transfer.sender.id || !expectedType.equals(message.optString("receive_type")) || transfer.content.length() != transfer.expected || (!transfer.direct && session.featureState().channelId != transfer.channel))
        {
            limitedError("The attachment was interrupted and was not opened");
            return;
        }
        try
        {
            String plain = decryptAttachment(transfer, transfer.content.toString());
            String name;
            String mime;
            byte[] data;
            if (transfer.picture)
            {
                if (!imageUri(plain, Attachment.MAX_BYTES))
                {
                    throw new IllegalArgumentException("Invalid picture or picture larger than 4 MiB");
                }
                int comma = plain.indexOf(',');
                mime = plain.substring(5, plain.indexOf(';'));
                data = Base64.getDecoder().decode(plain.substring(comma + 1));
                name = "Picture." + (mime.equals("image/jpeg") ? "jpg" : mime.substring(6));
            }
            else
            {
                JSONObject file = ProtocolJson.object(plain, WIRE_LIMIT);
                JSONObject header = ProtocolJson.object(decryptAttachment(transfer, transfer.header), 8192);
                name = cleanName(ProtocolJson.string(file, "name", 1024));
                mime = cleanMime(ProtocolJson.string(file, "mime", 256));
                long size = ProtocolJson.integer(file, "size", 0, Attachment.MAX_BYTES);
                if (!name.equals(cleanName(header.getString("name"))) || !mime.equals(cleanMime(header.getString("mime"))) || size != header.getLong("size"))
                {
                    throw new IllegalArgumentException("The file does not match its authenticated header");
                }
                data = Base64.getDecoder().decode(ProtocolJson.string(file, "data", Attachment.MAX_BYTES * 4 / 3 + 8));
                if (data.length != size)
                {
                    throw new IllegalArgumentException("The file length is invalid");
                }
            }
            Attachment attachment = new Attachment(UUID.randomUUID().toString(), name, mime, data, transfer.picture);
            String conversation = transfer.direct ? transfer.sender.conversationId : "channel:" + transfer.channel;
            ChatMessage chat = new ChatMessage(session.featureNextId(), conversation, transfer.sender.id, transfer.sender.name, name, false, System.currentTimeMillis(), "", transfer.sender.publicKey, id, session.featureState().connectionId, "", null, Collections.emptyList(), attachment, false);
            session.featureEmit(chat);
        }
        catch (Exception error)
        {
            limitedError("The attachment could not be authenticated or exceeds the 4 MiB limit");
        }
    }

    private String decryptAttachment(Transfer transfer, String wire)
    {
        CryptoSession crypto = session.featureCrypto();
        if (transfer.direct)
        {
            return crypto.decryptDirectAttachment(wire, !transfer.picture);
        }
        try
        {
            return crypto.decryptChannelAttachment(transfer.keys, wire, !transfer.picture);
        }
        catch (RuntimeException error)
        {
            if (transfer.previousKeys == null)
            {
                throw error;
            }
            return crypto.decryptChannelAttachment(transfer.previousKeys, wire, !transfer.picture);
        }
    }

    void tick()
    {
        for (Transfer transfer : new ArrayList<>(incoming.values()))
        {
            if (now() > transfer.deadline)
            {
                discard(transfer, "The attachment transfer timed out");
            }
        }
        if (upload == null)
        {
            return;
        }
        if (!uploadCurrent() || now() > upload.deadline)
        {
            ChatMessage failed = upload.message.withDelivery("Not sent", -1);
            upload = null;
            session.featureEmit(failed);
            session.featureError("The attachment stopped because its conversation changed or the transfer timed out");
            return;
        }
        for (int count = 0; count < 4 && upload.cursor < upload.wire.length(); count++)
        {
            int end = Math.min(upload.wire.length(), upload.cursor + 16384);
            JSONObject chunk = body("file_send");
            put(chunk, "total_bytes_length", upload.wire.length());
            put(chunk, "data_part_base64", upload.wire.substring(upload.cursor, end));
            put(chunk, "is_new_file", upload.cursor == 0);
            if (upload.cursor == 0)
            {
                put(chunk, "file_send_intent", upload.intent);
            }
            session.featureSend(chunk);
            upload.cursor = end;
        }
    }

    boolean hasPendingWork()
    {
        return upload != null || !incoming.isEmpty();
    }

    private boolean uploadCurrent()
    {
        return upload != null && session.featureState().connected && !session.featureState().idle && upload.message.connectionId == session.featureState().connectionId && (upload.peer != null ? session.featureCurrent(upload.peer) : upload.channel == session.featureState().channelId);
    }

    private void discard(Transfer transfer, String reason)
    {
        incoming.remove(transfer.id);
        limitedError(reason);
    }

    private void limitedError(String reason)
    {
        if (now() - lastError >= 2000)
        {
            lastError = now();
            session.featureError(reason);
        }
    }

    static String cleanName(String name)
    {
        String result = name == null ? "attachment" : name.replace((char)92, '/');
        result = result.substring(result.lastIndexOf('/') + 1);
        StringBuilder clean = new StringBuilder();
        result.codePoints().filter(code -> code >= 32 && code != 127).limit(240).forEach(clean::appendCodePoint);
        result = clean.toString();
        return result.isEmpty() || result.equals(".") || result.equals("..") ? "attachment" : result;
    }

    static String cleanMime(String value)
    {
        return value != null && value.matches("[A-Za-z0-9!#$&^_.+-]+/[A-Za-z0-9!#$&^_.+-]+") && value.length() <= 128 ? value.toLowerCase(java.util.Locale.ROOT) : "application/octet-stream";
    }

    private static long now()
    {
        return System.nanoTime() / 1000000;
    }

    private static final class Transfer
    {
        final long id;
        final Member sender;
        final int channel;
        final boolean direct;
        final boolean picture;
        final int expected;
        final String header;
        final String keys;
        final String previousKeys;
        final StringBuilder content = new StringBuilder();
        long deadline = now() + 30000;

        Transfer(long id, Member sender, int channel, boolean direct, boolean picture, int expected, String header, String keys, String previousKeys)
        {
            this.id = id;
            this.sender = sender;
            this.channel = channel;
            this.direct = direct;
            this.picture = picture;
            this.expected = expected;
            this.header = header;
            this.keys = keys;
            this.previousKeys = previousKeys;
        }
    }

    private static final class Upload
    {
        final String wire;
        final String intent;
        final JSONObject extra;
        final ChatMessage message;
        final Member peer;
        final int channel;
        int cursor;
        boolean completed;
        long deadline = now() + 120000;

        Upload(String wire, String intent, JSONObject extra, ChatMessage message, Member peer, int channel)
        {
            this.wire = wire;
            this.intent = intent;
            this.extra = extra;
            this.message = message;
            this.peer = peer;
            this.channel = channel;
        }
    }
}
