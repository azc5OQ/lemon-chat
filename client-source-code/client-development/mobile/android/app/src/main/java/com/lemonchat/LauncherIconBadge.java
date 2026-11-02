package com.lemonchat;

import android.content.ComponentName;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

/**
 * Unread count on the launcher icon, duolingo style: the apk ships one icon per count
 * (aliases in the manifest) and exactly one launcher alias is enabled at a time.
 *
 * Switches are debounced, because every switch makes the launcher redraw the icon and
 * some launchers blink while doing it.
 */
public class LauncherIconBadge
{
	// index 0 = plain icon, 1..99 = that number, 100 = "99+"
	private static final int MAX_INDEX = 100;

	private static String aliasName(int index)
	{
		if (index == 0)
		{
			return "com.lemonchat.LauncherDefault";
		}

		if (index == MAX_INDEX)
		{
			return "com.lemonchat.LauncherBadge99plus";
		}

		return "com.lemonchat.LauncherBadge" + index;
	}

	private static final long DEBOUNCE_MS = 4000;

	private static final Handler handler = new Handler(Looper.getMainLooper());
	private static Runnable pendingSwitch = null;

	// -1 = unknown (fresh process); the enabled alias persists across restarts, we do not
	private static int currentIndex = -1;

	/** shows this unread total on the app icon (0 or less = plain icon). safe to call often. */
	public static void apply(Context context, int unreadCount)
	{
		final int targetIndex = (unreadCount <= 0) ? 0 : Math.min(unreadCount, MAX_INDEX);

		if (targetIndex == currentIndex && pendingSwitch == null)
		{
			return;
		}

		if (pendingSwitch != null)
		{
			handler.removeCallbacks(pendingSwitch);
		}

		final Context applicationContext = context.getApplicationContext();

		pendingSwitch = new Runnable()
		{
			public void run()
			{
				pendingSwitch = null;
				LauncherIconBadge.switchAlias(applicationContext, targetIndex);
			}
		};

		handler.postDelayed(pendingSwitch, DEBOUNCE_MS);
	}

	private static void switchAlias(Context context, int targetIndex)
	{
		if (targetIndex == currentIndex)
		{
			return;
		}

		try
		{
			PackageManager packageManager = context.getPackageManager();

			// enable the new alias first, so the launcher never sees zero entries
			packageManager.setComponentEnabledSetting(new ComponentName(context, aliasName(targetIndex)),
				PackageManager.COMPONENT_ENABLED_STATE_ENABLED, PackageManager.DONT_KILL_APP);

			if (currentIndex >= 0)
			{
				// normal case: only the alias we enabled last time is on, turn just that one off
				packageManager.setComponentEnabledSetting(new ComponentName(context, aliasName(currentIndex)),
					PackageManager.COMPONENT_ENABLED_STATE_DISABLED, PackageManager.DONT_KILL_APP);
			}
			else
			{
				// fresh process: the enabled alias persisted from last time, but we do not know which.
				// check states first - reads are silent, every write makes launchers redraw the icon
				for (int i = 0; i <= MAX_INDEX; i++)
				{
					if (i == targetIndex)
					{
						continue;
					}

					ComponentName alias = new ComponentName(context, aliasName(i));
					int state = packageManager.getComponentEnabledSetting(alias);

					// DEFAULT means "as the manifest says": on for the plain icon, off for badges
					boolean isOn = (state == PackageManager.COMPONENT_ENABLED_STATE_ENABLED)
						|| (state == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT && i == 0);

					if (isOn)
					{
						packageManager.setComponentEnabledSetting(alias,
							PackageManager.COMPONENT_ENABLED_STATE_DISABLED, PackageManager.DONT_KILL_APP);
					}
				}
			}

			currentIndex = targetIndex;
			Log.i("lemonchat", "launcher icon switched to " + aliasName(targetIndex));
		}
		catch (Exception switchFailed)
		{
			Log.w("lemonchat", "launcher icon switch failed", switchFailed);
		}
	}
}
