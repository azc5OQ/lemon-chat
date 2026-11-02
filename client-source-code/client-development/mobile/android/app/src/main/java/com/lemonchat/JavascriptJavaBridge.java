package com.lemonchat;

import android.app.DownloadManager;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.provider.MediaStore;
import android.util.Log;
import android.webkit.JavascriptInterface;
import android.widget.Toast;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;

public class JavascriptJavaBridge
{
	private Context mContext;

	private ChatSettings settings;

	BackgroundService backgroundServiceInstance;

	JavascriptJavaBridge(Context context, ChatSettings settings, BackgroundService backgroundServiceInstance)
	{
		this.mContext = context;
		this.settings = settings;
		this.backgroundServiceInstance = backgroundServiceInstance;
	}

	@JavascriptInterface public void JavaExport(String toast)
	{
		Toast.makeText(this.mContext, toast, Toast.LENGTH_SHORT).show();
	}

	/**
	 * the download button of a chat file. the webview cannot save a blob url, so the page hands the
	 * decoded bytes over and they land in Downloads: MediaStore on api 29+, below that the app's own
	 * downloads folder registered with the DownloadManager so the Downloads app lists it.
	 */
	@JavascriptInterface public void JavaExportSaveFile(String fileName, String mimeType, String base64)
	{
		String safeName = sanitizeFileName(fileName);
		String safeMime = (mimeType == null || mimeType.isEmpty()) ? "application/octet-stream" : mimeType;
		byte[] bytes;

		try
		{
			bytes = android.util.Base64.decode(base64, android.util.Base64.DEFAULT);
		}
		catch (IllegalArgumentException badBase64)
		{
			this.showToastOnMainThread("could not save " + safeName + ": the file data is damaged");
			return;
		}

		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
		{
			this.saveToMediaStoreDownloads(safeName, safeMime, bytes);
		}
		else
		{
			this.saveToAppDownloads(safeName, safeMime, bytes);
		}
	}

	/** no path separators or control characters, never empty */
	private static String sanitizeFileName(String fileName)
	{
		String name = (fileName == null) ? "" : fileName.replaceAll("[\\\\/\\p{Cntrl}]", "_").trim();

		if (name.isEmpty() || name.equals(".") || name.equals(".."))
		{
			name = "file";
		}

		if (name.length() > 200)
		{
			name = name.substring(0, 200);
		}

		return name;
	}

	/** api 29+: a pending MediaStore row, filled, then published. a duplicate name is renamed by the system */
	private void saveToMediaStoreDownloads(String safeName, String safeMime, byte[] bytes)
	{
		ContentResolver resolver = this.mContext.getContentResolver();
		ContentValues values = new ContentValues();
		values.put(MediaStore.Downloads.DISPLAY_NAME, safeName);
		values.put(MediaStore.Downloads.MIME_TYPE, safeMime);
		values.put(MediaStore.Downloads.IS_PENDING, 1);

		Uri uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);

		if (uri == null)
		{
			this.showToastOnMainThread("could not save " + safeName + " into Downloads");
			return;
		}

		try (OutputStream out = resolver.openOutputStream(uri))
		{
			if (out == null)
			{
				throw new IOException("no output stream");
			}

			out.write(bytes);
		}
		catch (IOException writeFailed)
		{
			Log.e("Info", "[lemonchat] saving chat file failed", writeFailed);
			resolver.delete(uri, null, null);
			this.showToastOnMainThread("could not save " + safeName + " into Downloads");
			return;
		}

		values.clear();
		values.put(MediaStore.Downloads.IS_PENDING, 0);
		resolver.update(uri, values, null, null);

		this.showToastOnMainThread("saved to Downloads/" + safeName);
	}

	/** api 26-28: the app's external downloads folder needs no permission; the DownloadManager entry makes it visible */
	@SuppressWarnings("deprecation")
	private void saveToAppDownloads(String safeName, String safeMime, byte[] bytes)
	{
		File directory = this.mContext.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS);

		if (directory == null)
		{
			this.showToastOnMainThread("could not save " + safeName + ": no storage available");
			return;
		}

		File target = new File(directory, safeName);
		int counter = 1;

		while (target.exists())
		{
			int dot = safeName.lastIndexOf('.');
			String stem = (dot > 0) ? safeName.substring(0, dot) : safeName;
			String extension = (dot > 0) ? safeName.substring(dot) : "";
			target = new File(directory, stem + " (" + counter + ")" + extension);
			counter++;
		}

		try (FileOutputStream out = new FileOutputStream(target))
		{
			out.write(bytes);
		}
		catch (IOException writeFailed)
		{
			Log.e("Info", "[lemonchat] saving chat file failed", writeFailed);
			this.showToastOnMainThread("could not save " + safeName);
			return;
		}

		DownloadManager downloadManager = (DownloadManager) this.mContext.getSystemService(Context.DOWNLOAD_SERVICE);

		if (downloadManager != null)
		{
			try
			{
				downloadManager.addCompletedDownload(target.getName(), "lemon chat file", true, safeMime, target.getAbsolutePath(), target.length(), true);
			}
			catch (Exception registerFailed)
			{
				Log.w("Info", "[lemonchat] could not register the saved file with the download manager", registerFailed);
			}
		}

		this.showToastOnMainThread("saved: " + target.getAbsolutePath());
	}

	private void showToastOnMainThread(final String text)
	{
		new Handler(Looper.getMainLooper()).post(new Runnable() {
			@Override public void run()
			{
				Toast.makeText(JavascriptJavaBridge.this.mContext, text, Toast.LENGTH_LONG).show();
			}
		});
	}

	@JavascriptInterface public void JavaExportSetContinousAudioBroadcastStatus(boolean isActive)
	{
		this.settings.setContinousAudioBroadcastEnabled(isActive);
		this.settings.JavascriptJavaBridgeOnSaveSettings();
	}

	@JavascriptInterface public void JavaExportSetSoundEffectsStatus(boolean isActive)
	{
		this.settings.setAreAudioEffectsEnabled(isActive);
		this.settings.JavascriptJavaBridgeOnSaveSettings();
	}

	//total unread messages (channels + private). launchers show it on the app icon
	@JavascriptInterface public void JavaExportSetUnreadBadge(int unreadCount)
	{
		if (this.backgroundServiceInstance != null)
		{
			this.backgroundServiceInstance.showUnreadBadge(unreadCount);
		}
	}

	//gets ran at the moment client connects to server
	//sets the default username. the microphone and sound effect settings used to be sent from
	//here too, but they belong to the local settings panel now and arrive with the settings
	@JavascriptInterface public void JavaExportOnConnected()
	{
		//will throw exception if not called inside webView thread

		//JavaExportOnConnected is called by client.html in end of process_client_list_from_server
		//its safe to change username after client list is received

		this.backgroundServiceInstance.webView.post(new Runnable() {
			@Override public void run()
			{
				String defaultUsername = JavascriptJavaBridge.this.settings.getDefaultUsername();
				if (defaultUsername.length() > 0)
				{
					//JSONObject.quote, not '...' concat: one apostrophe in a username made the
					//injected line a syntax error that evaluateJavascript swallowed silently
					JavascriptJavaBridge.this.backgroundServiceInstance.webView.evaluateJavascript("JavascriptJavaBridge__set_username_on_connect(" + JSONObject.quote(defaultUsername) + ");", null);
				}
			}
		});
	}

	@JavascriptInterface public void JavaExportStartCall(String callerName, int channelId)
	{
		Log.d("Info", "[lemonchat] received call request from " + callerName);
		IBinder service;
		this.backgroundServiceInstance.showIncomingCall(callerName, channelId);
	}

	/** the single on/off switch for the log file, used by the checkbox in local settings. */
	@JavascriptInterface public void JavaExportSetFileLogging(boolean isEnabled)
	{
		Log.d("Info", "[lemonchat] JavaExportSetFileLogging " + isEnabled);

		//save it first, then update the fast copy the console hook reads on every line
		this.settings.setFileLoggingEnabled(isEnabled);
		BackgroundService.isFileLoggingEnabled = isEnabled;

		if (this.backgroundServiceInstance.nodeBridge != null)
		{
			this.backgroundServiceInstance.nodeBridge.sendLoggingEnabled(isEnabled);
		}
	}

	@JavascriptInterface public void JavaExportGoToSettings()
	{
		Log.d("Info", "[lemonchat] JavaExportGoToSettings");
		this.backgroundServiceInstance.webView.post(new Runnable() {
			@Override public void run()
			{
				MainActivity.instance.showSettings();
			}
		});
	}

	public static boolean settingsAlreadyLoaded = false;

	// guards the one-shot fallback that answers the settings request without loopback fields
	// if node never announces (crashed) - the webview then connects directly as a degraded mode
	public static boolean settingsDeferFallbackArmed = false;

	/**
	 * a poke arrived. the page shows its own alert, but that is invisible while the app is in the
	 * background - which is precisely when being poked matters - so raise a real notification.
	 * skipped when the activity is on screen: the in-page alert already covers that case.
	 */
	@JavascriptInterface public void JavaExportShowPokeNotification(String senderName, String pokeMessage)
	{
		Log.d("Info", "[lemonchat] JavaExportShowPokeNotification");

		if (MainActivity.instance != null)
		{
			return; //app is in the foreground, the page's own alert is enough
		}

		JavascriptJavaBridge.this.backgroundServiceInstance.showPokeNotification(senderName, pokeMessage);
	}

	@JavascriptInterface public void JavaExportRequestCurrentSettingsFromAndroid()
	{
		Log.d("Info", "[lemonchat] JavaExportRequestCurrentSettingsFromAndroid");
		this.backgroundServiceInstance.webView.post(new Runnable() {
			@Override public void run()
			{
				// LoadSettings populates the native settings panel, so it needs a live activity.
				// the json answer must NOT: the page asks exactly once, at page load, and on a
				// first run that moment often falls while the user is off in the overlay/battery
				// permission screens (activity paused, MainActivity.instance == null). dropping
				// the whole answer there left the page waiting forever - no theme, no autoconnect
				if (MainActivity.instance != null && JavascriptJavaBridge.settingsAlreadyLoaded == false)
				{
					JavascriptJavaBridge.settingsAlreadyLoaded = true;
					MainActivity.instance.LoadSettings();
				}

				String jsonSettings = JavascriptJavaBridge.this.settings.getJsonSettings();

				// node is the connection authority: when its loopback is up, point the webview at it.
				// the real host stays in the json - client.html still needs it for stun/turn
				NodeBridge nodeBridge = JavascriptJavaBridge.this.backgroundServiceInstance.nodeBridge;

				// the webview must never connect directly while node is coming up - that made two
				// connections with one identity and the server kicked one. defer until the loopback
				// announce re-invokes this; the one-shot fallback covers a node that never comes up
				if (nodeBridge != null && nodeBridge.getLoopbackPort() == 0 && JavascriptJavaBridge.settingsDeferFallbackArmed == false)
				{
					JavascriptJavaBridge.settingsDeferFallbackArmed = true;

					JavascriptJavaBridge.this.backgroundServiceInstance.webView.postDelayed(new Runnable() {
						@Override public void run()
						{
							JavascriptJavaBridge.this.JavaExportRequestCurrentSettingsFromAndroid();
						}
					}, 8000);

					Log.i("Info", "[lemonchat] deferring settings until node announces its loopback");
					return;
				}

				if (jsonSettings.length() > 0 && nodeBridge != null && nodeBridge.getLoopbackPort() > 0)
				{
					try
					{
						JSONObject augmented = new JSONObject(jsonSettings);
						augmented.put("loopback_port", nodeBridge.getLoopbackPort());
						augmented.put("loopback_token", nodeBridge.getLoopbackToken());
						jsonSettings = augmented.toString();
					}
					catch (Exception augmentFailed)
					{
						Log.e("Info", "[lemonchat] could not add loopback fields to settings", augmentFailed);
					}
				}

				if (jsonSettings.length() > 0)
				{
					// hand the json over as a PROPERLY QUOTED js string literal. pasting it between
					// single quotes was a silent trap: one apostrophe in a username, host or key
					// (or a backslash) makes the injected line a syntax error, evaluateJavascript
					// swallows it, and the page simply never receives its settings - no theme, no
					// autoconnect details, no identity, with nothing logged anywhere
					String javascript_argument = JSONObject.quote(jsonSettings);

					JavascriptJavaBridge.this.backgroundServiceInstance.webView.evaluateJavascript(
						"JavascriptJavaBridge__accept_current_settings_from_android(" + javascript_argument + ");", null);

					//the phase node reported before this page loaded (e.g. key generation)
					if (nodeBridge != null)
					{
						nodeBridge.pushConnectionPhaseToWebview();
					}
				}
			}
		});
	}
}