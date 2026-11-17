package com.lemonchat.service;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** resumes an explicitly enabled saved connection after boot or app update */
public final class BackgroundStartReceiver extends BroadcastReceiver
{
    @Override
    public void onReceive(Context context, Intent intent)
    {
        String action = intent != null ? intent.getAction() : null;
        if (Intent.ACTION_BOOT_COMPLETED.equals(action) || Intent.ACTION_MY_PACKAGE_REPLACED.equals(action))
        {
            ChatService.startAutomatic(context);
        }
    }
}
