package com.lemonchat;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;
import org.json.JSONObject;
import java.security.SecureRandom;
import java.util.Iterator;

/**
 * Stores the committed settings and identity.
 */
public class ChatSettings
{
	private String ipAddress = "";
	private int websocketPort;
	private String defaultUsername = "";
	private boolean continousAudioBroadcastEnabled;
	private boolean areAudioEffectsEnabled;
	private JSONObject committed;
	private JSONObject identity;
	private String initializationError = "identity_not_initialized";
	private Boolean fileLoggingEnabled;
	private boolean publicationPending;
	private SharedPreferences preferences;
	private static Context applicationContext;
	public static ChatSettings instance;
	public static volatile Runnable onSettingsSaved;

	/**
	 * Returns the shared settings owner without loading an identity.
	 *
	 * @return the shared settings instance
	 */
	public static synchronized ChatSettings getInstance()
	{
		if (instance == null)
		{
			instance = new ChatSettings();
		}
		return instance;
	}

	/**
	 * Keeps the first application context for preference access.
	 *
	 * @param context Android context, or {@code null} to keep the current context
	 */
	public static synchronized void attachContext(Context context)
	{
		if (applicationContext == null && context != null)
		{
			applicationContext = context.getApplicationContext();
		}
	}

	/**
	 * Opens the preference file also used by {@code MainActivity.getPreferences()}.
	 *
	 * @return the cached application preferences
	 * @throws IllegalStateException if no application context has been attached
	 */
	private SharedPreferences preferences()
	{
		if (this.preferences == null && applicationContext != null)
		{
			this.preferences = applicationContext.getSharedPreferences("MainActivity", Context.MODE_PRIVATE);
		}
		if (this.preferences == null)
		{
			throw new IllegalStateException("settings context unavailable");
		}
		return this.preferences;
	}

	/**
	 * Generates an alphanumeric identity passphrase of 200 characters.
	 *
	 * @return a passphrase generated with {@link SecureRandom}
	 */
	public static String generateIdentitySeed()
	{
		String chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
		SecureRandom random = new SecureRandom();
		StringBuilder seed = new StringBuilder(200);
		for (int i = 0; i < 200; i++)
		{
			seed.append(chars.charAt(random.nextInt(chars.length())));
		}
		return seed.toString();
	}

	/**
	 * Checks whether the RSA key size is supported by the identity format.
	 *
	 * @param bits the requested modulus size in bits
	 * @return {@code true} for a supported size
	 */
	private static boolean validBits(int bits)
	{
		return bits == 2048 || bits == 3072 || bits == 4096 || bits == 6144 || bits == 8192;
	}

	/**
	 * Updates native fields from committed settings.
	 *
	 * @param settings committed settings
	 */
	private void applyFields(JSONObject settings)
	{
		this.ipAddress = settings.optString("host", "");
		this.websocketPort = settings.optInt("port", 0);
		this.defaultUsername = settings.optString("default_username", "");
		this.continousAudioBroadcastEnabled = settings.optBoolean("is_microphone_always_on", false);
		this.areAudioEffectsEnabled = settings.optBoolean("is_audio_effect_enabled", false);
	}

	/**
	 * Loads settings and identity once, preserving stored data on failure.
	 *
	 * @return {@code true} if committed settings are available
	 */
	public synchronized boolean loadPersistedState()
	{
		if (this.committed != null)
		{
			return true;
		}
		try
		{
			SharedPreferences store = this.preferences();
			String stored = store.getString("settings", "");
			JSONObject settings = stored.isEmpty() ? new JSONObject() : new JSONObject(stored);
			String record = store.getString("identity_record_v1", "");
			JSONObject canonical;
			if (!record.isEmpty())
			{
				canonical = new JSONObject(record);
			}
			else
			{
				String settingsSeed = settings.optString("identity_string", "");
				String legacySeed = store.getString("identity_seed", "");
				if (!settingsSeed.isEmpty() && !legacySeed.isEmpty() && !settingsSeed.equals(legacySeed))
				{
					this.initializationError = "legacy_identity_conflict";
					throw new IllegalStateException("conflicting legacy identities require recovery");
				}
				String seed = settingsSeed.isEmpty() ? legacySeed : settingsSeed;
				if (seed.isEmpty())
				{
					seed = generateIdentitySeed();
				}
				canonical = new JSONObject();
				canonical.put("passphrase", seed);
				canonical.put("rsa_key_bits", settings.optInt("rsa_key_bits", 2048));
				canonical.put("revision", 1);
				canonical.put("operation_id", "initialization");
			}
			if (canonical.getString("passphrase").length() < 199 || !validBits(canonical.getInt("rsa_key_bits"))
				|| canonical.optLong("revision", 0) < 1)
			{
				throw new IllegalStateException("invalid stored identity");
			}
			this.complete(settings, canonical);
			if (!store.edit()
				.putString("identity_record_v1", BridgeProtocol.escapeSurrogates(canonical.toString()))
				.putString("settings", BridgeProtocol.escapeSurrogates(settings.toString())).commit())
			{
				throw new IllegalStateException("identity initialization could not be saved");
			}
			this.identity = canonical;
			this.committed = settings;
			this.applyFields(settings);
			this.initializationError = "";
			return true;
		}
		catch (Exception failed)
		{
			if (this.initializationError.isEmpty() || this.initializationError.equals("identity_not_initialized"))
			{
				this.initializationError = "identity_storage_unavailable";
			}
			Log.e("lemonchat", "settings initialization failed; existing data retained");
			return false;
		}
	}

	/**
	 * Adds defaults and copies the saved identity and logging values.
	 *
	 * @param settings settings to complete
	 * @param canonical saved identity record
	 * @throws Exception if a required identity field or preference cannot be read
	 */
	private void complete(JSONObject settings, JSONObject canonical) throws Exception
	{
		JSONObject defaults = new JSONObject();
		defaults.put("host", "");
		defaults.put("port", 0);
		defaults.put("default_username", "");
		defaults.put("is_microphone_always_on", false);
		defaults.put("is_autoconnect_enabled", true);
		defaults.put("is_audio_effect_enabled", false);
		defaults.put("is_app_log_enabled", false);
		defaults.put("app_mode", this.preferences().getString("app_mode", "advanced"));
		defaults.put("metadata_keys", new org.json.JSONArray());
		Iterator<String> keys = defaults.keys();
		while (keys.hasNext())
		{
			String key = keys.next();
			if (!settings.has(key))
			{
				settings.put(key, defaults.get(key));
			}
		}
		settings.put("identity_string", canonical.getString("passphrase"));
		settings.put("rsa_key_bits", canonical.getInt("rsa_key_bits"));
		settings.put("identity_revision", canonical.getLong("revision"));
		settings.put("is_file_logging_enabled", this.isFileLoggingEnabled());
	}

	/**
	 * Serializes committed settings, including the identity passphrase.
	 *
	 * @return escaped JSON settings, or an {@code identity_error} object before initialization
	 */
	public synchronized String getJsonSettings()
	{
		return this.committed == null ? "{\"identity_error\":" + JSONObject.quote(this.initializationError) + "}"
			: BridgeProtocol.escapeSurrogates(this.committed.toString());
	}

	/**
	 * Returns the most recent initialization failure code.
	 *
	 * @return the error code, or an empty string after successful initialization
	 */
	public synchronized String getInitializationError()
	{
		return this.initializationError;
	}

	/**
	 * Recovers an identity from the selected legacy store.
	 *
	 * @param useSettingsIdentity {@code true} for the settings identity, {@code false} for the
	 *                           standalone seed with a key size of 2048 bits
	 * @return {@code true} if the selected identity was committed
	 */
	public synchronized boolean recoverLegacyIdentity(boolean useSettingsIdentity)
	{
		try
		{
			JSONObject saved = new JSONObject(this.preferences().getString("settings", "{}"));
			String seed = useSettingsIdentity ? saved.optString("identity_string", "") : this.preferences().getString("identity_seed", "");
			int bits = useSettingsIdentity ? saved.optInt("rsa_key_bits", 2048) : 2048;
			return new JSONObject(this.replaceIdentity(seed, bits, java.util.UUID.randomUUID().toString(), 0)).optBoolean("ok", false);
		}
		catch (Exception failed)
		{
			return false;
		}
	}

	/**
	 * Saves a settings patch while preserving the identity fields.
	 *
	 * @param patchJson JSON fields to update
	 * @return {@code true} if the merged settings were committed
	 */
	public synchronized boolean saveJsonSettings(String patchJson)
	{
		try
		{
			if (this.committed == null)
			{
				return false;
			}
			JSONObject next = new JSONObject(this.committed.toString());
			JSONObject patch = new JSONObject(patchJson);
			Iterator<String> keys = patch.keys();
			while (keys.hasNext())
			{
				String key = keys.next();
				if (!key.equals("identity_string") && !key.equals("rsa_key_bits") && !key.equals("identity_revision"))
				{
					next.put(key, patch.get(key));
				}
			}
			this.complete(next, this.identity);
			if (!this.preferences().edit().putString("settings", BridgeProtocol.escapeSurrogates(next.toString())).commit())
			{
				return false;
			}
			this.committed = next;
			this.applyFields(next);
			this.publish();
			return true;
		}
		catch (Exception failed)
		{
			Log.w("lemonchat", "settings save failed");
			return false;
		}
	}

	/**
	 * Saves one setting without replacing unrelated fields.
	 *
	 * @param key the setting name
	 * @param value value supported by JSON
	 * @return {@code true} if the settings patch was committed
	 */
	public synchronized boolean updateSetting(String key, Object value)
	{
		try
		{
			return this.saveJsonSettings(new JSONObject().put(key, value).toString());
		}
		catch (Exception failed)
		{
			return false;
		}
	}

	/**
	 * Replaces the identity when the operation and revision are valid.
	 *
	 * <p>Repeating the same operation and identity returns the existing revision.
	 * Different data for that operation or an outdated revision is rejected.</p>
	 *
	 * @param seed exact passphrase including whitespace
	 * @param bits the supported RSA modulus size
	 * @param operationId the caller's unique operation identifier
	 * @param expectedRevision the current revision, or zero before an identity exists
	 * @return JSON containing {@code ok} and the committed revision or an error code
	 */
	public synchronized String replaceIdentity(String seed, int bits, String operationId, long expectedRevision)
	{
		try
		{
			if (seed == null || seed.length() < 199 || seed.length() > 65536
				|| !validBits(bits) || operationId == null || operationId.length() < 16 || operationId.length() > 128)
			{
				return "{\"ok\":false,\"error\":\"invalid_identity\"}";
			}
			long revision = this.identity == null ? 0 : this.identity.getLong("revision");
			if (this.identity != null && operationId.equals(this.identity.optString("operation_id", "")))
			{
				if (!seed.equals(this.identity.getString("passphrase")) || bits != this.identity.getInt("rsa_key_bits"))
				{
					return "{\"ok\":false,\"error\":\"operation_conflict\"}";
				}
				return "{\"ok\":true,\"identity_revision\":" + revision + "}";
			}
			if (expectedRevision != revision)
			{
				return "{\"ok\":false,\"error\":\"stale_identity_revision\"}";
			}
			if (identityFileJson(seed, bits).getBytes(java.nio.charset.StandardCharsets.UTF_8).length > 65536)
			{
				return "{\"ok\":false,\"error\":\"identity_too_large\"}";
			}
			JSONObject nextIdentity = new JSONObject();
			nextIdentity.put("passphrase", seed);
			nextIdentity.put("rsa_key_bits", bits);
			nextIdentity.put("revision", revision + 1);
			nextIdentity.put("operation_id", operationId);
			JSONObject next = this.committed == null ? new JSONObject(this.preferences().getString("settings", "{}"))
				: new JSONObject(this.committed.toString());
			this.complete(next, nextIdentity);
			if (!this.preferences().edit()
				.putString("identity_record_v1", BridgeProtocol.escapeSurrogates(nextIdentity.toString()))
				.putString("settings", BridgeProtocol.escapeSurrogates(next.toString())).commit())
			{
				return "{\"ok\":false,\"error\":\"storage_failed\"}";
			}
			this.identity = nextIdentity;
			this.committed = next;
			this.initializationError = "";
			this.applyFields(next);
			this.publish();
			return "{\"ok\":true,\"identity_revision\":" + (revision + 1) + "}";
		}
		catch (Exception failed)
		{
			return "{\"ok\":false,\"error\":\"identity_failed\"}";
		}
	}

	/**
	 * Serializes the identity in {@code .lmn} format version 1.
	 *
	 * @return JSON containing the exact passphrase and its RSA key size
	 * @throws IllegalStateException if the identity is unavailable or exceeds the file size limit
	 * @throws Exception if required identity fields cannot be read or serialized
	 */
	public synchronized String exportIdentity() throws Exception
	{
		if (this.identity == null)
		{
			throw new IllegalStateException("identity unavailable");
		}
		String file = identityFileJson(this.identity.getString("passphrase"), this.identity.getInt("rsa_key_bits"));
		if (file.getBytes(java.nio.charset.StandardCharsets.UTF_8).length > 65536)
		{
			throw new IllegalStateException("identity exceeds file size limit");
		}
		return file;
	}

	/**
	 * Builds identity JSON with escaped lone UTF16 surrogates.
	 *
	 * @param passphrase the exact identity passphrase
	 * @param bits the associated RSA modulus size
	 * @return identity JSON in format version 1
	 * @throws Exception if the JSON fields cannot be serialized
	 */
	private static String identityFileJson(String passphrase, int bits) throws Exception
	{
		JSONObject file = new JSONObject();
		file.put("format", "lemon-chat-identity");
		file.put("version", 1);
		file.put("passphrase", passphrase);
		file.put("rsa_key_bits", bits);
		return BridgeProtocol.escapeSurrogates(file.toString());
	}

	/**
	 * Replaces the RSA size while preserving the canonical passphrase.
	 *
	 * @param bits the new supported RSA modulus size
	 * @param operationId the caller's unique operation identifier
	 * @param expectedRevision the revision to replace
	 * @return the replacement result, or an {@code identity_unavailable} error
	 */
	public synchronized String resizeIdentity(int bits, String operationId, long expectedRevision)
	{
		if (this.identity == null)
		{
			return "{\"ok\":false,\"error\":\"identity_unavailable\"}";
		}
		return this.replaceIdentity(this.identity.optString("passphrase", ""), bits, operationId, expectedRevision);
	}

	/**
	 * Combines saved changes into one notification on the main thread.
	 */
	private void publish()
	{
		if (this.publicationPending)
		{
			return;
		}
		this.publicationPending = true;
		new android.os.Handler(android.os.Looper.getMainLooper()).post(() ->
		{
			Runnable listener;
			synchronized (ChatSettings.this)
			{
				ChatSettings.this.publicationPending = false;
				listener = onSettingsSaved;
			}
			if (listener != null)
			{
				listener.run();
			}
		});
	}

	/**
	 * Loads the logging preference once, then returns its cached value.
	 *
	 * @return {@code true} when file logging is enabled
	 */
	public synchronized boolean isFileLoggingEnabled()
	{
		if (this.fileLoggingEnabled == null)
		{
			this.fileLoggingEnabled = this.preferences().getBoolean("file_logging_enabled", false);
		}
		return this.fileLoggingEnabled;
	}

	/**
	 * Saves the logging preference and settings together.
	 *
	 * @param enabled whether file logging should be enabled
	 * @return {@code true} if the preference was saved
	 */
	public synchronized boolean setFileLoggingEnabled(boolean enabled)
	{
		this.isFileLoggingEnabled();
		try
		{
			JSONObject next = this.committed == null ? null : new JSONObject(this.committed.toString());
			SharedPreferences.Editor editor = this.preferences().edit().putBoolean("file_logging_enabled", enabled);
			if (next != null)
			{
				next.put("is_file_logging_enabled", enabled);
				editor.putString("settings", BridgeProtocol.escapeSurrogates(next.toString()));
			}
			if (!editor.commit())
			{
				return false;
			}
			this.fileLoggingEnabled = enabled;
			if (next != null)
			{
				this.committed = next;
				this.publish();
			}
			return true;
		}
		catch (Exception failed)
		{
			Log.w("lemonchat", "logging preference save failed");
			return false;
		}
	}

	/**
	 * Returns the configured server host.
	 *
	 * @return the host name or address, or an empty string before configuration
	 */
	public synchronized String getIpAddress()
	{
		return this.ipAddress;
	}

	/**
	 * Returns the configured remote WebSocket port.
	 *
	 * @return the port, or zero before configuration
	 */
	public synchronized int getWebsocketPort()
	{
		return this.websocketPort;
	}

	/**
	 * Returns the username requested when connecting.
	 *
	 * @return the configured name, or an empty string to use the server's default
	 */
	public synchronized String getDefaultUsername()
	{
		return this.defaultUsername;
	}

	/**
	 * Returns the saved continuous microphone preference.
	 *
	 * @return {@code true} when continuous microphone capture is selected
	 */
	public synchronized boolean isMicrophoneAlwayson()
	{
		return this.continousAudioBroadcastEnabled;
	}

	/**
	 * Returns whether application sound effects are enabled.
	 *
	 * @return the committed sound effects preference
	 */
	public synchronized boolean getAreAudioEffectsEnabled()
	{
		return this.areAudioEffectsEnabled;
	}
	/**
	 * Returns whether application sound effects are disabled.
	 *
	 * @return {@code true} when application sound effects are disabled
	 */
	public synchronized boolean getAreAudioEffectsDisabled()
	{
		return !this.areAudioEffectsEnabled;
	}

}
