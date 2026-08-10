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

	//gets ran at the moment client connects to server
	//sets username, sets if microphone should be on be turned on by default or not
	//sets if sound effects should be active or not
	@JavascriptInterface public void JavaExportOnConnected()
	{
		//will throw exception if not called inside webView thread

		//JavaExportOnConnected is called by client.html in end of process_client_list_from_server
		//its safe to change username after client list is received

		this.backgroundServiceInstance.webView.post(new Runnable() {
			@Override public void run()
			{
				if (JavascriptJavaBridge.this.settings.isMicrophoneAlwayson())
				{
					JavascriptJavaBridge.this.backgroundServiceInstance.webView.evaluateJavascript("JavascriptJavaBridge__activate_continous_audio_broadcast();", null);
				}

				String defaultUsername = JavascriptJavaBridge.this.settings.getDefaultUsername();
				if (defaultUsername.length() > 0)
				{
					//JSONObject.quote, not '...' concat: one apostrophe in a username made the
					//injected line a syntax error that evaluateJavascript swallowed silently
					JavascriptJavaBridge.this.backgroundServiceInstance.webView.evaluateJavascript("JavascriptJavaBridge__set_username_on_connect(" + JSONObject.quote(defaultUsername) + ");", null);
				}

				if (JavascriptJavaBridge.this.settings.getAreAudioEffectsEnabled() == false)
				{
					JavascriptJavaBridge.this.backgroundServiceInstance.webView.evaluateJavascript("JavascriptJavaBridge__set_audio_effects_status();", null);
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
				}
			}
		});
	}
}