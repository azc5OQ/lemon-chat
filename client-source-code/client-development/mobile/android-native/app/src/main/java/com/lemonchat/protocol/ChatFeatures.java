package com.lemonchat.protocol;

import com.lemonchat.model.*;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import java.util.ArrayList;
import java.util.ArrayDeque;
import java.util.Base64;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;

/** handles encrypted chat controls on the session worker */
final class ChatFeatures
{
    private final ChatSession session;
    final ChatAttachments attachments;
    final AvatarLoader avatars;
    private ClientOptions options = ClientOptions.defaults();
    private ClientCapabilities capabilities = ClientCapabilities.none();
    private final Map<Long, ChatMessage> history = new LinkedHashMap<>();
    private final Map<SequenceKey, Long> sequences = new LinkedHashMap<>();
    private final Map<String, ServerEmoji> emojis = new LinkedHashMap<>();
    private final LinkedHashSet<String> requestedEmojis = new LinkedHashSet<>();
    private final LinkedHashSet<String> unavailableEmojis = new LinkedHashSet<>();
    private final ArrayDeque<String> emojiImages = new ArrayDeque<>();
    private final LinkedHashSet<Long> readMessages = new LinkedHashSet<>();
    private final ArrayDeque<ChatMessage> pendingReceipts = new ArrayDeque<>();
    private boolean catalogRequested;

    ChatFeatures(ChatSession session)
    {
        this.session = session;
        attachments = new ChatAttachments(session);
        avatars = new AvatarLoader(new AvatarLoader.Host()
        {
            @Override
            public Member member(int id)
            {
                return session.featureMember(id);
            }

            @Override
            public void update(Member member)
            {
                session.featureProfile(member);
            }

            @Override
            public void send(JSONObject message)
            {
                session.featureSend(message);
            }
        });
    }

    ClientCapabilities capabilities()
    {
        return capabilities;
    }

    List<ServerEmoji> emojis()
    {
        return new ArrayList<>(emojis.values());
    }

    ClientOptions options()
    {
        return options;
    }

    void setOptions(ClientOptions value)
    {
        options = value;
        if (session.featureState().connected)
        {
            sendPreferences();
            requestCatalog();
        }
    }

    void policy(JSONObject value)
    {
        capabilities = new ClientCapabilities(value.optBoolean("is_blocking_direct_chat_pictures_active"), value.optBoolean("is_blocking_direct_chat_files_active"), value.optBoolean("is_blocking_channel_chat_pictures_active"), value.optBoolean("is_blocking_channel_chat_files_active"), value.optBoolean("is_avatar_upload_allowed"), value.optBoolean("is_file_upload_allowed"), value.optBoolean("is_chat_picture_allowed"), Math.max(0, Math.min(92160, value.optInt("avatar_max_size", 92160))), Math.max(0, value.optLong("file_upload_max_size", 0)), Math.max(0, value.optLong("chat_picture_max_size", 0)));
        avatars.setEnabled(capabilities.avatarUpload);
        sendPreferences();
        requestCatalog();
    }

    private void requestCatalog()
    {
        if (options.reactions && !catalogRequested)
        {
            session.featureSend(body("load_server_emojis"));
            catalogRequested = true;
        }
    }

    private void sendPreferences()
    {
        JSONObject request = body("set_attachment_receive_preferences");
        put(request, "block_direct_chat_pictures", options.blockDirectPictures);
        put(request, "block_direct_chat_files", options.blockDirectFiles);
        put(request, "block_channel_chat_pictures", options.blockChannelPictures);
        put(request, "block_channel_chat_files", options.blockChannelFiles);
        session.featureSend(request);
    }

    void reset()
    {
        attachments.reset();
        history.clear();
        sequences.clear();
        emojis.clear();
        emojiImages.clear();
        unavailableEmojis.clear();
        avatars.reset();
        requestedEmojis.clear();
        readMessages.clear();
        pendingReceipts.clear();
        capabilities = ClientCapabilities.none();
        catalogRequested = false;
    }

    void remember(ChatMessage message)
    {
        history.put(message.id, message);
        long textBytes = 0;
        long attachmentBytes = 0;
        for (ChatMessage item : history.values())
        {
            textBytes += messageBytes(item);
            attachmentBytes += item.attachment == null ? 0 : item.attachment.size;
        }
        while (history.size() > 500 || textBytes > 2 * 1024 * 1024 || attachmentBytes > 8 * 1024 * 1024)
        {
            Long first = history.keySet().iterator().next();
            ChatMessage removed = history.remove(first);
            textBytes -= messageBytes(removed);
            attachmentBytes -= removed.attachment == null ? 0 : removed.attachment.size;
            readMessages.remove(first);
        }
        attachments.delivered(message);
    }

    ReplyQuote reply(ChatMessage selected, String conversation)
    {
        if (selected == null)
        {
            return null;
        }
        ChatMessage current = history.get(selected.id);
        if (!options.replies || current == null || selected.connectionId != session.featureState().connectionId || !current.conversationId.equals(conversation) || !validId(current.reactionId) || current.authorPublicKey.isEmpty())
        {
            throw new IllegalArgumentException("That message cannot be quoted in this conversation");
        }
        return new ReplyQuote(current.reactionId, current.authorPublicKey, truncate(current.senderName, 128), truncate(current.quoteText(), 320));
    }

    static JSONObject quote(ReplyQuote value)
    {
        if (value == null)
        {
            return null;
        }
        JSONObject object = new JSONObject();
        put(object, "message_id", value.messageId);
        put(object, "author_public_key", value.authorPublicKey);
        put(object, "author_name", value.authorName);
        put(object, "text", value.text);
        return object;
    }

    static ReplyQuote readQuote(JSONObject object)
    {
        if (object == null)
        {
            return null;
        }
        String id = object.optString("message_id", "");
        String key = object.optString("author_public_key", "");
        if (!validId(id) || key.isEmpty() || key.length() > 16384)
        {
            return null;
        }
        return new ReplyQuote(id, key, truncate(object.optString("author_name", ""), 128), truncate(object.optString("text", ""), 320));
    }

    void read(String conversation)
    {
        Member peer = session.featurePeer(conversation);
        if (peer == null || !conversation.startsWith("user:"))
        {
            return;
        }
        for (ChatMessage message : new ArrayList<>(history.values()))
        {
            if (!message.outgoing && message.conversationId.equals(conversation) && message.serverId >= 0 && message.connectionId == session.featureState().connectionId && message.authorPublicKey.equals(peer.publicKey) && readMessages.add(message.id) && options.sendReceipts)
            {
                if (pendingReceipts.size() < 500)
                {
                    pendingReceipts.addLast(message);
                }
            }
        }
    }

    void receipt(Member sender, JSONObject content)
    {
        String number = content.optString("value", "");
        if (!"message_seen".equals(content.optString("type")) || !number.matches("[0-9]{1,16}"))
        {
            return;
        }
        long id;
        try
        {
            id = Long.parseLong(number);
        }
        catch (NumberFormatException error)
        {
            return;
        }
        for (ChatMessage message : new ArrayList<>(history.values()))
        {
            if (message.outgoing && message.serverId == id && message.conversationId.equals(sender.conversationId) && !message.seen)
            {
                session.featureEmit(message.withSeen());
                return;
            }
        }
    }

    boolean content(String conversation, Member sender, JSONObject value, long sequence)
    {
        String type = value.optString("type");
        if (!type.equals("chat_reaction") && !type.equals("chat_message_edit"))
        {
            return false;
        }
        String targetId = value.optString("message_id", "");
        String author = value.optString("author_public_key", "");
        if (!validId(targetId) || sender.publicKey.isEmpty())
        {
            return true;
        }
        for (ChatMessage target : new ArrayList<>(history.values()))
        {
            if (!target.conversationId.equals(conversation) || !target.reactionId.equals(targetId) || !target.authorPublicKey.equals(author))
            {
                continue;
            }
            if (type.equals("chat_message_edit"))
            {
                String text = value.optString("new_message_value", "");
                if (sender.publicKey.equals(author) && !text.trim().isEmpty() && text.length() <= 65536 && newer(target, sender.publicKey, "edit", sequence))
                {
                    session.featureEmit(target.withText(text));
                }
            }
            else
            {
                applyReaction(target, sender.publicKey, value, sequence);
            }
            return true;
        }
        return true;
    }

    private boolean newer(ChatMessage target, String actor, String kind, long sequence)
    {
        if (sequence < 0)
        {
            return false;
        }
        SequenceKey key = new SequenceKey(target.id, actor, kind);
        Long previous = sequences.get(key);
        if (previous != null && previous >= sequence)
        {
            return false;
        }
        sequences.put(key, sequence);
        while (sequences.size() > 4096)
        {
            sequences.remove(sequences.keySet().iterator().next());
        }
        return true;
    }

    void react(ChatMessage selected, String emojiId)
    {
        ChatMessage target = history.get(selected.id);
        ServerEmoji emoji = emojis.get(emojiId);
        if (!options.reactions || target == null || selected.connectionId != session.featureState().connectionId || !target.conversationId.equals(selected.conversationId) || !validId(target.reactionId) || emoji == null)
        {
            session.featureError("That message or emoji is no longer available");
            return;
        }
        boolean active = true;
        for (Reaction reaction : target.reactions)
        {
            if (reaction.emojiId.equals(emojiId) && reaction.mine)
            {
                active = false;
            }
        }
        JSONObject payload = body("chat_reaction");
        put(payload, "message_id", target.reactionId);
        put(payload, "author_public_key", target.authorPublicKey);
        put(payload, "emoji", "server:" + emojiId);
        put(payload, "emoji_name", emoji.name);
        put(payload, "active", active);
        if (sendContent(target.conversationId, payload.toString()))
        {
            applyReaction(target, session.featureKey(), payload, Long.MAX_VALUE);
        }
    }

    private void applyReaction(ChatMessage target, String actor, JSONObject payload, long sequence)
    {
        String emoji = payload.optString("emoji", "");
        String name = payload.optString("emoji_name", "");
        if (!emoji.matches("server:[a-f0-9]{32}") || !name.matches("[a-z0-9_]{1,32}") || !(payload.opt("active") instanceof Boolean))
        {
            return;
        }
        String id = emoji.substring(7);
        boolean active = payload.optBoolean("active");
        if (active && !emojis.containsKey(id))
        {
            return;
        }
        if (sequence != Long.MAX_VALUE && !newer(target, actor, emoji, sequence))
        {
            return;
        }
        List<Reaction> changes = new ArrayList<>();
        boolean found = false;
        for (Reaction reaction : target.reactions)
        {
            if (!reaction.emojiId.equals(id))
            {
                changes.add(reaction);
                continue;
            }
            found = true;
            List<String> actors = new ArrayList<>(reaction.authorKeys);
            actors.remove(actor);
            if (active && actors.size() < 128)
            {
                actors.add(actor);
            }
            if (!actors.isEmpty())
            {
                changes.add(new Reaction(id, name, actors, session.featureKey()));
            }
        }
        if (!found && active && changes.size() < 32)
        {
            changes.add(new Reaction(id, name, Collections.singletonList(actor), session.featureKey()));
        }
        session.featureEmit(target.withReactions(changes));
    }

    private boolean sendContent(String conversation, String payload)
    {
        JSONObject request;
        if (conversation.startsWith("user:"))
        {
            Member peer = session.featurePeer(conversation);
            if (peer == null)
            {
                session.featureError("That person is no longer connected");
                return false;
            }
            JSONObject content = body("direct_chat_message");
            put(content, "value", payload);
            request = session.featureDirect(peer, "conversation", content.toString(), session.featureNextId());
        }
        else
        {
            if (!conversation.equals("channel:" + session.featureState().channelId) || session.featureKeys() == null)
            {
                session.featureError("Join this channel before sending");
                return false;
            }
            request = body("channel_chat_message");
            put(request, "receiver_id", session.featureState().channelId);
            put(request, "local_message_id", session.featureNextId());
            put(request, "value", session.featureCrypto().encryptChannel(session.featureKeys(), payload));
        }
        session.featureSend(request);
        return true;
    }

    void requestAvatars(int[] ids)
    {
        avatars.hint(ids);
    }

    void uploadAvatar(String dataUri)
    {
        if (!capabilities.avatarUpload || !imageUri(dataUri, capabilities.avatarMaxBytes))
        {
            session.featureError("Choose a PNG, JPEG, GIF or WebP avatar within the server's size limit");
            return;
        }
        JSONObject message = body("avatar_upload");
        put(message, "base64_avatar", dataUri);
        session.featureSend(message);
    }

    void deleteAvatar()
    {
        session.featureSend(body("delete_avatar"));
    }

    void requestEmoji(String id)
    {
        ServerEmoji emoji = emojis.get(id);
        if (emoji != null && emoji.unicode.isEmpty() && emoji.dataUri.isEmpty() && !unavailableEmojis.contains(id) && requestedEmojis.size() < 32 && requestedEmojis.add(id))
        {
            JSONObject message = body("load_server_emoji");
            put(message, "emoji_id", id);
            session.featureSend(message);
        }
    }

    boolean control(String type, JSONObject message) throws JSONException
    {
        if (type.equals("client_avatar"))
        {
            int id = number(message, "client_id");
            String data = message.optString("base64_avatar", "");
            avatars.received(id, data);
            return true;
        }
        if (type.equals("avatar_changed"))
        {
            int id = number(message, "client_id");
            avatars.changed(id);
            return true;
        }
        if (type.equals("client_country_code_changed"))
        {
            Member member = session.featureMember(number(message, "client_id"));
            String country = message.optString("country_iso_code", "");
            if (member != null)
            {
                session.featureProfile(member.withProfile(country.matches("[A-Z]{2}") ? country : "", member.avatarDataUri));
            }
            return true;
        }
        if (type.equals("server_emoji_list"))
        {
            JSONArray list = message.getJSONArray("emojis");
            if (list.length() > 256)
            {
                throw new JSONException("Too many server emojis");
            }
            emojis.clear();
            requestedEmojis.clear();
            unavailableEmojis.clear();
            emojiImages.clear();
            for (int i = 0; i < list.length(); i++)
            {
                addEmoji(list.getJSONObject(i));
            }
            return true;
        }
        if (type.equals("server_emoji_added"))
        {
            addEmoji(message);
            return true;
        }
        if (type.equals("server_emoji_deleted"))
        {
            String id = message.optString("emoji_id");
            emojis.remove(id);
            requestedEmojis.remove(id);
            unavailableEmojis.remove(id);
            emojiImages.remove(id);
            return true;
        }
        if (type.equals("server_emoji_error"))
        {
            String id = message.optString("emoji_id");
            if (requestedEmojis.remove(id) && emojis.containsKey(id))
            {
                unavailableEmojis.add(id);
            }
            session.featureError(ProtocolJson.string(message, "reason", 512));
            return true;
        }
        if (type.equals("server_emoji_image"))
        {
            String id = message.optString("emoji_id");
            ServerEmoji emoji = emojis.get(id);
            String data = message.optString("base64", "");
            boolean requested = requestedEmojis.remove(id);
            if (emoji != null && requested)
            {
                if (!imageUri(data, 262144))
                {
                    unavailableEmojis.add(id);
                    return true;
                }
                long total = data.length() - emoji.dataUri.length();
                for (ServerEmoji other : emojis.values())
                {
                    total += other.dataUri.length();
                }
                emojiImages.remove(id);
                while (total > 2 * 1024 * 1024 && !emojiImages.isEmpty())
                {
                    String oldest = emojiImages.removeFirst();
                    ServerEmoji previous = emojis.get(oldest);
                    if (previous != null)
                    {
                        total -= previous.dataUri.length();
                        emojis.put(oldest, new ServerEmoji(oldest, previous.name, previous.unicode, ""));
                    }
                }
                emojis.put(id, new ServerEmoji(id, emoji.name, "", data));
                emojiImages.addLast(id);
            }
            return true;
        }
        return attachments.control(type, message);
    }

    private void addEmoji(JSONObject value)
    {
        String id = value.optString("id");
        String name = value.optString("name");
        String unicode = value.optString("unicode", "");
        if (emojis.size() < 256 && validId(id) && name.matches("[a-z0-9_]{1,32}") && unicode.length() <= 32 && unicode.codePoints().noneMatch(code -> code < 32 || code == 127))
        {
            unavailableEmojis.remove(id);
            emojiImages.remove(id);
            emojis.put(id, new ServerEmoji(id, name, unicode, ""));
        }
    }

    static boolean imageUri(String value, int maxBytes)
    {
        if (value == null || value.length() > maxBytes * 4L / 3 + 64 || !value.matches("data:image/(png|jpeg|gif|webp);base64,[A-Za-z0-9+/]+={0,2}"))
        {
            return false;
        }
        try
        {
            return Base64.getDecoder().decode(value.substring(value.indexOf(',') + 1)).length <= maxBytes;
        }
        catch (IllegalArgumentException error)
        {
            return false;
        }
    }

    static boolean validId(String value)
    {
        return value != null && value.matches("[a-f0-9]{32}");
    }

    static String truncate(String value, int length)
    {
        return value.length() <= length ? value : value.substring(0, length);
    }

    static int number(JSONObject value, String field) throws JSONException
    {
        return (int) ProtocolJson.integer(value, field, 0, Integer.MAX_VALUE);
    }

    static JSONObject body(String type)
    {
        JSONObject value = new JSONObject();
        put(value, "type", type);
        return value;
    }

    static void put(JSONObject value, String name, Object data)
    {
        try
        {
            value.put(name, data);
        }
        catch (JSONException error)
        {
            throw new IllegalArgumentException(error);
        }
    }

    void tick()
    {
        attachments.tick();
        avatars.tick(java.util.concurrent.TimeUnit.NANOSECONDS.toMillis(System.nanoTime()));
        if (!options.sendReceipts)
        {
            pendingReceipts.clear();
        }
        for (int count = 0; count < 4 && !pendingReceipts.isEmpty(); count++)
        {
            ChatMessage message = pendingReceipts.removeFirst();
            Member peer = session.featurePeer(message.conversationId);
            if (peer != null && peer.publicKey.equals(message.authorPublicKey) && message.connectionId == session.featureState().connectionId)
            {
                JSONObject content = body("message_seen");
                put(content, "value", Long.toString(message.serverId));
                session.featureSend(session.featureDirect(peer, "seen_receipt", content.toString(), session.featureNextId()));
            }
        }
    }

    long nextWorkDelay()
    {
        if (!pendingReceipts.isEmpty() || attachments.hasPendingWork())
        {
            return 100;
        }
        return avatars.nextDelay(java.util.concurrent.TimeUnit.NANOSECONDS.toMillis(System.nanoTime()));
    }

    private static long messageBytes(ChatMessage message)
    {
        return MessageBudget.metadataBytes(message);
    }

    private static final class SequenceKey
    {
        final long message;
        final String actor;
        final String kind;

        SequenceKey(long message, String actor, String kind)
        {
            this.message = message;
            this.actor = actor;
            this.kind = kind;
        }

        @Override
        public boolean equals(Object other)
        {
            if (!(other instanceof SequenceKey))
            {
                return false;
            }
            SequenceKey value = (SequenceKey) other;
            return message == value.message && actor.equals(value.actor) && kind.equals(value.kind);
        }

        @Override
        public int hashCode()
        {
            return java.util.Objects.hash(message, actor, kind);
        }
    }
}
