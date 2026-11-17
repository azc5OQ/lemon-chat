package com.lemonchat.audio;

import android.content.Context;
import android.os.SystemClock;
import android.net.Uri;
import com.lemonchat.crypto.CryptoSession;
import com.lemonchat.crypto.MediaCrypto;
import com.lemonchat.model.Member;
import com.lemonchat.protocol.BoundedWebSocket;
import java.net.URI;
import java.security.SecureRandom;
import java.util.Arrays;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.CompletableFuture;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * owns media transport and devices on a separate bounded audio worker
 */
public final class VoiceController implements AutoCloseable
{
    public interface ControlSender
    {
        boolean send(long connectionId, JSONObject message);
    }

    public interface Listener
    {
        void onVoiceStatus(long connectionId, String status, boolean ready, boolean talking);
    }

    private static final int MAX_FRAME = 3869;
    private final Context context;
    private final AudioWorker worker = new AudioWorker();
    private final ControlSender control;
    private final Listener listener;
    private final ScheduledExecutorService timer;
    private ScheduledFuture<?> healthTimer;
    private ScheduledFuture<?> rtcTimer;
    private final AtomicBoolean closed = new AtomicBoolean();
    private final Object lifecycle = new Object();
    private final AtomicLong requestedEpoch = new AtomicLong();
    private final AtomicReference<Connection> pendingConnection = new AtomicReference<>();
    private final AtomicBoolean connectionQueued = new AtomicBoolean();
    private final AtomicReference<VoiceState> pendingState = new AtomicReference<>();
    private final AtomicBoolean stateQueued = new AtomicBoolean();
    private final AtomicReference<AudioOptions> requestedOptions = new AtomicReference<>(new AudioOptions("", "webrtc", true));
    private volatile long requestedConnectionId = -1;
    private long activeEpoch;
    private long chatConnectionId = -1;
    private final AtomicInteger pendingFrames = new AtomicInteger();
    private final AtomicInteger pendingCaptureFrames = new AtomicInteger();
    private final AtomicBoolean rtcPollQueued = new AtomicBoolean();
    private final Map<String, Float> peerGains = new java.util.concurrent.ConcurrentHashMap<>();
    private final Map<Integer, Member> members = new HashMap<>();
    private final Map<Integer, byte[][]> botKeys = new HashMap<>();
    private final Map<Integer, Long> botRequests = new HashMap<>();
    private final Map<Integer, Long> botDiscardThrough = new HashMap<>();
    private final MediaReplayGuard senderCounters = new MediaReplayGuard();
    private volatile boolean talkingRequested;
    private volatile boolean microphoneAvailable;
    private volatile AudioEngine audio;
    private MediaCrypto crypto;
    private URI endpoint;
    private BoundedWebSocket socket;
    private volatile WebRtcTransport webRtc;
    private AudioOptions options = new AudioOptions("", "webrtc", true);
    private String transport = "";
    private long preferredFailedUntil;
    private long rtcCooldownUntil;
    private boolean offerApplied;
    private volatile Mp3Streamer music;
    private volatile boolean streamingMusic;
    private volatile String streamedName = "";
    private Uri pendingMusic;
    private long musicGeneration;
    private String channelKeys;
    private String authKey;
    private String ticket;
    private int localId = -1;
    private int channelId = -1;
    private int sequence;
    private int requestedAudioState = -1;
    private long generation;
    private long deviceGeneration;
    private long pendingUntil;
    private long retryAfter;
    private long lastReceived;
    private long lastProbe;
    private long frameCounter;
    private long counterConnectionId = -1;
    private long receivedCounter;
    private long talkingSince;
    private boolean haveReceivedCounter;
    private boolean authenticated;
    private boolean attached;
    private boolean talkingConfirmed;
    private boolean websocketAllowed = true;
    private boolean webrtcAllowed = true;
    private int stunPort;
    private boolean voiceAllowed = true;
    private boolean channelVoiceAllowed = true;
    private boolean musicAllowed = true;
    private boolean audioInterrupted;
    private String lastStatus = "Voice disconnected";
    private boolean lastReady;
    private boolean lastTalking;

    private static final class Connection
    {
        final long epoch;
        final long id;
        final URI endpoint;
        final int localId;
        final MediaCrypto crypto;

        Connection(long epoch, long id, URI endpoint, int localId, MediaCrypto crypto)
        {
            this.epoch = epoch;
            this.id = id;
            this.endpoint = endpoint;
            this.localId = localId;
            this.crypto = crypto;
        }
    }

    private static final class VoiceState
    {
        final long epoch;
        final int channelId;
        final boolean voiceAllowed;
        final String keys;
        final List<Member> members;

        VoiceState(long epoch, int channelId, boolean voiceAllowed, String keys, List<Member> members)
        {
            this.epoch = epoch;
            this.channelId = channelId;
            this.voiceAllowed = voiceAllowed;
            this.keys = keys;
            this.members = new ArrayList<>(members);
        }
    }

    /**
     * creates an inactive audio owner without starting its workers
     *
     * @param context application context
     * @param control authenticated metadata sender
     * @param listener status callback on the audio executor
     */
    public VoiceController(Context context, ControlSender control, Listener listener)
    {
        this.context = context.getApplicationContext();
        this.control = control;
        this.listener = listener;
        ScheduledThreadPoolExecutor timers = new ScheduledThreadPoolExecutor(1, task -> new Thread(task, "native-audio-timers"));
        timers.setRemoveOnCancelPolicy(true);
        timers.setKeepAliveTime(5, TimeUnit.SECONDS);
        timers.allowCoreThreadTimeOut(true);
        timer = timers;
    }

    private void startRtcPolling()
    {
        rtcTimer = timer.scheduleWithFixedDelay(() ->
        {
            if (webRtc != null && rtcPollQueued.compareAndSet(false, true))
            {
                if (!dispatch(() ->
                {
                    try
                    {
                        pollWebRtc();
                    }
                    finally
                    {
                        rtcPollQueued.set(false);
                    }
                }, () -> rtcPollQueued.set(false)))
                {
                    rtcPollQueued.set(false);
                }
            }
        }, 10, 10, TimeUnit.MILLISECONDS);
    }

    public void applyOptions(AudioOptions value)
    {
        if (value != null)
        {
            requestedOptions.set(value);
            if (requestedConnectionId >= 0)
            {
                dispatch(() -> applyOptionsOnWorker(requestedOptions.get()));
            }
        }
    }

    private void applyOptionsOnWorker(AudioOptions value)
    {
        boolean changedDevice = !options.microphoneDevice.equals(value.microphoneDevice);
        boolean changedTransport = !options.transport.equals(value.transport);
        options = value;
        if (changedDevice)
        {
            releaseTalk();
            if (audio != null)
            {
                audio.setMicrophoneDevice(value.microphoneDevice);
            }
        }
        if (changedTransport)
        {
            preferredFailedUntil = 0;
            dropSocket();
            retryAfter = 0;
            tick();
        }
    }

    public void setTransportPreference(String value)
    {
        requestedOptions.updateAndGet(previous -> new AudioOptions(previous.microphoneDevice, value, previous.soundEffects));
        if (requestedConnectionId >= 0)
        {
            dispatch(() -> applyOptionsOnWorker(requestedOptions.get()));
        }
    }

    /**
     * returns the saved playback level for this service session
     *
     * @param publicKey sender public key
     * @return playback multiplier from zero to two
     */
    public float peerGain(String publicKey)
    {
        return publicKey == null ? 1f : peerGains.getOrDefault(publicKey, 1f);
    }

    public void setPeerGain(String publicKey, float gain)
    {
        if (publicKey == null || publicKey.isEmpty() || !Float.isFinite(gain))
        {
            return;
        }
        if (peerGains.size() >= 1024 && !peerGains.containsKey(publicKey))
        {
            return;
        }
        peerGains.put(publicKey, Math.max(0, Math.min(2, gain)));
        if (requestedConnectionId < 0)
        {
            return;
        }
        dispatch(() ->
        {
            if (audio != null)
            {
                for (Member member : members.values())
                {
                    if (member.publicKey.equals(publicKey))
                    {
                        audio.setGain(member.id, peerGains.get(publicKey));
                    }
                }
            }
        });
    }

    public boolean musicActive()
    {
        return streamingMusic;
    }

    public String musicName()
    {
        return streamedName;
    }

    public void startMusic(Uri uri, String name)
    {
        if (requestedConnectionId < 0)
        {
            return;
        }
        dispatch(() ->
        {
            if (!ready() || uri == null || !"content".equals(uri.getScheme()))
            {
                publish("Join a channel with working audio before streaming music");
                return;
            }
            releaseTalk();
            pendingMusic = uri;
            streamedName = name == null || name.isEmpty() ? "Music" : name.substring(0, Math.min(120, name.length()));
            streamingMusic = true;
            talkingRequested = true;
            talkingConfirmed = false;
            talkingSince = SystemClock.elapsedRealtime();
            sequence = (sequence + 4096) & 0xffff;
            if (!requestAudioState(1))
            {
                releaseTalk();
            }
            else
            {
                publish("Preparing " + streamedName);
            }
        });
    }

    public void stopMusic()
    {
        Mp3Streamer active = music;
        if (active != null)
        {
            active.close();
        }
        if (requestedConnectionId >= 0)
        {
            dispatch(this::releaseTalk);
        }
    }

    /**
     * starts voice after the chat handshake completes
     *
     * @param endpoint chat WebSocket endpoint
     * @param localId authenticated client ID
     * @param connectionId originating chat connection
     * @param crypto owned copy of the authenticated media keys
     */
    public void onAuthenticated(URI endpoint, int localId, long connectionId, MediaCrypto crypto)
    {
        synchronized (lifecycle)
        {
            if (closed.get())
            {
                crypto.close();
                return;
            }
            long epoch = requestedEpoch.incrementAndGet();
            requestedConnectionId = connectionId;
            replaceConnection(new Connection(epoch, connectionId, endpoint, localId, crypto));
        }
    }

    private void activate(Connection connection)
    {
        disconnectOnWorker();
        if (connection.crypto == null)
        {
            return;
        }
        if (closed.get() || connection.epoch != requestedEpoch.get())
        {
            connection.crypto.close();
            return;
        }
        activeEpoch = connection.epoch;
        chatConnectionId = connection.id;
        endpoint = connection.endpoint;
        localId = connection.localId;
        crypto = connection.crypto;
        options = requestedOptions.get();
        if (counterConnectionId != connection.id)
        {
            counterConnectionId = connection.id;
            frameCounter = new SecureRandom().nextLong() & Long.MAX_VALUE;
            receivedCounter = 0;
            haveReceivedCounter = false;
            senderCounters.clear();
        }
        authenticated = true;
        retryAfter = 0;
        healthTimer = timer.scheduleWithFixedDelay(() -> dispatch(this::tick), 1, 1, TimeUnit.SECONDS);
        publish("Connecting voice");
    }

    /**
     * replaces queued channel state without queuing another member list
     *
     * @param channelId current joined channel
     * @param voiceAllowed whether that channel permits voice
     * @param keys validated channel key array or null
     * @param members immutable member values from the chat owner
     */
    public void onState(int channelId, boolean voiceAllowed, String keys, List<Member> members)
    {
        if (closed.get() || requestedConnectionId < 0)
        {
            return;
        }
        pendingState.set(new VoiceState(requestedEpoch.get(), channelId, voiceAllowed, keys, members));
        if (stateQueued.compareAndSet(false, true))
        {
            if (!dispatch(() ->
            {
                stateQueued.set(false);
                applyPendingState();
                tick();
            }, () -> stateQueued.set(false)))
            {
                stateQueued.set(false);
            }
        }
    }

    private void applyPendingState()
    {
        VoiceState state = pendingState.getAndSet(null);
        if (state != null && state.epoch == activeEpoch && state.epoch == requestedEpoch.get() && authenticated)
        {
            onChannelChanged(state.channelId);
            onChannelPolicy(state.voiceAllowed);
            onChannelKeys(state.keys);
            onMembers(state.members);
        }
    }

    /**
     * replaces the authenticated membership snapshot
     *
     * @param values current online clients
     */
    private void onMembers(List<Member> values)
    {
        Map<Integer, Member> previous = new HashMap<>(members);
        members.clear();
        for (Member member : values)
        {
            Member old = previous.get(member.id);
            if (old != null && (!Objects.equals(old.publicKey, member.publicKey) || old.musicBot != member.musicBot || (old.idle && !member.idle)))
            {
                senderCounters.forget(member.id);
            }
            members.put(member.id, member);
            if (audio != null)
            {
                audio.setGain(member.id, peerGains.getOrDefault(member.publicKey, 1f));
            }
            if (member.musicBot && member.channelId == channelId && member.musicBotKeys != null)
            {
                try
                {
                    byte[][] keys = readBotKeys(new JSONArray(member.musicBotKeys));
                    botKeys.put(member.id, keys);
                }
                catch (JSONException | IllegalArgumentException ignored)
                {
                }
            }
        }
        for (int id : previous.keySet())
        {
            if (!members.containsKey(id))
            {
                senderCounters.forget(id);
                if (audio != null)
                {
                    audio.forgetGain(id);
                }
            }
        }
        botKeys.keySet().removeIf(id -> !members.containsKey(id) || members.get(id).channelId != channelId);
        applyAudioPolicy();
    }

    /**
     * replaces the current channel keys
     *
     * @param keysJson validated channel key array or null
     */
    private void onChannelKeys(String keysJson)
    {
        channelKeys = keysJson;
        if (keysJson == null)
        {
            releaseTalk();
        }
        publish(talkingConfirmed && talkingRequested ? "Talking" : (attached ? idleStatus() : lastStatus));
    }

    /**
     * drops old channel audio before changing membership
     *
     * @param value current joined channel ID
     */
    private void onChannelChanged(int value)
    {
        if (channelId == value)
        {
            return;
        }
        releaseTalk();
        channelId = value;
        channelKeys = null;
        channelVoiceAllowed = true;
        botKeys.clear();
        botRequests.clear();
        botDiscardThrough.clear();
        AudioEngine active = audio;
        if (active != null)
        {
            active.clearPlayback();
        }
    }

    /**
     * applies the joined channel's voice setting
     *
     * @param allowed whether the channel permits voice
     */
    private void onChannelPolicy(boolean allowed)
    {
        channelVoiceAllowed = allowed;
        if (!allowed)
        {
            releaseTalk();
        }
        applyAudioPolicy();
        publish(allowed ? lastStatus : "Voice is disabled in this channel");
    }

    /**
     * handles policy, attachment tickets and speaking acknowledgements
     *
     * @param connectionId originating chat connection
     * @param message authenticated message body
     */
    public void onControl(long connectionId, JSONObject message)
    {
        if (closed.get() || connectionId != requestedConnectionId)
        {
            return;
        }
        String type = message.optString("type");
        if (!type.equals("authentication_status") && !type.equals("server_policy") && !type.equals("audio_websocket_session") && !type.equals("sdp_offer") && !type.equals("ice_candidate") && !type.equals("datachannel_cooldown") && !type.equals("audio_state_of_single_client") && !type.equals("music_bot_key") && !type.equals("stop_song_stream"))
        {
            return;
        }
        String copy = message.toString();
        if (copy.length() > 131072 || !dispatch(() ->
        {
            if (chatConnectionId == connectionId)
            {
                try
                {
                    processControl(new JSONObject(copy));
                }
                catch (JSONException invalid)
                {
                    failTransport();
                }
            }
        }))
        {
            onDisconnected();
        }
    }

    private void processControl(JSONObject message)
    {
        String type = message.optString("type");
        if (type.equals("authentication_status") || type.equals("server_policy"))
        {
            if (message.has("stun_port"))
            {
                int port = message.optInt("stun_port", 0);
                stunPort = port > 0 && port <= 65535 ? port : 0;
            }
            websocketAllowed = message.optBoolean("is_audio_over_websocket_allowed", websocketAllowed);
            webrtcAllowed = message.optBoolean("is_audio_over_webrtc_allowed", webrtcAllowed);
            voiceAllowed = message.optBoolean("is_voice_chat_active", voiceAllowed);
            musicAllowed = message.optBoolean("is_music_bot_audio_active", musicAllowed);
            if ((!websocketAllowed && transport.equals("websocket")) || (!webrtcAllowed && transport.equals("webrtc")) || (!voiceAllowed && !musicAllowed))
            {
                dropSocket();
                publish("Audio connection settings changed");
            }
            else if (!voiceAllowed)
            {
                releaseTalk();
            }
            applyAudioPolicy();
        }
        else if (type.equals("audio_websocket_session"))
        {
            attach(message);
        }
        else if (type.equals("sdp_offer") && webRtc != null && !offerApplied && SystemClock.elapsedRealtime() <= pendingUntil)
        {
            try
            {
                JSONObject offer = new JSONObject(message.getString("value"));
                if (!"offer".equals(offer.getString("type")) || !webRtc.offer(offer.getString("sdp")))
                {
                    throw new IllegalArgumentException("Invalid WebRTC offer");
                }
                offerApplied = true;
            }
            catch (JSONException | IllegalArgumentException failure)
            {
                failTransport();
            }
        }
        else if (type.equals("ice_candidate") && webRtc != null)
        {
            JSONObject candidate = message.optJSONObject("value");
            if (candidate != null && !candidate.optString("candidate").isEmpty() && !webRtc.candidate(candidate.optString("candidate"), candidate.optString("sdpMid")))
            {
                failTransport();
            }
        }
        else if (type.equals("datachannel_cooldown"))
        {
            rtcCooldownUntil = SystemClock.elapsedRealtime() + Math.max(1000, Math.min(300000, message.optLong("seconds_left", 30) * 1000));
            if (transport.equals("webrtc"))
            {
                failTransport();
            }
        }
        else if (type.equals("audio_state_of_single_client") && message.optInt("client_id", -1) == localId)
        {
            boolean wasTalking = talkingConfirmed;
            talkingConfirmed = message.optInt("value") == 1 && talkingRequested && ready();
            if (talkingConfirmed)
            {
                if (pendingMusic != null)
                {
                    beginMusic();
                }
                else if (!streamingMusic && ensureAudio())
                {
                    synchronized (lifecycle)
                    {
                        if (talkingRequested && talkingConfirmed && microphoneAvailable && ready())
                        {
                            audio.startCapture();
                            publish("Talking");
                        }
                    }
                }
            }
            else
            {
                stopCapture();
                if (wasTalking)
                {
                    finishMusic(false);
                    talkingRequested = false;
                    publish("Speaking stopped by the server");
                }
            }
        }
        else if (type.equals("music_bot_key"))
        {
            int id = message.optInt("client_id", -1);
            Member member = members.get(id);
            if (member != null && member.musicBot && member.channelId == channelId && message.optInt("channel_id", -1) == channelId)
            {
                try
                {
                    botKeys.put(id, readBotKeys(message.getJSONArray("music_bot_keys")));
                }
                catch (JSONException | IllegalArgumentException ignored)
                {
                }
            }
        }
        else if (type.equals("stop_song_stream"))
        {
            int id = message.optInt("client_id", -1);
            if (id == localId && streamingMusic)
            {
                finishMusic(false);
                releaseTalk();
                return;
            }
            Member member = members.get(id);
            if (member != null && member.musicBot && member.channelId == channelId)
            {
                try
                {
                    long counter = Long.parseUnsignedLong(message.getString("discard_audio_through_counter"));
                    Long previous = botDiscardThrough.get(id);
                    if (previous == null || Long.compareUnsigned(counter, previous) >= 0)
                    {
                        botDiscardThrough.put(id, counter);
                        if (audio != null)
                        {
                            audio.discard(id);
                        }
                    }
                }
                catch (JSONException | NumberFormatException ignored)
                {
                }
            }
        }
    }

    /**
     * records the current permission and releases capture after revocation
     *
     * @param available whether microphone permission is granted
     */
    public void setMicrophoneAvailable(boolean available)
    {
        synchronized (lifecycle)
        {
            microphoneAvailable = available;
            if (!available && !streamingMusic)
            {
                talkingRequested = false;
                stopCapture();
            }
        }
        if (requestedConnectionId < 0)
        {
            return;
        }
        dispatch(() ->
        {
            if (!available && !streamingMusic)
            {
                releaseTalk();
            }
            publish(lastStatus);
        });
    }

    /**
     * requests speaking while the user holds the microphone button
     *
     * @param value whether the button remains pressed
     */
    public void setTalking(boolean value)
    {
        synchronized (lifecycle)
        {
            talkingRequested = value && microphoneAvailable && !closed.get();
            if (!talkingRequested)
            {
                stopCapture();
            }
        }
        if (requestedConnectionId < 0)
        {
            talkingRequested = false;
            return;
        }
        dispatch(() ->
        {
            if (streamingMusic)
            {
                finishMusic(true);
            }
            if (!talkingRequested)
            {
                releaseTalk();
                return;
            }
            if (!ready())
            {
                talkingRequested = false;
                publish("Voice is not ready in this channel");
                return;
            }
            talkingSince = SystemClock.elapsedRealtime();
            talkingConfirmed = false;
            if (!requestAudioState(1))
            {
                releaseTalk();
                publish("Cannot request the microphone while disconnected");
            }
        });
    }

    /**
     * clears the old session without retaining keys or queued sound
     */
    public void onDisconnected()
    {
        synchronized (lifecycle)
        {
            requestedConnectionId = -1;
            long epoch = requestedEpoch.incrementAndGet();
            talkingRequested = false;
            stopCapture();
            Mp3Streamer active = music;
            if (active != null)
            {
                active.close();
            }
            pendingState.set(null);
            if (!closed.get())
            {
                replaceConnection(new Connection(epoch, -1, null, -1, null));
            }
        }
    }

    private void replaceConnection(Connection replacement)
    {
        Connection previous = pendingConnection.getAndSet(replacement);
        if (previous != null && previous.crypto != null)
        {
            previous.crypto.close();
        }
        if (connectionQueued.compareAndSet(false, true))
        {
            worker.executeRetirement(this::drainConnections);
        }
    }

    private void drainConnections()
    {
        do
        {
            Connection next;
            while ((next = pendingConnection.getAndSet(null)) != null)
            {
                try
                {
                    activate(next);
                }
                catch (RuntimeException failure)
                {
                    disconnectOnWorker();
                    if (next.crypto != null)
                    {
                        next.crypto.close();
                    }
                    publish("Audio could not start");
                }
            }
            connectionQueued.set(false);
        }
        while (pendingConnection.get() != null && connectionQueued.compareAndSet(false, true));
        synchronized (lifecycle)
        {
            if (closed.get() && !connectionQueued.get() && pendingConnection.get() == null)
            {
                worker.close();
            }
        }
    }

    private void disconnectOnWorker()
    {
        authenticated = false;
        if (healthTimer != null)
        {
            healthTimer.cancel(false);
            healthTimer = null;
        }
        dropSocket();
        if (crypto != null)
        {
            crypto.close();
        }
        crypto = null;
        endpoint = null;
        channelKeys = null;
        localId = -1;
        channelId = -1;
        sequence = 0;
        senderCounters.clear();
        websocketAllowed = true;
        webrtcAllowed = true;
        stunPort = 0;
        preferredFailedUntil = 0;
        rtcCooldownUntil = 0;
        voiceAllowed = true;
        channelVoiceAllowed = true;
        musicAllowed = true;
        audioInterrupted = false;
        members.clear();
        botKeys.clear();
        botRequests.clear();
        botDiscardThrough.clear();
        publish("Voice disconnected");
    }

    /**
     * retries expired attachments and microphone requests
     */
    private void tick()
    {
        applyPendingState();
        if (closed.get() || !authenticated || activeEpoch != requestedEpoch.get() || (!websocketAllowed && !webrtcAllowed) || (!voiceAllowed && !musicAllowed))
        {
            return;
        }
        long now = SystemClock.elapsedRealtime();
        if (talkingRequested && !talkingConfirmed && now - talkingSince > 5000)
        {
            releaseTalk();
            publish("The server did not enable your microphone");
        }
        if (attached)
        {
            if (audio != null)
            {
                audio.keepAwake();
            }
            if (webRtc != null && now - lastProbe >= 5000)
            {
                lastProbe = now;
                sendFrame(new byte[1]);
            }
            if (now - lastReceived > 20000)
            {
                failTransport();
                publish("Voice connection lost");
            }
            return;
        }
        if (now < pendingUntil || now < retryAfter || !members.containsKey(localId))
        {
            return;
        }
        if (socket != null || webRtc != null)
        {
            failTransport();
            return;
        }
        boolean preferRtc = options.transport.equals("webrtc");
        if (now < preferredFailedUntil)
        {
            preferRtc = !preferRtc;
        }
        if (preferRtc && (!webrtcAllowed || now < rtcCooldownUntil))
        {
            preferRtc = false;
        }
        if (!preferRtc && !websocketAllowed)
        {
            if (!webrtcAllowed || now < rtcCooldownUntil)
            {
                return;
            }
            preferRtc = true;
        }
        transport = preferRtc ? "webrtc" : "websocket";
        pendingUntil = now + 10000;
        if (preferRtc)
        {
            try
            {
                webRtc = new WebRtcTransport(endpoint.getHost(), stunPort);
                generation++;
                offerApplied = false;
                startRtcPolling();
            }
            catch (RuntimeException | UnsatisfiedLinkError failure)
            {
                failTransport();
                return;
            }
        }
        if (!sendControl(preferRtc ? "create_new_webrtc_datachannel_connection" : "request_audio_websocket", null, 0))
        {
            pendingUntil = 0;
            retryAfter = now + 3000;
        }
    }

    /**
     * opens a media socket using the current one use ticket
     *
     * @param message authenticated audio session response
     */
    private void attach(JSONObject message)
    {
        if (!authenticated || !websocketAllowed || !transport.equals("websocket") || socket != null || SystemClock.elapsedRealtime() > pendingUntil || message.optInt("client_id", -1) != localId)
        {
            return;
        }
        try
        {
            authKey = message.getString("auth_key");
            ticket = message.getString("ticket");
            byte[] proof = CryptoSession.mediaProof(authKey, ticket, localId);
            if (proof == null || proof.length != 53)
            {
                throw new IllegalArgumentException("Invalid audio ticket");
            }
            URI address = new URI(endpoint.getScheme(), null, endpoint.getHost(), endpoint.getPort(), "/audio", null, null);
            long connection = ++generation;
            socket = new BoundedWebSocket(address, MAX_FRAME, 8192, new BoundedWebSocket.Listener()
            {
                @Override
                public void onOpen()
                {
                    if (!dispatch(() ->
                    {
                        try
                        {
                            if (current(connection) && !socket.sendBinary(proof))
                            {
                                failTransport();
                            }
                        }
                        finally
                        {
                            Arrays.fill(proof, (byte)0);
                        }
                    }, () -> Arrays.fill(proof, (byte)0)))
                    {
                        Arrays.fill(proof, (byte)0);
                    }
                }

                @Override
                public void onText(String text)
                {
                    dispatch(() ->
                    {
                        if (current(connection))
                        {
                            failTransport();
                        }
                    });
                }

                @Override
                public void onBinary(byte[] bytes)
                {
                    if (pendingFrames.incrementAndGet() > 64)
                    {
                        pendingFrames.decrementAndGet();
                        return;
                    }
                    if (!dispatch(() ->
                    {
                        try
                        {
                            if (current(connection))
                            {
                                receive(bytes);
                            }
                        }
                        finally
                        {
                            pendingFrames.decrementAndGet();
                        }
                    }, () -> pendingFrames.decrementAndGet()))
                    {
                        pendingFrames.decrementAndGet();
                    }
                }

                @Override
                public void onClosed(int code, String reason)
                {
                    dispatch(() ->
                    {
                        if (current(connection))
                        {
                            failTransport();
                            publish("Voice connection lost");
                        }
                    });
                }

                @Override
                public void onError(Exception error)
                {
                    dispatch(() ->
                    {
                        if (current(connection))
                        {
                            failTransport();
                            publish("Voice connection failed");
                        }
                    });
                }
            });
            socket.connect();
        }
        catch (Exception failure)
        {
            failTransport();
            publish("Voice could not connect");
        }
    }

    /**
     * verifies the attachment or decrypts audio from a current channel member
     *
     * @param frame complete binary audio frame
     */
    private void receive(byte[] frame)
    {
        if (activeEpoch != requestedEpoch.get() || !authenticated)
        {
            return;
        }
        if (!attached)
        {
            boolean valid;
            try
            {
                valid = CryptoSession.verifyMediaAcknowledgement(authKey, ticket, frame);
            }
            catch (RuntimeException failure)
            {
                valid = false;
            }
            if (!valid || activeEpoch != requestedEpoch.get())
            {
                dropSocket();
                publish("Voice authentication failed");
                return;
            }
            authKey = null;
            ticket = null;
            attached = true;
            pendingUntil = 0;
            lastReceived = SystemClock.elapsedRealtime();
            requestAudioState(2);
            if (audioWanted() && ensureAudio())
            {
                publish(idleStatus());
            }
            return;
        }
        lastReceived = SystemClock.elapsedRealtime();
        if (webRtc != null && frame.length == 1)
        {
            return;
        }
        if (frame.length == 0)
        {
            sendFrame(frame);
            return;
        }
        try
        {
            CryptoSession.MediaPacket packet = crypto.openMedia(frame);
            if (packet == null || (haveReceivedCounter && Long.compareUnsigned(packet.frameCounter, receivedCounter) <= 0))
            {
                return;
            }
            receivedCounter = packet.frameCounter;
            haveReceivedCounter = true;
            Member member = members.get(packet.sender);
            if (member == null || member.id == localId || member.channelId != channelId || member.idle || packet.content.length < 3)
            {
                return;
            }
            if (!senderCounters.accept(packet.sender, packet.senderCounter))
            {
                return;
            }
            byte[] content = packet.content;
            if (member.musicBot)
            {
                if (!musicAllowed)
                {
                    return;
                }
                Long discarded = botDiscardThrough.get(member.id);
                if (discarded != null && Long.compareUnsigned(packet.senderCounter, discarded) <= 0)
                {
                    return;
                }
                byte[][] keys = botKeys.get(member.id);
                if (keys == null)
                {
                    long now = SystemClock.elapsedRealtime();
                    Long last = botRequests.get(member.id);
                    if (last == null || now - last > 1000)
                    {
                        botRequests.put(member.id, now);
                        sendControl("request_music_bot_key", "client_id", member.id);
                    }
                    return;
                }
                for (int index = keys.length - 1; index >= 0; index--)
                {
                    content = CryptoSession.aesMediaLayer(keys[index], null, packet.sender, packet.senderCounter, content);
                }
            }
            else
            {
                if (!voiceAllowed || !channelVoiceAllowed || channelKeys == null)
                {
                    return;
                }
                content = CryptoSession.contentMediaLayer(channelKeys, packet.sender, packet.senderCounter, content, false);
            }
            AudioEngine active = audio;
            if (active != null && content != null && content.length >= 3 && activeEpoch == requestedEpoch.get())
            {
                int value = (content[0] & 255) | ((content[1] & 255) << 8);
                active.receive(packet.sender, value, Arrays.copyOfRange(content, 2, content.length));
            }
        }
        catch (RuntimeException ignored)
        {
        }
    }

    /**
     * opens audio devices for this socket generation
     *
     * @return true when audio devices are available
     */
    private boolean ensureAudio()
    {
        if (closed.get() || !authenticated || activeEpoch != requestedEpoch.get())
        {
            return false;
        }
        if (audio != null)
        {
            return true;
        }
        try
        {
            final long connection = generation;
            final long devices = ++deviceGeneration;
            audio = new AudioEngine(context, new AudioEngine.Listener()
            {
                @Override
                public void onEncoded(byte[] packet, long captureId)
                {
                    if (pendingCaptureFrames.incrementAndGet() > 4)
                    {
                        pendingCaptureFrames.decrementAndGet();
                        return;
                    }
                    long capturedAt = SystemClock.elapsedRealtime();
                    if (!dispatch(() ->
                    {
                        try
                        {
                            if (SystemClock.elapsedRealtime() - capturedAt <= 200)
                            {
                                sendVoice(connection, devices, packet, captureId);
                            }
                        }
                        finally
                        {
                            pendingCaptureFrames.decrementAndGet();
                        }
                    }, () -> pendingCaptureFrames.decrementAndGet()))
                    {
                        pendingCaptureFrames.decrementAndGet();
                    }
                }

                @Override
                public void onError(String message)
                {
                    dispatch(() ->
                    {
                        if (current(connection) && deviceGeneration == devices)
                        {
                            releaseTalk();
                            audioInterrupted = true;
                            closeAudio();
                            publish(message);
                        }
                    });
                }

                @Override
                public void onFocusChanged(boolean available, boolean permanent)
                {
                    dispatch(() ->
                    {
                        if (current(connection) && deviceGeneration == devices)
                        {
                            if (!available)
                            {
                                releaseTalk();
                                if (permanent)
                                {
                                    audioInterrupted = true;
                                    closeAudio();
                                }
                            }
                            publish(available ? idleStatus() : "Audio paused by another app");
                        }
                    });
                }
            });
            if (closed.get() || activeEpoch != requestedEpoch.get())
            {
                closeAudio();
                return false;
            }
            audioInterrupted = false;
            audio.setMicrophoneDevice(options.microphoneDevice);
            for (Member member : members.values())
            {
                audio.setGain(member.id, peerGains.getOrDefault(member.publicKey, 1f));
            }
            return true;
        }
        catch (RuntimeException | UnsatisfiedLinkError failure)
        {
            releaseTalk();
            audioInterrupted = true;
            publish(failure.getMessage() == null ? "Audio devices are unavailable" : failure.getMessage());
            return false;
        }
    }

    /**
     * checks whether this channel needs audio devices
     *
     * @return whether the current channel has enabled voice or music
     */
    private boolean audioWanted()
    {
        if (channelId < 0)
        {
            return false;
        }
        if (voiceAllowed && channelVoiceAllowed)
        {
            return true;
        }
        if (musicAllowed)
        {
            for (Member member : members.values())
            {
                if (member.musicBot && member.channelId == channelId)
                {
                    return true;
                }
            }
        }
        return false;
    }

    /**
     * releases devices when this channel has no enabled audio source
     */
    private void applyAudioPolicy()
    {
        if (!audioWanted())
        {
            closeAudio();
        }
        else if (attached && !audioInterrupted)
        {
            ensureAudio();
        }
    }

    /**
     * encrypts fresh capture after checking the press and connection
     *
     * @param connection socket generation
     * @param devices device generation
     * @param packet encoded Opus frame
     * @param captureId microphone press generation
     */
    private void sendVoice(long connection, long devices, byte[] packet, long captureId)
    {
        AudioEngine active = audio;
        if (!current(connection) || deviceGeneration != devices || !ready() || !talkingRequested || !talkingConfirmed || active == null || !active.isCurrentCapture(captureId))
        {
            return;
        }
        sendEncoded(packet);
    }

    private boolean sendEncoded(byte[] packet)
    {
        if (frameCounter == -1L)
        {
            releaseTalk();
            publish("Reconnect before sending more audio");
            return false;
        }
        frameCounter++;
        byte[] content = new byte[packet.length + 2];
        content[0] = (byte)sequence;
        content[1] = (byte)(sequence >>> 8);
        sequence = (sequence + 1) & 0xffff;
        System.arraycopy(packet, 0, content, 2, packet.length);
        try
        {
            content = CryptoSession.contentMediaLayer(channelKeys, localId, frameCounter, content, true);
            byte[] frame = crypto.sealMedia(localId, frameCounter, content);
            return frame != null && sendFrame(frame);
        }
        catch (RuntimeException failure)
        {
            releaseTalk();
            publish("Voice encryption failed");
            return false;
        }
    }

    /**
     * stops capture and returns the server to listening state
     */
    private void releaseTalk()
    {
        finishMusic(true);
        talkingRequested = false;
        talkingConfirmed = false;
        stopCapture();
        if (attached && authenticated)
        {
            requestAudioState(2);
        }
        publish(attached ? idleStatus() : lastStatus);
    }

    /**
     * stops the current device without waiting on the session worker
     */
    private void stopCapture()
    {
        AudioEngine active = audio;
        if (active != null)
        {
            active.stopCapture();
        }
    }

    /**
     * retires device callbacks before closing the audio workers
     */
    private void closeAudio()
    {
        deviceGeneration++;
        AudioEngine active = audio;
        audio = null;
        if (active != null)
        {
            active.close();
        }
    }

    /**
     * closes media without resetting the connection's frame counters
     */
    private void dropSocket()
    {
        if (rtcTimer != null)
        {
            rtcTimer.cancel(false);
            rtcTimer = null;
        }
        finishMusic(false);
        generation++;
        talkingRequested = false;
        talkingConfirmed = false;
        attached = false;
        requestedAudioState = -1;
        pendingUntil = 0;
        retryAfter = SystemClock.elapsedRealtime() + 3000;
        authKey = null;
        ticket = null;
        BoundedWebSocket previous = socket;
        socket = null;
        WebRtcTransport previousRtc = webRtc;
        webRtc = null;
        if (previousRtc != null)
        {
            previousRtc.close();
        }
        if (previous != null)
        {
            previous.close();
        }
        closeAudio();
    }

    /**
     * @return whether this channel can accept a microphone request
     */
    private boolean ready()
    {
        return !closed.get() && activeEpoch == requestedEpoch.get() && authenticated && attached && voiceAllowed && channelVoiceAllowed && channelKeys != null && (audio == null || audio.hasFocus());
    }

    /**
     * @return listening state for the current channel
     */
    private String idleStatus()
    {
        if (audioInterrupted || (audio != null && !audio.hasFocus()))
        {
            return "Audio paused by another app";
        }
        if (!voiceAllowed || !channelVoiceAllowed)
        {
            return "Voice is disabled in this channel";
        }
        if (channelKeys == null)
        {
            return "Waiting for channel keys";
        }
        return "Hold to talk";
    }

    /**
     * @param value generation captured by a socket callback
     * @return whether the callback belongs to the active socket
     */
    private boolean current(long value)
    {
        return !closed.get() && authenticated && activeEpoch == requestedEpoch.get() && (socket != null || webRtc != null) && generation == value;
    }

    private boolean sendFrame(byte[] frame)
    {
        if (closed.get() || !authenticated || activeEpoch != requestedEpoch.get())
        {
            return false;
        }
        return webRtc != null ? webRtc.send(frame) : socket != null && socket.sendBinary(frame);
    }

    private void failTransport()
    {
        if (transport.equals(options.transport))
        {
            preferredFailedUntil = SystemClock.elapsedRealtime() + 60000;
        }
        dropSocket();
        publish("Audio connection lost. Trying another allowed connection…");
    }

    private void pollWebRtc()
    {
        WebRtcTransport current = webRtc;
        if (current == null || !authenticated || activeEpoch != requestedEpoch.get())
        {
            return;
        }
        for (int count = 0; count < 32 && current == webRtc; count++)
        {
            WebRtcTransport.Event event = current.poll();
            if (event == null)
            {
                return;
            }
            try
            {
                if (event.type == WebRtcTransport.DESCRIPTION)
                {
                    JSONObject value = new JSONObject().put("type", event.detail).put("sdp", event.text());
                    if (!sendScopedControl(new JSONObject().put("type", "sdp_answer").put("value", value)))
                    {
                        failTransport();
                    }
                }
                else if (event.type == WebRtcTransport.CANDIDATE)
                {
                    JSONObject value = new JSONObject().put("candidate", event.text()).put("sdpMid", event.detail).put("sdpMLineIndex", 0);
                    if (!sendScopedControl(new JSONObject().put("type", "ice_candidate").put("value", value)))
                    {
                        failTransport();
                    }
                }
                else if (event.type == WebRtcTransport.OPEN && !attached)
                {
                    attached = true;
                    pendingUntil = 0;
                    lastReceived = SystemClock.elapsedRealtime();
                    requestAudioState(2);
                    if (audioWanted() && ensureAudio())
                    {
                        publish(idleStatus());
                    }
                }
                else if (event.type == WebRtcTransport.FRAME && attached)
                {
                    receive(event.payload);
                }
                else if (event.type == WebRtcTransport.CLOSED)
                {
                    failTransport();
                }
            }
            catch (JSONException | RuntimeException error)
            {
                failTransport();
            }
        }
    }

    private void beginMusic()
    {
        Uri selected = pendingMusic;
        pendingMusic = null;
        long origin = generation;
        long song = ++musicGeneration;
        try
        {
            Mp3Streamer created = new Mp3Streamer(context, selected, new Mp3Streamer.Listener()
            {
                @Override
                public boolean onReady()
                {
                    CompletableFuture<Boolean> result = new CompletableFuture<>();
                    timer.schedule(() ->
                    {
                        if (!dispatch(() ->
                        {
                            if (!current(origin) || song != musicGeneration || !streamingMusic || !talkingConfirmed)
                            {
                                result.complete(false);
                                return;
                            }
                            try
                            {
                                result.complete(sendScopedControl(new JSONObject().put("type", "start_song_stream").put("song_name", streamedName)));
                                publish("Streaming " + streamedName);
                            }
                            catch (JSONException error)
                            {
                                result.complete(false);
                            }
                        }, () -> result.complete(false)))
                        {
                            result.complete(false);
                        }
                    }, 150, TimeUnit.MILLISECONDS);
                    try
                    {
                        return result.get(5, TimeUnit.SECONDS);
                    }
                    catch (Exception error)
                    {
                        return false;
                    }
                }

                @Override
                public boolean onPacket(byte[] packet)
                {
                    if (pendingCaptureFrames.incrementAndGet() > 4)
                    {
                        pendingCaptureFrames.decrementAndGet();
                        return false;
                    }
                    if (!dispatch(() ->
                    {
                        try
                        {
                            if (current(origin) && song == musicGeneration && streamingMusic && talkingRequested && talkingConfirmed && ready())
                            {
                                if (!sendEncoded(packet))
                                {
                                    releaseTalk();
                                    publish("The connection could not keep up with the music stream");
                                }
                            }
                        }
                        finally
                        {
                            pendingCaptureFrames.decrementAndGet();
                        }
                    }, () -> pendingCaptureFrames.decrementAndGet()))
                    {
                        pendingCaptureFrames.decrementAndGet();
                        return false;
                    }
                    return true;
                }

                @Override
                public void onFinished(String error)
                {
                    dispatch(() ->
                    {
                        if (current(origin) && song == musicGeneration && streamingMusic)
                        {
                            releaseTalk();
                            if (error != null)
                            {
                                publish(error);
                            }
                        }
                    });
                }
            });
            synchronized (lifecycle)
            {
                if (!current(origin) || !talkingRequested)
                {
                    created.close();
                    return;
                }
                music = created;
                created.start();
            }
        }
        catch (RuntimeException failure)
        {
            releaseTalk();
            publish("The selected music could not start");
        }
    }

    private void finishMusic(boolean notifyServer)
    {
        boolean wasStreaming = streamingMusic;
        musicGeneration++;
        streamingMusic = false;
        pendingMusic = null;
        streamedName = "";
        Mp3Streamer active = music;
        music = null;
        if (active != null)
        {
            active.close();
        }
        if (wasStreaming && notifyServer && authenticated)
        {
            sendControl("stop_song_stream", null, 0);
        }
    }

    /**
     * avoids sending the same microphone state after each model update
     *
     * @param state requested microphone state
     * @return whether this state is already pending or was queued
     */
    private boolean requestAudioState(int state)
    {
        if (requestedAudioState == state)
        {
            return true;
        }
        if (!sendControl("microphone_usage", "value", state))
        {
            return false;
        }
        requestedAudioState = state;
        return true;
    }

    /**
     * sends one audio control through authenticated chat metadata
     *
     * @param type protocol message type
     * @param field optional numeric field name
     * @param value numeric field value
     * @return whether the chat connection accepted the message
     */
    private boolean sendControl(String type, String field, int value)
    {
        try
        {
            JSONObject message = new JSONObject().put("type", type);
            if (field != null)
            {
                message.put(field, value);
            }
            return sendScopedControl(message);
        }
        catch (JSONException failure)
        {
            return false;
        }
    }

    private boolean sendScopedControl(JSONObject message)
    {
        return authenticated && activeEpoch == requestedEpoch.get() && control.send(chatConnectionId, message);
    }

    /**
     * publishes changed audio status with its chat connection
     *
     * @param text user facing audio state
     */
    private void publish(String text)
    {
        if (closed.get())
        {
            return;
        }
        boolean ready = ready();
        boolean talking = talkingConfirmed && talkingRequested;
        if (!text.equals(lastStatus) || ready != lastReady || talking != lastTalking)
        {
            lastStatus = text;
            lastReady = ready;
            lastTalking = talking;
            listener.onVoiceStatus(chatConnectionId, text, ready, talking);
        }
    }

    /**
     * queues work only while the controller owns a live executor
     *
     * @param action callback to serialize
     * @return whether the callback was queued
     */
    private boolean dispatch(Runnable action)
    {
        return dispatch(action, () ->
        {
        });
    }

    private boolean dispatch(Runnable action, Runnable discarded)
    {
        if (closed.get() || requestedConnectionId < 0)
        {
            return false;
        }
        try
        {
            long epoch = requestedEpoch.get();
            worker.execute(() ->
            {
                if (closed.get() || epoch != requestedEpoch.get())
                {
                    discarded.run();
                    return;
                }
                try
                {
                    action.run();
                }
                catch (RuntimeException failure)
                {
                    disconnectOnWorker();
                    publish("Audio stopped after a processing error");
                }
            });
            return true;
        }
        catch (RejectedExecutionException ignored)
        {
            return false;
        }
    }

    /**
     * validates the four music bot keys
     *
     * @param values key array from authenticated metadata
     * @return four decoded AES keys
     * @throws JSONException when a key is not text
     */
    private static byte[][] readBotKeys(JSONArray values) throws JSONException
    {
        if (values.length() != 4)
        {
            throw new IllegalArgumentException("Invalid music keys");
        }
        byte[][] keys = new byte[4][32];
        for (int index = 0; index < 4; index++)
        {
            String value = values.getString(index);
            if (value.length() != 64)
            {
                throw new IllegalArgumentException("Invalid music key size");
            }
            for (int offset = 0; offset < 32; offset++)
            {
                int high = Character.digit(value.charAt(offset * 2), 16);
                int low = Character.digit(value.charAt(offset * 2 + 1), 16);
                if (high < 0 || low < 0)
                {
                    throw new IllegalArgumentException("Invalid music key");
                }
                keys[index][offset] = (byte)((high << 4) | low);
            }
        }
        return keys;
    }

    /**
     * retires queued callbacks and closes devices before the worker stops
     */
    @Override
    public void close()
    {
        synchronized (lifecycle)
        {
            if (!closed.compareAndSet(false, true))
            {
                return;
            }
            requestedConnectionId = -1;
            long epoch = requestedEpoch.incrementAndGet();
            talkingRequested = false;
            stopCapture();
            Mp3Streamer active = music;
            if (active != null)
            {
                active.close();
            }
            timer.shutdownNow();
            pendingState.set(null);
            replaceConnection(new Connection(epoch, -1, null, -1, null));
        }
    }
}
