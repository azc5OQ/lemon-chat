package com.lemonchat.protocol;

import com.lemonchat.audio.VoiceController;
import com.lemonchat.crypto.CryptoSession;
import com.lemonchat.crypto.Identity;
import com.lemonchat.model.Channel;
import com.lemonchat.model.AdminGroup;
import com.lemonchat.model.AdminSettings;
import com.lemonchat.model.AdminState;
import com.lemonchat.model.PermissionDefinition;
import com.lemonchat.model.CallInvitation;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.model.ReplyQuote;
import com.lemonchat.model.Attachment;
import java.util.Collections;
import java.util.Arrays;
import com.lemonchat.model.Group;
import com.lemonchat.model.Member;
import com.lemonchat.model.PokeMessage;
import com.lemonchat.model.ServerIcon;
import com.lemonchat.model.SessionSnapshot;
import com.lemonchat.model.SessionSnapshot.Status;
import java.net.URI;
import java.util.ArrayList;
import java.util.ArrayDeque;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** owns one server connection and its state on a serial worker */
public final class ChatSession implements AutoCloseable
{
    public interface Listener
    {
        void onState(SessionSnapshot state);
        void onMessage(ChatMessage message);
        void onError(String message);

        default void onCall(CallInvitation invitation, String reason)
        {
        }

        default void onPoke(PokeMessage message)
        {
        }
    }

    private static final int MESSAGE_LIMIT = 4 * 1024 * 1024;
    private static final int STATE_LIMIT = 10000;
    private static final int TEXT_LIMIT = 65536;
    private static final int ICON_CHARACTERS_LIMIT = 2 * 1024 * 1024;
    private static final long EXACT_INTEGER_LIMIT = 9007199254740991L;
    private final Executor callbacks;
    private final ScheduledThreadPoolExecutor worker = new ScheduledThreadPoolExecutor(1);
    private ScheduledFuture<?> tickTask;
    private long tickAt;
    private final Semaphore commandSlots = new Semaphore(256);
    private final AtomicLong wantedConnection = new AtomicLong();
    private final AtomicBoolean closed = new AtomicBoolean();
    private final AtomicInteger pendingFrames = new AtomicInteger();
    private final AtomicLong pendingBytes = new AtomicLong();
    private final AtomicLong overloadedConnection = new AtomicLong(-1);
    private final AtomicReference<Runnable> connectionIntent = new AtomicReference<>();
    private final AtomicBoolean intentScheduled = new AtomicBoolean();
    private final Map<Integer, Channel> channels = new LinkedHashMap<>();
    private final Map<Integer, Member> members = new LinkedHashMap<>();
    private final Map<Integer, Group> serverGroups = new LinkedHashMap<>();
    private final Map<Integer, Group> channelGroups = new LinkedHashMap<>();
    private final Map<Long, ServerIcon> icons = new LinkedHashMap<>();
    private int iconCharacters;
    private final Map<Long, ChatMessage> pendingMessages = new LinkedHashMap<>();
    private final ChannelKeyState keys = new ChannelKeyState();
    private final Object callbackLock = new Object();
    private final Object mediaLock = new Object();
    private final ArrayDeque<CallbackTask> callbackQueue = new ArrayDeque<>();
    private Runnable pendingStateCallback;
    private long callbackBytes;
    private boolean callbackScheduled;
    private volatile Listener listener;
    private volatile SessionSnapshot published;
    private AutomaticWebSocket socket;
    private CryptoSession crypto;
    private volatile VoiceController voice;
    private URI endpoint;
    private JSONObject authenticationPolicy;
    private long connectionId;
    private long nextMessageId;
    private int phase;
    private int clientId = -1;
    private int channelId = -1;
    private int voiceClientId = -1;
    private String username = "";
    private String publicKey = "";
    private String publicMix = "";
    private Status status = Status.DISCONNECTED;
    private String detail = "Choose a server to connect";
    private long loginDeadline;
    private long lastKeepalive;
    private long lastReply;
    private long lastContentError;
    private int pendingChannel = -1;
    private long channelJoinDeadline;
    private boolean idleAllowed;
    private boolean connectionCheckEnforced = true;
    private Boolean desiredIdle;
    private int idleRequest = -1;
    private long idleDeadline;
    private int returnChannel;
    private int resumeChannel = -1;
    private String resumePassword = "";
    private long lastControlSent;
    private long lastCallSent;
    private long nextIncomingCall;
    private CallInvitation call;
    private volatile boolean mediaAllowed;
    private boolean retryableFailure;
    private final ChatFeatures features = new ChatFeatures(this);
    private final AdminController administration = new AdminController(new AdminController.Host()
    {
        @Override
        public void send(JSONObject message)
        {
            ChatSession.this.send(message);
        }

        @Override
        public long now()
        {
            return ChatSession.now();
        }

        @Override
        public long lastSent()
        {
            return lastControlSent;
        }

        @Override
        public SessionSnapshot state()
        {
            return snapshot();
        }
    });
    private final AtomicBoolean preparingAttachment = new AtomicBoolean();

    /**
     * creates the session worker
     *
     * @param callbackExecutor executor used for listener calls
     */
    public ChatSession(Executor callbackExecutor)
    {
        callbacks = callbackExecutor;
        worker.setRemoveOnCancelPolicy(true);
        worker.setExecuteExistingDelayedTasksAfterShutdownPolicy(false);
        published = snapshot();
    }

    /**
     * replaces the observer and immediately publishes current state
     *
     * @param value observer, or null to detach
     */
    public void setListener(Listener value)
    {
        listener = value;
        if (value != null)
        {
            SessionSnapshot state = published;
            queueCallback(() ->
            {
                if (listener == value && published == state)
                {
                    value.onState(state);
                }
            }, 0, true);
        }
    }

    /** @return the last immutable session state */
    public SessionSnapshot state()
    {
        return published;
    }

    /** @return the public key of the currently connected local identity */
    public String localPublicKey()
    {
        SessionSnapshot selected = published;
        for (Member member : selected.members)
        {
            if (member.id == selected.clientId)
            {
                return member.publicKey;
            }
        }
        return "";
    }

    /**
     * applies local protocol choices without reconnecting
     *
     * @param options validated local choices
     */
    public void setOptions(ClientOptions options)
    {
        if (options != null)
        {
            enqueue(() ->
            {
                features.setOptions(options);
                if (voice != null)
                {
                    voice.setTransportPreference(options.audioTransport);
                }
            });
        }
    }

    /**
     * sends text with a quote from this channel
     *
     * @param text message body
     * @param reply message being quoted, or null
     */
    public void sendChannelMessage(String text, ChatMessage reply)
    {
        SessionSnapshot selected = published;
        enqueue(() -> sendText(selected, null, text, reply));
    }

    /**
     * sends text with a quote from this private conversation
     *
     * @param target online recipient
     * @param text message body
     * @param reply message being quoted, or null
     */
    public void sendDirectMessage(int target, String text, ChatMessage reply)
    {
        SessionSnapshot selected = published;
        Member captured = selectedMember(selected, target);
        enqueue(() ->
        {
            if (captured == null)
            {
                error("That person is no longer connected");
                return;
            }
            sendText(selected, captured, text, reply);
        });
    }

    public void sendChannelText(String text, ChatMessage reply)
    {
        sendChannelMessage(text, reply);
    }

    public void sendDirectText(int target, String text, ChatMessage reply)
    {
        sendDirectMessage(target, text, reply);
    }

    /**
     * starts one bounded attachment upload for this channel
     *
     * @param name original filename
     * @param mime content type
     * @param bytes complete file bytes
     * @param picture whether to use the picture protocol
     */
    public void sendChannelAttachment(String name, String mime, byte[] bytes, boolean picture)
    {
        sendAttachment(-1, name, mime, bytes, picture);
    }

    /**
     * starts one bounded private attachment upload
     *
     * @param target online recipient
     * @param name original filename
     * @param mime content type
     * @param bytes complete file bytes
     * @param picture whether to use the picture protocol
     */
    public void sendDirectAttachment(int target, String name, String mime, byte[] bytes, boolean picture)
    {
        if (target < 0)
        {
            return;
        }
        sendAttachment(target, name, mime, bytes, picture);
    }

    private void sendAttachment(int target, String name, String mime, byte[] bytes, boolean picture)
    {
        if (bytes == null || bytes.length > Attachment.MAX_BYTES || !preparingAttachment.compareAndSet(false, true))
        {
            enqueue(() -> error("Choose an attachment no larger than 4 MiB and wait for the current upload"));
            return;
        }
        SessionSnapshot selected = published;
        Member captured = target < 0 ? null : selectedMember(selected, target);
        byte[] copy = bytes.clone();
        if (!enqueue(() ->
        {
            try
            {
                if (target >= 0 && captured == null)
                {
                    error("That person is no longer connected");
                    return;
                }
                features.attachments.sendAttachment(selected, captured, name, mime, copy, picture);
            }
            finally
            {
                Arrays.fill(copy, (byte)0);
                preparingAttachment.set(false);
            }
        }))
        {
            Arrays.fill(copy, (byte)0);
            preparingAttachment.set(false);
        }
    }

    /**
     * changes this identity's reaction to a live message
     *
     * @param message message in the current connection
     * @param emojiId actual server catalog identifier
     */
    public void reactToMessage(ChatMessage message, String emojiId)
    {
        if (message != null)
        {
            featureAction(() -> features.react(message, emojiId));
        }
    }

    /**
     * marks messages read after their conversation is visible
     *
     * @param conversationId the displayed private conversation
     */
    public void markConversationRead(String conversationId)
    {
        if (conversationId != null)
        {
            featureAction(() -> features.read(conversationId));
        }
    }

    /** @param clientIds visible members whose avatars are needed */
    public void requestAvatars(int[] clientIds)
    {
        if (clientIds != null && clientIds.length <= 100)
        {
            int[] copy = clientIds.clone();
            featureAction(() -> features.requestAvatars(copy));
        }
    }

    /** @param emojiId actual server catalog identifier */
    public void requestEmoji(String emojiId)
    {
        featureAction(() -> features.requestEmoji(emojiId));
    }

    /** @param dataUri bounded raster image data URI */
    public void uploadAvatar(String dataUri)
    {
        if (dataUri != null && dataUri.length() <= 123000)
        {
            featureAction(() -> features.uploadAvatar(dataUri));
        }
        else
        {
            enqueue(() -> error("The avatar exceeds the 90 KiB limit"));
        }
    }

    /** removes the current identity's avatar */
    public void deleteAvatar()
    {
        featureAction(features::deleteAvatar);
    }

    private void featureAction(Runnable action)
    {
        long origin = wantedConnection.get();
        enqueue(() ->
        {
            if (isCurrent(origin) && status == Status.CONNECTED)
            {
                action.run();
                publish();
            }
        });
    }

    private interface AdminAction
    {
        void run() throws JSONException;
    }

    private boolean adminAction(long origin, AdminAction action)
    {
        if (closed.get() || origin != wantedConnection.get() || !state().connected)
        {
            return false;
        }
        return enqueue(() ->
        {
            if (!isCurrent(origin) || status != Status.CONNECTED)
            {
                return;
            }
            try
            {
                action.run();
            }
            catch (IllegalArgumentException | JSONException invalid)
            {
                administration.problem(invalid.getMessage());
            }
            publish();
        });
    }

    public boolean adminLogin(long origin, String password)
    {
        return adminAction(origin, () -> administration.login(password));
    }

    public boolean loadAdministration(long origin)
    {
        return adminAction(origin, administration::load);
    }

    public boolean saveServerSettings(long origin, AdminSettings original, Map<String, Object> values, boolean includeMusic)
    {
        Map<String, Object> copy = new LinkedHashMap<>(values);
        return adminAction(origin, () -> administration.save(original, copy, includeMusic));
    }

    public boolean changeAdminPassword(long origin, String password)
    {
        return adminAction(origin, () -> administration.changePassword(password));
    }

    public boolean createAdminGroup(long origin, String name, String type, boolean channelGroup)
    {
        return adminAction(origin, () -> administration.createGroup(name, type, channelGroup));
    }

    public boolean renameAdminGroup(long origin, AdminGroup original, String name)
    {
        return adminAction(origin, () -> administration.renameGroup(original, name));
    }

    public boolean deleteAdminGroup(long origin, AdminGroup original)
    {
        return adminAction(origin, () -> administration.deleteGroup(original));
    }

    public boolean saveAdminPermissions(long origin, AdminGroup original, PermissionDefinition edited)
    {
        return adminAction(origin, () -> administration.permissions(original, edited));
    }

    public boolean assignAdminGroup(long origin, AdminGroup original, Member member, boolean add)
    {
        return adminAction(origin, () -> administration.assign(original, member, add));
    }

    public boolean saveAdminGroups(long origin)
    {
        return adminAction(origin, administration::saveGroups);
    }

    private static Member selectedMember(SessionSnapshot selected, int target)
    {
        for (Member member : selected.members)
        {
            if (member.id == target)
            {
                return member;
            }
        }
        return null;
    }

    SessionSnapshot featureState()
    {
        return snapshot();
    }

    CryptoSession featureCrypto()
    {
        return crypto;
    }

    String featureKey()
    {
        return publicKey;
    }

    String featureKeys()
    {
        return keys.current;
    }

    String featurePreviousKeys()
    {
        return keys.previous;
    }

    long featureNextId()
    {
        return ++nextMessageId;
    }

    Member featureMember(int id)
    {
        return members.get(id);
    }

    Member featurePeer(String conversation)
    {
        for (Member member : members.values())
        {
            if (!member.musicBot && member.id != clientId && !member.publicKey.isEmpty() && member.conversationId.equals(conversation))
            {
                return member;
            }
        }
        return null;
    }

    boolean featureCurrent(Member captured)
    {
        Member member = members.get(captured.id);
        return member != null && !member.musicBot && member.id != clientId && !member.publicKey.isEmpty() && member.publicKey.equals(captured.publicKey);
    }

    void featureProfile(Member member)
    {
        members.put(member.id, member);
    }

    void featureSend(JSONObject message)
    {
        send(message);
    }

    JSONObject featureDirect(Member recipient, String purpose, String content, long id)
    {
        return direct(recipient, purpose, content, id);
    }

    void featurePending(ChatMessage message)
    {
        if (pendingMessages.size() >= 256)
        {
            Long first = pendingMessages.keySet().iterator().next();
            emit(pendingMessages.remove(first).withDelivery("Not confirmed", -1));
        }
        pendingMessages.put(message.id, message);
    }

    void featureEmit(ChatMessage message)
    {
        if ("Not sent".equals(message.delivery))
        {
            pendingMessages.remove(message.id);
        }
        emit(message);
    }

    void featureError(String message)
    {
        error(message);
    }

    /** @return the bounded serial executor used by the audio controller */
    public Executor executor()
    {
        return action ->
        {
            if (!enqueue(action))
            {
                throw new RejectedExecutionException("The session worker cannot accept another operation");
            }
        };
    }

    /**
     * attaches the native voice controller
     *
     * @param controller controller owned by the application service
     */
    public void setVoiceController(VoiceController controller)
    {
        enqueue(() ->
        {
            if (voice != null && voice != controller)
            {
                voice.close();
            }
            voice = controller;
            voiceClientId = -1;
            if (voice != null)
            {
                voice.setTransportPreference(features.options().audioTransport);
            }
            updateVoice();
        });
    }

    /**
     * starts a fresh connection with the selected identity
     *
     * @param address explicit ws or wss URL
     * @param name requested display name
     * @param identity persistent identity to use
     * @param serverKeys optional server encryption settings
     */
    public void connect(String address, String name, Identity identity, String serverKeys)
    {
        long requested = wantedConnection.incrementAndGet();
        enqueueConnectionIntent(() -> begin(requested, address, name, identity, serverKeys));
    }

    /** disconnects without replacing the saved identity */
    public void disconnect()
    {
        long requested = wantedConnection.incrementAndGet();
        enqueueConnectionIntent(() ->
        {
            stopConnection();
            connectionId = requested;
        administration.reset(requested);
            status = Status.DISCONNECTED;
            detail = "Disconnected";
            publish();
        });
    }

    /**
     * asks the server to move this client
     *
     * @param target channel identifier
     * @param password password for this request
     */
    public void joinChannel(int target, String password)
    {
        long requested = wantedConnection.get();
        enqueue(() ->
        {
            if (!isCurrent(requested) || status != Status.CONNECTED)
            {
                error("Connect before choosing a channel");
                return;
            }
            if (!channels.containsKey(target) || password == null || password.codePointCount(0, password.length()) > 30)
            {
                error("Choose a valid channel and password");
                return;
            }
            if (localIdle() || idleRequest == 1)
            {
                resumeChannel = target;
                resumePassword = password;
                desiredIdle = false;
                finishCall("cancelled");
                return;
            }
            if (target == channelId)
            {
                return;
            }
            JSONObject message = body("join_channel_request");
            put(message, "channel_id", target);
            put(message, "channel_password", password);
            send(message);
            pendingChannel = target;
            channelJoinDeadline = now() + 10000;
        });
    }

    /** asks the server to keep this client available for calls without channel media */
    public void enterIdle()
    {
        long origin = wantedConnection.get();
        enqueue(() ->
        {
            if (isCurrent(origin) && status == Status.CONNECTED && idleAllowed && clientId >= 0)
            {
                desiredIdle = true;
                setTalking(false);
            }
        });
    }

    /** returns to the previous channel unless an incoming invitation needs an answer */
    public void leaveIdle()
    {
        long origin = wantedConnection.get();
        enqueue(() ->
        {
            if (isCurrent(origin) && status == Status.CONNECTED && call == null)
            {
                desiredIdle = false;
            }
        });
    }

    /**
     * invites an idle person into the current channel
     *
     * @param target idle client identifier
     */
    public void callMember(int target)
    {
        SessionSnapshot selected = published;
        Member captured = findMember(selected, target);
        enqueue(() ->
        {
            Member current = currentRecipient(selected, captured);
            if (current == null || !current.idle || !idleAllowed || localIdle() || channelId < 0)
            {
                error("Only an idle person can be called from your current channel");
                return;
            }
            if (now() - lastCallSent < 1000)
            {
                return;
            }
            JSONObject message = body("call_idle_client_request");
            put(message, "client_id", current.id);
            send(message);
            lastCallSent = now();
        });
    }

    /**
     * accepts the current invitation without trusting a notification's channel identifier
     *
     * @param callId invitation token
     * @param origin connection that owns the token
     */
    public void acceptCall(String callId, long origin)
    {
        enqueue(() ->
        {
            if (!validCall(callId, origin))
            {
                return;
            }
            if (call.channelId < 0)
            {
                finishCall("cancelled");
                error("The caller's channel is hidden. Ask them to call from a visible channel");
                return;
            }
            resumeChannel = call.channelId;
            resumePassword = "";
            desiredIdle = false;
            finishCall("accepted");
        });
    }

    /**
     * dismisses only the matching invitation and leaves the client idle
     *
     * @param callId invitation token
     * @param origin connection that owns the token
     */
    public void declineCall(String callId, long origin)
    {
        enqueue(() ->
        {
            if (validCall(callId, origin))
            {
                desiredIdle = true;
                finishCall("declined");
            }
        });
    }

    /**
     * sends a short attention message using the server's poke protocol
     *
     * @param target recipient client identifier
     * @param text attention message, visible to the server
     */
    public void sendPoke(int target, String text)
    {
        SessionSnapshot selected = published;
        Member captured = findMember(selected, target);
        enqueue(() ->
        {
            Member current = currentRecipient(selected, captured);
            if (current == null || text == null || text.trim().isEmpty() || text.length() > 16384)
            {
                error("Choose a connected person and a shorter poke message");
                return;
            }
            JSONObject message = body("poke_client");
            put(message, "client_id", current.id);
            put(message, "poke_message", text);
            send(message);
        });
    }

    private static Member findMember(SessionSnapshot state, int id)
    {
        for (Member member : state.members)
        {
            if (member.id == id)
            {
                return member;
            }
        }
        return null;
    }

    /** @return whether the server has confirmed local idle mode */
    private boolean localIdle()
    {
        Member local = members.get(clientId);
        return local != null && local.idle;
    }

    /** sends one presence change and waits for its server confirmation */
    private void applyIdleIntent()
    {
        if (idleRequest >= 0)
        {
            if (now() >= idleDeadline)
            {
                idleRequest = -1;
                desiredIdle = null;
                resumeChannel = -1;
                resumePassword = "";
                pendingChannel = -1;
                error("The server did not confirm the presence change. Try again");
            }
            return;
        }
        if (desiredIdle == null || clientId < 0)
        {
            return;
        }
        if (desiredIdle == localIdle())
        {
            desiredIdle = null;
            return;
        }
        if (now() - lastControlSent < 150 || call != null)
        {
            return;
        }
        if (desiredIdle)
        {
            if (!idleAllowed)
            {
                desiredIdle = null;
                return;
            }
            idleRequest = 1;
            send(body("go_to_idle_mode_request"));
        }
        else
        {
            int target = resumeChannel >= 0 ? resumeChannel : returnChannel;
            if (!channels.containsKey(target))
            {
                target = 0;
            }
            JSONObject message = body("come_back_from_idle_mode_request");
            put(message, "channel_id", target);
            put(message, "channel_password", resumePassword);
            idleRequest = 0;
            resumeChannel = target;
            pendingChannel = target;
            channelJoinDeadline = now() + 10000;
            send(message);
        }
        idleDeadline = now() + 10000;
    }

    private void receiveCall(JSONObject message) throws JSONException
    {
        Member caller = members.get(id(message, "caller_client_id"));
        if (!localIdle() || !idleAllowed || call != null || now() < nextIncomingCall || caller == null || caller.id == clientId || caller.idle || caller.musicBot || caller.publicKey.isEmpty() || (caller.channelId >= 0 && !channels.containsKey(caller.channelId)))
        {
            return;
        }
        call = new CallInvitation(UUID.randomUUID().toString(), connectionId, System.currentTimeMillis() + 60000, System.nanoTime() + TimeUnit.SECONDS.toNanos(60), caller.id, caller.channelId, caller.name, caller.publicKey);
        desiredIdle = null;
        nextIncomingCall = now() + 5000;
        publish();
        emitCall(call, "incoming");
    }

    private boolean validCall(String token, long origin)
    {
        validateCurrentCall();
        return isCurrent(origin) && status == Status.CONNECTED && call != null && call.connectionId == origin && call.callId.equals(token);
    }

    private void validateCurrentCall()
    {
        if (call == null)
        {
            return;
        }
        Member caller = members.get(call.callerId);
        if (call.expired())
        {
            finishCall("expired");
        }
        else if (!localIdle() || caller == null || caller.idle || caller.musicBot || caller.channelId != call.channelId || !caller.publicKey.equals(call.callerPublicKey) || (call.channelId >= 0 && !channels.containsKey(call.channelId)))
        {
            finishCall("cancelled");
        }
    }

    private void finishCall(String reason)
    {
        if (call == null)
        {
            return;
        }
        CallInvitation ended = call;
        call = null;
        publish();
        emitCall(ended, reason);
    }

    private void emitCall(CallInvitation invitation, String reason)
    {
        Listener target = listener;
        if (target != null)
        {
            queueCallback(() ->
            {
                if (listener == target && invitation.connectionId == wantedConnection.get() && (!reason.equals("incoming") || published.call == invitation))
                {
                    target.onCall(invitation, reason);
                }
            }, 1024 + invitation.callerPublicKey.length() * 2, false);
        }
    }

    private void receivePoke(JSONObject message) throws JSONException
    {
        Member sender = members.get(id(message, "client_id"));
        Object value = message.opt("poke_message");
        if (sender == null || sender.id == clientId || sender.musicBot || !(value instanceof String) || ((String) value).trim().isEmpty() || ((String) value).length() > 16384)
        {
            return;
        }
        PokeMessage poke = new PokeMessage(connectionId, sender, (String) value, System.currentTimeMillis());
        Listener target = listener;
        if (target != null)
        {
            queueCallback(() ->
            {
                if (listener == target && poke.connectionId == wantedConnection.get())
                {
                    target.onPoke(poke);
                }
            }, 1024 + poke.text.length() * 2 + poke.senderPublicKey.length() * 2, false);
        }
    }

    private Member currentRecipient(SessionSnapshot selected, Member captured)
    {
        Member current = captured == null ? null : members.get(captured.id);
        return selected.connectionId == connectionId && isCurrent(connectionId) && status == Status.CONNECTED && current != null && current.id != clientId && !current.musicBot && !current.publicKey.isEmpty() && current.publicKey.equals(captured.publicKey) ? current : null;
    }

    /**
     * sends encrypted text to the channel shown when this call was made
     *
     * @param text plain message text
     */
    public void sendChannelMessage(String text)
    {
        SessionSnapshot selected = published;
        enqueue(() -> sendText(selected, null, text, null));
    }

    /**
     * sends encrypted text to the selected person's current identity
     *
     * @param target online client identifier
     * @param text plain message text
     */
    public void sendDirectMessage(int target, String text)
    {
        SessionSnapshot selected = published;
        Member recipient = null;
        for (Member member : selected.members)
        {
            if (member.id == target)
            {
                recipient = member;
                break;
            }
        }
        Member captured = recipient;
        enqueue(() ->
        {
            if (captured == null)
            {
                error("That person is no longer connected");
                return;
            }
            sendText(selected, captured, text, null);
        });
    }

    /**
     * updates the push to talk button state
     *
     * @param talking whether the user is holding the button
     */
    public void setTalking(boolean talking)
    {
        VoiceController active = voice;
        if (active != null)
        {
            active.setTalking(talking && mediaAllowed);
        }
    }

    /**
     * enables media only after the service has a valid foreground media state
     *
     * @param allowed whether channel media may run
     */
    public void setMediaAllowed(boolean allowed)
    {
        synchronized (mediaLock)
        {
            mediaAllowed = allowed;
            if (!allowed && voice != null)
            {
                voice.onDisconnected();
            }
        }
        enqueue(() ->
        {
            if (!allowed)
            {
                voiceClientId = -1;
            }
            updateVoice();
        });
    }

    /**
     * updates microphone permission without starting capture
     *
     * @param available whether microphone permission is granted
     */
    public void setMicrophoneAvailable(boolean available)
    {
        VoiceController active = voice;
        if (active != null)
        {
            active.setMicrophoneAvailable(available);
        }
    }

    /**
     * sends an audio control through the authenticated metadata channel
     *
     * @param message inner protocol message
     * @return whether the control was accepted by the session worker
     */
    public boolean sendControl(JSONObject message)
    {
        return sendControl(wantedConnection.get(), message);
    }

    /**
     * queues a media control only for the connection that created it
     *
     * @param origin owning connection generation
     * @param message inner protocol message
     * @return whether the current session accepted the queued control
     */
    public boolean sendControl(long origin, JSONObject message)
    {
        if (message == null || origin != wantedConnection.get() || published.connectionId != origin || published.status != Status.CONNECTED)
        {
            return false;
        }
        String copy = message.toString();
        return enqueue(() ->
        {
            if (isCurrent(origin) && status == Status.CONNECTED)
            {
                send(parse(copy));
            }
        });
    }

    private void begin(long requested, String address, String name, Identity identity, String serverKeys)
    {
        if (requested != wantedConnection.get())
        {
            return;
        }
        stopConnection();
        connectionId = requested;
        administration.reset(requested);
        status = Status.CONNECTING;
        detail = "Preparing your identity";
        publish();
        try
        {
            endpoint = validateEndpoint(address);
            if (identity == null || name == null || name.trim().isEmpty() || name.codePointCount(0, name.length()) > 50)
            {
                throw new IllegalArgumentException("Enter a name with at most 50 characters");
            }
            username = name.trim();
            crypto = new CryptoSession(identity, serverKeys == null ? "" : serverKeys);
            publicKey = crypto.getPublicKey();
            publicMix = crypto.beginDh();
            if (!isCurrent(requested))
            {
                stopConnection();
                return;
            }
            loginDeadline = now() + 30_000;
            socket = new AutomaticWebSocket(endpoint, MESSAGE_LIMIT, new BoundedWebSocket.Listener()
            {
                @Override
                public void onOpen()
                {
                    enqueue(() -> opened(requested));
                }

                @Override
                public void onText(String text)
                {
                    receive(requested, text);
                }

                @Override
                public void onBinary(byte[] bytes)
                {
                    enqueue(() -> failCurrent(requested, "Unexpected binary frame on the chat connection"));
                }

                @Override
                public void onClosed(int code, String reason)
                {
                    String message = code == 4001 ? "You were kicked from the server" : code == 4002 ? "You were banned from the server" : "Connection lost";
                    enqueue(() -> failCurrent(requested, message, code == 1006 || code == 1001 || code == 1011 || code == 1012 || code == 1013 || (code == 1000 && phase == 3)));
                }

                @Override
                public void onError(Exception failure)
                {
                    boolean retryable = failure instanceof java.io.IOException && !(failure instanceof javax.net.ssl.SSLException);
                    String message = AutomaticWebSocket.certificateFailure(failure) ? "The server certificate could not be verified. Check the server address and certificate" : "Could not connect. Check the server address and port";
                    enqueue(() -> failCurrent(requested, message, retryable));
                }
            });
            detail = "Connecting to " + endpoint.getHost();
            publish();
            socket.connect();
        }
        catch (RuntimeException failure)
        {
            failCurrent(requested, safeReason(failure, "Could not prepare the connection"));
        }
    }

    private void opened(long requested)
    {
        if (!isCurrent(requested) || socket == null || phase != 0)
        {
            return;
        }
        endpoint = socket.endpoint();
        phase = 1;
        status = Status.AUTHENTICATING;
        detail = "Verifying your identity";
        JSONObject request = body("public_key_info");
        put(request, "value", publicKey);
        put(request, "verification_string", "welcome");
        put(request, "dh_public_mix", publicMix);
        put(request, "crypto_version", 2);
        send(request);
        publish();
    }

    private void receive(long requested, String text)
    {
        if (closed.get() || requested != wantedConnection.get())
        {
            return;
        }
        long bytes = text.length() * 2L;
        int count = pendingFrames.incrementAndGet();
        long total = pendingBytes.addAndGet(bytes);
        if (count > 128 || total > MESSAGE_LIMIT * 4L)
        {
            pendingFrames.decrementAndGet();
            pendingBytes.addAndGet(-bytes);
            rejectReceive(requested);
            return;
        }
        if (!enqueue(() ->
        {
            try
            {
                if (isCurrent(requested) && crypto != null)
                {
                    received(text);
                }
            }
            finally
            {
                pendingFrames.decrementAndGet();
                pendingBytes.addAndGet(-bytes);
            }
        }))
        {
            pendingFrames.decrementAndGet();
            pendingBytes.addAndGet(-bytes);
            rejectReceive(requested);
        }
    }

    private void rejectReceive(long requested)
    {
        long previous = overloadedConnection.getAndSet(requested);
        if (previous == requested || closed.get())
        {
            return;
        }
        try
        {
            worker.execute(() -> failCurrent(requested, "The server exceeded the receive queue limit"));
        }
        catch (RejectedExecutionException ignored)
        {
        }
    }

    private void enqueueConnectionIntent(Runnable action)
    {
        if (closed.get())
        {
            return;
        }
        connectionIntent.set(action);
        scheduleConnectionIntent();
    }

    private void scheduleConnectionIntent()
    {
        if (!intentScheduled.compareAndSet(false, true))
        {
            return;
        }
        try
        {
            worker.execute(this::drainConnectionIntent);
        }
        catch (RejectedExecutionException ignored)
        {
            intentScheduled.set(false);
            connectionIntent.set(null);
        }
    }

    private void drainConnectionIntent()
    {
        try
        {
            Runnable action;
            while (!closed.get() && (action = connectionIntent.getAndSet(null)) != null)
            {
                action.run();
            }
        }
        finally
        {
            intentScheduled.set(false);
            if (connectionIntent.get() != null)
            {
                scheduleConnectionIntent();
            }
            scheduleTick();
        }
    }

    private void received(String wire)
    {
        try
        {
            JSONObject envelope = ProtocolJson.object(wire, MESSAGE_LIMIT);
            JSONObject plaintextException = envelope.optJSONObject("message");
            if (plaintextException != null)
            {
                if (phase == 1 && "rsa_key_too_weak".equals(plaintextException.optString("type")))
                {
                    fail("This server requires an identity with at least " + plaintextException.optInt("minimum_rsa_key_bits") + " RSA bits");
                    return;
                }
                throw new JSONException("Unencrypted server reply");
            }
            JSONObject root = ProtocolJson.object(crypto.decrypt(wire), MESSAGE_LIMIT);
            JSONObject message = root.getJSONObject("message");
            String type = ProtocolJson.string(message, "type", 100);
            lastReply = now();
            if (phase == 1 && "crypto_policy".equals(type))
            {
                send(crypto.negotiate(message));
            }
            else if (phase == 1 && "public_key_challenge".equals(type))
            {
                String challenge = crypto.decryptChallenge(ProtocolJson.string(message, "value", 16384));
                crypto.finishNegotiation(message);
                JSONObject response = body("public_key_challenge_response");
                put(response, "value", challenge);
                put(response, "chosen_username", username);
                put(response, "fast_reconnect", false);
                phase = 2;
                send(response);
            }
            else if (phase == 2 && "authentication_status".equals(type) && "success".equals(message.optString("value")))
            {
                phase = 3;
                status = Status.CONNECTED;
                authenticationPolicy = message;
                rememberPolicy(message);
                keys.readPolicy(message, clientId);
                lastKeepalive = now();
                detail = "Connected. Waiting for channel keys";
                send(body("load_server_info"));
                features.policy(message);
                publish();
            }
            else if (phase == 3)
            {
                dispatch(type, message);
            }
            else
            {
                throw new JSONException("Unexpected login reply");
            }
        }
        catch (Exception failure)
        {
            fail(phase < 3 ? "Login could not be verified. Check the server encryption keys" : "The server sent an invalid encrypted reply");
        }
    }

    private void dispatch(String type, JSONObject message) throws JSONException
    {
        administration.control(type, message);
        if (features.control(type, message))
        {
            publish();
            return;
        }
        switch (type)
        {
            case "client_list":
                readMembers(message.getJSONArray("clients"));
                break;
            case "channel_list":
                readChannels(message.getJSONArray("channels"));
                break;
            case "client_connect":
                storeMember(readMember(message));
                break;
            case "client_disconnect":
            {
                int id = id(message, "client_id");
                Member previous = members.remove(id);
                features.avatars.memberRemoved(id);
                if (id == clientId)
                {
                    fail("The server ended this session");
                    return;
                }
                keys.memberChanged(previous, null, members.values(), channelId, clientId);
                break;
            }
            case "channel_join":
                changePresence(message, false);
                break;
            case "client_going_to_idle_mode":
                changePresence(message, true);
                break;
            case "client_coming_back_from_idle_mode":
                changePresence(message, false);
                break;
            case "call":
                receiveCall(message);
                break;
            case "poke":
                receivePoke(message);
                break;
            case "client_rename":
            {
                Member member = members.get(id(message, "client_id"));
                if (member != null)
                {
                    storeMember(member.withName(ProtocolJson.string(message, "new_username", 256)));
                }
                break;
            }
            case "channel_create":
            case "channel_edit":
                storeChannel(readChannel(message));
                break;
            case "channel_delete":
                channels.remove(id(message, "channel_id"));
                break;
            case "channel_icon_changed":
            {
                Channel channel = channels.get(id(message, "channel_id"));
                if (channel != null)
                {
                    channels.put(channel.id, channel.withIcon(message.getBoolean("has_channel_icon") ? ProtocolJson.integer(message, "channel_icon_id", 0, EXACT_INTEGER_LIMIT) : -1));
                }
                break;
            }
            case "channel_maintainer_id":
                readMaintainer(message);
                break;
            case "server_policy":
            {
                rememberPolicy(message);
                JSONObject latestPolicy = authenticationPolicy != null ? authenticationPolicy : message;
                features.policy(latestPolicy);
                keys.readPolicy(latestPolicy, clientId);
                if (message.has("is_permission_system_enabled") && !message.getBoolean("is_permission_system_enabled"))
                {
                    serverGroups.clear();
                    channelGroups.clear();
                    icons.clear();
                    iconCharacters = 0;
                }
                break;
            }
            case "group_list":
                readGroups(message);
                break;
            case "group_add":
            {
                Group group = readGroup(message, false);
                serverGroups.put(group.id, group);
                break;
            }
            case "group_delete":
                removeGroup((int) ProtocolJson.integer(message, "group_id", 0, 999));
                break;
            case "group_add_to_client":
                changeMemberGroup(message, true);
                break;
            case "remove_group_from_client":
                changeMemberGroup(message, false);
                break;
            case "group_icon_changed":
            {
                int groupId = (int) ProtocolJson.integer(message, "group_id", 0, 999);
                Group group = serverGroups.get(groupId);
                if (group != null)
                {
                    long icon = message.getBoolean("has_icon") ? ProtocolJson.integer(message, "group_linked_icon_id", 0, EXACT_INTEGER_LIMIT) : -1;
                    serverGroups.put(groupId, group.withIcon(icon));
                }
                break;
            }
            case "icon_list":
                readIcons(message.getJSONArray("icons"));
                break;
            case "icon_add":
                storeIcon(readIcon(message));
                break;
            case "icon_delete":
                removeIcon(ProtocolJson.integer(message, "icon_id", 0, EXACT_INTEGER_LIMIT));
                break;
            case "channel_group_changed":
            {
                Member member = members.get(id(message, "client_id"));
                int channel = id(message, "channel_id");
                int role = (int) ProtocolJson.integer(message, "channel_group_id", 0, 127);
                if (member != null && member.channelId == channel && !member.idle)
                {
                    members.put(member.id, member.withGroups(member.groupIds, role));
                }
                break;
            }
            case "audio_state_of_single_client":
            {
                Member member = members.get(id(message, "client_id"));
                if (member != null)
                {
                    members.put(member.id, member.withPresence(member.channelId, member.idle, (int) ProtocolJson.integer(message, "value", 1, 4)));
                }
                break;
            }
            case "current_channel_active_microphone_usage":
                readAudioStates(message.getJSONArray("clients"));
                break;
            case "start_song_stream":
            case "stop_song_stream":
            {
                Member member = members.get(id(message, "client_id"));
                if (member != null)
                {
                    boolean active = type.equals("start_song_stream");
                    members.put(member.id, member.withSong(active, active ? ProtocolJson.string(message, "song_name", 512) : ""));
                }
                break;
            }
            case "direct_chat_message":
                receiveDirect(message);
                break;
            case "offline_chat_message":
                receiveOffline(message);
                break;
            case "channel_chat_message":
                receiveChannel(message);
                break;
            case "server_chat_message_id_for_local_message_id":
                acknowledge(message);
                break;
            case "access_denied":
                pendingChannel = -1;
                error(ProtocolJson.string(message, "reason", 1000));
                break;
            case "permission_denied":
                error("The server did not allow that action" + (message.has("permission") ? ": " + ProtocolJson.string(message, "permission", 100) : ""));
                break;
            case "channel_full":
                pendingChannel = -1;
                error("That channel is full");
                break;
            default:
                break;
        }
        administration.afterState();
        validateCurrentCall();
        publish();
        updateVoice();
        if (voice != null && mediaAllowed && voiceClientId >= 0)
        {
            voice.onControl(connectionId, message);
        }
    }

    private void readMembers(JSONArray list) throws JSONException
    {
        if (list.length() > STATE_LIMIT)
        {
            throw new JSONException("Too many clients");
        }
        Map<Integer, Member> replacement = new LinkedHashMap<>();
        for (int i = 0; i < list.length(); i++)
        {
            Member member = readMember(list.getJSONObject(i));
            if (replacement.put(member.id, member) != null)
            {
                throw new JSONException("Duplicate client identifier");
            }
        }
        members.clear();
        members.putAll(replacement);
        features.avatars.replaceMembers(new ArrayList<>(members.values()));
        for (Member member : members.values())
        {
            updateLocal(member);
        }
        if (keys.maintainer >= 0)
        {
            keys.refresh(clientId);
        }
    }

    private Member readMember(JSONObject data) throws JSONException
    {
        String key = ProtocolJson.string(data, "public_key", 16384);
        boolean idle = data.optBoolean("is_idle");
        int channel = idle ? -1 : (int) ProtocolJson.integer(data, "channel_id", -1, Integer.MAX_VALUE);
        int role = data.has("channel_group_id") ? (int) ProtocolJson.integer(data, "channel_group_id", 0, 127) : 0;
        int audio = data.has("audio_state") ? (int) ProtocolJson.integer(data, "audio_state", 1, 4) : 3;
        Member member = new Member(id(data, "client_id"), channel, ProtocolJson.string(data, "username", 256), data.optString("identity_hash", ""), key, data.optBoolean("is_music_bot"), idle, audio, data.optInt("music_bot_encryption", 0), data.optJSONArray("music_bot_keys") == null ? "" : data.getJSONArray("music_bot_keys").toString(), readGroupIds(data.has("group_ids") ? data.getJSONArray("group_ids") : null), idle || channel < 0 ? 0 : role, data.optString("country_iso_code", "").matches("[A-Z]{2}") ? data.optString("country_iso_code") : "", "");
        return readSong(member, data);
    }

    private Member readSong(Member member, JSONObject data) throws JSONException
    {
        if (!data.has("is_streaming_song"))
        {
            return member;
        }
        boolean active = data.getBoolean("is_streaming_song");
        return member.withSong(active, active && data.has("song_name") ? ProtocolJson.string(data, "song_name", 512) : "");
    }

    /**
     * reads the server catalog without inventing missing group names
     *
     * @param message complete server and channel group catalogs
     * @throws JSONException when a catalog is invalid
     */
    private void readGroups(JSONObject message) throws JSONException
    {
        Map<Integer, Group> server = readGroupCatalog(message.getJSONArray("groups"), false);
        JSONArray roles = message.has("channel_groups") ? message.getJSONArray("channel_groups") : null;
        Map<Integer, Group> channel = readGroupCatalog(roles != null ? roles : new JSONArray(), true);
        serverGroups.clear();
        serverGroups.putAll(server);
        channelGroups.clear();
        channelGroups.putAll(channel);
    }

    private Map<Integer, Group> readGroupCatalog(JSONArray rows, boolean channel) throws JSONException
    {
        if (rows.length() > (channel ? 127 : 1000))
        {
            throw new JSONException("Too many groups");
        }
        Map<Integer, Group> result = new LinkedHashMap<>();
        for (int index = 0; index < rows.length(); index++)
        {
            Group group = readGroup(rows.getJSONObject(index), channel);
            if (result.put(group.id, group) != null)
            {
                throw new JSONException("Duplicate group identifier");
            }
        }
        return result;
    }

    private Group readGroup(JSONObject data, boolean channel) throws JSONException
    {
        int id = (int) ProtocolJson.integer(data, "group_id", channel ? 1 : 0, channel ? 127 : 999);
        String name = ProtocolJson.string(data, channel ? "name" : "group_name", 256);
        String type = channel ? "channel" : ProtocolJson.string(data, "group_type", 20);
        if (name.isEmpty() || (!channel && !type.equals("normal") && !type.equals("guest") && !type.equals("moderator") && !type.equals("admin")))
        {
            throw new JSONException("Invalid group description");
        }
        long icon = channel ? ProtocolJson.integer(data, "icon_id", -2, EXACT_INTEGER_LIMIT) : data.getBoolean("has_icon") ? ProtocolJson.integer(data, "group_linked_icon_id", 0, EXACT_INTEGER_LIMIT) : -1;
        return new Group(id, name, type, channel, icon);
    }

    private List<Integer> readGroupIds(JSONArray values) throws JSONException
    {
        LinkedHashSet<Integer> ids = new LinkedHashSet<>();
        if (values == null)
        {
            return new ArrayList<>();
        }
        if (values.length() > 1000)
        {
            throw new JSONException("Too many group memberships");
        }
        for (int index = 0; index < values.length(); index++)
        {
            Object item = values.get(index);
            if (!(item instanceof Number))
            {
                throw new JSONException("Invalid group identifier");
            }
            double value = ((Number) item).doubleValue();
            if (!Double.isFinite(value) || value != Math.rint(value) || value < 0 || value >= 1000)
            {
                throw new JSONException("Invalid group identifier");
            }
            ids.add((int)value);
        }
        return new ArrayList<>(ids);
    }

    /** replaces the image catalog within the server count and local memory limits */
    private void readIcons(JSONArray values) throws JSONException
    {
        if (values.length() > 1000)
        {
            throw new JSONException("Too many server icons");
        }
        Map<Long, ServerIcon> replacement = new LinkedHashMap<>();
        int characters = 0;
        for (int index = 0; index < values.length(); index++)
        {
            ServerIcon icon = readIcon(values.getJSONObject(index));
            if (icon == null || replacement.containsKey(icon.id) || characters + icon.data.length() > ICON_CHARACTERS_LIMIT)
            {
                continue;
            }
            replacement.put(icon.id, icon);
            characters += icon.data.length();
        }
        icons.clear();
        icons.putAll(replacement);
        iconCharacters = characters;
    }

    private ServerIcon readIcon(JSONObject value) throws JSONException
    {
        long id = ProtocolJson.integer(value, "icon_id", 0, EXACT_INTEGER_LIMIT);
        Object encoded = value.opt("base64_icon");
        if (!(encoded instanceof String))
        {
            return null;
        }
        String data = (String) encoded;
        if (data.length() > 40959 || !data.matches("(?i)data:image/(png|jpeg|gif|webp|svg\\+xml);base64,[A-Za-z0-9+/=\\r\\n]+"))
        {
            return null;
        }
        return new ServerIcon(id, data);
    }

    private void storeIcon(ServerIcon icon)
    {
        if (icon == null)
        {
            return;
        }
        ServerIcon previous = icons.get(icon.id);
        int characters = iconCharacters - (previous == null ? 0 : previous.data.length()) + icon.data.length();
        if ((previous == null && icons.size() >= 1000) || characters > ICON_CHARACTERS_LIMIT)
        {
            return;
        }
        icons.put(icon.id, icon);
        iconCharacters = characters;
    }

    private void removeIcon(long id)
    {
        ServerIcon removed = icons.remove(id);
        if (removed != null)
        {
            iconCharacters -= removed.data.length();
        }
        removeGroupIcon(id);
        for (Map.Entry<Integer, Channel> entry : channels.entrySet())
        {
            if (entry.getValue().iconId == id)
            {
                entry.setValue(entry.getValue().withIcon(-1));
            }
        }
    }

    private void changeMemberGroup(JSONObject message, boolean add) throws JSONException
    {
        int group = (int) ProtocolJson.integer(message, "group_id", 0, 999);
        Member member = members.get(id(message, "client_id"));
        if (member != null)
        {
            ArrayList<Integer> ids = new ArrayList<>(member.groupIds);
            if (add && !ids.contains(group))
            {
                ids.add(group);
            }
            else if (!add)
            {
                ids.remove(Integer.valueOf(group));
            }
            members.put(member.id, member.withGroups(ids, member.channelGroupId));
        }
    }

    private void removeGroup(int id)
    {
        serverGroups.remove(id);
        for (Member member : new ArrayList<>(members.values()))
        {
            if (member.groupIds.contains(id))
            {
                ArrayList<Integer> ids = new ArrayList<>(member.groupIds);
                ids.remove(Integer.valueOf(id));
                members.put(member.id, member.withGroups(ids, member.channelGroupId));
            }
        }
    }

    private void removeGroupIcon(long icon)
    {
        for (Map<Integer, Group> catalog : java.util.Arrays.asList(serverGroups, channelGroups))
        {
            for (Group group : new ArrayList<>(catalog.values()))
            {
                if (group.iconId == icon)
                {
                    catalog.put(group.id, group.withIcon(-1));
                }
            }
        }
    }

    private void storeMember(Member member) throws JSONException
    {
        if (!members.containsKey(member.id) && members.size() >= STATE_LIMIT)
        {
            throw new JSONException("Too many clients");
        }
        Member old = members.put(member.id, member);
        features.avatars.memberChanged(member);
        updateLocal(member);
        keys.memberChanged(old, member, members.values(), channelId, clientId);
    }

    private void updateLocal(Member member)
    {
        if (!member.publicKey.isEmpty() && member.publicKey.equals(publicKey))
        {
            clientId = member.id;
            username = member.name;
            if (!member.idle && member.channelId >= 0)
            {
                returnChannel = member.channelId;
            }
            if (channelId != member.channelId)
            {
                channelId = member.channelId;
                keys.reset();
                clearHiddenSongTitles();
            }
            if (pendingChannel == channelId)
            {
                pendingChannel = -1;
            }
            if (idleRequest >= 0 && member.idle == (idleRequest == 1))
            {
                if (idleRequest == 0)
                {
                    if (resumeChannel >= 0 && resumeChannel != channelId)
                    {
                        error("That channel could not be joined. You returned to the lobby");
                    }
                    resumeChannel = -1;
                    resumePassword = "";
                    pendingChannel = -1;
                }
                idleRequest = -1;
            }
        }
    }

    private void changePresence(JSONObject message, boolean idle) throws JSONException
    {
        Member old = members.get(id(message, "client_id"));
        if (old == null)
        {
            return;
        }
        int channel = idle ? -1 : message.has("channel_id") ? (int) ProtocolJson.integer(message, "channel_id", -1, Integer.MAX_VALUE) : old.channelId;
        int audio = message.has("audio_state") ? (int) ProtocolJson.integer(message, "audio_state", 1, 4) : old.audioState;
        Member next = old.withPresence(channel, idle, audio);
        if (message.has("channel_group_id") && !idle && channel >= 0)
        {
            next = next.withGroups(next.groupIds, (int) ProtocolJson.integer(message, "channel_group_id", 0, 127));
        }
        storeMember(readSong(next, message));
        if (next.id == clientId && next.idle != old.idle)
        {
            keys.reset();
        }
    }

    private void readChannels(JSONArray list) throws JSONException
    {
        if (list.length() > STATE_LIMIT)
        {
            throw new JSONException("Too many channels");
        }
        Map<Integer, Channel> replacement = new LinkedHashMap<>();
        for (int i = 0; i < list.length(); i++)
        {
            Channel channel = readChannel(list.getJSONObject(i));
            if (replacement.put(channel.id, channel) != null)
            {
                throw new JSONException("Duplicate channel identifier");
            }
        }
        channels.clear();
        channels.putAll(replacement);
    }

    private Channel readChannel(JSONObject data) throws JSONException
    {
        int channel = id(data, "channel_id");
        Channel previous = channels.get(channel);
        int parent = data.optBoolean("is_root_channel") ? -1 : data.has("parent_channel_id") ? (int) ProtocolJson.integer(data, "parent_channel_id", -1, Integer.MAX_VALUE) : previous != null ? previous.parentId : -1;
        String nameField = data.has("name") ? "name" : "channel_name";
        String descriptionField = data.has("description") ? "description" : "channel_description";
        long icon = data.has("has_channel_icon") ? data.getBoolean("has_channel_icon") ? ProtocolJson.integer(data, "channel_icon_id", 0, EXACT_INTEGER_LIMIT) : -1 : previous != null ? previous.iconId : -1;
        return new Channel(channel, parent, ProtocolJson.string(data, nameField, 256), data.optString(descriptionField, ""), data.optBoolean("is_using_password"), data.optBoolean("is_audio_enabled", true), icon);
    }

    private void storeChannel(Channel channel) throws JSONException
    {
        if (!channels.containsKey(channel.id) && channels.size() >= STATE_LIMIT)
        {
            throw new JSONException("Too many channels");
        }
        channels.put(channel.id, channel);
    }

    private void readMaintainer(JSONObject message) throws JSONException
    {
        if (id(message, "channel_id") != channelId)
        {
            return;
        }
        int maintainer = message.optBoolean("has_maintainer") ? id(message, "maintainer_id") : -1;
        long generation = ProtocolJson.integer(message, "maintainer_generation", 0, EXACT_INTEGER_LIMIT);
        Member member = members.get(maintainer);
        if (maintainer >= 0 && (member == null || member.channelId != channelId || member.idle || member.musicBot))
        {
            throw new JSONException("Invalid channel maintainer");
        }
        keys.assign(maintainer, generation, clientId);
    }

    private void readAudioStates(JSONArray values) throws JSONException
    {
        if (values.length() > STATE_LIMIT)
        {
            throw new JSONException("Too many audio states");
        }
        for (int i = 0; i < values.length(); i++)
        {
            JSONObject value = values.getJSONObject(i);
            Member member = members.get(id(value, "client_id"));
            if (member != null && member.channelId == channelId)
            {
                members.put(member.id, readSong(member.withPresence(member.channelId, member.idle, (int) ProtocolJson.integer(value, "audio_state", 1, 4)), value));
            }
        }
    }

    private void sendText(SessionSnapshot selected, Member target, String text, ChatMessage selectedReply)
    {
        if (selected.connectionId != connectionId || !isCurrent(connectionId) || status != Status.CONNECTED || text == null || text.trim().isEmpty() || text.length() > TEXT_LIMIT)
        {
            error("Message was not sent. Check the connection and message length");
            return;
        }
        try
        {
            JSONObject formatted = new JSONObject();
            put(formatted, "value", text);
            put(formatted, "reaction_id", UUID.randomUUID().toString().replace("-", ""));
            String conversationId = target == null ? "channel:" + selected.channelId : target.conversationId;
            ReplyQuote reply = features.reply(selectedReply, conversationId);
            put(formatted, "reply_to", reply == null ? JSONObject.NULL : ChatFeatures.quote(reply));
            put(formatted, "font", "Arial");
            put(formatted, "font_size", 12);

            // page default colour
            put(formatted, "font_color", "#ffffff");
            long localId = ++nextMessageId;
            JSONObject message;
            String conversation;
            if (target == null)
            {
                if (selected.channelId != channelId || keys.current == null)
                {
                    error("Wait for the channel's encryption keys before sending");
                    return;
                }
                message = body("channel_chat_message");
                put(message, "receiver_id", channelId);
                put(message, "value", crypto.encryptChannel(keys.current, formatted.toString()));
                conversation = "channel:" + channelId;
            }
            else
            {
                Member current = members.get(target.id);
                if (current == null || current.id == clientId || current.musicBot || current.publicKey.isEmpty() || !current.publicKey.equals(target.publicKey))
                {
                    error("That person is no longer available for this conversation");
                    return;
                }
                JSONObject content = body("direct_chat_message");
                put(content, "value", formatted.toString());
                message = direct(current, "conversation", content.toString(), localId);
                conversation = current.conversationId;
            }
            put(message, "local_message_id", localId);
            send(message);
            ChatMessage chat = new ChatMessage(localId, conversation, clientId, username, text, true, System.currentTimeMillis(), "Sending", publicKey, -1, connectionId, formatted.optString("reaction_id"), reply, Collections.emptyList(), null, false);
            if (pendingMessages.size() >= 256)
            {
                Long first = pendingMessages.keySet().iterator().next();
                emit(pendingMessages.remove(first).withDelivery("Not confirmed", -1));
            }
            pendingMessages.put(localId, chat);
            emit(chat);
        }
        catch (RuntimeException failure)
        {
            error("Message could not be encrypted or sent");
        }
    }

    private JSONObject direct(Member recipient, String purpose, String content, long localId)
    {
        JSONObject message = body("direct_chat_message");
        put(message, "purpose", purpose);
        put(message, "receiver_id", recipient.id);
        put(message, "local_message_id", localId);
        put(message, "value", crypto.encryptDirect(recipient.publicKey, content));
        return message;
    }

    private void receiveDirect(JSONObject message)
    {
        try
        {
            Member sender = members.get(id(message, "sender_id"));
            if (sender == null || sender.id == clientId || sender.musicBot)
            {
                return;
            }
            JSONObject content = ProtocolJson.object(crypto.decryptDirect(ProtocolJson.string(message, "value", MESSAGE_LIMIT)), MESSAGE_LIMIT);
            String purpose = message.optString("purpose", "conversation");
            if ("channel_keys".equals(purpose))
            {
                if (sender.channelId == channelId && !sender.idle && keys.accept(sender.id, channelId, content))
                {
                    detail = "Connected";
                }
                return;
            }
            if ("seen_receipt".equals(purpose))
            {
                features.receipt(sender, content);
                return;
            }
            if ("conversation".equals(purpose) && "direct_chat_message".equals(content.optString("type")))
            {
                receiveText(sender.conversationId, sender, ProtocolJson.string(content, "value", TEXT_LIMIT * 2), ProtocolJson.integer(message, "server_chat_message_id", 0, EXACT_INTEGER_LIMIT));
            }
        }
        catch (Exception failure)
        {
            contentError();
        }
    }

    private void receiveChannel(JSONObject message) throws JSONException
    {
        if (id(message, "channel_id") != channelId || keys.current == null)
        {
            return;
        }
        Member sender = members.get(id(message, "sender_id"));
        if (sender == null || sender.id == clientId)
        {
            return;
        }
        String envelope = ProtocolJson.string(message, "value", MESSAGE_LIMIT);
        String content;
        try
        {
            content = crypto.decryptChannel(keys.current, envelope);
        }
        catch (RuntimeException failure)
        {
            if (keys.previous == null)
            {
                contentError();
                return;
            }
            try
            {
                content = crypto.decryptChannel(keys.previous, envelope);
            }
            catch (RuntimeException previousFailure)
            {
                contentError();
                return;
            }
        }
        receiveText("channel:" + channelId, sender, content, ProtocolJson.integer(message, "server_chat_message_id", 0, EXACT_INTEGER_LIMIT));
    }

    private void receiveOffline(JSONObject message)
    {
        try
        {
            JSONObject content = ProtocolJson.object(crypto.decryptDirect(ProtocolJson.string(message, "value", MESSAGE_LIMIT)), MESSAGE_LIMIT);
            if (!"direct_chat_message".equals(content.optString("type")))
            {
                return;
            }
            String hash = ProtocolJson.string(message, "sender_identity_hash", 64);
            String name = ProtocolJson.string(message, "sender_name", 256);
            long timestamp = ProtocolJson.integer(message, "queued_unix_seconds", 0, 9223372036854775L) * 1000;
            Member sender = new Member(-1, -1, name, hash, "", false, true, 4, 0, "");
            receiveText(Member.conversationForIdentityHash(hash), sender, ProtocolJson.string(content, "value", TEXT_LIMIT * 2), -1, timestamp);
        }
        catch (Exception failure)
        {
            contentError();
        }
    }

    private void receiveText(String conversation, Member sender, String content, long serverId)
    {
        receiveText(conversation, sender, content, serverId, System.currentTimeMillis());
    }

    private void receiveText(String conversation, Member sender, String content, long serverId, long timestamp)
    {
        String text = content;
        String reactionId = "";
        ReplyQuote reply = null;
        String fontFamily = "Arial";
        int fontSize = 12;
        String fontColor = "#000000";
        try
        {
            JSONObject formatted = ProtocolJson.object(content, TEXT_LIMIT * 2);
            if (features.content(conversation, sender, formatted, serverId))
            {
                return;
            }
            if (formatted.has("type"))
            {
                return;
            }
            text = ProtocolJson.string(formatted, "value", TEXT_LIMIT);
            reactionId = ChatFeatures.validId(formatted.optString("reaction_id")) ? formatted.optString("reaction_id") : "";
            reply = ChatFeatures.readQuote(formatted.optJSONObject("reply_to"));
            fontFamily = formatted.optString("font", "Arial");
            fontSize = formatted.optInt("font_size", 12);
            fontColor = formatted.optString("font_color", "#000000");
        }
        catch (JSONException legacyText)
        {
            if (content.startsWith("{"))
            {
                return;
            }
        }
        if (!text.isEmpty() && text.length() <= TEXT_LIMIT)
        {
            emit(new ChatMessage(++nextMessageId, conversation, sender.id, sender.name, text, false, timestamp, "", sender.publicKey, serverId, connectionId, reactionId, reply, Collections.emptyList(), null, false, fontFamily, fontSize, fontColor));
        }
    }

    private void acknowledge(JSONObject message) throws JSONException
    {
        long localId = ProtocolJson.integer(message, "local_message_id", 1, EXACT_INTEGER_LIMIT);
        long serverId = ProtocolJson.integer(message, "server_chat_message_id", 0, EXACT_INTEGER_LIMIT);
        ChatMessage pending = pendingMessages.remove(localId);
        if (pending != null)
        {
            emit(pending.withDelivery("Sent", serverId));
        }
    }

    private void updateVoice()
    {
        synchronized (mediaLock)
        {
            if (voice == null || !mediaAllowed || status != Status.CONNECTED || crypto == null || clientId < 0)
            {
                return;
            }
            if (voiceClientId != clientId)
            {
                voice.onAuthenticated(endpoint, clientId, connectionId, crypto.createMediaCrypto());
                voiceClientId = clientId;
                if (authenticationPolicy != null)
                {
                    voice.onControl(connectionId, authenticationPolicy);
                }
            }
            Channel channel = channels.get(channelId);
            voice.onState(channelId, channel != null && channel.audioEnabled, keys.current, new ArrayList<>(members.values()));
        }
    }

    /** keeps the latest authenticated policy for media recreated after idle */
    private void rememberPolicy(JSONObject message) throws JSONException
    {
        administration.control("policy", message);
        idleAllowed = message.optBoolean("is_idle_mode_allowed", idleAllowed);
        connectionCheckEnforced = message.optBoolean("is_connection_check_enforced", connectionCheckEnforced);
        if (authenticationPolicy != null && authenticationPolicy != message)
        {
            java.util.Iterator<String> names = message.keys();
            while (names.hasNext())
            {
                String name = names.next();
                if (!name.equals("type"))
                {
                    authenticationPolicy.put(name, message.get(name));
                }
            }
        }
        clearHiddenSongTitles();
    }

    /** removes titles whose future changes are no longer sent to this client */
    private void clearHiddenSongTitles()
    {
        if (authenticationPolicy != null && authenticationPolicy.optBoolean("is_music_bot_marquee_visible_to_everyone", false))
        {
            return;
        }
        for (Member member : members.values())
        {
            if (member.streamingSong && member.channelId != channelId)
            {
                members.put(member.id, member.withSong(false, ""));
            }
        }
    }

    private void tickSafely()
    {
        try
        {
            if (!isCurrent(connectionId))
            {
                return;
            }
            if (phase < 3 && socket != null && now() >= loginDeadline)
            {
                fail("Login timed out. Check the address and server encryption keys", phase == 0);
            }
            if (status != Status.CONNECTED)
            {
                return;
            }
            validateCurrentCall();
            if (administration.tick())
            {
                publish();
            }
            applyIdleIntent();
            if (desiredIdle == null && idleRequest < 0)
            {
                features.tick();
            }
            long silenceLimit = idleAllowed && localIdle() ? 150000 : 45000;
            if (connectionCheckEnforced && now() - lastReply >= silenceLimit)
            {
                fail("The server stopped responding", true);
                return;
            }
            if (now() - lastKeepalive >= heartbeatInterval())
            {
                send(body("client_connection_check"));
                lastKeepalive = now();
            }
            if (!Boolean.TRUE.equals(desiredIdle))
            {
                distributeKeys();
            }
            if (pendingChannel >= 0 && now() >= channelJoinDeadline)
            {
                pendingChannel = -1;
                error("The channel did not change. Try joining it again");
            }
            if (keys.current == null && channelId >= 0 && !Boolean.TRUE.equals(desiredIdle) && now() - TimeUnit.NANOSECONDS.toMillis(keys.waitingSince) >= 15000)
            {
                send(body("reset_channel_maintainer"));
                keys.waitingSince = System.nanoTime();
            }
            List<Long> expired = new ArrayList<>();
            for (Map.Entry<Long, ChatMessage> entry : pendingMessages.entrySet())
            {
                if (System.currentTimeMillis() - entry.getValue().timestamp > 30000)
                {
                    emit(entry.getValue().withDelivery("Not confirmed", -1));
                    expired.add(entry.getKey());
                }
            }
            for (Long id : expired)
            {
                pendingMessages.remove(id);
            }
        }
        catch (Exception failure)
        {
            fail("The secure connection could not continue");
        }
    }

    private long heartbeatInterval()
    {
        return idleAllowed && localIdle() ? 120000 : 10000;
    }

    /** @return milliseconds until the next required protocol operation, or no deadline */
    private long nextTickDelay()
    {
        if (closed.get() || !isCurrent(connectionId))
        {
            return -1;
        }
        long timestamp = now();
        if (status != Status.CONNECTED)
        {
            return socket == null ? -1 : Math.max(1, loginDeadline - timestamp);
        }
        if (!localIdle() || desiredIdle != null || idleRequest >= 0 || pendingChannel >= 0 || !pendingMessages.isEmpty())
        {
            return 100;
        }
        long delay = Math.max(1, lastKeepalive + heartbeatInterval() - timestamp);
        long adminDelay = administration.nextDelay();
        if (adminDelay >= 0)
        {
            delay = Math.min(delay, adminDelay);
        }
        long featureDelay = features.nextWorkDelay();
        if (featureDelay >= 0)
        {
            delay = Math.min(delay, featureDelay);
        }
        if (connectionCheckEnforced)
        {
            long silenceLimit = idleAllowed ? 150000 : 45000;
            delay = Math.min(delay, Math.max(1, lastReply + silenceLimit - timestamp));
        }
        if (call != null)
        {
            delay = Math.min(delay, Math.max(1, TimeUnit.NANOSECONDS.toMillis(call.expiresAtNanos - System.nanoTime())));
        }
        return delay;
    }

    /** keeps one deadline and advances it when incoming work needs attention */
    private void scheduleTick()
    {
        long delay = nextTickDelay();
        if (delay < 0)
        {
            cancelTick();
            return;
        }
        long deadline = now() + delay;
        if (tickTask != null && !tickTask.isDone() && tickAt <= deadline)
        {
            return;
        }
        cancelTick();
        tickAt = deadline;
        try
        {
            tickTask = worker.schedule(() ->
            {
                tickTask = null;
                tickAt = 0;
                tickSafely();
                scheduleTick();
            }, delay, TimeUnit.MILLISECONDS);
        }
        catch (RejectedExecutionException ignored)
        {
            tickAt = 0;
        }
    }

    private void cancelTick()
    {
        if (tickTask != null)
        {
            tickTask.cancel(false);
            tickTask = null;
        }
        tickAt = 0;
    }

    private void distributeKeys() throws JSONException
    {
        Member local = members.get(clientId);
        if (local == null || local.idle || keys.maintainer != clientId || clientId < 0)
        {
            return;
        }
        if (keys.rotate)
        {
            keys.installGenerated(crypto.newContentKeys(), members.values(), channelId, clientId);
            detail = "Connected";
            updateVoice();
            publish();
        }
        Member captured = keys.nextRecipient();
        if (captured == null || keys.current == null)
        {
            return;
        }
        Member recipient = members.get(captured.id);
        if (recipient == null || recipient.channelId != channelId || recipient.idle || recipient.musicBot || !recipient.publicKey.equals(captured.publicKey))
        {
            return;
        }
        JSONObject content = body("channel_keys_from_maintainer");
        put(content, "channel_id", channelId);
        put(content, "maintainer_generation", keys.generation);
        put(content, "value", keys.current);
        send(direct(recipient, "channel_keys", content.toString(), ++nextMessageId));
    }

    private void send(JSONObject message)
    {
        if (crypto == null || socket == null || !isCurrent(connectionId))
        {
            throw new IllegalStateException("Connection is not available");
        }
        JSONObject wrapper = new JSONObject();
        put(wrapper, "message", message);
        if (!socket.sendText(crypto.encrypt(wrapper.toString())))
        {
            throw new IllegalStateException("Outgoing connection queue is full");
        }
        lastControlSent = now();
    }

    private boolean enqueue(Runnable action)
    {
        if (closed.get() || !commandSlots.tryAcquire())
        {
            return false;
        }
        try
        {
            worker.execute(() ->
            {
                try
                {
                    action.run();
                }
                catch (RuntimeException failure)
                {
                    if (!closed.get())
                    {
                        fail(safeReason(failure, "The operation could not finish"));
                    }
                }
                finally
                {
                    commandSlots.release();
                    scheduleTick();
                }
            });
            return true;
        }
        catch (RejectedExecutionException failure)
        {
            commandSlots.release();
            return false;
        }
    }

    private boolean isCurrent(long requested)
    {
        return !closed.get() && requested == connectionId && requested == wantedConnection.get();
    }

    private void failCurrent(long requested, String reason)
    {
        failCurrent(requested, reason, false);
    }

    private void failCurrent(long requested, String reason, boolean retryable)
    {
        if (isCurrent(requested) && status != Status.FAILED && status != Status.DISCONNECTED)
        {
            fail(reason, retryable);
        }
    }

    private void fail(String reason)
    {
        fail(reason, false);
    }

    private void fail(String reason, boolean retryable)
    {
        stopConnection();
        retryableFailure = retryable;
        status = Status.FAILED;
        detail = reason;
        publish();
        error(reason);
    }

    private void stopConnection()
    {
        cancelTick();
        finishCall("cancelled");
        desiredIdle = null;
        idleRequest = -1;
        idleAllowed = false;
        connectionCheckEnforced = true;
        returnChannel = 0;
        resumeChannel = -1;
        resumePassword = "";
        lastCallSent = 0;
        nextIncomingCall = 0;
        retryableFailure = false;
        if (voice != null)
        {
            voice.onDisconnected();
        }
        voiceClientId = -1;
        if (socket != null)
        {
            socket.close();
            socket = null;
        }
        if (crypto != null)
        {
            crypto.close();
            crypto = null;
        }
        for (ChatMessage pending : pendingMessages.values())
        {
            emit(pending.withDelivery("Not confirmed", -1));
        }
        pendingMessages.clear();
        features.reset();
        administration.reset(connectionId);
        channels.clear();
        members.clear();
        serverGroups.clear();
        channelGroups.clear();
        keys.reset();
        icons.clear();
        iconCharacters = 0;
        clientId = -1;
        channelId = -1;
        pendingChannel = -1;
        phase = 0;
        authenticationPolicy = null;
        publicKey = "";
        publicMix = "";
    }

    private SessionSnapshot snapshot()
    {
        ArrayList<Group> groups = new ArrayList<>(serverGroups.values());
        groups.addAll(channelGroups.values());
        boolean allTitles = authenticationPolicy != null && authenticationPolicy.optBoolean("is_music_bot_marquee_visible_to_everyone", false);
        ArrayList<Member> visibleMembers = new ArrayList<>();
        for (Member member : members.values())
        {
            boolean visibleTitle = !member.idle && member.channelId >= 0 && (allTitles || member.channelId == channelId);
            visibleMembers.add(member.streamingSong && !visibleTitle ? member.withSong(false, "") : member);
        }
        return new SessionSnapshot(status, detail, clientId, channelId, username, keys.current != null, new ArrayList<>(channels.values()), visibleMembers, groups, connectionId, localIdle(), idleAllowed, call, retryableFailure, new ArrayList<>(icons.values()), features.emojis(), features.capabilities(), administration.snapshot());
    }

    private void publish()
    {
        SessionSnapshot state = snapshot();
        published = state;
        Listener target = listener;
        if (target != null)
        {
            queueCallback(() ->
            {
                if (listener == target && state.connectionId == wantedConnection.get())
                {
                    target.onState(state);
                }
            }, 0, true);
        }
    }

    private void emit(ChatMessage message)
    {
        features.remember(message);
        Listener target = listener;
        long origin = connectionId;
        if (target != null)
        {
            queueCallback(() ->
            {
                if (listener == target && origin == wantedConnection.get())
                {
                    target.onMessage(message);
                }
            }, (int)Math.min(Integer.MAX_VALUE, 512 + com.lemonchat.model.MessageBudget.messageBytes(message)), false);
        }
    }

    private void error(String message)
    {
        Listener target = listener;
        long origin = connectionId;
        if (target != null)
        {
            queueCallback(() ->
            {
                if (listener == target && origin == wantedConnection.get())
                {
                    target.onError(message);
                }
            }, 128 + message.length() * 2, false);
        }
    }

    private void queueCallback(Runnable action, int bytes, boolean state)
    {
        boolean schedule = false;
        boolean rejected = false;
        synchronized (callbackLock)
        {
            if (closed.get())
            {
                return;
            }
            if (state)
            {
                pendingStateCallback = action;
            }
            else if (callbackQueue.size() >= 256 || callbackBytes + bytes > MESSAGE_LIMIT * 4L)
            {
                rejected = true;
            }
            else
            {
                callbackQueue.addLast(new CallbackTask(action, bytes));
                callbackBytes += bytes;
            }
            if (!callbackScheduled)
            {
                callbackScheduled = true;
                schedule = true;
            }
        }
        if (rejected)
        {
            rejectReceive(wantedConnection.get());
        }
        if (schedule)
        {
            scheduleCallbacks();
        }
    }

    private void scheduleCallbacks()
    {
        try
        {
            callbacks.execute(this::drainCallbacks);
        }
        catch (RejectedExecutionException rejected)
        {
            synchronized (callbackLock)
            {
                callbackQueue.clear();
                pendingStateCallback = null;
                callbackBytes = 0;
                callbackScheduled = false;
            }
            rejectReceive(wantedConnection.get());
        }
    }

    private void drainCallbacks()
    {
        for (int i = 0; i < 16; i++)
        {
            Runnable action;
            synchronized (callbackLock)
            {
                if (pendingStateCallback != null)
                {
                    action = pendingStateCallback;
                    pendingStateCallback = null;
                }
                else
                {
                    CallbackTask next = callbackQueue.pollFirst();
                    if (next == null)
                    {
                        callbackScheduled = false;
                        return;
                    }
                    callbackBytes -= next.bytes;
                    action = next.action;
                }
            }
            try
            {
                action.run();
            }
            catch (RuntimeException observerFailure)
            {
                rejectReceive(wantedConnection.get());
            }
        }
        scheduleCallbacks();
    }

    private static final class CallbackTask
    {
        final Runnable action;
        final int bytes;

        CallbackTask(Runnable action, int bytes)
        {
            this.action = action;
            this.bytes = bytes;
        }
    }

    private void contentError()
    {
        if (now() - lastContentError > 5000)
        {
            lastContentError = now();
            error("A message could not be decrypted");
        }
    }

    private static String safeReason(RuntimeException error, String fallback)
    {
        return error instanceof IllegalArgumentException && error.getMessage() != null ? error.getMessage() : fallback;
    }

    private static long now()
    {
        return TimeUnit.NANOSECONDS.toMillis(System.nanoTime());
    }

    private static int id(JSONObject message, String field) throws JSONException
    {
        return (int) ProtocolJson.integer(message, field, 0, Integer.MAX_VALUE);
    }

    private static JSONObject body(String type)
    {
        JSONObject message = new JSONObject();
        put(message, "type", type);
        return message;
    }

    private static JSONObject parse(String text)
    {
        try
        {
            return ProtocolJson.object(text, MESSAGE_LIMIT);
        }
        catch (JSONException error)
        {
            throw new IllegalArgumentException("Invalid control message", error);
        }
    }

    private static void put(JSONObject object, String name, Object value)
    {
        try
        {
            object.put(name, value);
        }
        catch (JSONException error)
        {
            throw new IllegalArgumentException("Invalid protocol field", error);
        }
    }

    /**
     * validates an explicit server URL without downgrading TLS
     *
     * @param address ws or wss URL
     * @return validated endpoint
     */
    public static URI validateEndpoint(String address)
    {
        try
        {
            if (address == null || address.length() > 2048)
            {
                throw new IllegalArgumentException();
            }
            URI value = URI.create(address.trim());
            if ((!"ws".equals(value.getScheme()) && !"wss".equals(value.getScheme())) || value.getHost() == null || value.getRawUserInfo() != null || value.getRawFragment() != null || value.getPort() == 0 || value.getPort() > 65535)
            {
                throw new IllegalArgumentException();
            }
            return value;
        }
        catch (IllegalArgumentException failure)
        {
            throw new IllegalArgumentException("Check the server address and port");
        }
    }

    /** releases the connection and worker when its owning service ends */
    @Override
    public void close()
    {
        if (!closed.compareAndSet(false, true))
        {
            return;
        }
        wantedConnection.incrementAndGet();
        listener = null;
        VoiceController currentVoice = voice;
        if (currentVoice != null)
        {
            currentVoice.setTalking(false);
        }
        synchronized (callbackLock)
        {
            callbackQueue.clear();
            pendingStateCallback = null;
            callbackBytes = 0;
        }
        worker.execute(() ->
        {
            stopConnection();
            if (voice != null)
            {
                voice.close();
                voice = null;
            }
        });
        worker.shutdown();
    }
}
