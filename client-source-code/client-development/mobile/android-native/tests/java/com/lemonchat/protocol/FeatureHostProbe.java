package com.lemonchat.protocol;

import com.lemonchat.model.*;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

/** checks malformed feature packets and identity isolation */
public final class FeatureHostProbe
{
    private static int checks;

    public static void run() throws Exception
    {
        byte[] source = new byte[]{1, 2, 3};
        Attachment attachment = new Attachment("test", "file.bin", "application/octet-stream", source, false);
        source[0] = 9;
        byte[] opened = attachment.data();
        opened[1] = 9;
        check(attachment.data()[0] == 1 && attachment.data()[1] == 2, "attachment bytes cannot be mutated through caller arrays");
        check(ChatAttachments.cleanName("../../unsafe.txt").equals("unsafe.txt"), "incoming names cannot select a filesystem path");
        check(ChatAttachments.cleanMime("text/html; script").equals("application/octet-stream"), "invalid MIME parameters do not reach open actions");
        check(!ChatFeatures.imageUri("https://example.invalid/avatar.png", 92160), "avatar metadata cannot cause external network fetches");
        check(!ChatFeatures.imageUri("data:image/svg+xml;base64,PHN2Zz4=", 92160), "active image formats are not accepted as avatars");
        JSONObject malformed = new JSONObject().put("message_id", "7").put("author_public_key", "peer");
        check(ChatFeatures.readQuote(malformed) == null, "numeric server identifiers cannot impersonate encrypted quote references");
        try (ChatSession session = new ChatSession(Runnable::run))
        {
            List<ChatMessage> delivered = new ArrayList<>();
            session.setListener(new ChatSession.Listener()
            {
                @Override
                public void onState(SessionSnapshot snapshot)
                {
                }

                @Override
                public void onMessage(ChatMessage message)
                {
                    delivered.add(message);
                }

                @Override
                public void onError(String message)
                {
                }
            });
            FutureTask<Void> task = new FutureTask<>(() ->
            {
                set(session, "status", SessionSnapshot.Status.CONNECTED);
                Member peer = new Member(7, 0, "Peer", "", "peer-key", false, false, 3, 0, "");
                Member stranger = new Member(8, 0, "Other", "", "other-key", false, false, 3, 0, "");
                session.featureProfile(peer);
                session.featureProfile(stranger);
                ChatFeatures features = (ChatFeatures) get(session, "features");
                ChatMessage original = new ChatMessage(1, peer.conversationId, 1, "You", "Original", true, 1, "Sent", "local-key", 90, 0, "a".repeat(32), null, Collections.emptyList(), null, false);
                session.featureEmit(original);
                int before = delivered.size();
                features.receipt(stranger, new JSONObject().put("type", "message_seen").put("value", "90"));
                check(delivered.size() == before, "a receipt from another identity cannot mark a private message read");
                features.content(peer.conversationId, peer, new JSONObject().put("type", "chat_message_edit").put("message_id", original.reactionId).put("author_public_key", "local-key").put("new_message_value", "Forged"), 100);
                check(delivered.size() == before, "a peer cannot replace another author's message text");
                features.receipt(peer, new JSONObject().put("type", "message_seen").put("value", "90"));
                check(delivered.get(delivered.size() - 1).seen, "the intended peer's encrypted receipt marks the known message");
                JSONObject metadata = new JSONObject().put("sender_id", 7).put("picture_id", 120).put("size", ChatAttachments.WIRE_LIMIT + 1);
                features.control("direct_chat_picture_metadata", metadata);
                Map<?, ?> transfers = (Map<?, ?>) get(features.attachments, "incoming");
                check(transfers.isEmpty(), "oversized transfer metadata allocates no receiving buffer");
                metadata.put("size", 4);
                features.control("direct_chat_picture_metadata", metadata);
                check(transfers.size() == 1, "a bounded transfer starts only for a known peer");
                features.control("file_receive_chunk", new JSONObject().put("sender_id", 8).put("server_chat_message_id", 120).put("value", "AAAA"));
                check(transfers.isEmpty(), "another sender cannot append to the pending attachment");
                features.control("direct_chat_picture_metadata", metadata);
                features.control("file_receive_chunk", new JSONObject().put("sender_id", 7).put("server_chat_message_id", 120).put("value", "AAAAA"));
                check(transfers.isEmpty(), "a chunk beyond the declared size retires the transfer");
                features.control("direct_chat_picture_metadata", metadata);
                features.control("file_receive_completed", new JSONObject().put("sender_id", 7).put("server_chat_message_id", 120).put("receive_type", "direct_chat_picture"));
                check(transfers.isEmpty(), "an incomplete body cannot be presented as an attachment");
                features.control("direct_chat_picture_metadata", metadata);
                Attachment picture = new Attachment("quote-picture", "photo.png", "image/png", new byte[]{1}, true);
                ChatMessage imageMessage = new ChatMessage(2, peer.conversationId, 7, "Peer", "", false, 2, "Received", "peer-key", 91, 0, "b".repeat(32), null, Collections.emptyList(), picture, false);
                session.featureEmit(imageMessage);
                ReplyQuote imageQuote = features.reply(imageMessage, peer.conversationId);
                check(imageQuote.text.equals("Picture: photo.png"), "image-only replies carry a visible picture description");
                ReplyQuote decodedQuote = ChatFeatures.readQuote(ChatFeatures.quote(imageQuote));
                check(decodedQuote != null && decodedQuote.text.equals(imageQuote.text) && decodedQuote.messageId.equals(imageMessage.reactionId), "attachment descriptions use the existing encrypted quote format");
                check(imageMessage.withText("Image caption").quoteText().equals("Image caption"), "quoted images retain their caption when present");
                check(imageMessage.withAttachment(new Attachment("quote-file", "notes.txt", "text/plain", new byte[]{1}, false)).quoteText().equals("File: notes.txt"), "file-only replies identify the quoted file");
                emojiChecks(features);
                features.reset();
                check(transfers.isEmpty(), "connection replacement retires pending encrypted bodies");
                return null;
            });
            session.executor().execute(task);
            task.get(10, TimeUnit.SECONDS);
        }
        List<Runnable> pausedUi = new ArrayList<>();
        try (ChatSession session = new ChatSession(pausedUi::add))
        {
            session.setListener(new ChatSession.Listener()
            {
                @Override
                public void onState(SessionSnapshot snapshot)
                {
                }

                @Override
                public void onMessage(ChatMessage message)
                {
                }

                @Override
                public void onError(String message)
                {
                }
            });
            FutureTask<Void> task = new FutureTask<>(() ->
            {
                List<String> actors = new ArrayList<>();
                for (int index = 0; index < 128; index++)
                {
                    actors.add("actor-" + index + "x".repeat(4096));
                }
                Reaction reaction = new Reaction("server:" + "b".repeat(32), "wave", actors, false);
                for (int index = 0; index < 20; index++)
                {
                    session.featureEmit(new ChatMessage(index + 1, "channel:0", 2, "Peer", "body", false, index, "", "peer-key", index, 0, "a".repeat(32), null, Collections.singletonList(reaction), null, false));
                }
                long queuedBytes = ((Number)get(session, "callbackBytes")).longValue();
                int queuedCount = ((java.util.Collection<?>)get(session, "callbackQueue")).size();
                check(queuedCount < 20 && queuedBytes > 8 * 1024 * 1024 && queuedBytes <= 16 * 1024 * 1024, "queued reaction metadata cannot bypass the busy UI memory limit");
                return null;
            });
            session.executor().execute(task);
            task.get(10, TimeUnit.SECONDS);
        }
        System.out.println("PASS " + checks + " chat feature isolation checks");
    }

    private static Object get(Object target, String name) throws Exception
    {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(target);
    }

    @SuppressWarnings("unchecked")
    private static void emojiChecks(ChatFeatures features) throws Exception
    {
        org.json.JSONArray catalog = new org.json.JSONArray();
        for (int index = 0; index < 9; index++)
        {
            catalog.put(new JSONObject().put("id", String.format("%032x", index + 1)).put("name", "custom_" + index));
        }
        features.control("server_emoji_list", new JSONObject().put("emojis", catalog));
        check(features.emojis().size() == 9, "the actual custom emoji catalog is retained");
        java.util.Set<String> pending = (java.util.Set<String>)get(features, "requestedEmojis");
        java.util.Set<String> unavailable = (java.util.Set<String>)get(features, "unavailableEmojis");
        String data = "data:image/gif;base64," + java.util.Base64.getEncoder().encodeToString(new byte[200000]);
        for (int index = 0; index < 9; index++)
        {
            String id = String.format("%032x", index + 1);
            pending.add(id);
            features.control("server_emoji_image", new JSONObject().put("emoji_id", id).put("base64", data));
        }
        long bytes = features.emojis().stream().mapToLong(emoji -> emoji.dataUri.length()).sum();
        check(bytes <= 2 * 1024 * 1024 && features.emojis().size() == 9, "encoded emoji eviction preserves the bounded cache and every catalog choice");
        check(features.emojis().get(0).dataUri.isEmpty() && !features.emojis().get(8).dataUri.isEmpty(), "new emoji images replace old encoded sources instead of being rejected repeatedly");
        String oldest = features.emojis().get(0).id;
        pending.add(oldest);
        features.control("server_emoji_image", new JSONObject().put("emoji_id", oldest).put("base64", data));
        check(!features.emojis().get(0).dataUri.isEmpty(), "an evicted emoji can be loaded again when needed");
        String removed = features.emojis().get(1).id;
        pending.add(removed);
        features.control("server_emoji_deleted", new JSONObject().put("emoji_id", removed));
        check(!pending.contains(removed) && features.emojis().stream().noneMatch(emoji -> emoji.id.equals(removed)), "emoji deletion releases its pending request and catalog entry");
        features.control("server_emoji_image", new JSONObject().put("emoji_id", removed).put("base64", data));
        check(features.emojis().stream().noneMatch(emoji -> emoji.id.equals(removed)), "a late image cannot restore a deleted emoji");
        String invalid = String.format("%032x", 10);
        features.control("server_emoji_added", new JSONObject().put("id", invalid).put("name", "new_custom"));
        pending.add(invalid);
        features.control("server_emoji_image", new JSONObject().put("emoji_id", invalid).put("base64", "https://example.invalid/image.gif"));
        features.requestEmoji(invalid);
        check(unavailable.contains(invalid) && !pending.contains(invalid), "invalid image data cannot trigger repeated UI image requests");
        features.control("server_emoji_added", new JSONObject().put("id", invalid).put("name", "new_custom"));
        check(!unavailable.contains(invalid), "a fresh server catalog update permits another valid image attempt");
        pending.add(invalid);
        features.control("server_emoji_error", new JSONObject().put("emoji_id", invalid).put("reason", "No longer available"));
        features.requestEmoji(invalid);
        check(unavailable.contains(invalid) && !pending.contains(invalid), "a server image error stops repeated missing-image requests");
        features.control("server_emoji_list", new JSONObject().put("emojis", catalog));
        check(pending.isEmpty() && unavailable.isEmpty(), "catalog replacement clears old image request state");
    }

    private static void set(Object target, String name, Object value) throws Exception
    {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(target, value);
    }

    private static void check(boolean condition, String description)
    {
        if (!condition)
        {
            throw new AssertionError(description);
        }
        checks++;
        System.out.println("PASS " + description);
    }
}
