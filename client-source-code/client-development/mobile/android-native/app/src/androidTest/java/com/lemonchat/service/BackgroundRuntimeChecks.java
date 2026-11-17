package com.lemonchat.service;

import android.content.Context;
import android.content.SharedPreferences;

/** checks actual CPU lease ownership and isolated recovery preferences */
public final class BackgroundRuntimeChecks
{
    public static int verify(Context context)
    {
        int checks = 0;
        BackgroundConnectionLease lease = new BackgroundConnectionLease(context);
        try
        {
            require(!lease.isHeld(), "creating a lease kept the CPU awake");
            checks++;
            require(lease.setActive(true) && lease.isHeld(), "requested connection lease was not acquired");
            checks++;
            require(lease.setActive(true) && lease.isHeld(), "repeating active state lost the lease");
            checks++;
            require(lease.setActive(false) && !lease.isHeld(), "offline wait retained a CPU lease");
            checks++;
            require(lease.setActive(true) && lease.isHeld(), "connection lease did not resume");
            checks++;
        }
        finally
        {
            lease.close();
        }
        require(!lease.isHeld(), "service teardown retained the CPU lease");
        checks++;
        require(!lease.setActive(true) && !lease.isHeld(), "closed lease was reactivated");
        checks++;

        SharedPreferences preferences = context.getSharedPreferences("background-recovery-checks", Context.MODE_PRIVATE);
        try
        {
            for (int flags = 0; flags < 16; flags++)
            {
                boolean valid = (flags & 1) != 0;
                boolean requested = (flags & 2) != 0;
                boolean automatic = (flags & 4) != 0;
                boolean suppressed = (flags & 8) != 0;
                preferences.edit().clear().putBoolean("saved_connection_valid", valid).putBoolean("connection_requested", requested).putBoolean("automatic_connection", automatic).putBoolean("reconnect_suppressed", suppressed).commit();
                require(ConnectionRestartJob.allowsRecovery(preferences) == (valid && !suppressed && (requested || automatic)), "recovery ignored saved state or explicit disconnect");
                checks++;
            }
        }
        finally
        {
            preferences.edit().clear().commit();
        }
        require(BackgroundPermissions.read(context) != null, "background permission state is unavailable");
        checks++;
        return checks;
    }

    private static void require(boolean condition, String message)
    {
        if (!condition)
        {
            throw new AssertionError(message);
        }
    }
}
