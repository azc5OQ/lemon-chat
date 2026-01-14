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
		finish(); //cancel IncomingCallActivity
	}

	public void onAcceptCall(View view)
	{
		Intent intent1 = new Intent(this, MainActivity.class);
		intent1.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK); //needed flag
		this.startActivity(intent1); //go to MainActivity that has html client

		finish(); //cancel IncomingCallActivity
	}
}
