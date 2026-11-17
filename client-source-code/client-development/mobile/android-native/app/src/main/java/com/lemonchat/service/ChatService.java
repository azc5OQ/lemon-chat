package com.lemonchat.service;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.net.ConnectivityManager;
import android.net.Network;
import com.lemonchat.R;
import com.lemonchat.audio.VoiceController;
import com.lemonchat.audio.AudioOptions;
import com.lemonchat.audio.SoundEffects;
import com.lemonchat.crypto.Identity;
import com.lemonchat.crypto.IdentityFile;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.model.CallInvitation;
import com.lemonchat.model.PokeMessage;
import com.lemonchat.model.Member;
import com.lemonchat.model.MessageBudget;
import com.lemonchat.model.SessionSnapshot;
import com.lemonchat.model.AdminState;
import com.lemonchat.model.AdminGroup;
import com.lemonchat.model.AdminSettings;
import com.lemonchat.model.PermissionDefinition;
import com.lemonchat.protocol.ChatSession;
import com.lemonchat.protocol.ClientOptions;
import com.lemonchat.settings.LocalSettings;
import com.lemonchat.storage.ConversationArchive;
import com.lemonchat.storage.IdentityVault;
import com.lemonchat.storage.HistoryPolicy;
import com.lemonchat.storage.HistoryStore;
import com.lemonchat.storage.HistoryMaintenanceJob;
import com.lemonchat.ui.AttachmentProvider;
import com.lemonchat.ui.MainActivity;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicLong;

/** owns the connection and audio while activities attach and detach */
public final class ChatService extends Service implements ChatSession.Listener
{
    public interface Observer
    {
        void onChanged();
        void onProblem(String message);
    }

    public interface Completion
    {
        void finished(String error);
    }

    public interface HistoryCallback
    {
        void onComplete(String error);
    }

    public final class LocalBinder extends Binder
    {
        public ChatService service()
        {
            return ChatService.this;
        }
    }

    public static final String EXTRA_CONVERSATION_ID = "conversation_id";
    public static final String EXTRA_CONVERSATION_SCOPE = "conversation_scope";
    public static final String EXTRA_CALL_ID = "call_id";
    public static final String EXTRA_CONNECTION_ID = "connection_id";
    public static final String EXTRA_SHOW_CHANNEL = "show_channel";
    public static final String EXTRA_ANSWER_CALL = "answer_call";
    static final String DISCONNECT_ACTION = "com.lemonchat.DISCONNECT";
    static final String DECLINE_ACTION = "com.lemonchat.DECLINE";
    private static final String AUTOMATIC_ACTION = "com.lemonchat.AUTOMATIC";
    private static final ExecutorService STORAGE = Executors.newSingleThreadExecutor();
    private static final Object STORAGE_LIMITS = new Object();
    private static final AtomicLong HISTORY_EPOCH = new AtomicLong();
    private static int pendingArchiveWrites;
    private static long pendingArchiveBytes;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final List<Observer> observers = new ArrayList<>();
    private final Set<Observer> chatObservers = new HashSet<>();
    private final ReconnectBackoff backoff = new ReconnectBackoff();
    private final Map<String, ArrayList<ChatMessage>> conversations = new LinkedHashMap<>();
    private final Map<String, String> conversationTitles = new LinkedHashMap<>();
    private final Map<String, Long> visibleArchiveRecords = new LinkedHashMap<>();
    // record ids of messages the user deleted on this device; later updates of these messages are ignored
    private final Set<String> locallyDeletedRecords = new HashSet<>();
    private final Map<String, PokeMessage> pendingPokes = new LinkedHashMap<>();
    private final MessageUpdates messageUpdates = new MessageUpdates();
    private final String archiveRun = UUID.randomUUID().toString();
    private final LocalBinder binder = new LocalBinder();
    private ChatSession session;
    private VoiceController voice;
    private SoundEffects sounds;
    private LocalSettings localSettings;
    private ConnectionLog connectionLog;
    private BackgroundConnectionLease connectionLease;
    private boolean settingsPending;
    private IdentityVault vault;
    private HistoryStore archive;
    private volatile HistoryPolicy historyPolicy;
    private boolean historyPending;
    private boolean historySweepPending;
    private long highestMessageId;
    private long historyMinimumMessageId = 1;
    private Identity identity;
    private String serverKeys = "";
    private String activeScope = "";
    private SessionSnapshot snapshot;
    private long historyConnection = -1;
    private long requestedConnection = 0;
    private boolean destroyed;
    private boolean identityLoading = true;
    private boolean importPending;
    private boolean connectPending;
    private boolean connectionActive;
    private boolean archiveProblem;
    private boolean foreground;
    private boolean voiceReady;
    private boolean talking;
    private String voiceStatus = "Connect to a channel to use voice";
    private String identityError;
    private String pendingProblem;
    private SharedPreferences preferences;
    private ChatNotifications notifications;
    private ConnectivityManager connectivity;
    private ConnectivityManager.NetworkCallback networkCallback;
    private String savedEndpoint = "";
    private String savedUsername = "";
    private boolean savedConnectionValid;
    private boolean savedKeysValid;
    private boolean automaticConnection;
    private boolean reconnectSuppressed;
    private boolean connectionWanted;
    private boolean automaticPending;
    private boolean retryPending;
    private boolean mediaEnabled;
    private boolean forceBackgroundIdle;
    private boolean manualIdle;
    private boolean suppressNextResume;
    private boolean isPickerOpen;
    private long connectedSince;
    private long idleRequestedConnection = -1;
    private final Runnable retry = () ->
    {
        retryPending = false;
        startSavedConnection();
    };
    private final Runnable backgroundIdle = () ->
    {
        if (!destroyed && chatObservers.isEmpty() && !isPickerOpen)
        {
            enterBackgroundIdle();
        }
    };
    private final Runnable historySweep = this::maintainHistory;

    /**
     * starts the saved connection only after the user enables automatic startup
     *
     * @param context receiver or activity context
     */
    public static void startAutomatic(Context context)
    {
        SharedPreferences settings = context.getSharedPreferences("preferences", MODE_PRIVATE);
        if (!settings.getBoolean("automatic_connection", false) || settings.getBoolean("reconnect_suppressed", false) || !settings.getBoolean("saved_connection_valid", false))
        {
            return;
        }
        try
        {
            context.startForegroundService(new Intent(context, ChatService.class).setAction(AUTOMATIC_ACTION));
        }
        catch (RuntimeException error)
        {
            android.util.Log.w("LemonNative", "Android deferred automatic connection", error);
        }
    }

    @Override
    public void onCreate()
    {
        super.onCreate();
        preferences = getSharedPreferences("preferences", MODE_PRIVATE);
        connectionLease = new BackgroundConnectionLease(this);
        localSettings = LocalSettings.load(this);
        connectionLog = new ConnectionLog(this, STORAGE);
        connectionLog.setFileEnabled(localSettings.getBoolean("native_file_logging"));
        connectionLog.add(ConnectionLog.Event.STARTED);
        sounds = new SoundEffects(this);
        savedEndpoint = preferences.getString("endpoint", "");
        savedUsername = preferences.getString("username", "");
        savedConnectionValid = preferences.getBoolean("saved_connection_valid", false);
        automaticConnection = preferences.getBoolean("automatic_connection", false);
        reconnectSuppressed = preferences.getBoolean("reconnect_suppressed", false);
        notifications = new ChatNotifications(this);
        vault = new IdentityVault(this);
        historyPolicy = HistoryPolicy.load(this);
        archive = new HistoryStore(this, vault);
        session = new ChatSession(main::post);
        session.setListener(this);
        voice = new VoiceController(getApplicationContext(), session::sendControl, (origin, status, ready, active) -> main.post(() ->
        {
            if (destroyed)
            {
                return;
            }
            SessionSnapshot current = session.state();
            if (origin >= 0 && (current == null || current.connectionId != origin))
            {
                return;
            }
            if (origin < 0 && current != null && current.connected)
            {
                return;
            }
            if ((ready || active) && (current == null || !current.connected || !mediaEnabled))
            {
                return;
            }
            voiceStatus = status;
            voiceReady = ready;
            talking = active;
            if (foreground && !updateForeground(active && !voice.musicActive()))
            {
                disconnect();
            }
            notifyObservers();
        }));
        session.setVoiceController(voice);
        applyLocalSettings();
        registerNetworkChanges();
        long historyVersion = HISTORY_EPOCH.get();
        STORAGE.execute(() ->
        {
            Identity loadedIdentity = null;
            String loadedKeys = "";
            String loadedScope = "";
            ConversationArchive.Loaded history = new ConversationArchive.Loaded();
            String failure = null;
            HistoryPolicy initialPolicy = HistoryPolicy.load(this);
            try
            {
                if (initialPolicy.erasePending)
                {
                    eraseHistoryFiles();
                    initialPolicy = HistoryPolicy.save(this, initialPolicy.retentionDays, false);
                }
            }
            catch (Exception error)
            {
                failure = "Saved history deletion is incomplete. Retry Delete saved history.";
            }
            HistoryPolicy restoredPolicy = initialPolicy;
            try
            {
                loadedIdentity = savedEndpoint.isEmpty() ? vault.load(localSettings.getInt("lemon_rsa_key_bits")) : vault.loadExisting();
            }
            catch (Exception error)
            {
                failure = "Your saved identity could not be opened. Import your identity backup to continue.";
            }
            boolean keysValid = false;
            try
            {
                loadedKeys = savedEndpoint.isEmpty() ? vault.loadServerKeys() : vault.loadExistingServerKeys();
                keysValid = true;
            }
            catch (Exception error)
            {
                if (failure == null)
                {
                    failure = "Your server encryption keys could not be opened. Enter the server settings again before connecting.";
                }
            }
            if (loadedIdentity != null && !savedEndpoint.isEmpty())
            {
                try
                {
                    loadedScope = ConversationArchive.scope(savedEndpoint, loadedIdentity);
                    history = archive.load(loadedScope, restoredPolicy);
                }
                catch (Exception error)
                {
                    failure = "Some saved private messages could not be opened";
                }
            }
            Identity restoredIdentity = loadedIdentity;
            String restoredKeys = loadedKeys;
            String restoredScope = loadedScope;
            ConversationArchive.Loaded restoredHistory = history;
            String result = failure;
            boolean restoredKeysValid = keysValid;
            main.post(() ->
            {
                if (!destroyed)
                {
                    identityLoading = false;
                    identity = restoredIdentity;
                    serverKeys = restoredKeys;
                    savedKeysValid = restoredKeysValid;
                    identityError = restoredIdentity == null ? result : null;
                    if (historyVersion == HISTORY_EPOCH.get())
                    {
                        historyPolicy = restoredPolicy;
                    }
                    restoreHistory(restoredScope, restoredHistory, historyVersion);
                    scheduleHistorySweep(1000);
                    HistoryMaintenanceJob.schedule(this, historyPolicy);
                    if (result != null)
                    {
                        onError(result);
                    }
                    if (automaticPending || (automaticConnection && !reconnectSuppressed && !chatObservers.isEmpty()))
                    {
                        connectionWanted = true;
                        startSavedConnection();
                    }
                    notifyObservers();
                }
            });
        });
    }

    @Override
    public IBinder onBind(Intent intent)
    {
        return binder;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId)
    {
        if (intent != null && DISCONNECT_ACTION.equals(intent.getAction()))
        {
            disconnect();
        }
        else if (intent != null && DECLINE_ACTION.equals(intent.getAction()))
        {
            declineCall(intent.getStringExtra(EXTRA_CALL_ID), intent.getLongExtra(EXTRA_CONNECTION_ID, -1));
            if (!connectionWanted && !foreground)
            {
                stopSelf(startId);
            }
        }
        else if (intent == null || AUTOMATIC_ACTION.equals(intent.getAction()) || ConnectionRestartJob.ACTION_RECOVER.equals(intent.getAction()))
        {
            if (connectionActive || connectPending)
            {
                return connectionWanted ? START_STICKY : START_NOT_STICKY;
            }
            boolean recovery = intent != null && ConnectionRestartJob.ACTION_RECOVER.equals(intent.getAction());
            boolean resume = intent == null || recovery ? preferences.getBoolean("connection_requested", false) || (recovery && automaticConnection) : automaticConnection;
            if (!resume || reconnectSuppressed || !savedConnectionValid)
            {
                ConnectionRestartJob.cancel(this);
                stopSelf(startId);
                return START_NOT_STICKY;
            }
            connectionWanted = true;
            automaticPending = true;
            forceBackgroundIdle = chatObservers.isEmpty();
            mediaEnabled = false;
            session.setMediaAllowed(false);
            if (!updateForeground(false))
            {
                stopSelf(startId);
                return START_NOT_STICKY;
            }
            if (!identityLoading)
            {
                startSavedConnection();
            }
        }
        else if (!connectionActive && !connectPending && !foreground && !identityLoading)
        {
            stopSelf(startId);
        }
        return connectionWanted ? START_STICKY : START_NOT_STICKY;
    }

    /**
     * starts a connection after its settings have been saved
     *
     * @param endpoint WebSocket server address
     * @param username displayed name
     * @param keys optional static server keys
     */
    public void connect(String endpoint, String username, String keys)
    {
        if (destroyed)
        {
            return;
        }
        if (importPending || connectPending)
        {
            onError("Wait for the current identity or connection change to finish");
            return;
        }
        if (identity == null)
        {
            onError(identityError != null ? identityError : "Your identity is still loading");
            return;
        }
        try
        {
            ChatSession.validateEndpoint(endpoint);
        }
        catch (IllegalArgumentException error)
        {
            onError(error.getMessage());
            return;
        }
        Identity connectionIdentity = identity;
        long request = ++requestedConnection;
        cancelRetry();
        backoff.reset();
        connectionWanted = true;
        reconnectSuppressed = false;
        automaticPending = false;
        manualIdle = false;
        forceBackgroundIdle = false;
        savedConnectionValid = false;
        preferences.edit().putBoolean("reconnect_suppressed", false).putBoolean("connection_requested", true).apply();
        connectPending = true;
        setMediaEnabled(false);
        session.disconnect();
        long historyVersion = HISTORY_EPOCH.get();
        STORAGE.execute(() ->
        {
            try
            {
                if (!preferences.edit().putBoolean("saved_connection_valid", false).commit())
                {
                    throw new java.io.IOException("Connection settings could not be prepared");
                }
                vault.saveServerKeys(keys);
                if (!preferences.edit().putString("endpoint", endpoint).putString("username", username).putBoolean("saved_connection_valid", true).commit())
                {
                    throw new java.io.IOException("Connection settings could not be saved");
                }
                String scope = ConversationArchive.scope(endpoint, connectionIdentity);
                ConversationArchive.Loaded history = new ConversationArchive.Loaded();
                String archiveFailure = null;
                try
                {
                    history = archive.load(scope, historyPolicy);
                }
                catch (Exception error)
                {
                    archiveFailure = "Saved private messages could not be opened";
                }
                ConversationArchive.Loaded restored = history;
                String warning = archiveFailure;
                main.post(() ->
                {
                    if (!destroyed && request == requestedConnection)
                    {
                        connectPending = false;
                        if (observers.isEmpty() && !foreground)
                        {
                            connectionWanted = false;
                            preferences.edit().putBoolean("connection_requested", false).apply();
                            onError("Open the app before starting a connection");
                            return;
                        }
                        try
                        {
                            startService(new Intent(this, ChatService.class));
                            if (!updateForeground(false))
                            {
                                connectionWanted = false;
                                preferences.edit().putBoolean("connection_requested", false).apply();
                                stopSelf();
                                return;
                            }
                            serverKeys = keys;
                            savedKeysValid = true;
                            savedEndpoint = endpoint;
                            savedUsername = username;
                            savedConnectionValid = true;
                            restoreHistory(scope, restored, historyVersion);
                            scheduleHistorySweep(3600000);
                            connectionActive = true;
                            ConnectionRestartJob.schedule(this);
                            setMediaEnabled(!chatObservers.isEmpty());
                            session.connect(endpoint, username, connectionIdentity, keys);
                            if (warning != null)
                            {
                                onError(warning);
                            }
                        }
                        catch (RuntimeException error)
                        {
                            connectionActive = false;
                            connectionWanted = false;
                            preferences.edit().putBoolean("connection_requested", false).apply();
                            stopForeground(STOP_FOREGROUND_REMOVE);
                            foreground = false;
                            stopSelf();
                            onError("Android could not start the connection. Open the app and try again.");
                        }
                    }
                });
            }
            catch (Exception error)
            {
                main.post(() ->
                {
                    if (!destroyed && request == requestedConnection)
                    {
                        connectPending = false;
                        connectionWanted = false;
                        preferences.edit().putBoolean("connection_requested", false).apply();
                        onError(error.getMessage() != null ? error.getMessage() : "Connection settings could not be saved");
                    }
                });
            }
        });
    }

    /** stops voice and retires pending connection requests */
    public void disconnect()
    {
        if (destroyed)
        {
            return;
        }
        requestedConnection++;
        connectionWanted = false;
        reconnectSuppressed = true;
        connectionLease.setActive(false);
        ConnectionRestartJob.cancel(this);
        automaticPending = false;
        preferences.edit().putBoolean("reconnect_suppressed", true).putBoolean("connection_requested", false).apply();
        cancelRetry();
        connectPending = false;
        connectionActive = false;
        session.setTalking(false);
        setMediaEnabled(false);
        notifications.clearCall();
        session.disconnect();
        talking = false;
        stopForeground(STOP_FOREGROUND_REMOVE);
        foreground = false;
        stopSelf();
    }

    /**
     * imports an identity while disconnected
     *
     * @param file complete identity file
     * @param legacyBits original size for a legacy file
     * @param completion result delivered on the main thread
     */
    public void importIdentity(byte[] file, int legacyBits, Completion completion)
    {
        if (destroyed)
        {
            completion.finished("The connection service has closed");
            return;
        }
        if (identityLoading || importPending)
        {
            completion.finished("Wait for the current identity operation to finish");
            return;
        }
        if (connectPending || connectionActive || connectionWanted || (snapshot != null && snapshot.status != SessionSnapshot.Status.DISCONNECTED && snapshot.status != SessionSnapshot.Status.FAILED))
        {
            completion.finished("Disconnect before changing your identity");
            return;
        }
        if (file == null || file.length > 65536)
        {
            completion.finished("Choose an identity file no larger than 64 KiB");
            return;
        }
        byte[] contents = file.clone();
        importPending = true;
        long historyVersion = HISTORY_EPOCH.get();
        STORAGE.execute(() ->
        {
            String failure = null;
            Identity imported = null;
            String scope = "";
            ConversationArchive.Loaded history = new ConversationArchive.Loaded();
            try
            {
                Identity parsed = IdentityFile.parse(contents, legacyBits);
                vault.save(parsed);
                imported = parsed;
                String endpoint = getSharedPreferences("preferences", MODE_PRIVATE).getString("endpoint", "");
                if (!endpoint.isEmpty())
                {
                    scope = ConversationArchive.scope(endpoint, imported);
                    history = archive.load(scope, historyPolicy);
                }
            }
            catch (Exception error)
            {
                failure = error.getMessage() != null ? error.getMessage() : "The identity could not be imported";
            }
            finally
            {
                Arrays.fill(contents, (byte)0);
            }
            String result = failure;
            Identity replacement = imported;
            String restoredScope = scope;
            ConversationArchive.Loaded restored = history;
            main.post(() ->
            {
                if (!destroyed)
                {
                    importPending = false;
                    if (replacement != null)
                    {
                        identity = replacement;
                        identityError = null;
                        restoreHistory(restoredScope, restored, historyVersion);
                    }
                    completion.finished(result);
                    notifyObservers();
                }
            });
        });
    }

    public Identity identity()
    {
        return identity;
    }

    public HistoryPolicy historyPolicy()
    {
        return historyPolicy;
    }

    /**
     * runs a scheduled expiry on the same worker as all archive writes
     *
     * @param context application context
     * @param completion completion on the main thread
     */
    public static void maintainSavedHistory(Context context, Runnable completion)
    {
        Context application = context.getApplicationContext();
        STORAGE.execute(() ->
        {
            try
            {
                HistoryPolicy policy = HistoryPolicy.load(application);
                if (policy.enabled() && policy.retentionDays > 0)
                {
                    try (HistoryStore store = new HistoryStore(application, new IdentityVault(application)))
                    {
                        store.maintain(policy);
                    }
                }
            }
            catch (Exception error)
            {
                android.util.Log.w("LemonNative", "Saved history maintenance could not finish");
            }
            finally
            {
                new Handler(Looper.getMainLooper()).post(completion);
            }
        });
    }

    public boolean historyBusy()
    {
        return historyPending;
    }

    /**
     * applies an explicit history choice without changing the connection
     *
     * @param days minus one disables saving, zero keeps history until deletion
     * @param callback completion on the main thread
     */
    public void setHistoryPolicy(int days, HistoryCallback callback)
    {
        changeHistoryPolicy(days, days == -1, callback);
    }

    /**
     * deletes disk history while keeping current conversations visible
     *
     * @param callback completion on the main thread
     */
    public void clearSavedHistory(HistoryCallback callback)
    {
        changeHistoryPolicy(historyPolicy.configured ? historyPolicy.retentionDays : -1, true, callback);
    }

    /**
     * @brief deletes one message on this device: from its open conversation and, for a private message, from
     *        the saved history. Updates of the message that arrive later (delivery, reactions) are ignored, so it
     *        does not appear again. Other people keep their copies.
     *
     * @param message the message to delete
     */
    public void deleteMessageLocally(ChatMessage message)
    {
        // find the record id
        String recordId = archiveRun + ":" + message.connectionId + ":" + message.id;
        for (Map.Entry<String, Long> entry : visibleArchiveRecords.entrySet())
        {
            if (entry.getValue() == message.id)
            {
                recordId = entry.getKey();
                break;
            }
        }
        locallyDeletedRecords.add(recordId);
        visibleArchiveRecords.remove(recordId);

        // remove from the conversation
        List<ChatMessage> values = conversations.get(message.conversationId);
        if (values != null)
        {
            values.removeIf(value -> value.id == message.id);
        }
        notifyObservers();

        // remove from saved history
        String scope = activeScope;
        String savedRecord = recordId;
        if (message.conversationId.startsWith("user:") && !scope.isEmpty())
        {
            STORAGE.execute(() ->
            {
                try
                {
                    archive.delete(scope, savedRecord, historyPolicy);
                }
                catch (Exception error)
                {
                    main.post(() -> onError("The saved copy of this message could not be deleted."));
                }
            });
        }
    }

    /**
     * @brief deletes every message on this device: all open channel and private conversations and the saved
     *        history files. Updates of these messages that arrive later are ignored. The caller asks the user
     *        before calling this.
     *
     * @param callback completion on the main thread, with an error text or null
     */
    public void deleteAllMessages(HistoryCallback callback)
    {
        // ignore later updates
        locallyDeletedRecords.addAll(visibleArchiveRecords.keySet());

        // clear open conversations
        conversations.clear();
        visibleArchiveRecords.clear();
        messageUpdates.clear();
        notifications.clearMessages();
        notifyObservers();

        // delete saved history
        clearSavedHistory(callback);
    }

    private void changeHistoryPolicy(int days, boolean erase, HistoryCallback callback)
    {
        if (destroyed || historyPending || !HistoryPolicy.validDays(days))
        {
            callback.onComplete(destroyed ? "The app is closing" : "Wait for the current history change to finish");
            return;
        }
        HistoryPolicy previous = historyPolicy;
        boolean wasEnabled = previous.enabled();
        boolean mustErase = erase || previous.erasePending;
        historyPending = true;
        historyPolicy = previous.suspended();
        long epoch = HISTORY_EPOCH.incrementAndGet();
        if (mustErase || !wasEnabled)
        {
            historyMinimumMessageId = highestMessageId + 1;
        }
        main.removeCallbacks(historySweep);
        historySweepPending = false;
        String scope = activeScope;
        notifyObservers();
        STORAGE.execute(() ->
        {
            String failure = null;
            HistoryPolicy committed;
            ConversationArchive.Loaded loaded = new ConversationArchive.Loaded();
            try
            {
                committed = HistoryPolicy.save(this, days, mustErase);
                if (mustErase)
                {
                    eraseHistoryFiles();
                    committed = HistoryPolicy.save(this, days, false);
                }
                if (committed.enabled())
                {
                    if (!wasEnabled && !mustErase)
                    {
                        loaded = archive.load(scope, committed);
                    }
                    else
                    {
                        archive.maintain(committed);
                    }
                }
            }
            catch (Exception error)
            {
                failure = mustErase ? "Saved history deletion could not finish. Retry Delete saved history." : "Your history preference could not be fully applied. Existing saved messages have been kept.";
            }
            committed = HistoryPolicy.load(this);
            HistoryPolicy resultPolicy = committed;
            ConversationArchive.Loaded restored = loaded;
            String result = failure;
            main.post(() ->
            {
                if (destroyed || epoch != HISTORY_EPOCH.get())
                {
                    return;
                }
                historyPending = false;
                historyPolicy = resultPolicy;
                archiveProblem = false;
                if (result == null && scope.equals(activeScope))
                {
                    mergeHistory(restored);
                }
                reloadCurrentHistory();
                scheduleHistorySweep(1000);
                HistoryMaintenanceJob.schedule(this, historyPolicy);
                notifyObservers();
                callback.onComplete(result);
            });
        });
    }

    private void eraseHistoryFiles() throws Exception
    {
        Exception failure = null;
        try
        {
            archive.erase();
        }
        catch (Exception error)
        {
            failure = error;
        }
        try
        {
            AttachmentProvider.clearOpenedCopies(this);
        }
        catch (Exception error)
        {
            if (failure == null)
            {
                failure = error;
            }
            else
            {
                failure.addSuppressed(error);
            }
        }
        if (failure != null)
        {
            throw failure;
        }
    }

    private void scheduleHistorySweep(long delay)
    {
        if (!destroyed && !historyPending && !historySweepPending && historyPolicy.enabled() && historyPolicy.retentionDays > 0)
        {
            historySweepPending = true;
            main.postDelayed(historySweep, Math.max(1000, Math.min(3600000, delay)));
        }
    }

    private void maintainHistory()
    {
        historySweepPending = false;
        if (destroyed || historyPending || !historyPolicy.enabled() || historyPolicy.retentionDays <= 0)
        {
            return;
        }
        long epoch = HISTORY_EPOCH.get();
        STORAGE.execute(() ->
        {
            long next = 3600000;
            boolean failed = false;
            try
            {
                if (epoch == HISTORY_EPOCH.get() && historyPolicy.enabled())
                {
                    next = archive.maintain(historyPolicy);
                }
            }
            catch (Exception error)
            {
                failed = true;
            }
            long delay = next;
            boolean problem = failed;
            main.post(() ->
            {
                if (!destroyed && epoch == HISTORY_EPOCH.get())
                {
                    if (problem)
                    {
                        reportArchiveFailure();
                    }
                    scheduleHistorySweep(delay);
                }
            });
        });
    }

    public String identityError()
    {
        return identityError;
    }

    public String serverKeys()
    {
        return serverKeys;
    }

    public String conversationScope()
    {
        return activeScope;
    }

    /** @return administration state for the currently displayed connection */
    public AdminState adminState()
    {
        return snapshot.admin;
    }

    public boolean adminLogin(long connection, String password)
    {
        return session.adminLogin(connection, password);
    }

    public boolean loadAdministration(long connection)
    {
        return session.loadAdministration(connection);
    }

    public boolean saveServerSettings(long connection, AdminSettings original, Map<String, Object> draft, boolean includeMusic)
    {
        return session.saveServerSettings(connection, original, draft, includeMusic);
    }

    public boolean changeAdminPassword(long connection, String password)
    {
        return session.changeAdminPassword(connection, password);
    }

    public boolean createAdminGroup(long connection, String name, String type, boolean channelGroup)
    {
        return session.createAdminGroup(connection, name, type, channelGroup);
    }

    public boolean renameAdminGroup(long connection, int groupId, boolean channelGroup, String name)
    {
        AdminGroup original = snapshot.admin.group(groupId, channelGroup);
        return original != null && session.renameAdminGroup(connection, original, name);
    }

    public boolean deleteAdminGroup(long connection, int groupId, boolean channelGroup)
    {
        AdminGroup original = snapshot.admin.group(groupId, channelGroup);
        return original != null && session.deleteAdminGroup(connection, original);
    }

    public boolean saveAdminPermissions(long connection, int groupId, boolean channelGroup, PermissionDefinition edited)
    {
        AdminGroup original = snapshot.admin.group(groupId, channelGroup);
        return original != null && session.saveAdminPermissions(connection, original, edited);
    }

    public boolean assignAdminGroup(long connection, int groupId, int clientId, boolean add)
    {
        AdminGroup original = snapshot.admin.group(groupId, false);
        Member member = adminMember(clientId);
        return original != null && member != null && session.assignAdminGroup(connection, original, member, add);
    }

    public boolean assignAdminChannelGroup(long connection, int groupId, int clientId)
    {
        Member member = adminMember(clientId);
        AdminGroup original = snapshot.admin.group(groupId == 0 && member != null ? member.channelGroupId : groupId, true);
        return original != null && member != null && session.assignAdminGroup(connection, original, member, groupId != 0);
    }

    public boolean saveAdminGroups(long connection)
    {
        return session.saveAdminGroups(connection);
    }

    private Member adminMember(int clientId)
    {
        for (Member member : snapshot.members)
        {
            if (member.id == clientId && !member.musicBot)
            {
                return member;
            }
        }
        return null;
    }

    public ChatSession session()
    {
        return session;
    }

    /** @return local choices currently applied to this service */
    public LocalSettings localSettings()
    {
        return localSettings;
    }

    /** @return recent connection events without private content */
    public String connectionLog()
    {
        return connectionLog.text();
    }

    /**
     * persists a validated draft before applying it to the active connection
     *
     * @param draft proposed local preferences
     * @param completion result delivered on the main thread
     */
    public void saveLocalSettings(Map<String, ?> draft, Completion completion)
    {
        if (destroyed || settingsPending)
        {
            completion.finished("Wait for the current settings save to finish");
            return;
        }
        final LocalSettings checked;
        try
        {
            checked = LocalSettings.validate(draft);
        }
        catch (IllegalArgumentException error)
        {
            completion.finished(error.getMessage());
            return;
        }
        settingsPending = true;
        STORAGE.execute(() ->
        {
            boolean persisted;
            try
            {
                persisted = checked.save(getApplicationContext());
            }
            catch (RuntimeException error)
            {
                persisted = false;
            }
            boolean saved = persisted;
            main.post(() ->
            {
                settingsPending = false;
                if (destroyed)
                {
                    completion.finished("The app closed before settings could be applied");
                    return;
                }
                if (!saved)
                {
                    completion.finished("The settings could not be saved");
                    return;
                }
                if (localSettings.getBoolean("lemon_continuous_mic") != checked.getBoolean("lemon_continuous_mic"))
                {
                    setTalking(false);
                }
                localSettings = checked;
                applyLocalSettings();
                trimHistory();
                connectionLog.add(ConnectionLog.Event.SETTINGS_SAVED);
                notifyObservers();
                completion.finished(null);
            });
        });
    }

    private void applyLocalSettings()
    {
        session.setOptions(new ClientOptions(localSettings.getBoolean("lemon_chat_reactions"), localSettings.getBoolean("lemon_chat_replies"), localSettings.getBoolean("lemon_send_seen"), localSettings.getBoolean("lemon_seen_indicator"), localSettings.getBoolean("lemon_block_direct_chat_pictures"), localSettings.getBoolean("lemon_block_direct_chat_files"), localSettings.getBoolean("lemon_block_channel_chat_pictures"), localSettings.getBoolean("lemon_block_channel_chat_files"), localSettings.getString("lemon_audio_connection")));
        voice.applyOptions(new AudioOptions(localSettings.getString("lemon_mic_device_id"), localSettings.getString("lemon_audio_connection"), localSettings.getBoolean("lemon_sound_effects")));
        sounds.setEnabled(localSettings.getBoolean("lemon_sound_effects"));
        connectionLog.setFileEnabled(localSettings.getBoolean("native_file_logging"));
    }

    /**
     * starts an explicitly selected music file while the chat is visible
     *
     * @param uri readable document selected by the user
     * @param name display name sent with the stream
     */
    public void startMusic(android.net.Uri uri, String name)
    {
        if (destroyed || chatObservers.isEmpty() || snapshot == null || !snapshot.connected || !mediaEnabled)
        {
            onError("Open a connected channel before streaming music");
            return;
        }
        setTalking(false);
        voice.startMusic(uri, name);
    }

    public void stopMusic()
    {
        voice.stopMusic();
    }

    public boolean musicActive()
    {
        return voice.musicActive();
    }

    public String musicName()
    {
        return voice.musicName();
    }

    /**
     * changes playback gain for one public key
     *
     * @param publicKey member identity
     * @param gain playback multiplier from zero to two
     */
    public void setPeerGain(String publicKey, float gain)
    {
        voice.setPeerGain(publicKey, gain);
    }

    /** @return saved playback multiplier for this member */
    public float peerGain(String publicKey)
    {
        return voice.peerGain(publicKey);
    }

    public SessionSnapshot snapshot()
    {
        return snapshot;
    }

    public boolean voiceReady()
    {
        return voiceReady;
    }

    public boolean talking()
    {
        return talking;
    }

    public String voiceStatus()
    {
        return voiceStatus;
    }

    public boolean automaticConnectionEnabled()
    {
        return automaticConnection;
    }

    public boolean hasSavedConnection()
    {
        return !identityLoading && identity != null && savedKeysValid && savedConnectionValid && !savedEndpoint.isEmpty() && !savedUsername.isEmpty();
    }

    /**
     * saves automatic startup without changing the selected identity
     *
     * @param enabled whether startup and boot may connect to the saved server
     */
    public void setAutomaticConnectionEnabled(boolean enabled)
    {
        if (enabled && !hasSavedConnection())
        {
            onError("Connect to your server once before enabling automatic connection");
            return;
        }
        automaticConnection = enabled;
        SharedPreferences.Editor editor = preferences.edit().putBoolean("automatic_connection", enabled);
        if (enabled)
        {
            reconnectSuppressed = false;
            connectionWanted = true;
            editor.putBoolean("reconnect_suppressed", false).putBoolean("connection_requested", true);
        }
        editor.apply();
        ConnectionRestartJob.schedule(this);
        if (enabled && !connectionActive && !connectPending)
        {
            backoff.reset();
            startSavedConnection();
        }
        notifyObservers();
    }

    public void enterIdle()
    {
        manualIdle = true;
        setMediaEnabled(false);
        session.enterIdle();
    }

    public void leaveIdle()
    {
        if (chatObservers.isEmpty() || (snapshot != null && snapshot.call != null))
        {
            return;
        }
        manualIdle = false;
        forceBackgroundIdle = false;
        suppressNextResume = false;
        if (setMediaEnabled(true))
        {
            session.leaveIdle();
        }
    }

    public boolean callMember(int target)
    {
        if (snapshot == null || !snapshot.connected || snapshot.idle || snapshot.channelId < 0)
        {
            return false;
        }
        for (Member member : snapshot.members)
        {
            if (member.id == target && member.idle && !member.musicBot && member.id != snapshot.clientId)
            {
                session.callMember(target);
                return true;
            }
        }
        return false;
    }

    public boolean answerCall(String token, long connectionId)
    {
        if (!validCall(token, connectionId))
        {
            return false;
        }
        if (snapshot.call.channelId < 0)
        {
            declineCall(token, connectionId);
            onError("The caller's channel is hidden. Ask them to invite you from a channel you can see.");
            return false;
        }
        if (observers.isEmpty() || !setMediaEnabled(true))
        {
            return false;
        }
        manualIdle = false;
        forceBackgroundIdle = false;
        suppressNextResume = false;
        notifications.clearCall();
        session.acceptCall(token, connectionId);
        return true;
    }

    public boolean declineCall(String token, long connectionId)
    {
        if (!validCall(token, connectionId))
        {
            return false;
        }
        suppressNextResume = true;
        notifications.clearCall();
        session.declineCall(token, connectionId);
        return true;
    }

    private boolean validCall(String token, long connectionId)
    {
        return snapshot != null && snapshot.connected && snapshot.connectionId == connectionId && snapshot.call != null && snapshot.call.connectionId == connectionId && snapshot.call.callId.equals(token) && !snapshot.call.expired();
    }

    public boolean canOpenNotification(String conversationId, String scope, long connectionId)
    {
        if (conversationId == null || scope == null || scope.isEmpty() || !scope.equals(activeScope))
        {
            return false;
        }
        if (conversationId.startsWith("user:"))
        {
            if (conversations.containsKey(conversationId) || pendingPokes.containsKey(conversationId))
            {
                return true;
            }
            if (snapshot != null)
            {
                for (Member member : snapshot.members)
                {
                    if (!member.musicBot && member.conversationId.equals(conversationId))
                    {
                        return true;
                    }
                }
            }
            return false;
        }
        return snapshot != null && snapshot.connected && snapshot.connectionId == connectionId && conversationId.equals("channel:" + snapshot.channelId);
    }

    /**
     * returns the latest unread poke for this saved conversation scope
     *
     * @param conversationId private conversation selected by the notification
     * @return pending poke or null
     */
    public PokeMessage consumePoke(String conversationId)
    {
        return pendingPokes.remove(conversationId);
    }

    /**
     * applies microphone permission before starting capture
     *
     * @param active whether the user is holding push to talk
     */
    public void setTalking(boolean active)
    {
        if (destroyed)
        {
            return;
        }
        boolean allowed = checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED;
        session.setMicrophoneAvailable(allowed);
        if (active && allowed && (!foreground || chatObservers.isEmpty() || !mediaEnabled || !updateForeground(true)))
        {
            session.setTalking(false);
            return;
        }
        session.setTalking(active && allowed);
        if (!active && foreground && !updateForeground(false))
        {
            disconnect();
        }
    }

    public void addObserver(Observer observer)
    {
        addObserver(observer, true);
    }

    /**
     * attaches a visible screen without treating a call prompt as channel activity
     *
     * @param observer visible screen
     * @param resumeIdle whether this is the main chat screen
     */
    public void addObserver(Observer observer, boolean resumeIdle)
    {
        if (destroyed)
        {
            return;
        }
        if (!observers.contains(observer))
        {
            observers.add(observer);
        }
        if (resumeIdle && chatObservers.add(observer))
        {
            isPickerOpen = false;
            main.removeCallbacks(backgroundIdle);
            notifications.clearMessages();
            if (!manualIdle && !suppressNextResume && (snapshot == null || snapshot.call == null))
            {
                forceBackgroundIdle = false;
                if (connectionActive && setMediaEnabled(true))
                {
                    session.leaveIdle();
                }
            }
            suppressNextResume = false;
            if (automaticConnection && !reconnectSuppressed && !identityLoading && !connectionActive && !connectPending && !retryPending)
            {
                connectionWanted = true;
                startSavedConnection();
            }
        }
        observer.onChanged();
        if (pendingProblem != null)
        {
            String problem = pendingProblem;
            pendingProblem = null;
            observer.onProblem(problem);
        }
    }

    /**
     * @brief keeps the session in its channel while a file picker started by the app is shown. The activity is
     *        stopped while the picker is in front, which otherwise moves the session to the idle channel after
     *        500 ms. The value is set back to false when the activity is shown again (addObserver).
     *
     * @param isOpen true before a picker is started, false when starting it failed
     */
    public void setPickerOpen(boolean isOpen)
    {
        isPickerOpen = isOpen;
    }

    public void removeObserver(Observer observer)
    {
        observers.remove(observer);
        if (chatObservers.remove(observer) && chatObservers.isEmpty())
        {
            main.removeCallbacks(backgroundIdle);
            main.postDelayed(backgroundIdle, 500);
        }
    }

    private void enterBackgroundIdle()
    {
        forceBackgroundIdle = true;
        session.setTalking(false);
        setMediaEnabled(false);
        session.enterIdle();
    }

    public List<ChatMessage> messages(String conversationId)
    {
        List<ChatMessage> values = conversations.get(conversationId);
        return values == null ? Collections.emptyList() : new ArrayList<>(values);
    }

    public List<String> conversationIds()
    {
        List<String> result = new ArrayList<>();
        for (String id : conversations.keySet())
        {
            if (id.startsWith("user:"))
            {
                result.add(id);
            }
        }
        return result;
    }

    public String conversationTitle(String conversationId)
    {
        if (snapshot != null)
        {
            for (Member member : snapshot.members)
            {
                if (member.conversationId.equals(conversationId))
                {
                    return member.name;
                }
            }
        }
        List<ChatMessage> values = conversations.get(conversationId);
        if (values != null)
        {
            for (int index = values.size() - 1; index >= 0; index--)
            {
                if (!values.get(index).outgoing)
                {
                    return values.get(index).senderName;
                }
            }
        }
        return conversationTitles.getOrDefault(conversationId, "Saved chat");
    }

    @Override
    public void onState(SessionSnapshot value)
    {
        if (destroyed)
        {
            return;
        }
        SessionSnapshot previous = snapshot;
        boolean wasConnected = previous != null && previous.connected;
        if (previous == null || previous.status != value.status)
        {
            connectionLog.add(ConnectionLog.Event.valueOf(value.status.name()));
        }
        playStateSound(previous, value);
        snapshot = value;
        if (historyConnection != value.connectionId)
        {
            historyConnection = value.connectionId;
            conversations.entrySet().removeIf(entry -> !entry.getKey().startsWith("user:"));
        }
        if (value.status == SessionSnapshot.Status.DISCONNECTED || value.status == SessionSnapshot.Status.FAILED)
        {
            connectionActive = false;
            talking = false;
            if (!connectPending && !identityLoading && !automaticPending)
            {
                setMediaEnabled(false);
                if (connectionWanted && !reconnectSuppressed && value.retryableFailure && hasSavedConnection())
                {
                    if (connectedSince != 0 && SystemClock.elapsedRealtime() - connectedSince >= 30000)
                    {
                        backoff.reset();
                    }
                    scheduleRetry();
                }
                else if (!retryPending)
                {
                    connectionWanted = false;
                    preferences.edit().putBoolean("connection_requested", false).apply();
                    stopForeground(STOP_FOREGROUND_REMOVE);
                    foreground = false;
                    stopSelf();
                }
            }
            connectedSince = 0;
        }
        else if (foreground)
        {
            if (!updateForeground(mediaEnabled && talking && !voice.musicActive()))
            {
                disconnect();
            }
        }
        updateConnectionLease();
        if (value.connected && !wasConnected)
        {
            connectedSince = SystemClock.elapsedRealtime();
        }
        if (value.connected && value.clientId >= 0 && value.idleAllowed && idleRequestedConnection != value.connectionId && (forceBackgroundIdle || manualIdle || (chatObservers.isEmpty() && !isPickerOpen)))
        {
            idleRequestedConnection = value.connectionId;
            setMediaEnabled(false);
            session.enterIdle();
        }
        notifyObservers();
    }

    @Override
    public void onMessage(ChatMessage message)
    {
        if (destroyed)
        {
            return;
        }
        List<ChatMessage> existing = conversations.get(message.conversationId);
        boolean known = existing != null && existing.stream().anyMatch(value -> value.id == message.id);
        highestMessageId = Math.max(highestMessageId, message.id);
        String recordId = archiveRun + ":" + message.connectionId + ":" + message.id;
        if (locallyDeletedRecords.contains(recordId))
        {
            return;
        }
        MessageUpdates.Update update = messageUpdates.begin(recordId, known);
        if (!message.outgoing && !chatObservers.isEmpty() && update.first)
        {
            sounds.play("message_received");
        }
        if (!message.conversationId.startsWith("user:") || activeScope.isEmpty() || !historyPolicy.enabled() || historyPending || message.id < historyMinimumMessageId)
        {
            remember(message, recordId);
            if (messageUpdates.takeNotification(update))
            {
                notifyMessage(message, activeScope, historyConnection);
            }
            messageUpdates.finish(update);
            notifyObservers();
            return;
        }
        String scope = activeScope;
        long messageConnection = historyConnection;
        long historyVersion = HISTORY_EPOCH.get();
        long receivedAt = System.currentTimeMillis();
        String title = message.outgoing ? conversationTitle(message.conversationId) : message.senderName;
        long bytes = MessageBudget.messageBytes(message) + 1024;
        synchronized (STORAGE_LIMITS)
        {
            if (pendingArchiveWrites >= 128 || pendingArchiveBytes + bytes > MessageBudget.MAX_TOTAL_BYTES)
            {
                remember(message, recordId);
                if (messageUpdates.takeNotification(update))
                {
                    notifyMessage(message, scope, messageConnection);
                }
                messageUpdates.finish(update);
                reportArchiveFailure();
                notifyObservers();
                return;
            }
            pendingArchiveWrites++;
            pendingArchiveBytes += bytes;
        }
        messageUpdates.queued(update);
        STORAGE.execute(() ->
        {
            boolean saved = false;
            boolean attempted = false;
            try
            {
                if (historyVersion == HISTORY_EPOCH.get() && historyPolicy.enabled())
                {
                    attempted = true;
                    archive.save(scope, recordId, message, title, receivedAt, update.first, historyPolicy);
                    saved = true;
                }
            }
            catch (Exception error)
            {
                // keep the received message visible when local storage fails
            }
            boolean persisted = saved;
            boolean storageAttempted = attempted;
            main.post(() ->
            {
                try
                {
                    if (!destroyed && scope.equals(activeScope) && messageUpdates.current(update))
                    {
                        conversationTitles.put(message.conversationId, title);
                        remember(message, recordId);
                        if (messageUpdates.takeNotification(update))
                        {
                            notifyMessage(message, scope, messageConnection);
                        }
                        if (storageAttempted && !persisted)
                        {
                            reportArchiveFailure();
                        }
                        scheduleHistorySweep(3600000);
                        notifyObservers();
                    }
                }
                finally
                {
                    messageUpdates.finish(update);
                    synchronized (STORAGE_LIMITS)
                    {
                        pendingArchiveWrites--;
                        pendingArchiveBytes -= bytes;
                    }
                }
            });
        });
    }

    private void playStateSound(SessionSnapshot previous, SessionSnapshot current)
    {
        if (previous == null || chatObservers.isEmpty())
        {
            return;
        }
        if (!previous.connected && current.connected)
        {
            sounds.play("connected");
            return;
        }
        if (previous.connected && !current.connected)
        {
            sounds.play(reconnectSuppressed ? "disconnected" : "connection_lost");
            return;
        }
        if (!previous.connected || !current.connected || previous.connectionId != current.connectionId)
        {
            return;
        }
        if (previous.channelId != current.channelId)
        {
            sounds.play("channel_switched");
            return;
        }
        Set<Integer> oldMembers = new HashSet<>();
        Set<Integer> newMembers = new HashSet<>();
        for (Member member : previous.members)
        {
            if (member.channelId == current.channelId && member.id != current.clientId)
            {
                oldMembers.add(member.id);
            }
        }
        for (Member member : current.members)
        {
            if (member.channelId == current.channelId && member.id != current.clientId)
            {
                newMembers.add(member.id);
            }
        }
        if (!oldMembers.containsAll(newMembers))
        {
            sounds.play("user_joined_your_channel");
        }
        else if (!newMembers.containsAll(oldMembers))
        {
            sounds.play("user_left_your_channel");
        }
    }

    private void remember(ChatMessage message)
    {
        ArrayList<ChatMessage> values = conversations.get(message.conversationId);
        if (values == null)
        {
            values = new ArrayList<>();
            conversations.put(message.conversationId, values);
        }
        boolean replaced = false;
        for (int i = 0; i < values.size(); i++)
        {
            if (values.get(i).id == message.id)
            {
                values.set(i, message);
                replaced = true;
                break;
            }
        }
        if (!replaced)
        {
            int index = values.size();
            while (index > 0 && values.get(index - 1).timestamp > message.timestamp)
            {
                index--;
            }
            values.add(index, message);
        }
        trimHistory();
    }

    private void remember(ChatMessage message, String recordId)
    {
        if (recordId != null && locallyDeletedRecords.contains(recordId))
        {
            return;
        }
        if (recordId != null)
        {
            visibleArchiveRecords.put(recordId, message.id);
        }
        remember(message);
    }

    private void notifyMessage(ChatMessage message, String scope, long connection)
    {
        if (!destroyed && chatObservers.isEmpty() && scope.equals(activeScope))
        {
            notifications.message(message, scope, connection);
        }
    }

    private void trimHistory()
    {
        int count = 0;
        long bytes = 0;
        long attachmentBytes = 0;
        for (List<ChatMessage> values : conversations.values())
        {
            if (localSettings.getBoolean("lemon_limit_chat_messages"))
            {
                int maximum = localSettings.getInt("lemon_chat_message_limit");
                while (values.size() > maximum)
                {
                    values.remove(0);
                }
            }
            count += values.size();
            for (ChatMessage message : values)
            {
                bytes += MessageBudget.metadataBytes(message);
                attachmentBytes += MessageBudget.attachmentBytes(message);
            }
        }
        while (count > MessageBudget.MAX_MESSAGES || bytes > MessageBudget.MAX_METADATA_BYTES || attachmentBytes > MessageBudget.MAX_ATTACHMENT_BYTES)
        {
            String oldestConversation = null;
            long oldestTime = Long.MAX_VALUE;
            for (Map.Entry<String, ArrayList<ChatMessage>> entry : conversations.entrySet())
            {
                if (!entry.getValue().isEmpty() && (oldestConversation == null || entry.getValue().get(0).timestamp < oldestTime))
                {
                    oldestConversation = entry.getKey();
                    oldestTime = entry.getValue().get(0).timestamp;
                }
            }
            if (oldestConversation == null)
            {
                break;
            }
            ArrayList<ChatMessage> values = conversations.get(oldestConversation);
            ChatMessage removed = values.remove(0);
            bytes -= MessageBudget.metadataBytes(removed);
            attachmentBytes -= MessageBudget.attachmentBytes(removed);
            count--;
            if (values.isEmpty())
            {
                conversations.remove(oldestConversation);
                conversationTitles.remove(oldestConversation);
            }
        }
        Set<Long> visibleIds = new HashSet<>();
        for (List<ChatMessage> values : conversations.values())
        {
            for (ChatMessage message : values)
            {
                visibleIds.add(message.id);
            }
        }
        visibleArchiveRecords.values().removeIf(id -> !visibleIds.contains(id));
    }

    private void restoreHistory(String scope, ConversationArchive.Loaded history, long version)
    {
        if (!activeScope.equals(scope))
        {
            messageUpdates.clear();
            pendingPokes.clear();
            notifications.clearMessages();
            conversations.clear();
            conversationTitles.clear();
            visibleArchiveRecords.clear();
        }
        activeScope = scope;
        archiveProblem = false;
        if (version == HISTORY_EPOCH.get() && historyPolicy.enabled())
        {
            mergeHistory(history);
        }
        else
        {
            reloadCurrentHistory();
        }
    }

    private void mergeHistory(ConversationArchive.Loaded history)
    {
        for (Map.Entry<String, String> title : history.titles.entrySet())
        {
            conversationTitles.putIfAbsent(title.getKey(), title.getValue());
        }
        for (ChatMessage message : history.messages)
        {
            String recordId = history.recordIds.get(message.id);
            if (recordId == null || !visibleArchiveRecords.containsKey(recordId))
            {
                remember(message, recordId);
            }
        }
    }

    private void reloadCurrentHistory()
    {
        if (destroyed || historyPending || !historyPolicy.enabled() || activeScope.isEmpty())
        {
            return;
        }
        String scope = activeScope;
        long epoch = HISTORY_EPOCH.get();
        STORAGE.execute(() ->
        {
            ConversationArchive.Loaded loaded;
            try
            {
                if (epoch != HISTORY_EPOCH.get() || !historyPolicy.enabled())
                {
                    return;
                }
                loaded = archive.load(scope, historyPolicy);
            }
            catch (Exception error)
            {
                main.post(() ->
                {
                    if (!destroyed && epoch == HISTORY_EPOCH.get() && scope.equals(activeScope))
                    {
                        reportArchiveFailure();
                    }
                });
                return;
            }
            main.post(() ->
            {
                if (!destroyed && epoch == HISTORY_EPOCH.get() && scope.equals(activeScope) && historyPolicy.enabled())
                {
                    mergeHistory(loaded);
                    notifyObservers();
                }
            });
        });
    }

    private void reportArchiveFailure()
    {
        if (!archiveProblem)
        {
            archiveProblem = true;
            connectionLog.add(ConnectionLog.Event.STORAGE_FAILED);
            onError("Some private messages could not be saved. Keep the app open until you have read them.");
        }
    }

    @Override
    public void onError(String message)
    {
        if (!destroyed)
        {
            if (observers.isEmpty())
            {
                pendingProblem = message;
            }
            for (Observer observer : new ArrayList<>(observers))
            {
                observer.onProblem(message);
            }
        }
    }

    private void notifyObservers()
    {
        if (!destroyed)
        {
            for (Observer observer : new ArrayList<>(observers))
            {
                observer.onChanged();
            }
        }
    }

    private boolean updateForeground(boolean microphone)
    {
        String detail = retryPending ? "Connection lost. Reconnecting…" : snapshot != null && snapshot.connected ? "Connected as " + snapshot.username : "Connecting to your server";
        Notification notification = notifications.connection(detail);
        try
        {
            if (Build.VERSION.SDK_INT >= 29)
            {
                int types = Build.VERSION.SDK_INT >= 34 ? ServiceInfo.FOREGROUND_SERVICE_TYPE_REMOTE_MESSAGING : 0;
                if (mediaEnabled)
                {
                    types |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK;
                }
                if (microphone && Build.VERSION.SDK_INT >= 30)
                {
                    types |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE;
                }
                startForeground(1, notification, types);
            }
            else
            {
                startForeground(1, notification);
            }
            foreground = true;
            updateConnectionLease();
            ConnectionRestartJob.schedule(this);
            return true;
        }
        catch (RuntimeException error)
        {
            onError("Android could not keep the connection in the background");
            return false;
        }
    }

    @Override
    public void onDestroy()
    {
        destroyed = true;
        connectionLease.close();
        main.removeCallbacks(backgroundIdle);
        main.removeCallbacks(historySweep);
        connectionLog.add(ConnectionLog.Event.STOPPED);
        sounds.close();
        requestedConnection++;
        connectPending = false;
        connectionActive = false;
        cancelRetry();
        notifications.clearCall();
        if (connectivity != null && networkCallback != null)
        {
            try
            {
                connectivity.unregisterNetworkCallback(networkCallback);
            }
            catch (RuntimeException error)
            {
                android.util.Log.w("LemonNative", "Network monitoring already stopped", error);
            }
        }
        session.setListener(null);
        try
        {
            session.close();
        }
        catch (RuntimeException error)
        {
            android.util.Log.e("LemonNative", "Connection shutdown failed", error);
        }
        finally
        {
            STORAGE.execute(archive::close);
        }
        observers.clear();
        chatObservers.clear();
        super.onDestroy();
    }

    @Override
    public void onTaskRemoved(Intent rootIntent)
    {
        main.removeCallbacks(backgroundIdle);
        enterBackgroundIdle();
        super.onTaskRemoved(rootIntent);
    }

    @Override
    public void onCall(CallInvitation invitation, String reason)
    {
        if (destroyed)
        {
            return;
        }
        if ("incoming".equals(reason))
        {
            connectionLog.add(ConnectionLog.Event.INCOMING_CALL);
            notifications.call(invitation);
        }
        else
        {
            notifications.endCall(invitation, reason, activeScope);
        }
    }

    @Override
    public void onPoke(PokeMessage poke)
    {
        if (destroyed || snapshot == null || snapshot.connectionId != poke.connectionId)
        {
            return;
        }
        if (chatObservers.isEmpty())
        {
            pendingPokes.remove(poke.conversationId);
            pendingPokes.put(poke.conversationId, poke);
            while (pendingPokes.size() > 32)
            {
                pendingPokes.remove(pendingPokes.keySet().iterator().next());
            }
            notifications.poke(poke, activeScope);
        }
        else
        {
            sounds.play("poke");
            onError(poke.senderName + " poked you" + (poke.text.isEmpty() ? "" : ": " + poke.text));
        }
    }

    private boolean setMediaEnabled(boolean enabled)
    {
        if (enabled && !foreground)
        {
            return false;
        }
        boolean previous = mediaEnabled;
        mediaEnabled = enabled;
        if (!enabled)
        {
            talking = false;
            voiceReady = false;
            voiceStatus = "Audio is paused while you are away";
        }
        if (foreground && previous != enabled && !updateForeground(false))
        {
            mediaEnabled = false;
            session.setMediaAllowed(false);
            return false;
        }
        session.setMediaAllowed(enabled);
        return true;
    }

    private void startSavedConnection()
    {
        if (destroyed || identityLoading || connectPending || connectionActive || !connectionWanted || reconnectSuppressed)
        {
            return;
        }
        automaticPending = false;
        if (!hasSavedConnection())
        {
            connectionWanted = false;
            preferences.edit().putBoolean("connection_requested", false).apply();
            stopForeground(STOP_FOREGROUND_REMOVE);
            foreground = false;
            stopSelf();
            onError(identityError != null ? identityError : "Open the app and save your server connection first");
            return;
        }
        try
        {
            ChatSession.validateEndpoint(savedEndpoint);
            if (!foreground && chatObservers.isEmpty())
            {
                onError("Open the app to reconnect to your server");
                return;
            }
            cancelRetry();
            startService(new Intent(this, ChatService.class));
            if (!updateForeground(false))
            {
                connectionWanted = false;
                stopSelf();
                return;
            }
            connectionActive = true;
            forceBackgroundIdle = chatObservers.isEmpty();
            setMediaEnabled(!chatObservers.isEmpty() && !manualIdle);
            preferences.edit().putBoolean("connection_requested", true).apply();
            session.connect(savedEndpoint, savedUsername, identity, serverKeys);
        }
        catch (RuntimeException error)
        {
            connectionActive = false;
            connectionWanted = false;
            preferences.edit().putBoolean("connection_requested", false).apply();
            stopForeground(STOP_FOREGROUND_REMOVE);
            foreground = false;
            stopSelf();
            onError("Android could not reconnect. Open the app and try again.");
        }
    }

    private void updateConnectionLease()
    {
        boolean needed = !destroyed && foreground && connectionWanted && !retryPending && (connectionActive || connectPending || automaticPending || identityLoading);
        connectionLease.setActive(needed);
    }

    private void scheduleRetry()
    {
        if (retryPending)
        {
            return;
        }
        retryPending = true;
        connectionLog.add(ConnectionLog.Event.RECONNECT_SCHEDULED);
        main.postDelayed(retry, backoff.nextDelay());
        if (!updateForeground(false))
        {
            cancelRetry();
            connectionWanted = false;
            stopSelf();
        }
    }

    private void cancelRetry()
    {
        main.removeCallbacks(retry);
        retryPending = false;
    }

    private void registerNetworkChanges()
    {
        connectivity = getSystemService(ConnectivityManager.class);
        networkCallback = new ConnectivityManager.NetworkCallback()
        {
            @Override
            public void onAvailable(Network network)
            {
                main.post(() ->
                {
                    if (!destroyed && retryPending && connectionWanted && !reconnectSuppressed)
                    {
                        main.removeCallbacks(retry);
                        main.postDelayed(retry, 2000);
                    }
                });
            }
        };
        try
        {
            connectivity.registerDefaultNetworkCallback(networkCallback);
        }
        catch (RuntimeException error)
        {
            networkCallback = null;
            android.util.Log.w("LemonNative", "Network change monitoring is unavailable", error);
        }
    }
}
