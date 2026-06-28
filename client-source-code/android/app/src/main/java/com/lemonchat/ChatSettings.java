package com.lemonchat;

import android.content.SharedPreferences;
import android.util.Log;

import org.json.JSONObject;

public class ChatSettings
{
	private String ipAddress = "";
	private int websocketPort = 0;

	private String defaultUsername = "anon";

	private boolean continousAudioBroadcastEnabled = false;

	private boolean areAudioEffectsEnabled = false;

	private String jsonSettings = "";

	private SharedPreferences preferences;

	public static ChatSettings instance;

	public static ChatSettings getInstance()
	{
		if (instance == null)
		{
			ChatSettings.instance = new ChatSettings();
		}
		return ChatSettings.instance;
	}

	public ChatSettings()
	{
	}

	public void setPreferences(SharedPreferences prefs)
	{
		this.preferences = prefs;
	}

	public void JavascriptJavaBridgeOnSaveSettings()
	{
		try
		{
			String json = this.preferences.getString("settings", "");

			if (json.equals("") == false)
			{
				JSONObject settingsJson = new JSONObject(json);

				settingsJson.put("is_microphone_always_on", this.isMicrophoneAlwayson());
				settingsJson.put("is_audio_effect_enabled", this.getAreAudioEffectsDisabled());
				settingsJson.put("default_username", this.defaultUsername);
				this.saveJsonSettings(settingsJson.toString());
			}
		}
		catch (Exception ex)
		{
			Log.d("Info", "[lemonchat] SaveSettings exception:" + ex.getMessage());
		}
	}

	public String getIpAddress()
	{
		return this.ipAddress;
	}

	public void setIpAddress(String ipAddress)
	{
		this.ipAddress = ipAddress;
	}

	public int getWebsocketPort()
	{
		return this.websocketPort;
	}

	public void setWebsocketPort(int websocketPort)
	{
		this.websocketPort = websocketPort;
	}

	public boolean isMicrophoneAlwayson()
	{
		return this.continousAudioBroadcastEnabled;
	}

	public void setContinousAudioBroadcastEnabled(boolean continousAudioBroadcastEnabled)
	{
		this.continousAudioBroadcastEnabled = continousAudioBroadcastEnabled;
	}

	public String getDefaultUsername()
	{
		return this.defaultUsername;
	}

	public void setDefaultUsername(String defaultUsername)
	{
		this.defaultUsername = defaultUsername;
	}

	public boolean getAreAudioEffectsEnabled()
	{
		return areAudioEffectsEnabled;
	}

	public boolean getAreAudioEffectsDisabled()
	{
		return !areAudioEffectsEnabled;
	}

	public void setAreAudioEffectsEnabled(boolean areAudioEffectsEnabled)
	{
		this.areAudioEffectsEnabled = areAudioEffectsEnabled;
	}

	public String getJsonSettings()
	{
		return this.jsonSettings;
	}

	public void saveJsonSettings(String jsonSettings)
	{
		try
		{
			this.jsonSettings = jsonSettings;
			SharedPreferences.Editor editor = this.preferences.edit();
			editor.putString("settings", this.jsonSettings);
			editor.apply();
		}
		catch (Exception ex)
		{
			Log.d("Info", "[lemonchat] SaveSettings exception:" + ex.getMessage());
		}
	}
}
