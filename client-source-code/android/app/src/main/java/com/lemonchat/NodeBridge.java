package com.lemonchat;

import android.util.Log;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.UUID;

/**
 * The java side of the java <-> node bridge: a loopback TCP server speaking JSON lines.
 *
 * Node dials in (it reads the port and token from bridge.json, written by NodeRuntime) and must
 * send the token as its first line - loopback is reachable by every app on the device.
 *
 * Commands out: "settings" (connect with this json), "disconnect" (park the socket).
 * Events in: "status", "member_change", "chat_activity" - chat_activity raises the notification.
 */
public class NodeBridge
{
	private static final String TAG = "lemonchat-node";

	private final BackgroundService backgroundService;
	private final String token = UUID.randomUUID().toString();

	private ServerSocket serverSocket = null;
	private PrintWriter currentWriter = null;

	// node's loopback ui endpoint, announced in its hello; the webview settings point at it
	private volatile int loopbackPort = 0;
	private volatile String loopbackToken = "";

	public int getLoopbackPort()
	{
		return this.loopbackPort;
	}

	public String getLoopbackToken()
	{
		return this.loopbackToken;
	}

	public NodeBridge(BackgroundService backgroundService)
	{
		this.backgroundService = backgroundService;
	}

	public int getPort()
	{
		return (this.serverSocket != null) ? this.serverSocket.getLocalPort() : 0;
	}

	public String getToken()
	{
		return this.token;
	}

	/** binds the loopback server and starts the accept loop; call before NodeRuntime.start */
	public boolean start()
	{
		try
		{
			this.serverSocket = new ServerSocket(0, 4, InetAddress.getByName("127.0.0.1"));
		}
		catch (Exception bindFailed)
		{
			Log.e(TAG, "bridge could not bind a loopback port", bindFailed);
			return false;
		}

		Thread acceptThread = new Thread(new Runnable()
		{
			@Override public void run()
			{
				while (true)
				{
					try
					{
						Socket connection = NodeBridge.this.serverSocket.accept();
					Log.i(TAG, "bridge accepted a connection");
						NodeBridge.this.serveConnection(connection);
					}
					catch (Exception acceptFailed)
					{
						Log.e(TAG, "bridge accept failed", acceptFailed);
						return;
					}
				}
			}
		}, "lemonchat-node-bridge");

		acceptThread.setDaemon(true);
		acceptThread.start();

		Log.i(TAG, "bridge listening on 127.0.0.1:" + this.getPort());
		return true;
	}

	private void serveConnection(Socket connection)
	{
		try
		{
			BufferedReader reader = new BufferedReader(new InputStreamReader(connection.getInputStream(), "UTF-8"));

			String firstLine = reader.readLine();

			if (firstLine == null || firstLine.trim().equals(this.token) == false)
			{
				Log.e(TAG, "bridge connection with wrong token, dropping it");
				connection.close();
				return;
			}

			synchronized (this)
			{
				this.currentWriter = new PrintWriter(new OutputStreamWriter(connection.getOutputStream(), "UTF-8"), true);
			}

			Log.i(TAG, "node connected to the bridge");

			String line;
			while ((line = reader.readLine()) != null)
			{
				this.handleEvent(line.trim());
			}
		}
		catch (Exception connectionDied)
		{
			Log.w(TAG, "bridge connection ended: " + connectionDied.getMessage());
		}
		finally
		{
			synchronized (this)
			{
				this.currentWriter = null;
			}
		}
	}

	private void handleEvent(String line)
	{
		if (line.length() == 0)
		{
			return;
		}

		try
		{
			JSONObject event = new JSONObject(line);
			String type = event.optString("type", "");

			Log.i(TAG, "bridge event: " + line);

			if (type.equals("loopback"))
			{
				this.loopbackPort = event.optInt("port", 0);
				this.loopbackToken = event.optString("token", "");

				// node is the connection authority: hand it the server details right away
				this.sendSettings();

				// and the network state - the startup one was sent before node was listening
				this.sendNetworkState(this.backgroundService.isNetworkAvailable());

				// node starts out logging, so if the user turned it off we must say so early
				this.sendLoggingEnabled(ChatSettings.getInstance().isFileLoggingEnabled());

				// the webview may have grabbed settings before this announce and connected directly;
				// re-push so it re-routes through the loopback (the client treats this as a change)
				if (this.backgroundService.javascriptJavaBridge != null)
				{
					this.backgroundService.javascriptJavaBridge.JavaExportRequestCurrentSettingsFromAndroid();
				}
			}
			else if (type.equals("unread"))
			{
				// node counts while nobody is looking, so the icon is right even when the app
				// is idle, closed, or was started headless at boot
				this.backgroundService.showUnreadBadge(event.optInt("count", 0));
			}
			else if (type.equals("call"))
			{
				// a call node heard about while there was no webview. same screen either way
				this.backgroundService.showIncomingCall(event.optString("caller", "somebody"),
					event.optInt("channel_id", 0));
			}
			else if (type.equals("chat_activity"))
			{
				// same rule as pokes: the page shows its own ui while the app is in the foreground
				if (MainActivity.instance != null)
				{
					return;
				}

				String sender = event.optString("last_sender", "");
				this.backgroundService.showMessageNotification(sender.length() > 0 ? sender : "lemon chat", "new message");
			}
		}
		catch (Exception badEvent)
		{
			Log.w(TAG, "bridge event unparseable: " + line);
		}
	}

	private void send(final String json)
	{
		// socket writes are forbidden on the ui thread (NetworkOnMainThreadException), and
		// settings saves arrive from exactly there - hop to a worker thread for every send
		new Thread(new Runnable()
		{
			public void run()
			{
				synchronized (NodeBridge.this)
				{
					if (NodeBridge.this.currentWriter != null)
					{
						NodeBridge.this.currentWriter.println(json);
					}
					else
					{
						Log.w(TAG, "bridge send with no node connected: " + json);
					}
				}
			}
		}).start();
	}

	/** hands node the current settings json - node connects with them (same json the webview gets) */
	public void sendSettings()
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

	/** parks node's server connection (the webview is taking over) */
	public void sendDisconnect()
	{
		this.send("{\"type\":\"disconnect\"}");
	}

	/** the device's real network state - node cannot see this, and the webview lies about it */
	public void sendNetworkState(boolean isAvailable)
	{
		this.send("{\"type\":\"network\",\"available\":" + (isAvailable ? "true" : "false") + "}");
	}

	/** accepting a call: leave idle mode and join the caller's channel. node owns the connection,
	    so this must not go through the webview - after a boot start there is no webview at all.
	    (the reverse, GOING idle, deliberately stays on the webview: it also shuts down audio and
	    the datachannel, which only the webview can do) */
	public void sendComeFromIdle(int channelId)
	{
		this.send("{\"type\":\"come_from_idle\",\"channel_id\":" + channelId + "}");
	}

	/** hands one line the webview printed over to node, so it ends up in the log file. */
	public void sendLogLine(String line)
	{
		this.send("{\"type\":\"log\",\"line\":" + JSONObject.quote(line) + "}");
	}

	/** tells node whether to write the log file, so one setting covers the whole app. */
	public void sendLoggingEnabled(boolean isEnabled)
	{
		this.send("{\"type\":\"log_enabled\",\"enabled\":" + (isEnabled ? "true" : "false") + "}");
	}

	/** tells node whether the app is actually on screen. node cannot work this out itself,
	    because the webview stays connected even while the app is in the background. */
	public void sendUiVisible(boolean isVisible)
	{
		this.send("{\"type\":\"ui_visible\",\"visible\":" + (isVisible ? "true" : "false") + "}");
	}
}
