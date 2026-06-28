package com.lemonchat;

import android.content.Context;
import android.os.IBinder;
import android.util.Log;
import android.webkit.JavascriptInterface;
import android.widget.Toast;

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
					JavascriptJavaBridge.this.backgroundServiceInstance.webView.evaluateJavascript("JavascriptJavaBridge__set_username_on_connect('" + defaultUsername + "');", null);
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

	@JavascriptInterface public void JavaExportRequestCurrentSettingsFromAndroid()
	{
		Log.d("Info", "[lemonchat] JavaExportRequestCurrentSettingsFromAndroid");
		this.backgroundServiceInstance.webView.post(new Runnable() {
			@Override public void run()
			{
				if (MainActivity.instance != null)
				{
					if (JavascriptJavaBridge.settingsAlreadyLoaded == false)
					{
						JavascriptJavaBridge.settingsAlreadyLoaded = true;
						MainActivity.instance.LoadSettings();
					}

					String jsonSettings = JavascriptJavaBridge.this.settings.getJsonSettings();

					if (jsonSettings.length() > 0)
					{
						JavascriptJavaBridge.this.backgroundServiceInstance.webView.evaluateJavascript("JavascriptJavaBridge__accept_current_settings_from_android('" + jsonSettings + "');", null);
					}
				}
			}
		});
	}
}