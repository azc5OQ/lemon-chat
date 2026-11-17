package com.lemonchat;

import android.content.ComponentName;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

/**
 * Shows unread totals through launcher aliases.
 */
public class LauncherIconBadge
{
	// index 0 = plain icon, 1..99 = that number, 100 = "99+"
	private static final int MAX_INDEX = 100;

	/**
	 * Returns the manifest component name for an icon index.
	 * @param index icon index from {@code 0} through {@code 100}
	 * @return launcher alias name
	 */
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

	// The enabled alias persists across restarts. This index does not.
	private static int currentIndex = -1;

	/**
	 * Updates the icon after the unread count stops changing.
	 * @param context context supplying app services
	 * @param unreadCount total to show. Zero or less clears the badge. Counts above 99 show {@code 99+}.
	 */
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

	/**
	 * Enables the chosen launcher alias and disables the old aliases.
	 * @param context application context
	 * @param targetIndex icon index from {@code 0} through {@code 100}
	 */
	private static void switchAlias(Context context, int targetIndex)
	{
		if (targetIndex == currentIndex)
		{
			return;
		}

		try
		{
			PackageManager packageManager = context.getPackageManager();

			// Enable the new alias before removing the old one.
			packageManager.setComponentEnabledSetting(new ComponentName(context, aliasName(targetIndex)),
				PackageManager.COMPONENT_ENABLED_STATE_ENABLED, PackageManager.DONT_KILL_APP);

			if (currentIndex >= 0)
			{
				// Disable the previous alias.
				packageManager.setComponentEnabledSetting(new ComponentName(context, aliasName(currentIndex)),
					PackageManager.COMPONENT_ENABLED_STATE_DISABLED, PackageManager.DONT_KILL_APP);
			}
			else
			{
				// Find persisted aliases without forcing unnecessary redraws.
				for (int i = 0; i <= MAX_INDEX; i++)
				{
					if (i == targetIndex)
					{
						continue;
					}

					ComponentName alias = new ComponentName(context, aliasName(i));
					int state = packageManager.getComponentEnabledSetting(alias);

					// Only the plain icon is enabled by default in the manifest.
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
