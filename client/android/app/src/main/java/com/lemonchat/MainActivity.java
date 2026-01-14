package com.lemonchat;

import static android.view.View.INVISIBLE;
import static android.view.View.VISIBLE;

import android.app.AlertDialog;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.text.InputType;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

public class MainActivity extends AppCompatActivity
{
	private BackgroundService backgroundService = null;

	private ChatSettings settings = null;

	public static MainActivity instance = null;

	SharedPreferences preferences = null;

	private Permissions permissions = null;

	public static int FILE_CHOOSER_RESULT_CODE = 987454;

	@Override protected void onResume()
	{
		Log.d("Info", "[lemonchat] onResume");

		super.onResume();

		MainActivity.instance = this;

		// Move WebView back from service overlay (if it was there)

		try
		{
			this.handleOnResumeOnCreate();
		}
		catch (Exception ex)
		{
			Log.d("Info", "[lemonchat] onResume Exception: " + ex.getMessage());
			ex.printStackTrace();
		}
	}

	@Override protected void onPause()
	{
		try
		{
			this.handleOnStopOnDestroyOnPause();
		}
		catch (Exception ex)
		{
			ex.printStackTrace();
		}

		super.onPause();
	}

	@Override public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults)
	{
		super.onRequestPermissionsResult(requestCode, permissions, grantResults);
		this.permissions.onRequestPermissionsResult(requestCode, permissions, grantResults);
	}

	//onActivityResult for file chooser in WebView
	@Override protected void onActivityResult(int requestCode, int resultCode, Intent data)
	{
		super.onActivityResult(requestCode, resultCode, data);
		if (requestCode == FILE_CHOOSER_RESULT_CODE)
		{
			Log.d("Info", "[lemonchat] onActivityResult :");

			if (resultCode == RESULT_OK && this.backgroundService.fileChooserCallback != null)
			{
				Log.d("Info", "[lemonchat] onActivityResult :");

				Uri[] result = null;

				if (data != null)
				{
					String dataString = data.getDataString();
					if (dataString != null)
					{
						result = new Uri[] { Uri.parse(dataString) };
					}
				}

				this.backgroundService.fileChooserCallback.onReceiveValue(result);
			}
			else
			{
				//user cancelled or error occured
				//pass null to reset the state of the file input

				if (this.backgroundService.fileChooserCallback != null)
				{
					this.backgroundService.fileChooserCallback.onReceiveValue(null);
				}
			}

			this.backgroundService.fileChooserCallback = null;
		}
	}

	private void startLemonchatBackgroundService()
	{
		ServiceConnection connection = new ServiceConnection() {
			@Override public void onServiceConnected(ComponentName name, IBinder service)
			{
				BackgroundService.LocalBinder binder = (BackgroundService.LocalBinder)service;
				MainActivity.this.backgroundService = binder.getService();

				//do something

				MainActivity.this.handleOnResumeOnCreate();
			}

			@Override public void onServiceDisconnected(ComponentName name)
			{
			}
		};

		//create service

		Intent intent = new Intent(this, BackgroundService.class);
		//this.startService(serviceIntent); -> gets cancelled shortly

		if (!BackgroundService.isRunning)
		{
			this.startForegroundService(intent); //calls createWebViewInServiceContext
			Log.d("Info", "[lemonchat] this.startForegroundService(intent) :");
		}
		else
		{
			Log.d("Info", "[lemonchat] service already running !:");
		}

		this.bindService(intent, connection, Context.BIND_AUTO_CREATE); //this is what triggers onServiceConnected
	}

	private void handleOnStopOnDestroyOnPause()
	{
		MainActivity.instance = null;

		Log.d("Info", "[lemonchat] handleOnStopOnDestroyOnPause");
		if (this.backgroundService != null)
		{
			// Move WebView back from service overlay (if it was there)
			if (backgroundService.webView != null)
			{
				if (backgroundService.isWebViewAttachedToHiddenWindow == false)
				{
					FrameLayout frameLayout = this.findViewById(R.id.root_container);

					if (frameLayout != null && this.backgroundService != null)
					{
						frameLayout.removeView(this.backgroundService.webView);
						this.backgroundService.attachWebViewToInvisibleWindow();
					}
				}
			}
		}
	}

	@Override protected void onCreate(Bundle savedInstanceState)
	{
		try
		{
			super.onCreate(savedInstanceState);
			setContentView(R.layout.main_activity);

			MainActivity.instance = this;

			this.preferences = this.getPreferences(Context.MODE_PRIVATE);
			this.settings = ChatSettings.getInstance();
			this.settings.setPreferences(this.preferences);

			this.permissions = new Permissions(this);
			this.permissions.beginCheck();

			//	sometimes onCreate is called if service is not running, sometimes its called when service is already running
			//	handle both cases
			if (this.backgroundService == null)
			{
				this.startLemonchatBackgroundService();
			}
			else
			{
				this.handleOnResumeOnCreate();
			}

			try
			{
				LinearLayout keysContainer = findViewById(R.id.keys_container);

				// Optional: add one key field by default

				if (keysContainer.getChildCount() == 0)
				{
					Button addKeyBtn = findViewById(R.id.add_key);
					addKeyBtn.setOnClickListener(v -> this.addKeyField(null));
					this.addKeyField(null);
				}
			}
			catch (Exception ex)
			{
				ex.printStackTrace();
			}
		}
		catch (Exception ex)
		{
			Log.d("Info", "[lemonchat] onCreate" + ex.getMessage());
			ex.printStackTrace();
		}
	}

	public void showSettings()
	{
		FrameLayout frameLayout = this.findViewById(R.id.root_container);

		if (frameLayout != null && this.backgroundService != null)
		{
			frameLayout.removeView(this.backgroundService.webView);
			this.backgroundService.attachWebViewToInvisibleWindow();
		}

		this.findViewById(R.id.settings_panel).setVisibility(VISIBLE);
	}

	// Add a key row
	private void addKeyField(String valueOfThatKeyField)
	{
		LinearLayout keysContainer;

		keysContainer = findViewById(R.id.keys_container);

		LinearLayout row = new LinearLayout(this);
		row.setOrientation(LinearLayout.HORIZONTAL);
		row.setPadding(0, 4, 0, 4);

		EditText keyInput = new EditText(this);
		keyInput.setHint("Key");
		keyInput.setTextColor(Color.BLACK);
		keyInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);

		if (valueOfThatKeyField != null)
		{
			keyInput.setText(valueOfThatKeyField);
		}
		keyInput.setLayoutParams(new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

		// Remove button
		Button removeBtn = new Button(this);
		removeBtn.setText("✕"); // Or use trash icon
		removeBtn.setLayoutParams(new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));

		removeBtn.setOnClickListener(v -> {
			if (keysContainer.getChildCount() > 1)
			{
				keysContainer.removeView(row);
			}
			else
			{
				CharSequence text = "at least one key is needed";
				int duration = Toast.LENGTH_LONG;
				Toast toast = Toast.makeText(this, text, duration);
				toast.show();
			}
		});

		row.addView(keyInput);
		row.addView(removeBtn);

		keysContainer.addView(row);
	}

	public void handleOnResumeOnCreate()
	{
		//check if WebView is attached to invisible window of background service
		if (this.backgroundService != null)
		{
			if (this.backgroundService.webView != null)
			{
				if (this.backgroundService.isWebViewAttachedToHiddenWindow == true)
				{
					FrameLayout frameLayout = this.findViewById(R.id.root_container);

					if (frameLayout != null)
					{
						this.backgroundService.detachWebViewFromInvisibleWindow();

						//layout params for frameLayout are needed, without them the WebView appears as black
						WindowManager.LayoutParams params = null;

						if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
						{
							params = new WindowManager.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, android.view.ViewGroup.LayoutParams.MATCH_PARENT,
												WindowManager.LayoutParams.TYPE_APPLICATION, //or is it TYPE_APPLICATION_PANEL
												WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE, PixelFormat.TRANSLUCENT);
						}
						else
						{
							params = new WindowManager.LayoutParams(android.view.ViewGroup.LayoutParams.WRAP_CONTENT, android.view.ViewGroup.LayoutParams.WRAP_CONTENT, WindowManager.LayoutParams.TYPE_PHONE, WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE, PixelFormat.TRANSLUCENT);
						}

						frameLayout.addView(this.backgroundService.webView, params);
					}
				}
				else
				{
					//the app was started and there is no attached window, probably first start
					Log.d("Info", "[lemonchat] app started for first time");
					FrameLayout frameLayout = this.findViewById(R.id.root_container);

					if (!(this.backgroundService.webView.getParent() instanceof ViewGroup))
					{
						frameLayout.addView(this.backgroundService.webView);
					}
				}

				this.backgroundService.webView.evaluateJavascript("JavascriptJavaBridge__send_come_from_idle_mode_request();", null);
			}
		}
	}

	@Override protected void onStop()
	{
		try
		{
			Log.d("Info", "[lemonchat] onStop");
			this.handleOnStopOnDestroyOnPause();
			//this.backgroundService.webView.evaluateJavascript("JavascriptJavaBridge__send_go_to_idle_mode_request();", null);

			super.onStop();
		}
		catch (Exception ex)
		{
			Log.d("Info", "[lemonchat] onCreate" + ex.getMessage());
			ex.printStackTrace();
		}
	}

	@Override protected void onDestroy()
	{
		try
		{
			Log.d("Info", "[lemonchat] onDestroy");
			this.handleOnStopOnDestroyOnPause();
			this.backgroundService.webView.evaluateJavascript("JavascriptJavaBridge__send_go_to_idle_mode_request();", null);

			super.onDestroy();
		}
		catch (Exception ex)
		{
			Log.d("Info", "[lemonchat] onDestroy" + ex.getMessage());
			ex.printStackTrace();
		}
	}

	//invoked by
	// @JavascriptInterface public void JavaExportRequestCurrentSettingsFromAndroid()
	public boolean LoadSettings()
	{
		boolean result = false;

		try
		{
			String json = this.preferences.getString("settings", "");

			this.settings.saveJsonSettings(json);

			if (json.equals("") == false)
			{
				Log.d("Info", "[lemonchat] settings string is not empty");

				JSONObject settingsJson = new JSONObject(json);

				String ipAddress = settingsJson.getString("host");
				int websocketPort = settingsJson.getInt("port");
				String defaultUsername = settingsJson.getString("default_username");
				boolean isMicAlwaysOnEnabled = settingsJson.getBoolean("is_microphone_always_on");
				boolean isAutoconnectEnabled = settingsJson.getBoolean("is_autoconnect_enabled");
				boolean isAudioEffectDisabled = settingsJson.getBoolean("is_audio_effect_enabled");

				LinearLayout keysContainerLinearLayout = findViewById(R.id.keys_container);
				EditText ipAddressEditText = findViewById(R.id.ip_address);
				EditText portEditText = findViewById(R.id.websocket_port);
				EditText defaultUsernameEditText = findViewById(R.id.default_username);
				Switch audioEnabledSwitch = findViewById(R.id.audio_enabled);
				Switch autoConnectSwitch = findViewById(R.id.settings_autoconnect_enabled);
				Switch soundEffectsSwitch = findViewById(R.id.settings_sound_effects_enabled);

				ipAddressEditText.setText(ipAddress);
				portEditText.setText(String.valueOf(websocketPort));
				defaultUsernameEditText.setText(defaultUsername);
				audioEnabledSwitch.setChecked(isMicAlwaysOnEnabled);
				autoConnectSwitch.setChecked(isAutoconnectEnabled);
				soundEffectsSwitch.setChecked(isAudioEffectDisabled);

                //clear out keys first
                LinearLayout keysContainer = findViewById(R.id.keys_container);
                keysContainer.removeAllViews();

                JSONArray keys = settingsJson.getJSONArray("metadata_keys");

				for (int i = 0; i < keys.length(); i++)
				{
					String value = keys.getString(i);
					this.addKeyField(value);
				}

				this.settings.setDefaultUsername(defaultUsername);
				this.settings.setWebsocketPort(websocketPort);
				this.settings.setAreAudioEffectsEnabled(!isAudioEffectDisabled);
				this.settings.setContinousAudioBroadcastEnabled(isMicAlwaysOnEnabled);
			}

			result = true;
		}
		catch (Exception ex)
		{
			Log.d("Info", "[lemonchat] LoadSettings exception:" + ex.getMessage());
			ex.printStackTrace();
		}

		return result;
	}

	public void saveSettings(View view)
	{
		FrameLayout frameLayout = this.findViewById(R.id.root_container);

		this.findViewById(R.id.settings_panel).setVisibility(INVISIBLE);

		if (frameLayout != null && this.backgroundService != null)
		{
			frameLayout.removeView(this.backgroundService.webView);

			this.backgroundService.detachWebViewFromInvisibleWindow();

			//layout params for frameLayout are needed, without them the WebView appears as black
			WindowManager.LayoutParams params = null;

			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
			{
				params = new WindowManager.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, android.view.ViewGroup.LayoutParams.MATCH_PARENT,
									WindowManager.LayoutParams.TYPE_APPLICATION, //or is it TYPE_APPLICATION_PANEL
									WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE, PixelFormat.TRANSLUCENT);
			}
			else
			{
				params = new WindowManager.LayoutParams(android.view.ViewGroup.LayoutParams.WRAP_CONTENT, android.view.ViewGroup.LayoutParams.WRAP_CONTENT, WindowManager.LayoutParams.TYPE_PHONE, WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE, PixelFormat.TRANSLUCENT);
			}

			frameLayout.addView(this.backgroundService.webView, params);
		}

		// Get UI values
		LinearLayout keysContainer = findViewById(R.id.keys_container);
		EditText ipAddress = findViewById(R.id.ip_address);
		EditText port = findViewById(R.id.websocket_port);
		EditText defaultUsername = findViewById(R.id.default_username);
		Switch audioEnabled = findViewById(R.id.audio_enabled);
		Switch autoConnect = findViewById(R.id.settings_autoconnect_enabled);
		Switch soundEffects = findViewById(R.id.settings_sound_effects_enabled);

		String ip = ipAddress.getText().toString().trim();
		String defaultUsernameString = defaultUsername.getText().toString().trim();

		int portStr = 0;
		try
		{
			portStr = Integer.parseInt(port.getText().toString().trim());
		}
		catch (Exception ex)
		{
			ex.printStackTrace();
		}

		boolean isContinousAudioBroadcastEnabled = audioEnabled.isChecked();
		boolean isAutoconnectEnabled = autoConnect.isChecked();
		boolean isAudioEffectsEnabled = soundEffects.isChecked();

		// Create the master JSONObject
		JSONObject settingsJson = new JSONObject();

		boolean isSomeOfServerKeysEmpty = false;

		try
		{
			// Put static values
			settingsJson.put("host", ip);
			settingsJson.put("port", portStr);
			settingsJson.put("default_username", defaultUsernameString);
			settingsJson.put("is_microphone_always_on", isContinousAudioBroadcastEnabled);
			settingsJson.put("is_autoconnect_enabled", isAutoconnectEnabled);
			settingsJson.put("is_audio_effect_enabled", isAudioEffectsEnabled);

			// Create a JSONArray for the dynamic keys
			JSONArray keysArray = new JSONArray();
			for (int i = 0; i < keysContainer.getChildCount(); i++)
			{
				LinearLayout row = (LinearLayout)keysContainer.getChildAt(i);
				EditText keyInput = (EditText)row.getChildAt(0);
				String key = keyInput.getText().toString().trim();
				if (!key.isEmpty())
				{
					keysArray.put(key);
				}
				else
				{
					CharSequence text = "Fail, keys cant be empty";
					int duration = Toast.LENGTH_LONG;
					Toast toast = Toast.makeText(this, text, duration);
					toast.show();

					isSomeOfServerKeysEmpty = true;
				}
			}

			settingsJson.put("metadata_keys", keysArray);

			if (isSomeOfServerKeysEmpty == false)
			{
				this.settings.saveJsonSettings(settingsJson.toString());

				this.backgroundService.javascriptJavaBridge.JavaExportRequestCurrentSettingsFromAndroid();
			}
		}
		catch (JSONException e)
		{
			e.printStackTrace();
		}
	}
}
