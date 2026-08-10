package com.lemonchat;

import android.content.SharedPreferences;
import android.util.Log;

import org.json.JSONObject;

import java.security.SecureRandom;

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

	//generates the 200-char identity passphrase seed; same charset and length the web client
	//uses for its own random identities (cryptico derives the RSA keypair from this string)
	public static String generateIdentitySeed()
	{
		String chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
		SecureRandom random = new SecureRandom();
		StringBuilder builder = new StringBuilder(200);

		for (int i = 0; i < 200; i++)
		{
			builder.append(chars.charAt(random.nextInt(chars.length())));
		}

		return builder.toString();
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
				//deliberately NOT default_username: this runs on mic/sound toggles from the page,
				//and rewriting the username from a field this path does not own overwrote the saved
				//one with "anon" whenever the field had not been populated yet. the settings panel's
				//Save is the only owner of the username
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
		//the in-memory copy is only filled by whoever called saveJsonSettings in THIS process; a
		//service-side caller can outlive (or precede) the activity that does that, so fall back to
		//the persisted copy instead of answering with an empty string
		if (this.jsonSettings.equals("") && this.preferences != null)
		{
			this.jsonSettings = this.preferences.getString("settings", "");
		}

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
