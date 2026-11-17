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

/**
 * Exposes native operations to the trusted bundled page.
 */
public class JavascriptJavaBridge
{
	private Context mContext;

	private ChatSettings settings;

	BackgroundService backgroundServiceInstance;

	/**
	 * Connects the page interface to the service and settings.
	 *
	 * @param context service context for storage and feedback
	 * @param settings shared settings owner
	 * @param backgroundServiceInstance the service that owns the trusted page
	 */
	JavascriptJavaBridge(Context context, ChatSettings settings, BackgroundService backgroundServiceInstance)
	{
		this.mContext = context;
		this.settings = settings;
		this.backgroundServiceInstance = backgroundServiceInstance;
	}

	/**
	 * Replaces the identity for the trusted page.
	 *
	 * @param passphrase the exact identity passphrase
	 * @param rsaKeyBits the supported RSA modulus size
	 * @param operationId the caller's unique operation identifier
	 * @param expectedRevision the identity revision the caller expects to replace
	 * @return JSON containing the committed revision or an error code
	 */
	@JavascriptInterface public String JavaExportReplaceIdentity(String passphrase, int rsaKeyBits, String operationId, long expectedRevision)
	{
		if (!this.backgroundServiceInstance.isTrustedPage())
		{
			return "{\"ok\":false,\"error\":\"untrusted_page\"}";
		}
		return this.settings.replaceIdentity(passphrase, rsaKeyBits, operationId, expectedRevision);
	}

	/**
	 * Saves the identity as an {@code .lmn} file in format version 1.
	 *
	 * @param fileName name to sanitize and give an {@code .lmn} suffix
	 * @return JSON containing {@code ok}, with an error code if export or storage fails
	 */
	@JavascriptInterface public String JavaExportExportIdentity(String fileName)
	{
		if (!this.backgroundServiceInstance.isTrustedPage())
		{
			return "{\"ok\":false,\"error\":\"untrusted_page\"}";
		}
		try
		{
			byte[] file = this.settings.exportIdentity().getBytes(java.nio.charset.StandardCharsets.UTF_8);
			String name = sanitizeFileName(fileName);
			if (!name.endsWith(".lmn"))
			{
				name += ".lmn";
			}
			boolean saved = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q
				? this.saveToMediaStoreDownloads(name, "application/json", file)
				: this.saveToAppDownloads(name, "application/json", file);
			return saved ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"storage_failed\"}";
		}
		catch (Exception failed)
		{
			return "{\"ok\":false,\"error\":\"export_failed\"}";
		}
	}

	/**
	 * Changes the RSA size while preserving the identity passphrase.
	 *
	 * @param rsaKeyBits the new supported RSA modulus size
	 * @param operationId the caller's unique operation identifier
	 * @param expectedRevision the identity revision to replace
	 * @return JSON containing the committed revision or an error code
	 */
	@JavascriptInterface public String JavaExportResizeIdentity(int rsaKeyBits, String operationId, long expectedRevision)
	{
		if (!this.backgroundServiceInstance.isTrustedPage())
		{
			return "{\"ok\":false,\"error\":\"untrusted_page\"}";
		}
		return this.settings.resizeIdentity(rsaKeyBits, operationId, expectedRevision);
	}

	/**
	 * Sends Connect through the native control channel.
	 */
	@JavascriptInterface public void JavaExportConnect()
	{
		if (!this.backgroundServiceInstance.isTrustedPage())
		{
			return;
		}
		final long generation = this.backgroundServiceInstance.getPageGeneration();
		final android.webkit.WebView webView = this.backgroundServiceInstance.webView;
		if (webView == null)
		{
			return;
		}
		webView.post(() ->
		{
			MainActivity owner = MainActivity.instance;
			if (owner != null && owner.isMediaActivityVisible() && this.backgroundServiceInstance.isTrustedPage() && generation == this.backgroundServiceInstance.getPageGeneration() && webView == this.backgroundServiceInstance.webView)
			{
				owner.requestConnection();
			}
		});
	}

	/**
	 * Shows a short message requested by the trusted page.
	 *
	 * @param toast the text to display
	 */
	@JavascriptInterface public void JavaExport(String toast)
	{
		if (!this.backgroundServiceInstance.isTrustedPage())
		{
			return;
		}
		Toast.makeText(this.mContext, toast, Toast.LENGTH_SHORT).show();
	}

	/**
	 * Sends Disconnect through the native control channel.
	 */
	@JavascriptInterface public void JavaExportDisconnect()
	{
		if (!this.backgroundServiceInstance.isTrustedPage())
		{
			return;
		}
		NodeBridge bridge = this.backgroundServiceInstance.nodeBridge;
		if (this.backgroundServiceInstance.isTrustedPage() && bridge != null)
		{
			bridge.sendDisconnect();
		}
	}

	/**
	 * Decodes and saves a chat file, then shows the result.
	 *
	 * @param fileName the requested file name, sanitized before saving
	 * @param mimeType media type, or empty or {@code null} for {@code application/octet-stream}
	 * @param base64 file bytes encoded as base64
	 */
	@JavascriptInterface public void JavaExportSaveFile(String fileName, String mimeType, String base64)
	{
		if (!this.backgroundServiceInstance.isTrustedPage())
		{
			return;
		}
		String safeName = sanitizeFileName(fileName);
		String safeMime = (mimeType == null || mimeType.isEmpty()) ? "application/octet-stream" : mimeType;
		byte[] bytes;

		try
		{
			if (base64 == null)
			{
				throw new IllegalArgumentException("missing file data");
			}
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

	/**
	 * Removes path separators and control characters from a file name.
	 *
	 * @param fileName the requested name, which may be {@code null}
	 * @return a name of at most 200 characters, or {@code file} if the input is unusable
	 */
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

	/**
	 * Saves to MediaStore Downloads and removes the pending entry on failure.
	 *
	 * @param safeName the sanitized display name
	 * @param safeMime the media type
	 * @param bytes the complete decoded file contents
	 * @return {@code true} if the entry was published
	 */
	@androidx.annotation.RequiresApi(Build.VERSION_CODES.Q)
	private boolean saveToMediaStoreDownloads(String safeName, String safeMime, byte[] bytes)
	{
		ContentResolver resolver = this.mContext.getContentResolver();
		Uri uri = null;
		try
		{
			ContentValues values = new ContentValues();
			values.put(MediaStore.Downloads.DISPLAY_NAME, safeName);
			values.put(MediaStore.Downloads.MIME_TYPE, safeMime);
			values.put(MediaStore.Downloads.IS_PENDING, 1);
			uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
			if (uri == null)
			{
				throw new IOException("storage unavailable");
			}
			try (OutputStream out = resolver.openOutputStream(uri))
			{
				if (out == null)
				{
					throw new IOException("storage unavailable");
				}
				out.write(bytes);
			}
			values.clear();
			values.put(MediaStore.Downloads.IS_PENDING, 0);
			if (resolver.update(uri, values, null, null) != 1)
			{
				throw new IOException("file publication failed");
			}
			this.showToastOnMainThread("Saved to Downloads/" + safeName);
			return true;
		}
		catch (IOException | RuntimeException failed)
		{
			if (uri != null)
			{
				try
				{
					resolver.delete(uri, null, null);
				}
				catch (RuntimeException cleanupFailed)
				{
					Log.w("lemonchat", "unfinished download cleanup failed");
				}
			}
			this.showToastOnMainThread("Could not save " + safeName + " into Downloads");
			return false;
		}
	}

	/**
	 * Saves a uniquely named file in the app's external downloads directory.
	 *
	 * <p>Failed writes remove the reserved file. Failed DownloadManager registration keeps it.</p>
	 *
	 * @param safeName the sanitized requested name
	 * @param safeMime media type for DownloadManager registration
	 * @param bytes the complete decoded file contents
	 * @return {@code true} if the file was written and closed successfully
	 */
	@SuppressWarnings("deprecation")
	private boolean saveToAppDownloads(String safeName, String safeMime, byte[] bytes)
	{
		File directory = this.mContext.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS);

		if (directory == null)
		{
			this.showToastOnMainThread("could not save " + safeName + ": no storage available");
			return false;
		}

		File target = new File(directory, safeName);
		int counter = 1;
		boolean created = false;

		try
		{
			while (!target.createNewFile())
			{
				int dot = safeName.lastIndexOf('.');
				String stem = (dot > 0) ? safeName.substring(0, dot) : safeName;
				String extension = (dot > 0) ? safeName.substring(dot) : "";
				target = new File(directory, stem + " (" + counter + ")" + extension);
				counter++;
			}
			created = true;
			try (FileOutputStream out = new FileOutputStream(target))
			{
				out.write(bytes);
			}
		}
		catch (IOException | RuntimeException writeFailed)
		{
			if (created)
			{
				try
				{
					if (!target.delete())
					{
						Log.w("lemonchat", "unfinished download cleanup failed");
					}
				}
				catch (SecurityException cleanupFailed)
				{
					Log.w("lemonchat", "unfinished download cleanup failed");
				}
			}
			this.showToastOnMainThread("could not save " + safeName);
			return false;
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
		return true;
	}

	/**
	 * Posts storage or settings feedback to the main thread.
	 *
	 * @param text the message to display
	 */
	private void showToastOnMainThread(final String text)
	{
		new Handler(Looper.getMainLooper()).post(new Runnable()
		{
			@Override public void run()
			{
				Toast.makeText(JavascriptJavaBridge.this.mContext, text, Toast.LENGTH_LONG).show();
			}
		});
	}

	/**
	 * Saves the continuous microphone preference without replacing unrelated settings.
	 *
	 * @param isActive whether continuous microphone capture is selected
	 */
	@JavascriptInterface public void JavaExportSetContinousAudioBroadcastStatus(boolean isActive)
	{
		if (!this.backgroundServiceInstance.isTrustedPage())
		{
			return;
		}
		if (!this.settings.updateSetting("is_microphone_always_on", isActive))
		{
			this.showToastOnMainThread("Microphone preference could not be saved");
		}
	}

	/**
	 * Saves the sound effects preference and reports a failed save.
	 *
	 * @param isActive whether application sound effects should be enabled
	 */
	@JavascriptInterface public void JavaExportSetSoundEffectsStatus(boolean isActive)
	{
		if (!this.backgroundServiceInstance.isTrustedPage())
		{
			return;
		}
		if (!this.settings.updateSetting("is_audio_effect_enabled", isActive))
		{
			this.showToastOnMainThread("Sound preference could not be saved");
		}
	}

	/**
	 * Updates the legacy unread badge only when no Node bridge owns the count.
	 *
	 * @param unreadCount total unread channel and private messages
	 */
	@JavascriptInterface public void JavaExportSetUnreadBadge(int unreadCount)
	{
		if (!this.backgroundServiceInstance.isTrustedPage())
		{
			return;
		}
		if (this.backgroundServiceInstance != null && this.backgroundServiceInstance.nodeBridge == null)
		{
			this.backgroundServiceInstance.showUnreadBadge(unreadCount);
		}
	}

	/**
	 * Keeps the legacy login callback without changing state.
	 */
	@JavascriptInterface public void JavaExportOnConnected()
	{
		if (!this.backgroundServiceInstance.isTrustedPage())
		{
			return;
		}
		// Node applies the saved name during login.
	}

	/**
	 * Ignores the legacy call request because Node owns incoming calls.
	 *
	 * @param callerName the unused legacy caller name
	 * @param channelId the unused legacy channel identifier
	 */
	@JavascriptInterface public void JavaExportStartCall(String callerName, int channelId)
	{
		if (!this.backgroundServiceInstance.isTrustedPage())
		{
			return;
		}
		// Node supplies the current call capability.
	}

	/**
	 * Saves the logging preference before updating the service and Node.
	 *
	 * @param isEnabled whether file logging should be enabled
	 */
	@JavascriptInterface public void JavaExportSetFileLogging(boolean isEnabled)
	{
		if (!this.backgroundServiceInstance.isTrustedPage())
		{
			return;
		}
		Log.d("Info", "[lemonchat] JavaExportSetFileLogging " + isEnabled);

		// Save before updating the copy used by the console hook.
		if (!this.settings.setFileLoggingEnabled(isEnabled))
		{
			this.showToastOnMainThread("Logging preference could not be saved");
			return;
		}
		BackgroundService.isFileLoggingEnabled = isEnabled;

		NodeBridge bridge = this.backgroundServiceInstance.nodeBridge;
		if (bridge != null)
		{
			bridge.sendLoggingEnabled(isEnabled);
		}
	}

	/**
	 * Opens native settings if the requesting page is still current and the activity is available.
	 */
	@JavascriptInterface public void JavaExportGoToSettings()
	{
		if (!this.backgroundServiceInstance.isTrustedPage())
		{
			return;
		}
		final long generation = this.backgroundServiceInstance.getPageGeneration();
		final android.webkit.WebView webView = this.backgroundServiceInstance.webView;
		if (webView == null)
		{
			return;
		}
		webView.post(new Runnable()
		{
			@Override public void run()
			{
				MainActivity activity = MainActivity.instance;
				if (activity != null && !activity.isFinishing() && JavascriptJavaBridge.this.backgroundServiceInstance.isTrustedPage()
					&& generation == JavascriptJavaBridge.this.backgroundServiceInstance.getPageGeneration())
				{
					activity.showSettings();
				}
			}
		});
	}

	public static boolean settingsAlreadyLoaded = false;

	// Retained for compatibility. The page cannot connect directly to a server.
	public static boolean settingsDeferFallbackArmed = false;

	/**
	 * Shows a legacy background poke notification when no Node bridge owns notifications.
	 *
	 * @param senderName the sender's display name
	 * @param pokeMessage the text shown in the notification
	 */
	@JavascriptInterface public void JavaExportShowPokeNotification(String senderName, String pokeMessage)
	{
		if (!this.backgroundServiceInstance.isTrustedPage())
		{
			return;
		}
		Log.d("Info", "[lemonchat] JavaExportShowPokeNotification");

		if (MainActivity.instance != null)
		{
			return; // The foreground page shows its own alert.
		}
		if (this.backgroundServiceInstance.nodeBridge != null)
		{
			return;
		}

		JavascriptJavaBridge.this.backgroundServiceInstance.showPokeNotification(senderName, pokeMessage);
	}

	/**
	 * Gives the current trusted page settings without the passphrase once Node is ready.
	 */
	@JavascriptInterface public void JavaExportRequestCurrentSettingsFromAndroid()
	{
		if (!this.backgroundServiceInstance.isTrustedPage())
		{
			return;
		}
		final long generation = this.backgroundServiceInstance.getPageGeneration();
		final android.webkit.WebView webView = this.backgroundServiceInstance.webView;
		if (webView == null)
		{
			return;
		}
		webView.post(new Runnable()
		{
			@Override public void run()
			{
				if (!JavascriptJavaBridge.this.backgroundServiceInstance.isTrustedPage()
					|| generation != JavascriptJavaBridge.this.backgroundServiceInstance.getPageGeneration())
				{
					return;
				}
				// Only the native panel needs an activity. The page still needs its reply while paused.
				if (MainActivity.instance != null && JavascriptJavaBridge.settingsAlreadyLoaded == false)
				{
					JavascriptJavaBridge.settingsAlreadyLoaded = true;
					MainActivity.instance.LoadSettings();
				}

				String jsonSettings = JavascriptJavaBridge.this.settings.getJsonSettings();

				// Node owns the connection. Keep the real host for STUN and TURN.
				NodeBridge nodeBridge = JavascriptJavaBridge.this.backgroundServiceInstance.nodeBridge;

				// Wait for the loopback announcement to retry. Show recovery if Node is unavailable.
				if (nodeBridge == null || nodeBridge.getLoopbackPort() == 0)
				{
					if (nodeBridge != null)
					{
						nodeBridge.pushConnectionPhaseToWebview();
					}
					return;
				}

				if (jsonSettings.length() > 0 && nodeBridge != null && nodeBridge.getLoopbackPort() > 0)
				{
					try
					{
						JSONObject augmented = new JSONObject(jsonSettings);
						augmented.remove("identity_string");
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
					// Quote the JSON so user values cannot break the JavaScript string.
					String javascript_argument = JSONObject.quote(jsonSettings);

					JavascriptJavaBridge.this.backgroundServiceInstance.webView.evaluateJavascript(
						"JavascriptJavaBridge__accept_current_settings_from_android(" + javascript_argument + ");", null);

					// Restore the phase Node reported before this page loaded.
					if (nodeBridge != null)
					{
						nodeBridge.pushConnectionPhaseToWebview();
					}
				}
			}
		});
	}
}
