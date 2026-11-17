package com.lemonchat.audio;

import com.lemonchat.crypto.CryptoSession;
import com.lemonchat.crypto.Identity;
import com.lemonchat.crypto.MediaCrypto;
import com.lemonchat.protocol.BoundedWebSocket;
import java.net.URI;
import java.util.Arrays;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Predicate;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * sends real Opus packets through an isolated server between two native crypto sessions
 */
public final class AudioWireProbe
{
    /**
     * verifies audio authentication, channel encryption and server relay
     *
     * @param arguments isolated server WebSocket address
     * @throws Exception when a protocol or codec check fails
     */
    public static void main(String[] arguments) throws Exception
    {
        URI address = new URI(arguments[0]);
        String transport = arguments.length > 1 ? arguments[1] : "websocket";
        try (Peer sender = new Peer(address, "voice sender", !transport.equals("websocket")); Peer receiver = new Peer(address, "voice receiver", transport.equals("webrtc")))
        {
            sender.awaitIdentity();
            receiver.awaitIdentity();
            require(sender.channel == receiver.channel, "test peers joined different channels");
            sender.attach();
            receiver.attach();
            sender.send(new JSONObject().put("type", "microphone_usage").put("value", 1));
            sender.await(message -> message.optString("type").equals("audio_state_of_single_client") && message.optInt("client_id", -1) == sender.id && message.optInt("value") == 1);
            String keys = sender.crypto.newContentKeys();
            long energy = 0;
            long lastCounter = 0;
            byte[] lastFrame = null;
            MediaReplayGuard guard = new MediaReplayGuard();
            try (OpusCodec encoder = new OpusCodec(true, 48000, 1); OpusCodec decoder = new OpusCodec(false, 48000, 2))
            {
                for (int sequence = 0; sequence < 10; sequence++)
                {
                    short[] pcm = new short[960];
                    for (int index = 0; index < pcm.length; index++)
                    {
                        pcm[index] = (short)(Math.sin(2 * Math.PI * 440 * (sequence * 960 + index) / 48000) * 10000);
                    }
                    byte[] opus = encoder.encode(pcm, pcm.length);
                    byte[] content = new byte[opus.length + 2];
                    content[0] = (byte)sequence;
                    content[1] = (byte)(sequence >>> 8);
                    System.arraycopy(opus, 0, content, 2, opus.length);
                    byte[] inner = CryptoSession.contentMediaLayer(keys, sender.id, sequence + 1, content, true);
                    require(!Arrays.equals(content, inner), "Opus content was not encrypted");
                    lastFrame = sender.mediaKeys.sealMedia(sender.id, sequence + 1, inner);
                    require(Arrays.equals(lastFrame, sender.crypto.sealMedia(sender.id, sequence + 1, inner)), "snapshot send differs from original crypto");
                    require(sender.sendMedia(lastFrame), "audio send queue refused a fresh frame");
                    byte[] relayed = receiver.frames.poll(10, TimeUnit.SECONDS);
                    receiver.checkFailure();
                    require(relayed != null, "server did not relay encrypted voice");
                    CryptoSession.MediaPacket packet = receiver.mediaKeys.openMedia(relayed);
                    require(packet.sender == sender.id && packet.senderCounter == sequence + 1, "wrong authenticated sender or counter");
                    require(guard.accept(packet.sender, packet.senderCounter), "receiver rejected fresh sender audio");
                    require(sequence == 0 || Long.compareUnsigned(packet.frameCounter, lastCounter) > 0, "server reused an outgoing frame counter");
                    lastCounter = packet.frameCounter;
                    byte[] opened = CryptoSession.contentMediaLayer(keys, packet.sender, packet.senderCounter, packet.content, false);
                    require(Arrays.equals(content, opened), "relayed channel content changed");
                    short[] decoded = decoder.decode(Arrays.copyOfRange(opened, 2, opened.length));
                    require(decoded != null && decoded.length == 1920, "relayed Opus frame did not decode");
                    for (short value : decoded)
                    {
                        energy += (long)value * value;
                    }
                    Thread.sleep(20);
                }
            }
            require(energy > 100000000, "relayed audio was silent");
            require(sender.sendMedia(lastFrame), "replay test frame was not queued");
            byte[] replay = receiver.frames.poll(10, TimeUnit.SECONDS);
            require(replay != null, "server replay behavior changed");
            CryptoSession.MediaPacket repeated = receiver.crypto.openMedia(replay);
            require(Long.compareUnsigned(repeated.frameCounter, lastCounter) > 0, "server did not renew its frame counter");
            require(!guard.accept(repeated.sender, repeated.senderCounter), "receiver accepted replayed sender audio");
            require(!guard.accept(sender.id, 1), "receiver accepted older sender audio");
            require(guard.accept(sender.id, Long.MAX_VALUE), "receiver rejected a fresh large counter");
            require(guard.accept(sender.id, Long.MIN_VALUE), "receiver compared an unsigned counter as signed");
            guard.forget(sender.id);
            require(guard.accept(sender.id, 0), "receiver retained a retired sender counter");
            byte[] tampered = lastFrame.clone();
            tampered[8] = 11;
            tampered[tampered.length - 1] ^= 1;
            sender.sendMedia(tampered);
            require(receiver.frames.poll(500, TimeUnit.MILLISECONDS) == null, "server relayed unauthenticated audio");
            sender.send(new JSONObject().put("type", "microphone_usage").put("value", 2));
            sender.await(message -> message.optString("type").equals("audio_state_of_single_client") && message.optInt("client_id", -1) == sender.id && message.optInt("value") == 2);
            MediaSnapshotChecks.verify(receiver.crypto, receiver.mediaKeys, replay, receiver.id);
            System.out.println("PASS " + transport + ": native login, audio attachment, speaking controls, ten real Opus frames through server relay, channel/session encryption, PCM decode, receiver replay rejection and server tamper rejection");
        }
    }

    private static final class Peer implements AutoCloseable
    {
        final CryptoSession crypto = new CryptoSession(Identity.create(2048), "");
        MediaCrypto mediaKeys;
        final ArrayBlockingQueue<JSONObject> messages = new ArrayBlockingQueue<>(128);
        final ArrayBlockingQueue<byte[]> frames = new ArrayBlockingQueue<>(32);
        final AtomicReference<Exception> failure = new AtomicReference<>();
        final CountDownLatch identified = new CountDownLatch(1);
        final URI endpoint;
        final String name;
        final BoundedWebSocket chat;
        BoundedWebSocket media;
        WebRtcTransport rtc;
        final boolean useRtc;
        final java.util.concurrent.ScheduledExecutorService pump = java.util.concurrent.Executors.newSingleThreadScheduledExecutor();
        final CountDownLatch rtcAttached = new CountDownLatch(1);
        volatile boolean closed;
        volatile int id = -1;
        volatile int channel = -1;
        volatile int stunPort;

        Peer(URI endpoint, String name, boolean useRtc)
        {
            this.endpoint = endpoint;
            this.name = name;
            this.useRtc = useRtc;
            boolean unreadyRejected = false;
            try
            {
                crypto.createMediaCrypto().close();
            }
            catch (com.lemonchat.crypto.CryptoException expected)
            {
                unreadyRejected = true;
            }
            require(unreadyRejected, "media snapshot accepted unauthenticated keys");
            String mix = crypto.beginDh();
            chat = new BoundedWebSocket(endpoint, 1024 * 1024, new BoundedWebSocket.Listener()
            {
                @Override
                public void onOpen()
                {
                    try
                    {
                        send(new JSONObject().put("type", "public_key_info").put("value", crypto.getPublicKey()).put("verification_string", "welcome").put("dh_public_mix", mix).put("crypto_version", 2));
                    }
                    catch (Exception error)
                    {
                        failure.compareAndSet(null, error);
                    }
                }

                @Override
                public void onText(String text)
                {
                    if (closed)
                    {
                        return;
                    }
                    try
                    {
                        JSONObject message = new JSONObject(crypto.decrypt(text)).getJSONObject("message");
                        String type = message.getString("type");
                        if (type.equals("crypto_policy"))
                        {
                            send(crypto.negotiate(message));
                        }
                        else if (type.equals("public_key_challenge"))
                        {
                            String challenge = crypto.decryptChallenge(message.getString("value"));
                            crypto.finishNegotiation(message);
                            send(new JSONObject().put("type", "public_key_challenge_response").put("value", challenge).put("chosen_username", name).put("fast_reconnect", false));
                        }
                        else if (type.equals("authentication_status"))
                        {
                            require(message.optString("value").equals("success"), "login rejected");
                            stunPort = message.optInt("stun_port", 0);
                            send(new JSONObject().put("type", "load_server_info"));
                        }
                        else if (type.equals("client_list"))
                        {
                            JSONArray clients = message.getJSONArray("clients");
                            for (int index = 0; index < clients.length(); index++)
                            {
                                JSONObject client = clients.getJSONObject(index);
                                if (client.optString("public_key").equals(crypto.getPublicKey()))
                                {
                                    id = client.getInt("client_id");
                                    channel = client.getInt("channel_id");
                                    identified.countDown();
                                }
                            }
                        }
                        else if (type.equals("sdp_offer") && rtc != null)
                        {
                            require(rtc.offer(new JSONObject(message.getString("value")).getString("sdp")), "native SDP offer failed");
                        }
                        else if (type.equals("ice_candidate") && rtc != null)
                        {
                            JSONObject candidate = message.getJSONObject("value");
                            if (!candidate.getString("candidate").isEmpty())
                            {
                                require(rtc.candidate(candidate.getString("candidate"), candidate.getString("sdpMid")), "native ICE candidate failed");
                            }
                        }
                        else
                        {
                            messages.offer(message);
                        }
                    }
                    catch (Exception error)
                    {
                        failure.compareAndSet(null, error);
                    }
                }

                @Override
                public void onBinary(byte[] bytes)
                {
                    failure.compareAndSet(null, new IllegalStateException("Unexpected binary metadata"));
                }

                @Override
                public void onClosed(int code, String reason)
                {
                    if (!closed)
                    {
                        failure.compareAndSet(null, new IllegalStateException("Chat closed during the audio test"));
                    }
                }

                @Override
                public void onError(Exception error)
                {
                    if (!closed)
                    {
                        failure.compareAndSet(null, error);
                    }
                }
            });
            chat.connect();
        }

        void awaitIdentity() throws Exception
        {
            require(identified.await(30, TimeUnit.SECONDS), "client list did not identify the peer");
            checkFailure();
            mediaKeys = crypto.createMediaCrypto();
        }

        void attach() throws Exception
        {
            if (useRtc)
            {
                attachRtc();
                return;
            }
            send(new JSONObject().put("type", "request_audio_websocket"));
            JSONObject session = await(message -> message.optString("type").equals("audio_websocket_session"));
            String key = session.getString("auth_key");
            String ticket = session.getString("ticket");
            require(session.getInt("client_id") == id, "ticket belongs to another client");
            CountDownLatch attached = new CountDownLatch(1);
            URI uri = new URI(endpoint.getScheme(), null, endpoint.getHost(), endpoint.getPort(), "/audio", null, null);
            media = new BoundedWebSocket(uri, 3869, 8192, new BoundedWebSocket.Listener()
            {
                @Override
                public void onOpen()
                {
                    media.sendBinary(CryptoSession.mediaProof(key, ticket, id));
                }

                @Override
                public void onText(String text)
                {
                    failure.compareAndSet(null, new IllegalStateException("Unexpected text audio"));
                }

                @Override
                public void onBinary(byte[] bytes)
                {
                    if (attached.getCount() != 0)
                    {
                        if (!CryptoSession.verifyMediaAcknowledgement(key, ticket, bytes))
                        {
                            failure.compareAndSet(null, new IllegalStateException("Invalid audio acknowledgement"));
                        }
                        attached.countDown();
                    }
                    else if (bytes.length == 0)
                    {
                        media.sendBinary(bytes);
                    }
                    else
                    {
                        frames.offer(bytes);
                    }
                }

                @Override
                public void onClosed(int code, String reason)
                {
                }

                @Override
                public void onError(Exception error)
                {
                    if (!closed)
                    {
                        failure.compareAndSet(null, error);
                    }
                }
            });
            media.connect();
            require(attached.await(10, TimeUnit.SECONDS), "audio attachment timed out");
            checkFailure();
            send(new JSONObject().put("type", "microphone_usage").put("value", 2));
            await(message -> message.optString("type").equals("audio_state_of_single_client") && message.optInt("client_id", -1) == id && message.optInt("value") == 2);
        }

        boolean sendMedia(byte[] frame)
        {
            return rtc != null ? rtc.send(frame) : media.sendBinary(frame);
        }

        void attachRtc() throws Exception
        {
            rtc = new WebRtcTransport(endpoint.getHost(), stunPort);
            pump.scheduleWithFixedDelay(() ->
            {
                try
                {
                    for (int count = 0; count < 32; count++)
                    {
                        WebRtcTransport.Event event = rtc.poll();
                        if (event == null)
                        {
                            break;
                        }
                        if (event.type == WebRtcTransport.DESCRIPTION)
                        {
                            send(new JSONObject().put("type", "sdp_answer").put("value", new JSONObject().put("type", event.detail).put("sdp", event.text())));
                        }
                        else if (event.type == WebRtcTransport.CANDIDATE)
                        {
                            send(new JSONObject().put("type", "ice_candidate").put("value", new JSONObject().put("candidate", event.text()).put("sdpMid", event.detail).put("sdpMLineIndex", 0)));
                        }
                        else if (event.type == WebRtcTransport.OPEN)
                        {
                            rtcAttached.countDown();
                        }
                        else if (event.type == WebRtcTransport.FRAME && event.payload.length > 1)
                        {
                            frames.offer(event.payload);
                        }
                        else if (event.type == WebRtcTransport.CLOSED && !closed)
                        {
                            throw new IllegalStateException("Native data channel closed");
                        }
                    }
                }
                catch (Exception error)
                {
                    failure.compareAndSet(null, error);
                }
            }, 0, 10, TimeUnit.MILLISECONDS);
            send(new JSONObject().put("type", "create_new_webrtc_datachannel_connection"));
            require(rtcAttached.await(20, TimeUnit.SECONDS), "native data channel attachment timed out: " + failure.get());
            checkFailure();
            send(new JSONObject().put("type", "microphone_usage").put("value", 2));
            await(message -> message.optString("type").equals("audio_state_of_single_client") && message.optInt("client_id", -1) == id && message.optInt("value") == 2);
        }

        synchronized void send(JSONObject message) throws Exception
        {
            require(chat.sendText(crypto.encrypt(new JSONObject().put("message", message).toString())), "metadata send was refused");
        }

        JSONObject await(Predicate<JSONObject> accept) throws Exception
        {
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(15);
            while (System.nanoTime() < deadline)
            {
                checkFailure();
                JSONObject message = messages.poll(100, TimeUnit.MILLISECONDS);
                if (message != null && accept.test(message))
                {
                    return message;
                }
            }
            throw new AssertionError("Expected audio control was not received");
        }

        void checkFailure() throws Exception
        {
            if (failure.get() != null)
            {
                throw failure.get();
            }
        }

        @Override
        public void close()
        {
            closed = true;
            pump.shutdownNow();
            if (rtc != null)
            {
                rtc.close();
            }
            if (media != null)
            {
                media.close();
            }
            chat.close();
            if (mediaKeys != null)
            {
                mediaKeys.close();
            }
            crypto.close();
        }
    }

    private static void require(boolean condition, String message)
    {
        if (!condition)
        {
            throw new IllegalStateException(message);
        }
    }
}
