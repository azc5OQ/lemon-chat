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

	@Override protected void onCreate(Bundle savedInstanceState)
	{
		try
		{
			super.onCreate(savedInstanceState);
			setContentView(R.layout.incoming_call_activity);

			this.callerName = getIntent().getStringExtra("callerName");
			this.channelId = getIntent().getIntExtra("channelId", 0);

			((TextView)findViewById(R.id.caller_name)).setText(callerName);
		}
		catch (Exception ex)
		{
			Log.d("Info", "[lemonchat] onCreate" + ex.getMessage());
			ex.printStackTrace();
		}
	}

	public void onDeclineCall(View view)
	{
		//route through the service like the notification's decline button: cancels the call
		//notification and flags the next resume to stay idle instead of rejoining a channel
		Intent declineIntent = new Intent(this, BackgroundService.class);
		declineIntent.setAction(BackgroundService.ACTION_DECLINE_CALL);
		this.startService(declineIntent);

		finish(); //cancel IncomingCallActivity
	}

	public void onAcceptCall(View view)
	{
		//route through the service like the notification's accept button: it joins the
		//CALLER'S channel (this activity never did, so accepting here landed in root) and
		//brings MainActivity to the front itself
		Intent acceptIntent = new Intent(this, BackgroundService.class);
		acceptIntent.setAction(BackgroundService.ACTION_ACCEPT_CALL);
		acceptIntent.putExtra("channelId", this.channelId);
		this.startService(acceptIntent);

		finish(); //cancel IncomingCallActivity
	}
}
