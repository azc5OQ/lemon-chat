package com.lemonchat;

import android.content.Context;
import android.os.IBinder;
import android.util.Log;
import android.webkit.JavascriptInterface;
import android.widget.Toast;

import org.json.JSONObject;

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