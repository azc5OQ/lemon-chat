package com.lemonchat.protocol;

import com.lemonchat.crypto.Identity;
import com.lemonchat.crypto.CryptoSession;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.model.CallInvitation;
import com.lemonchat.model.PokeMessage;
import com.lemonchat.model.Channel;
import com.lemonchat.model.Member;
import com.lemonchat.model.SessionSnapshot;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;
import org.java_websocket.enums.Role;
import org.java_websocket.exceptions.InvalidDataException;
import org.json.JSONArray;
import org.json.JSONObject;

/** host checks for protocol bounds and live server interoperability */
public final class ProtocolHostProbe
{
    private static int passed;
    private static volatile String runtimeFailure;

    public static void main(String[] arguments) throws Exception
    {
        ServerAddressChecks.main(new String[0]);
        parserChecks();
        transportChecks();
        keyChecks();
        lifecycleChecks();
        CallStateHostProbe.run();
        FeatureHostProbe.run();
        TimerPolicyHostProbe.run();
        AvatarHostProbe.run();
        TreeHostProbe.main(new String[0]);
        MusicStreamHostProbe.main(new String[0]);
        if (arguments.length > 0)
        {
            liveChecks(arguments[0]);
        }
        System.out.println("PASS " + passed + " protocol checks");
    }

    private static void check(boolean result, String message)
    {
        if (!result)
        {
            throw new AssertionError(message);
        }
        passed++;
        System.out.println("PASS " + message);
    }

    private static void rejects(CheckedAction action, String message) throws Exception
    {
        boolean rejected = false;
        try
        {
            action.run();
        }
        catch (Exception expected)
        {
            rejected = true;
        }
        check(rejected, message);
    }

    private static void parserChecks() throws Exception
    {
        check(ProtocolJson.object("{\"id\":7}", 100).getInt("id") == 7, "complete JSON accepted");
        rejects(() -> ProtocolJson.object("{} {}", 100), "trailing JSON rejected");
        rejects(() -> ProtocolJson.object("{\"x\":\"\\u0000\"}", 100), "escaped zero rejected");
        rejects(() -> ProtocolJson.object("{\"x\":" + "[".repeat(65) + "0" + "]".repeat(65) + "}", 1000), "deep JSON rejected");
        rejects(() -> ProtocolJson.integer(new JSONObject("{\"id\":\"7\"}"), "id", 0, 99), "string identifier rejected");
        rejects(() -> ProtocolJson.integer(new JSONObject("{\"id\":7.5}"), "id", 0, 99), "fractional identifier rejected");
        rejects(() -> ChatSession.validateEndpoint("wss://user:secret@example.org/"), "embedded URL credentials rejected");
        rejects(() -> ChatSession.validateEndpoint("https://example.org/"), "non WebSocket scheme rejected");
        check(ChatSession.validateEndpoint("wss://[::1]:3000/").getPort() == 3000, "IPv6 endpoint accepted");
    }

    private static void transportChecks() throws Exception
    {
        BoundedWebSocket.BoundedDraft draft = new BoundedWebSocket.BoundedDraft(4096);
        draft.setParseMode(Role.CLIENT);
        ByteBuffer header = ByteBuffer.allocate(10);
        header.put((byte) 0x82).put((byte) 127).putLong(1024L * 1024 * 1024).flip();
        rejects(() -> draft.translateFrame(header), "oversized declared frame rejected before allocation");
        rejects(() -> draft.checkAlloc(1024 * 1024), "allocation limit survives draft copying");
        check(draft.copyInstance() instanceof BoundedWebSocket.BoundedDraft, "copied draft preserves bounds");
        ByteBuffer handshake = ByteBuffer.wrap(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n" + "x".repeat(20000)).getBytes(StandardCharsets.UTF_8));
        check(draft.translateHandshake(handshake) != null, "complete handshake may share a read with a large frame");
        rejects(() -> draft.translateHandshake(ByteBuffer.wrap(("HTTP/1.1 101 Switching Protocols\r\nX: " + "x".repeat(20000)).getBytes(StandardCharsets.UTF_8))), "incomplete oversized handshake rejected");
    }

    private static void keyChecks() throws Exception
    {
        Member first = member(7, "first");
        Member replacement = member(7, "replacement");
        check(!first.conversationId.equals(replacement.conversationId), "reused client slot has a separate private conversation");
        check(first.conversationId.equals(member(12, "first").conversationId), "identity keeps its conversation after a slot change");
        ChannelKeyState state = new ChannelKeyState();
        state.reset();
        state.assign(7, 3, 8);
        String keys = keys();
        JSONObject content = new JSONObject().put("type", "channel_keys_from_maintainer").put("channel_id", 0).put("maintainer_generation", 3).put("value", keys);
        check(!state.accept(9, 0, content), "non maintainer keys rejected");
        check(!state.accept(7, 1, content), "keys for another channel rejected");
        content.put("maintainer_generation", 2);
        check(!state.accept(7, 0, content), "stale maintainer generation rejected");
        content.put("maintainer_generation", 3);
        check(state.accept(7, 0, content), "current maintainer keys accepted");
        state.memberChanged(first, null, Collections.singletonList(member(8, "local")), 0, 8);
        check(state.current == null && state.maintainer == -1, "maintainer departure invalidates channel keys");
        rejects(() -> ChannelKeyState.validate("[]"), "missing key layers rejected");
    }

    private static Member member(int id, String key)
    {
        return new Member(id, 0, "Member", "", key, false, false, 3, 0, "");
    }

    private static void lifecycleChecks() throws Exception
    {
        List<Runnable> callbacks = new ArrayList<>();
        ChatSession session = new ChatSession(callbacks::add);
        CountDownLatch started = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        AtomicInteger updates = new AtomicInteger();
        try
        {
            ChatSession.Listener observer = new ChatSession.Listener()
            {
                @Override
                public void onState(SessionSnapshot state)
                {
                    updates.incrementAndGet();
                }

                @Override
                public void onMessage(ChatMessage message)
                {
                }

                @Override
                public void onError(String message)
                {
                }
            };
            for (int index = 0; index < 1000; index++)
            {
                session.setListener(observer);
            }
            check(callbacks.size() == 1, "pending state updates use one callback task");
            callbacks.remove(0).run();
            check(updates.get() == 1, "state updates coalesce while the UI is busy");
            session.executor().execute(() ->
            {
                started.countDown();
                try
                {
                    release.await();
                }
                catch (InterruptedException interrupted)
                {
                    Thread.currentThread().interrupt();
                }
            });
            check(started.await(5, TimeUnit.SECONDS), "session worker accepts the initial audio task");
            for (int index = 0; index < 255; index++)
            {
                session.executor().execute(() ->
                {
                });
            }
            rejects(() -> session.executor().execute(() ->
            {
            }), "full worker queue reports rejection to audio callers");
            session.close();
            rejects(() -> session.executor().execute(() ->
            {
            }), "closed session refuses new audio tasks");
        }
        finally
        {
            release.countDown();
            session.close();
        }
    }

    private static String keys() throws Exception
    {
        JSONArray keys = new JSONArray();
        for (int layer = 0; layer < 4; layer++)
        {
            JSONArray key = new JSONArray();
            JSONArray iv = new JSONArray();
            for (int index = 0; index < 32; index++)
            {
                key.put(index);
                if (index < 16)
                {
                    iv.put(index);
                }
            }
            keys.put(new JSONObject().put("key_bytes", key).put("iv_bytes", iv));
        }
        return keys.toString();
    }

    private static void liveChecks(String endpoint) throws Exception
    {
        Probe first = new Probe();
        Probe second = new Probe();
        Identity firstIdentity = Identity.create(2048);
        Identity secondIdentity = Identity.create(2048);
        try
        {
            first.session.connect(endpoint, "Java One", firstIdentity, "");
            waitFor(() -> first.ready(), "first native Java login and channel keys");
            second.session.connect(endpoint, "Java Two", secondIdentity, "");
            waitFor(() -> first.ready() && second.ready() && first.session.state().members.size() >= 2, "second native Java login and channel key delivery");
            check(first.session.state().clientId != second.session.state().clientId, "independent server client identifiers");
            Thread.sleep(150);
            first.session.sendChannelMessage("Native channel hello");
            waitFor(() -> second.contains("Native channel hello"), "encrypted channel text received");
            waitFor(() -> first.sent("Native channel hello"), "channel delivery acknowledged");
            Thread.sleep(150);
            second.session.sendDirectMessage(first.session.state().clientId, "Native private hello");
            waitFor(() -> first.contains("Native private hello"), "encrypted private text received");
            waitFor(() -> second.sent("Native private hello"), "private delivery acknowledged");
            featureChecks(first, second, endpoint);
            Member offlineRecipient = first.session.state().members.stream().filter(member -> member.id == second.session.state().clientId).findFirst().get();
            String offlineConversation = second.session.state().members.stream().filter(member -> member.id == first.session.state().clientId).findFirst().get().conversationId;
            second.session.disconnect();
            waitFor(() -> first.ready() && first.session.state().members.size() == 1, "offline recipient leaves before queued message");
            try (CryptoSession sender = new CryptoSession(firstIdentity, ""))
            {
                JSONObject content = new JSONObject().put("type", "direct_chat_message").put("value", new JSONObject().put("value", "Queued private hello").toString());
                JSONObject offline = new JSONObject().put("type", "offline_chat_message").put("recipient_identity_hash", offlineRecipient.identityHash).put("value", sender.encryptDirect(offlineRecipient.publicKey, content.toString()));
                check(first.session.sendControl(offline), "encrypted offline message queued for server");
            }
            Thread.sleep(300);
            second.session.connect(endpoint, "Java Two", secondIdentity, "");
            waitFor(() -> second.ready() && second.contains("Queued private hello"), "queued private message decrypted after reconnect");
            check(second.messages.stream().anyMatch(message -> message.text.equals("Queued private hello") && message.conversationId.equals(offlineConversation)), "offline delivery preserves the sender identity conversation");
            Thread.sleep(150);
            JSONObject channel = new JSONObject().put("type", "create_channel_request").put("parent_channel_id", 0).put("channel_name", "Native test room").put("channel_description", "Protocol integration test").put("channel_password", "native-test").put("is_audio_enabled", false).put("is_video_stream_enabled", false).put("is_client_limit_active", false).put("max_client_count", 0);
            check(first.session.sendControl(channel), "channel creation request accepted by session");
            waitFor(() -> first.session.state().channels.stream().anyMatch(item -> item.name.equals("Native test room")), "channel creation announcement updates the native list");
            int room = first.session.state().channels.stream().filter(item -> item.name.equals("Native test room")).findFirst().get().id;
            first.session.joinChannel(room, "native-test");
            waitFor(() -> first.ready() && first.session.state().channelId == room, "password protected channel join establishes new keys");
            second.expectError = true;
            second.session.joinChannel(room, "wrong-password");
            waitFor(() -> second.lastError != null, "wrong channel password produces a visible error");
            check(second.session.state().connected && second.session.state().channelId != room, "rejected join keeps the existing connection and channel");
            Thread.sleep(150);
            second.session.joinChannel(room, "native-test");
            waitFor(() -> first.ready() && second.ready() && second.session.state().channelId == room, "second member receives keys after moving channels");
            Thread.sleep(150);
            second.session.sendChannelMessage("In the new channel");
            waitFor(() -> first.contains("In the new channel"), "encrypted chat works after channel movement");
            Thread.sleep(150);
            second.session.enterIdle();
            waitFor(() -> second.session.state().idle && first.session.state().members.stream().anyMatch(member -> member.id == second.session.state().clientId && member.idle), "server confirms idle presence and disables channel context");
            check(second.session.state().channelId == -1 && !second.session.state().channelKeysReady, "idle presence discards channel keys");
            second.session.leaveIdle();
            waitFor(() -> second.ready() && second.session.state().channelId == room, "idle return retains previously proven channel access");
            Thread.sleep(150);
            second.session.enterIdle();
            waitFor(() -> second.session.state().idle, "native client can become callable again");
            Thread.sleep(150);
            first.session.joinChannel(0, "");
            waitFor(() -> first.ready() && first.session.state().channelId == 0 && second.session.state().members.stream().anyMatch(member -> member.id == first.session.state().clientId && member.channelId == 0), "caller moves to a visible channel before inviting an idle member");
            Thread.sleep(150);
            first.session.sendPoke(second.session.state().clientId, "Native poke hello");
            waitFor(() -> second.pokes.stream().anyMatch(poke -> poke.text.equals("Native poke hello")), "idle native client receives a bounded poke");
            Thread.sleep(150);
            first.session.callMember(second.session.state().clientId);
            waitFor(() -> second.session.state().call != null, "idle native client receives a real server call invitation");
            CallInvitation invitation = second.session.state().call;
            second.session.acceptCall(invitation.callId, invitation.connectionId);
            waitFor(() -> second.ready() && !second.session.state().idle && second.session.state().channelId == 0, "accept returns to the caller channel and recovers encrypted media keys");
            Thread.sleep(150);
            second.session.sendChannelMessage("After answering the call");
            waitFor(() -> first.contains("After answering the call"), "encrypted channel messaging works after accepting a call");
            Thread.sleep(150);
            second.session.enterIdle();
            second.session.leaveIdle();
            Thread.sleep(400);
            waitFor(() -> second.ready() && !second.session.state().idle, "latest visible presence cancels a queued idle request");
            second.session.disconnect();
            waitFor(() -> first.ready() && first.session.state().members.size() == 1, "departure rotates channel keys");
            Thread.sleep(150);
            first.session.sendChannelMessage("After departure");
            waitFor(() -> first.sent("After departure"), "channel remains usable after departure");
            first.session.disconnect();
            waitFor(() -> first.session.state().status == SessionSnapshot.Status.DISCONNECTED, "explicit disconnect reaches the UI");
            first.session.connect(endpoint, "Java One", Identity.create(2048), "");
            first.session.disconnect();
            first.session.connect(endpoint, "Java Three", Identity.create(2048), "");
            waitFor(() -> first.ready() && "Java Three".equals(first.session.state().username), "last connection intent wins over queued close callbacks");
        }
        finally
        {
            first.session.close();
            second.session.close();
        }
    }

    private static void featureChecks(Probe first, Probe second, String endpoint) throws Exception
    {
        ClientOptions enabled = new ClientOptions(true, true, true, true, false, false, false, false, "websocket");
        first.session.setOptions(enabled);
        second.session.setOptions(enabled);
        Thread.sleep(200);
        ChatMessage original = second.messages.stream().filter(message -> !message.outgoing && message.text.equals("Native channel hello")).findFirst().get();
        second.session.sendChannelMessage("A native quoted reply", original);
        waitFor(() -> first.messages.stream().anyMatch(message -> message.text.equals("A native quoted reply") && message.reply != null && message.reply.messageId.equals(original.reactionId) && message.reply.text.equals(original.text)), "channel quote preserves browser reaction reference and excerpt");
        String peerConversation = first.session.state().members.stream().filter(member -> member.id == second.session.state().clientId).findFirst().get().conversationId;
        first.session.markConversationRead(peerConversation);
        waitFor(() -> second.messages.stream().anyMatch(message -> message.text.equals("Native private hello") && message.seen), "encrypted seen receipt updates the actual outgoing message");
        ChatMessage privateOriginal = first.messages.stream().filter(message -> !message.outgoing && message.text.equals("Native private hello")).findFirst().get();
        Thread.sleep(150);
        first.session.sendDirectMessage(second.session.state().clientId, "Private quoted reply", privateOriginal);
        waitFor(() -> second.messages.stream().anyMatch(message -> !message.outgoing && message.text.equals("Private quoted reply") && message.reply != null && message.reply.messageId.equals(privateOriginal.reactionId)), "private quote stays inside recipient encryption");
        first.session.setOptions(new ClientOptions(true, true, false, true, false, false, false, false, "websocket"));
        Thread.sleep(150);
        second.session.sendDirectMessage(first.session.state().clientId, "Read without receipt");
        waitFor(() -> first.contains("Read without receipt") && second.sent("Read without receipt"), "message arrives while receipt sending is disabled");
        first.session.markConversationRead(peerConversation);
        Thread.sleep(200);
        first.session.setOptions(enabled);
        first.session.markConversationRead(peerConversation);
        Thread.sleep(200);
        check(second.messages.stream().noneMatch(message -> message.text.equals("Read without receipt") && message.seen), "enabling receipts does not disclose earlier opted out reads");
        Thread.sleep(150);
        byte[] file = "Native file payload".getBytes(java.nio.charset.StandardCharsets.UTF_8);
        first.session.sendDirectAttachment(second.session.state().clientId, "notes.txt", "text/plain", file, false);
        waitFor(() -> second.messages.stream().anyMatch(message -> !message.outgoing && message.attachment != null && message.attachment.name.equals("notes.txt") && java.util.Arrays.equals(message.attachment.data(), file)), "authenticated private file header and complete body interoperate");
        waitFor(() -> first.sent("notes.txt"), "private attachment receives server delivery acknowledgement");
        Thread.sleep(150);
        first.session.sendChannelAttachment("channel.txt", "text/plain", file, false);
        waitFor(() -> second.messages.stream().anyMatch(message -> !message.outgoing && message.attachment != null && message.attachment.name.equals("channel.txt") && java.util.Arrays.equals(message.attachment.data(), file)), "authenticated channel file survives complete transfer");
        waitFor(() -> first.sent("channel.txt"), "channel attachment receives server delivery acknowledgement");
        byte[] image = java.nio.file.Files.readAllBytes(java.nio.file.Path.of(System.getProperty("lemon.test.image")));
        Thread.sleep(150);
        first.session.sendDirectAttachment(second.session.state().clientId, "pixel.png", "image/png", image, true);
        waitFor(() -> second.messages.stream().anyMatch(message -> !message.outgoing && message.attachment != null && message.attachment.picture && message.conversationId.startsWith("user:") && java.util.Arrays.equals(message.attachment.data(), image)), "private picture decrypts the browser data URI");
        waitFor(() -> first.sent("pixel.png"), "private picture receives server delivery acknowledgement");
        Thread.sleep(150);
        first.session.sendChannelAttachment("channel.png", "image/png", image, true);
        waitFor(() -> second.messages.stream().anyMatch(message -> !message.outgoing && message.attachment != null && message.attachment.picture && message.conversationId.startsWith("channel:") && java.util.Arrays.equals(message.attachment.data(), image)), "channel picture decrypts the browser data URI");
        waitFor(() -> first.sent("channel.png"), "channel picture receives server delivery acknowledgement");
        String dataUri = "data:image/png;base64," + java.util.Base64.getEncoder().encodeToString(image);
        Thread.sleep(150);
        first.session.uploadAvatar(dataUri);
        waitFor(() -> second.session.state().members.stream().anyMatch(member -> member.id == first.session.state().clientId && member.avatarDataUri.equals(dataUri)), "avatar upload announcement and requested image update the member");
        Probe detached = new Probe();
        try
        {
            detached.session.connect(endpoint, "Avatar observer", Identity.create(2048), "");
            waitFor(detached::ready, "a new headless session connects while an avatar already exists");
            waitFor(() -> detached.session.state().members.stream().anyMatch(member -> member.id == first.session.state().clientId && member.avatarDataUri.equals(dataUri)), "initial member avatars load without any Activity or visible avatar request");
        }
        finally
        {
            detached.session.close();
        }
        waitFor(() -> first.ready() && second.ready() && first.session.state().members.size() == 2, "avatar observer departure preserves channel key recovery");
        first.session.deleteAvatar();
        waitFor(() -> second.session.state().members.stream().anyMatch(member -> member.id == first.session.state().clientId && member.avatarDataUri.isEmpty()), "avatar removal updates the member");
        Thread.sleep(150);
        first.session.sendControl(new JSONObject().put("type", "upload_server_emoji").put("name", "native_test").put("base64", dataUri));
        waitFor(() -> first.session.state().emojis.stream().anyMatch(emoji -> emoji.name.equals("native_test")) && second.session.state().emojis.stream().anyMatch(emoji -> emoji.name.equals("native_test")), "actual server emoji catalog update reaches both native peers");
        String emojiId = first.session.state().emojis.stream().filter(emoji -> emoji.name.equals("native_test")).findFirst().get().id;
        second.session.requestEmoji(emojiId);
        waitFor(() -> second.session.state().emojis.stream().anyMatch(emoji -> emoji.id.equals(emojiId) && !emoji.dataUri.isEmpty()), "actual emoji image is requested and bounded");
        second.session.reactToMessage(original, emojiId);
        waitFor(() -> first.messages.stream().anyMatch(message -> message.reactionId.equals(original.reactionId) && message.reactions.stream().anyMatch(reaction -> reaction.emojiId.equals(emojiId) && reaction.authorKeys.size() == 1)), "encrypted reaction refers to the original channel message");
        Thread.sleep(150);
        second.session.reactToMessage(original, emojiId);
        waitFor(() -> first.messages.stream().filter(message -> message.reactionId.equals(original.reactionId)).reduce((a, b) -> b).get().reactions.isEmpty(), "reaction toggle removes the same identity once");
        Thread.sleep(150);
        first.session.reactToMessage(privateOriginal, emojiId);
        waitFor(() -> second.messages.stream().anyMatch(message -> message.reactionId.equals(privateOriginal.reactionId) && !message.reactions.isEmpty()), "private reaction arrives through recipient content encryption");
        second.session.setOptions(new ClientOptions(true, true, true, true, false, true, false, false, "websocket"));
        Thread.sleep(200);
        first.expectError = true;
        first.session.sendDirectAttachment(second.session.state().clientId, "blocked.txt", "text/plain", file, false);
        waitFor(() -> first.lastError != null && first.lastError.contains("blocked"), "server enforces the native direct file blocking preference");
        first.lastError = null;
        second.session.setOptions(enabled);
        Thread.sleep(150);
    }

    private static void waitFor(BooleanSupplier condition, String description) throws Exception
    {
        long deadline = System.nanoTime() + 40_000_000_000L;
        while (!condition.getAsBoolean() && System.nanoTime() < deadline)
        {
            if (runtimeFailure != null)
            {
                throw new AssertionError(description + ": " + runtimeFailure);
            }
            Thread.sleep(20);
        }
        check(condition.getAsBoolean(), description);
    }

    private interface CheckedAction
    {
        void run() throws Exception;
    }

    private static final class Probe implements ChatSession.Listener
    {
        final ChatSession session = new ChatSession(Runnable::run);
        final List<ChatMessage> messages = new CopyOnWriteArrayList<>();
        final List<PokeMessage> pokes = new CopyOnWriteArrayList<>();
        volatile boolean expectError;
        volatile String lastError;

        Probe()
        {
            session.setListener(this);
        }

        boolean ready()
        {
            return session.state().connected && session.state().channelKeysReady;
        }

        boolean contains(String value)
        {
            return messages.stream().anyMatch(message -> !message.outgoing && message.text.equals(value));
        }

        boolean sent(String value)
        {
            return messages.stream().anyMatch(message -> message.outgoing && message.text.equals(value) && message.delivery.equals("Sent"));
        }

        @Override
        public void onState(SessionSnapshot state)
        {
            System.out.println("STATE " + state.username + " " + state.status + " " + state.detail + " members=" + state.members.size());
        }

        @Override
        public void onMessage(ChatMessage message)
        {
            messages.add(message);
        }

        @Override
        public void onPoke(PokeMessage message)
        {
            pokes.add(message);
        }

        @Override
        public void onError(String message)
        {
            System.out.println("SESSION ERROR " + message);
            if (expectError)
            {
                lastError = message;
                expectError = false;
            }
            else
            {
                runtimeFailure = message;
            }
        }
    }
}
