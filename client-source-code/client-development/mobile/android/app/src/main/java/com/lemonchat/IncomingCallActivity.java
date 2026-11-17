package com.lemonchat;

import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;

public class IncomingCallActivity extends AppCompatActivity
{
	private String callerName = "";
	private int channelId = 0;
	private String callId = "";
	private String sessionId = "";
	private long expiresAt;

	/**
	 * Creates the incoming call screen and applies its scoped call intent.
	 *
	 * @param savedInstanceState framework state from a previous activity instance, if available
	 */
	@Override protected void onCreate(Bundle savedInstanceState)
	{
		try
		{
			super.onCreate(savedInstanceState);
			setContentView(R.layout.incoming_call_activity);
			android.view.ViewGroup content = findViewById(android.R.id.content);
			HybridWindowInsets.apply(this, content.getChildAt(0));

			this.applyCallIntent(this.getIntent());
		}
		catch (Exception ex)
		{
			Log.d("Info", "[lemonchat] onCreate" + ex.getMessage());
			ex.printStackTrace();
		}
	}

	/**
	 * Refreshes both caller text and call capability when Android reuses the screen.
	 *
	 * @param intent replacement incoming call intent
	 */
	@Override protected void onNewIntent(Intent intent)
	{
		super.onNewIntent(intent);
		this.setIntent(intent);
		this.applyCallIntent(intent);
	}

	/**
	 * Copies the caller and capability from an intent, closing the screen if it is invalid.
	 *
	 * @param intent incoming call intent, which may be {@code null}
	 */
	private void applyCallIntent(Intent intent)
	{
		this.callerName = intent == null ? "" : intent.getStringExtra("callerName");
		this.channelId = intent == null ? 0 : intent.getIntExtra("channelId", 0);
		this.callId = intent == null ? "" : intent.getStringExtra("call_id");
		this.sessionId = intent == null ? "" : intent.getStringExtra("session_id");
		this.expiresAt = intent == null ? 0 : intent.getLongExtra("expires_at", 0);
		if (!this.validCall())
		{
			this.finish();
			return;
		}
		((TextView)this.findViewById(R.id.caller_name)).setText(this.callerName);
	}

	/**
	 * Checks the local shape and expiry of the displayed call capability.
	 * The service and Node additionally verify that it is still their current call.
	 *
	 * @return whether the displayed identifiers are present and unexpired
	 */
	private boolean validCall()
	{
		return this.callId != null && this.callId.matches("[a-fA-F0-9]{32}") && this.sessionId != null
			&& !this.sessionId.isEmpty() && System.currentTimeMillis() < this.expiresAt;
	}

	/**
	 * Forwards Decline to the private service and removes the incoming call task.
	 *
	 * @param view button that triggered the action
	 */
	public void onDeclineCall(View view)
	{
		if (!this.validCall())
		{
			this.finish();
			return;
		}
		//route through the service like the notification's decline button: cancels the call
		//notification and flags the next resume to stay idle instead of rejoining a channel
		Intent declineIntent = new Intent(this, BackgroundService.class);
		declineIntent.setAction(BackgroundService.ACTION_DECLINE_CALL);
		declineIntent.putExtra("call_id", this.callId);
		declineIntent.putExtra("session_id", this.sessionId);
		this.startService(declineIntent);

		//remove the whole task, not just this screen: a plain finish() dropped the user into
		//whatever was behind it, which is how a declined call still opened the chat
		finishAndRemoveTask();
	}

	/**
	 * Opens the main activity with the current Accept capability before closing this screen.
	 *
	 * @param view button that triggered the action
	 */
	public void onAcceptCall(View view)
	{
		if (!this.validCall())
		{
			this.finish();
			return;
		}
		//route through the service like the notification's accept button: it joins the
		//CALLER'S channel (this activity never did, so accepting here landed in root) and
		//brings MainActivity to the front itself
		Intent acceptIntent = new Intent(this, MainActivity.class);
		acceptIntent.setAction(BackgroundService.ACTION_ACCEPT_CALL);
		acceptIntent.putExtra("call_id", this.callId);
		acceptIntent.putExtra("session_id", this.sessionId);
		acceptIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
		this.startActivity(acceptIntent);

		finish(); //cancel IncomingCallActivity
	}
}
