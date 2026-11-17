package com.lemonchat;

import android.util.Log;

import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;

/**
 * Exchanges settings, connection state and call actions with Node over local JSON lines.
 * Requires the private token before accepting events.
 */
public class NodeBridge
{
	private static final String TAG = "lemonchat-node";

	private volatile BackgroundService backgroundService;
	private final String token = randomToken();
	private boolean initialConnectSent;
	private PendingConnectIntent pendingConnectIntent;

	private ServerSocket serverSocket = null;
	private volatile Connection currentConnection;
	private volatile boolean closed;
	private volatile boolean uiVisible;
	private volatile boolean uiPreserveIdle;
	private boolean uiForceIdle;
	private volatile int lastUnread;
	private volatile JSONObject pendingCall;
	private PendingCallAction pendingCallAction;
	private ScheduledFuture<?> pendingCallActionExpiry;
	private volatile String runtimeFailureReason;
	private final ArrayDeque<PendingWrite> pendingWrites = new ArrayDeque<>();
	private final java.util.Set<Socket> candidateSockets = java.util.concurrent.ConcurrentHashMap.newKeySet();
	private int pendingBytes;
	private final ScheduledThreadPoolExecutor watchdog = new ScheduledThreadPoolExecutor(1, runnable ->
	{
		Thread thread = new Thread(runnable, "lemonchat-bridge-watchdog");
		thread.setDaemon(true);
		return thread;
	});
	private final ThreadPoolExecutor candidates = new ThreadPoolExecutor(2, 2, 0, TimeUnit.SECONDS,
		new ArrayBlockingQueue<Runnable>(2), runnable ->
		{
			Thread thread = new Thread(runnable, "lemonchat-bridge-auth");
			thread.setDaemon(true);
			return thread;
		});
	private final ThreadPoolExecutor events = new ThreadPoolExecutor(1, 1, 0, TimeUnit.SECONDS,
		new ArrayBlockingQueue<Runnable>(8), runnable ->
		{
			Thread thread = new Thread(runnable, "lemonchat-bridge-events");
			thread.setDaemon(true);
			return thread;
		});

	private static final class Connection
	{
		final Socket socket;
		final OutputStream output;

		/**
		 * Associates an authenticated socket with its output stream.
		 *
		 * @param socket authenticated control socket
		 * @throws IOException if its output stream cannot be opened
		 */
		Connection(Socket socket) throws IOException
		{
			this.socket = socket;
			this.output = socket.getOutputStream();
		}
	}

	private static final class PendingWrite
	{
		final Connection connection;
		final byte[] bytes;

		/**
		 * Records bytes for one connection's writer.
		 *
		 * @param connection connection that owns this write
		 * @param bytes encoded command including its line ending
		 */
		PendingWrite(Connection connection, byte[] bytes)
		{
			this.connection = connection;
			this.bytes = bytes;
		}
	}

	private static final class PendingCallAction
	{
		final String action;
		final String callId;
		final String sessionId;
		final long expiresAt;

		/**
		 * Retains a call action for retry until its original expiry.
		 *
		 * @param action accept or decline
		 * @param callId call capability
		 * @param sessionId session that owns the call
		 * @param expiresAt absolute expiry time in milliseconds
		 */
		PendingCallAction(String action, String callId, String sessionId, long expiresAt)
		{
			this.action = action;
			this.callId = callId;
			this.sessionId = sessionId;
			this.expiresAt = expiresAt;
		}

		/**
		 * Checks both identifiers against this pending action.
		 *
		 * @param callId call capability to check
		 * @param sessionId session to check
		 * @return whether both identifiers match
		 */
		boolean matches(String callId, String sessionId)
		{
			return this.callId.equals(callId) && this.sessionId.equals(sessionId);
		}
	}

	private static final class PendingConnectIntent
	{
		final String commandId = java.util.UUID.randomUUID().toString();
		final boolean connect;

		/**
		 * Records a connection action with its own acknowledgement ID.
		 *
		 * @param connect true for Connect, false for Disconnect
		 */
		PendingConnectIntent(boolean connect)
		{
			this.connect = connect;
		}
	}

	/**
	 * Replaces the native event recipient and replays the current bridge state.
	 *
	 * @param service service to attach, or {@code null} to remove the recipient
	 */
	public void attachService(BackgroundService service)
	{
		this.backgroundService = service;
		if (service != null && this.getLoopbackPort() > 0)
		{
			this.sendSettings();
			this.ensureInitialConnect();
			this.sendNetworkState(service.isNetworkAvailable());
			this.sendUiVisible(MainActivity.instance != null, true);
			service.showUnreadBadge(this.lastUnread);
			this.pushConnectionPhaseToWebview();
			JSONObject call = this.pendingCall;
			if (call != null)
			{
				this.publishCall(service, call);
			}
		}
	}

	/**
	 * Applies the saved autoconnect preference once after usable settings are available.
	 * An explicit connection action already recorded by the bridge takes precedence.
	 */
	private synchronized void ensureInitialConnect()
	{
		if (this.initialConnectSent)
		{
			return;
		}
		try
		{
			JSONObject settings = new JSONObject(ChatSettings.getInstance().getJsonSettings());
			if (settings.has("identity_error"))
			{
				return;
			}
			this.initialConnectSent = true;
			if (settings.optBoolean("is_autoconnect_enabled", true))
			{
				this.sendConnect();
			}
		}
		catch (Exception invalidSettings)
		{
			Log.w(TAG, "initial settings unavailable");
		}
	}

	/**
	 * Removes the event recipient only if it is still the specified service.
	 *
	 * @param service service being detached
	 */
	public void detachService(BackgroundService service)
	{
		if (this.backgroundService == service)
		{
			this.backgroundService = null;
		}
	}

	/**
	 * Closes a socket without letting cleanup failure interrupt ownership changes.
	 *
	 * @param socket socket to close
	 */
	private static void closeSocket(Socket socket)
	{
		try
		{
			socket.close();
		}
		catch (IOException ignored)
		{
		}
	}

	/**
	 * Creates the secret used to authenticate the local Node control connection.
	 *
	 * @return a 64 character hexadecimal token from 32 random bytes
	 */
	private static String randomToken()
	{
		byte[] bytes = new byte[32];
		new java.security.SecureRandom().nextBytes(bytes);
		StringBuilder value = new StringBuilder(64);
		for (byte item : bytes)
		{
			value.append(Character.forDigit((item & 255) >>> 4, 16));
			value.append(Character.forDigit(item & 15, 16));
		}
		return value.toString();
	}

	// node's loopback ui endpoint, announced in its hello; the webview settings point at it
	private volatile int loopbackPort = 0;
	private volatile String loopbackToken = "";

	// whether node's session is connected to the server, from its last status event
	public volatile boolean isServerConnected = false;

	// last connection phase from node, replayed to a page that loads later
	private volatile String lastPhaseState = "";
	private volatile String lastPhaseReason = "";

	/**
	 * Returns the page WebSocket port announced by the current runtime.
	 *
	 * @return the port, or zero before announcement or after terminal runtime failure
	 */
	public int getLoopbackPort()
	{
		return this.runtimeFailureReason == null ? this.loopbackPort : 0;
	}

	/**
	 * Retires the control transport and publishes an irreversible runtime failure state.
	 *
	 * @param reason recovery explanation, retained from the first failure
	 */
	public void reportRuntimeFailure(String reason)
	{
		synchronized (this)
		{
			if (this.runtimeFailureReason == null)
			{
				this.runtimeFailureReason = reason;
			}
			this.loopbackPort = 0;
			this.loopbackToken = "";
			this.lastPhaseState = "runtime_failed";
			this.lastPhaseReason = this.runtimeFailureReason;
			this.pendingCall = null;
			this.close();
		}
		BackgroundService service = this.backgroundService;
		if (service != null)
		{
			service.clearIncomingCall();
		}
		this.pushConnectionPhaseToWebview();
	}

	/**
	 * Returns the capability for the page WebSocket connection.
	 *
	 * @return the announced token, or an empty string if the runtime is unavailable
	 */
	public String getLoopbackToken()
	{
		return this.runtimeFailureReason == null ? this.loopbackToken : "";
	}

	/**
	 * Creates the process bridge without opening its listening socket.
	 *
	 * @param backgroundService initial native event recipient, which may be {@code null}
	 */
	public NodeBridge(BackgroundService backgroundService)
	{
		this.backgroundService = backgroundService;
	}

	/**
	 * Returns the Java control socket port for the private Node configuration.
	 *
	 * @return the listening port, or zero before the server socket exists
	 */
	public int getPort()
	{
		return (this.serverSocket != null) ? this.serverSocket.getLocalPort() : 0;
	}

	/**
	 * Returns the capability required as the first control connection line.
	 *
	 * @return the process bridge authentication token
	 */
	public String getToken()
	{
		return this.token;
	}

	/**
	 * Starts bounded authentication, event processing and the ordered writer.
	 *
	 * @return whether the listening socket is available
	 */
	public synchronized boolean start()
	{
		if (this.closed)
		{
			return false;
		}
		if (this.serverSocket != null)
		{
			return true;
		}
		try
		{
			this.serverSocket = new ServerSocket(0, 4, InetAddress.getByName("127.0.0.1"));
		}
		catch (IOException failed)
		{
			Log.e(TAG, "bridge bind failed");
			return false;
		}
		this.watchdog.setRemoveOnCancelPolicy(true);
		Thread writer = new Thread(this::writeLoop, "lemonchat-bridge-writer");
		writer.setDaemon(true);
		writer.start();
		Thread accept = new Thread(() ->
		{
			while (!this.closed)
			{
				try
				{
					this.queueCandidate(this.serverSocket.accept());
				}
				catch (IOException failed)
				{
					if (!this.closed)
					{
						Log.w(TAG, "bridge accept failed");
					}
				}
			}
		}, "lemonchat-bridge-accept");
		accept.setDaemon(true);
		accept.start();
		return true;
	}

	/**
	 * Admits one authentication candidate within the bounded worker queue and deadline.
	 *
	 * @param socket newly accepted local socket
	 */
	private synchronized void queueCandidate(Socket socket)
	{
		if (this.closed)
		{
			closeSocket(socket);
			return;
		}
		this.candidateSockets.add(socket);
		ScheduledFuture<?> deadline = null;
		try
		{
			socket.setSoTimeout(5000);
			deadline = this.watchdog.schedule(() -> closeSocket(socket), 5, TimeUnit.SECONDS);
			final ScheduledFuture<?> authenticationDeadline = deadline;
			this.candidates.execute(() -> this.authenticate(socket, authenticationDeadline));
		}
		catch (IOException | java.util.concurrent.RejectedExecutionException failed)
		{
			if (deadline != null)
			{
				deadline.cancel(false);
			}
			this.candidateSockets.remove(socket);
			closeSocket(socket);
		}
	}

	/**
	 * Checks the first token line before offering the socket to the serialized event owner.
	 *
	 * @param socket candidate connection
	 * @param deadline absolute authentication timeout to cancel after adoption or failure
	 */
	private void authenticate(Socket socket, ScheduledFuture<?> deadline)
	{
		boolean adopted = false;
		try
		{
			BufferedInputStream input = new BufferedInputStream(socket.getInputStream());
			String first = BridgeProtocol.readLine(input, 128);
			if (!this.token.equals(first))
			{
				return;
			}
			socket.setSoTimeout(0);
			Connection connection = new Connection(socket);
			this.events.execute(() -> this.adoptConnection(connection, input, deadline));
			adopted = true;
		}
		catch (Exception failed)
		{
			Log.w(TAG, "bridge authentication failed");
		}
		finally
		{
			if (!adopted)
			{
				deadline.cancel(false);
				this.candidateSockets.remove(socket);
				closeSocket(socket);
			}
		}
	}

	/**
	 * Installs an authenticated connection and retires its predecessor and queued writes.
	 *
	 * @param connection authenticated replacement connection
	 * @param input buffered stream that may already contain event bytes
	 * @param deadline pending authentication timeout
	 */
	private void adoptConnection(Connection connection, BufferedInputStream input, ScheduledFuture<?> deadline)
	{
		boolean started = false;
		try
		{
			synchronized (this)
			{
				if (this.closed || connection.socket.isClosed())
				{
					return;
				}
				Connection previous = this.currentConnection;
				this.currentConnection = connection;
				this.pendingWrites.clear();
				this.pendingBytes = 0;
				if (previous != null)
				{
					closeSocket(previous.socket);
				}
			}
			Thread reader = new Thread(() -> this.readLoop(connection, input), "lemonchat-bridge-reader");
			reader.setDaemon(true);
			reader.start();
			started = true;
		}
		finally
		{
			deadline.cancel(false);
			this.candidateSockets.remove(connection.socket);
			if (!started)
			{
				this.dropConnection(connection);
			}
		}
	}

	/**
	 * Reads bounded control event lines and queues callbacks scoped to their connection.
	 *
	 * @param connection owner whose closure must not clear a replacement
	 * @param input authenticated buffered input stream
	 */
	private void readLoop(Connection connection, BufferedInputStream input)
	{
		try
		{
			String line;
			while ((line = BridgeProtocol.readLine(input, BridgeProtocol.MAX_LINE_BYTES)) != null)
			{
				final String eventLine = line;
				this.events.execute(() ->
				{
					if (this.currentConnection == connection)
					{
						this.handleEvent(eventLine);
					}
				});
			}
		}
		catch (Exception failed)
		{
			Log.w(TAG, "bridge reader ended");
		}
		finally
		{
			try
			{
				this.events.execute(() -> this.dropConnection(connection));
			}
			catch (java.util.concurrent.RejectedExecutionException full)
			{
				this.dropConnection(connection);
			}
		}
	}

	/**
	 * Closes the specified socket and clears queued writes only if it still owns the bridge.
	 *
	 * @param connection connection being retired
	 */
	private synchronized void dropConnection(Connection connection)
	{
		closeSocket(connection.socket);
		if (this.currentConnection == connection)
		{
			this.currentConnection = null;
			this.pendingWrites.clear();
			this.pendingBytes = 0;
		}
	}

	/**
	 * Writes queued commands in order, with one independent timeout for each write.
	 */
	private void writeLoop()
	{
		while (!this.closed)
		{
			PendingWrite next;
			synchronized (this)
			{
				while (this.pendingWrites.isEmpty() && !this.closed)
				{
					try
					{
						this.wait();
					}
					catch (InterruptedException stopped)
					{
						Thread.currentThread().interrupt();
						return;
					}
				}
				if (this.closed)
				{
					return;
				}
				next = this.pendingWrites.removeFirst();
				this.pendingBytes -= next.bytes.length;
			}
			ScheduledFuture<?> deadline;
			synchronized (this)
			{
				if (this.closed || this.currentConnection != next.connection)
				{
					continue;
				}
				deadline = this.watchdog.schedule(() -> closeSocket(next.connection.socket), 5, TimeUnit.SECONDS);
			}
			try
			{
				next.connection.output.write(next.bytes);
				next.connection.output.flush();
			}
			catch (IOException failed)
			{
				this.dropConnection(next.connection);
			}
			finally
			{
				deadline.cancel(false);
			}
		}
	}

	/**
	 * Permanently closes listeners, sockets and executors and discards pending actions.
	 */
	public synchronized void close()
	{
		this.closed = true;
		this.pendingConnectIntent = null;
		this.clearPendingCallAction();
		if (this.currentConnection != null)
		{
			this.dropConnection(this.currentConnection);
		}
		try
		{
			if (this.serverSocket != null)
			{
				this.serverSocket.close();
			}
		}
		catch (IOException ignored)
		{
		}
		this.candidates.shutdownNow();
		this.events.shutdownNow();
		for (Socket candidate : this.candidateSockets)
		{
			closeSocket(candidate);
		}
		this.candidateSockets.clear();
		this.watchdog.shutdownNow();
		this.notifyAll();
	}

	/**
	 * Validates and applies one authenticated Node event without logging its payload.
	 *
	 * @param line complete UTF-8 JSON event line
	 */
	private void handleEvent(String line)
	{
		if (line.length() == 0 || this.runtimeFailureReason != null)
		{
			return;
		}

		try
		{
			JSONObject event = new JSONObject(line);
			String type = event.optString("type", "");
			synchronized (this)
			{
				if (this.runtimeFailureReason != null)
				{
					return;
				}
				if (type.equals("loopback"))
				{
					int port = event.optInt("port", 0);
					String announcedToken = event.optString("token", "");
					if (port < 1 || port > 65535 || !announcedToken.matches("[a-fA-F0-9]{32,128}"))
					{
						return;
					}
					this.loopbackPort = port;
					this.loopbackToken = announcedToken;
				}
				else if (type.equals("status_text"))
				{
					this.lastPhaseState = event.optString("state", "");
					this.lastPhaseReason = event.optString("reason", "");
				}
				else if (type.equals("unread"))
				{
					this.lastUnread = Math.max(0, event.optInt("count", 0));
				}
				else if (type.equals("status") && Boolean.FALSE.equals(event.opt("connected")))
				{
					this.pendingCall = null;
				}
				else if (type.equals("call"))
				{
					String callId = event.optString("call_id", "");
					String sessionId = event.optString("session_id", "");
					long expiresAt = event.optLong("expires_at", 0);
					long now = System.currentTimeMillis();
					if (!callId.matches("[a-fA-F0-9]{32}") || sessionId.isEmpty() || sessionId.length() > 128
						|| expiresAt <= now || expiresAt > now + 120000)
					{
						return;
					}
					if (this.pendingCallAction != null && this.pendingCallAction.matches(callId, sessionId))
					{
						return;
					}
					this.clearPendingCallAction();
					this.pendingCall = event;
				}
				else if (type.equals("call_action_result"))
				{
					if (this.pendingCallAction != null && event.has("accepted") && event.get("accepted") instanceof Boolean
						&& this.pendingCallAction.matches(event.optString("call_id", ""), event.optString("session_id", "")))
					{
						if (event.optBoolean("accepted", false) && "accept".equals(this.pendingCallAction.action)
							&& this.pendingCallAction.expiresAt > System.currentTimeMillis())
						{
							this.uiForceIdle = false;
						}
						this.clearPendingCallAction();
					}
					return;
				}
				else if (type.equals("connect_intent_result"))
				{
					if (this.pendingConnectIntent != null && event.has("accepted") && event.get("accepted") instanceof Boolean
						&& this.pendingConnectIntent.commandId.equals(event.optString("command_id", "")))
					{
						this.pendingConnectIntent = null;
					}
					return;
				}
			}

			BackgroundService service = this.backgroundService;
			if (this.runtimeFailureReason != null)
			{
				return;
			}

			if (type.equals("loopback"))
			{
				if (service != null)
				{
					// Node owns the connection; restore current native settings and intent first.
					this.sendSettings();
				}
				this.flushPendingConnectIntent();
				if (service != null)
				{
					this.ensureInitialConnect();
				}
				this.sendRetainedUiVisibility();
				// Accept must follow visibility replay so an older hidden state cannot undo it.
				this.flushPendingCallAction();
				if (service == null)
				{
					return;
				}

				// and the network state - the startup one was sent before node was listening
				this.sendNetworkState(service.isNetworkAvailable());

				// node starts out logging, so if the user turned it off we must say so early
				this.sendLoggingEnabled(ChatSettings.getInstance().isFileLoggingEnabled());

				// A page that loaded before the endpoint announcement is waiting for these settings.
				// Publish the announced endpoint so it can attach to Node's current session.
				if (service.javascriptJavaBridge != null)
				{
					service.javascriptJavaBridge.JavaExportRequestCurrentSettingsFromAndroid();
				}
			}
			else if (service == null)
			{
				return;
			}
			else if (type.equals("status_text"))
			{
				//the connect page's live phase line (key generation, connecting, retrying).
				//java carries it because node's thread is frozen during the key derive
				this.pushConnectionPhaseToWebview();
			}
			else if (type.equals("status"))
			{
				this.isServerConnected = event.optBoolean("connected", false);
				if (Boolean.FALSE.equals(event.opt("connected")))
				{
					service.clearIncomingCall();
				}
				//node just (re)connected: tell the page, so it reattaches right away
				//instead of waiting out its retry countdown
				final android.webkit.WebView webView = service.webView;
				if (event.optBoolean("connected", false) && webView != null)
				{
					final long generation = service.getPageGeneration();

					webView.post(new Runnable()
					{
						public void run()
						{
							if (NodeBridge.this.backgroundService != service || service.webView != webView
								|| !service.isTrustedPage() || service.getPageGeneration() != generation)
							{
								return;
							}
							webView.evaluateJavascript("if (typeof JavascriptJavaBridge__request_loopback_reattach === 'function')\n{\n    JavascriptJavaBridge__request_loopback_reattach();\n}", null);
						}
					});
				}
			}
			else if (type.equals("unread"))
			{
				// node counts while nobody is looking, so the icon is right even when the app
				// is idle, closed, or was started headless at boot
				service.showUnreadBadge(event.optInt("count", 0));
			}
			else if (type.equals("call"))
			{
				// a call node heard about while there was no webview. same screen either way
				this.publishCall(service, event);
			}
			else if (type.equals("poke"))
			{
				String sender = event.optString("username", "");
				String message = event.optString("message", "");
				if (sender.length() <= 512 && message.length() <= 16384 && MainActivity.instance == null)
				{
					service.showPokeNotification(sender, message);
				}
			}
			else if (type.equals("chat_activity"))
			{
				// same rule as pokes: the page shows its own ui while the app is in the foreground
				if (MainActivity.instance != null)
				{
					return;
				}

				String sender = event.optString("username", "");
				if (sender.length() > 512)
				{
					return;
				}
				service.showMessageNotification(sender.length() > 0 ? sender : "lemon chat", "new message");
			}
		}
		catch (Exception badEvent)
		{
			Log.w(TAG, "invalid bridge event");
		}
	}

	/**
	 * Publishes a bounded call only while its service and pending call ownership are current.
	 * Publication shares the service lock with notification cleanup and native call actions.
	 *
	 * @param service current service receiving the notification
	 * @param event validated call record containing its session, capability and expiry
	 */
	private void publishCall(BackgroundService service, JSONObject event)
	{
		String caller = event.optString("caller", "somebody");
		if (caller.length() > 512)
		{
			return;
		}
		synchronized (service)
		{
			if (this.backgroundService != service || this.pendingCall != event)
			{
				return;
			}
			service.showIncomingCall(caller, event.optInt("channel_id", 0), event.optString("call_id", ""),
				event.optString("session_id", ""), event.optLong("expires_at", 0));
		}
	}

	/**
	 * Enqueues one command for the current connection or closes it if capacity is exhausted.
	 *
	 * @param json command serialized as one JSON object
	 */
	private synchronized void send(final String json)
	{
		Connection connection = this.currentConnection;
		if (connection == null || this.closed)
		{
			return;
		}
		byte[] bytes = (BridgeProtocol.escapeSurrogates(json) + "\n").getBytes(StandardCharsets.UTF_8);
		if (bytes.length > BridgeProtocol.MAX_LINE_BYTES || this.pendingBytes + bytes.length > BridgeProtocol.MAX_PENDING_BYTES
			|| this.pendingWrites.size() >= 256)
		{
			Log.w(TAG, "bridge output capacity exceeded");
			this.dropConnection(connection);
			return;
		}
		this.pendingWrites.addLast(new PendingWrite(connection, bytes));
		this.pendingBytes += bytes.length;
		this.notifyAll();
	}

	/**
	 * Captures and queues settings under one lock to preserve publication order.
	 */
	public synchronized void sendSettings()
	{
		try
		{
			String jsonSettings = ChatSettings.getInstance().getJsonSettings();

			if (jsonSettings.length() == 0)
			{
				Log.w(TAG, "no settings to hand to node yet");
				return;
			}

			JSONObject command = new JSONObject();
			command.put("type", "settings");
			command.put("json", jsonSettings);

			this.send(command.toString());
		}
		catch (Exception buildFailed)
		{
			Log.e(TAG, "could not build settings command", buildFailed);
		}
	}

	/**
	 * Records an explicit Disconnect until Node acknowledges its unique command ID.
	 */
	public synchronized void sendDisconnect()
	{
		if (this.closed)
		{
			return;
		}
		this.initialConnectSent = true;
		this.pendingConnectIntent = new PendingConnectIntent(false);
		this.flushPendingConnectIntent();
	}

	/**
	 * Records an explicit Connect until Node acknowledges its unique command ID.
	 */
	public synchronized void sendConnect()
	{
		if (this.closed)
		{
			return;
		}
		this.initialConnectSent = true;
		this.pendingConnectIntent = new PendingConnectIntent(true);
		this.flushPendingConnectIntent();
	}

	/**
	 * Queues the latest unacknowledged connection action without changing its command ID.
	 */
	private synchronized void flushPendingConnectIntent()
	{
		if (this.pendingConnectIntent != null && this.currentConnection != null)
		{
			this.send("{\"type\":" + JSONObject.quote(this.pendingConnectIntent.connect ? "connect" : "disconnect")
				+ ",\"command_id\":" + JSONObject.quote(this.pendingConnectIntent.commandId) + "}");
		}
	}

	/**
	 * Retains a current call action until its matching Node result or original expiry.
	 * A retry reuses the same call and session capability.
	 *
	 * @param action {@code accept} or {@code decline}
	 * @param callId unpredictable ID of the current incoming call
	 * @param sessionId logical session that owns the call
	 */
	public synchronized void sendCallAction(String action, String callId, String sessionId)
	{
		if (this.closed || (!"accept".equals(action) && !"decline".equals(action)))
		{
			return;
		}
		if (this.pendingCallAction != null && this.pendingCallAction.matches(callId, sessionId))
		{
			this.flushPendingCallAction();
			return;
		}
		if (this.pendingCall == null || !this.pendingCall.optString("call_id", "").equals(callId)
			|| !this.pendingCall.optString("session_id", "").equals(sessionId))
		{
			return;
		}
		long expiresAt = this.pendingCall.optLong("expires_at", 0);
		this.pendingCall = null;
		if (expiresAt <= System.currentTimeMillis())
		{
			return;
		}
		this.clearPendingCallAction();
		PendingCallAction pending = new PendingCallAction(action, callId, sessionId, expiresAt);
		this.pendingCallAction = pending;
		this.pendingCallActionExpiry = this.watchdog.schedule(() ->
		{
			synchronized (NodeBridge.this)
			{
				if (NodeBridge.this.pendingCallAction == pending)
				{
					NodeBridge.this.clearPendingCallAction();
				}
			}
		}, Math.max(0, expiresAt - System.currentTimeMillis()), TimeUnit.MILLISECONDS);
		this.flushPendingCallAction();
	}

	/**
	 * Drops the retained call action and cancels its expiry callback.
	 */
	private synchronized void clearPendingCallAction()
	{
		this.pendingCallAction = null;
		if (this.pendingCallActionExpiry != null)
		{
			this.pendingCallActionExpiry.cancel(false);
			this.pendingCallActionExpiry = null;
		}
	}

	/**
	 * Replays the unexpired pending call action through the current control connection.
	 */
	private synchronized void flushPendingCallAction()
	{
		PendingCallAction pending = this.pendingCallAction;
		if (pending == null)
		{
			return;
		}
		if (pending.expiresAt <= System.currentTimeMillis())
		{
			this.clearPendingCallAction();
			return;
		}
		this.send("{\"type\":\"call_action\",\"action\":" + JSONObject.quote(pending.action)
			+ ",\"call_id\":" + JSONObject.quote(pending.callId) + ",\"session_id\":" + JSONObject.quote(pending.sessionId) + "}");
	}

	/**
	 * Reports the device network state to the Node connection owner.
	 *
	 * @param isAvailable whether Android reports an active network
	 */
	public void sendNetworkState(boolean isAvailable)
	{
		this.send("{\"type\":\"network\",\"available\":" + (isAvailable ? "true" : "false") + "}");
	}

	/**
	 * Posts the retained connection phase to the current trusted page.
	 * The callback checks both page generation and phase freshness before changing the UI.
	 */
	public void pushConnectionPhaseToWebview()
	{
		BackgroundService service = this.backgroundService;
		final android.webkit.WebView webView = service == null ? null : service.webView;
		if (this.lastPhaseState.length() == 0 || service == null || webView == null)
		{
			return;
		}

		final long generation = service.getPageGeneration();
		final String phaseState;
		final String phaseReason;
		synchronized (this)
		{
			phaseState = this.lastPhaseState;
			phaseReason = this.lastPhaseReason;
		}
		final String javascript = "if (typeof JavascriptJavaBridge__show_connection_phase === 'function')\n{\n    JavascriptJavaBridge__show_connection_phase("
			+ JSONObject.quote(phaseState) + ", " + JSONObject.quote(phaseReason) + ");\n}";

		webView.post(new Runnable()
		{
			public void run()
			{
				if (NodeBridge.this.backgroundService != service || service.webView != webView
					|| !service.isTrustedPage() || service.getPageGeneration() != generation
					|| !phaseState.equals(NodeBridge.this.lastPhaseState) || !phaseReason.equals(NodeBridge.this.lastPhaseReason))
				{
					return;
				}
				webView.evaluateJavascript(javascript, null);
			}
		});
	}

	/**
	 * Queues a legacy page log command that Node currently discards.
	 *
	 * @param line legacy log text, which must not contain secrets
	 */
	public void sendLogLine(String line)
	{
		this.send("{\"type\":\"log\",\"line\":" + JSONObject.quote(line) + "}");
	}

	/**
	 * Updates the Node runtime file logging switch.
	 *
	 * @param isEnabled whether file logging is enabled
	 */
	public void sendLoggingEnabled(boolean isEnabled)
	{
		this.send("{\"type\":\"log_enabled\",\"enabled\":" + (isEnabled ? "true" : "false") + "}");
	}

	/**
	 * Reports native visibility without a special idle override.
	 *
	 * @param isVisible whether the main activity is on screen
	 */
	public void sendUiVisible(boolean isVisible)
	{
		this.sendUiVisible(isVisible, false);
	}

	/**
	 * Reports visibility while optionally preserving the current idle state.
	 *
	 * @param isVisible whether the main activity is on screen
	 * @param preserveIdle whether this transition should avoid leaving idle
	 */
	public void sendUiVisible(boolean isVisible, boolean preserveIdle)
	{
		this.sendUiVisible(isVisible, preserveIdle, false);
	}

	/**
	 * Retains authoritative native visibility and presence intent for control reconnection.
	 * A newer forced task removal supersedes an Accept still awaiting its result.
	 *
	 * @param isVisible whether the main activity is on screen
	 * @param preserveIdle whether this transition should avoid leaving idle
	 * @param forceIdle whether a hidden transition explicitly forces idle
	 */
	public synchronized void sendUiVisible(boolean isVisible, boolean preserveIdle, boolean forceIdle)
	{
		if (!isVisible && forceIdle && this.pendingCallAction != null && "accept".equals(this.pendingCallAction.action))
		{
			// A later task removal supersedes an earlier Accept still awaiting its result.
			this.clearPendingCallAction();
		}
		this.uiVisible = isVisible;
		this.uiForceIdle = !isVisible && (forceIdle || this.uiForceIdle);
		this.uiPreserveIdle = preserveIdle && !this.uiForceIdle;
		this.sendRetainedUiVisibility();
	}

	/**
	 * Queues the latest native visibility and idle policy without inventing a new transition.
	 */
	private synchronized void sendRetainedUiVisibility()
	{
		this.send("{\"type\":\"ui_visible\",\"visible\":" + this.uiVisible
			+ ",\"preserve_idle\":" + this.uiPreserveIdle + ",\"force_idle\":" + this.uiForceIdle + "}");
	}
}
