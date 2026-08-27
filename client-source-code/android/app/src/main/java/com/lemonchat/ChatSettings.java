package com.lemonchat;

import android.content.Context;
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

	private static Context applicationContext;

	//the settings used to work only if MainActivity had handed them its SharedPreferences, so a
	//boot start (service, no activity) left them empty: node was told "no settings" and never
	//connected until the app was opened. now either side just attaches a context and the
	//preferences resolve themselves, so neither has to remember - and neither can forget
	public static void attachContext(Context context)
	{
		if (ChatSettings.applicationContext == null && context != null)
		{
			ChatSettings.applicationContext = context.getApplicationContext();
		}
	}

	//Activity.getPreferences() names the file after the activity class, so this asks for that
	//same "MainActivity" file rather than opening a second, empty one
	private SharedPreferences preferences()
	{
		if (this.preferences == null && ChatSettings.applicationContext != null)
		{
			this.preferences = ChatSettings.applicationContext.getSharedPreferences("MainActivity", Context.MODE_PRIVATE);
		}

		return this.preferences;
	}

	public void JavascriptJavaBridgeOnSaveSettings()
	{
		try
		{
			String json = this.preferences().getString("settings", "");

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

	/** the log file setting. this is the saved one, the copies elsewhere are read from here.
	 *  off unless the user turned it on in the local settings panel. */
	public boolean isFileLoggingEnabled()
	{
		return (this.preferences() != null) && this.preferences().getBoolean("file_logging_enabled", false);
	}

	public void setFileLoggingEnabled(boolean isEnabled)
	{
		if (this.preferences() != null)
		{
			this.preferences().edit().putBoolean("file_logging_enabled", isEnabled).apply();
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

	//set by BackgroundService; runs after every save so BOTH node and the webview always get
	//the fresh settings, no matter which code path saved them
	public static Runnable onSettingsSaved = null;

	public String getJsonSettings()
	{
		//the in-memory copy is only filled by whoever called saveJsonSettings in THIS process; a
		//service-side caller can outlive (or precede) the activity that does that, so fall back to
		//the persisted copy instead of answering with an empty string
		if (this.jsonSettings.equals("") && this.preferences() != null)
		{
			this.jsonSettings = this.preferences().getString("settings", "");
		}

		return this.completeSettingsJson(this.jsonSettings);
	}

	//every consumer gets a COMPLETE json: absent fields caused silent misbehaviour downstream
	//(a first-run push with no host made node dial ws://undefined and hang)
	private String completeSettingsJson(String json)
	{
		try
		{
			JSONObject settings = (json == null || json.equals("")) ? new JSONObject() : new JSONObject(json);

			if (!settings.has("host")) settings.put("host", "");
			if (!settings.has("port")) settings.put("port", 0);
			if (!settings.has("default_username")) settings.put("default_username", "");
			if (!settings.has("is_microphone_always_on")) settings.put("is_microphone_always_on", false);
			if (!settings.has("is_autoconnect_enabled")) settings.put("is_autoconnect_enabled", true);
			if (!settings.has("is_audio_effect_enabled")) settings.put("is_audio_effect_enabled", false);
			if (!settings.has("is_app_log_enabled")) settings.put("is_app_log_enabled", false);

			//always taken from the saved setting, so an old value sitting in this json
			//cannot override what the user actually chose
			settings.put("is_file_logging_enabled", this.isFileLoggingEnabled());
			if (!settings.has("app_mode")) settings.put("app_mode", this.preferences() != null ? this.preferences().getString("app_mode", "advanced") : "advanced");
			if (!settings.has("metadata_keys")) settings.put("metadata_keys", new org.json.JSONArray());

			//the identity seed must always exist - node authenticates with it from the first push
			if (!settings.has("identity_string") || settings.getString("identity_string").length() < 199)
			{
				String seed = this.preferences() != null ? this.preferences().getString("identity_seed", "") : "";

				if (seed.length() < 199)
				{
					seed = ChatSettings.generateIdentitySeed();

					if (this.preferences() != null)
					{
						this.preferences().edit().putString("identity_seed", seed).commit();
					}
				}

				settings.put("identity_string", seed);
			}

			return settings.toString();
		}
		catch (Exception ex)
		{
			Log.d("Info", "[lemonchat] completeSettingsJson exception:" + ex.getMessage());
			return json;
		}
	}

	public void saveJsonSettings(String jsonSettings)
	{
		try
		{
			this.jsonSettings = jsonSettings;
			SharedPreferences.Editor editor = this.preferences().edit();
			editor.putString("settings", this.jsonSettings);
			//commit, not apply: an app kill right after a save must not lose the write
			editor.commit();

			if (ChatSettings.onSettingsSaved != null)
			{
				ChatSettings.onSettingsSaved.run();
			}
		}
		catch (Exception ex)
		{
			Log.d("Info", "[lemonchat] SaveSettings exception:" + ex.getMessage());
		}
	}
}
