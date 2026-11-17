package com.lemonchat.server;

import android.app.Instrumentation;
import android.content.Intent;
import android.content.SharedPreferences;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.TextView;
import java.util.HashMap;
import java.util.Map;

/** checks guide navigation without starting or publishing a server */
final class FunnelUiChecks
{
    private FunnelUiChecks()
    {
    }

    static int Verify(Instrumentation instrumentation) throws Exception
    {
        SharedPreferences preferences = instrumentation.getTargetContext().getSharedPreferences("hosting", 0);
        Map<String, ?> before = new HashMap<>(preferences.getAll());
        instrumentation.runOnMainSync(() -> Require(!ServerService.active, "guide fixture requires a stopped server"));
        FunnelActivity activity = (FunnelActivity)instrumentation.startActivitySync(new Intent(instrumentation.getTargetContext(), FunnelActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        Instrumentation.ActivityMonitor monitor = null;
        try
        {
            FunnelActivity initial = activity;
            instrumentation.runOnMainSync(() ->
            {
                Require(Find(initial, "Step 1 of 4") != null, "guide starts at the explanation");
                Require(before.equals(preferences.getAll()), "opening the guide preserves hosting preferences");
                Require(!ServerService.active, "opening the guide leaves the service stopped");
                Click(initial, "Next");
                Require(Find(initial, "Step 2 of 4") != null, "Next opens account preparation");
                Require(Find(initial, "Open DNS settings") != null && Find(initial, "Open Access controls") != null, "account preparation offers the actual settings pages");
            });
            monitor = instrumentation.addMonitor(FunnelActivity.class.getName(), null, false);
            instrumentation.runOnMainSync(initial::recreate);
            FunnelActivity recreated = (FunnelActivity)instrumentation.waitForMonitorWithTimeout(monitor, 10000);
            Require(recreated != null, "guide returns after rotation");
            activity = recreated;
            instrumentation.runOnMainSync(() ->
            {
                Require(Find(recreated, "Step 2 of 4") != null, "rotation preserves the current step");
                Click(recreated, "Next");
                Require(Find(recreated, "Step 3 of 4") != null, "connection step follows account preparation");
                Require(Find(recreated, "Copy address") == null && Find(recreated, "Share address") == null, "stopped service has no shareable address");
                Require(!ServerService.active && before.equals(preferences.getAll()), "navigation does not enable or start public hosting");
                Click(recreated, "Back");
                Require(Find(recreated, "Step 2 of 4") != null, "Back returns to preparation");
                Click(recreated, "Back");
                Click(recreated, "Close");
            });
            instrumentation.waitForIdleSync();
            Require(before.equals(preferences.getAll()), "closing the guide preserves the hosting choice");
            return 12;
        }
        finally
        {
            if (monitor != null)
            {
                instrumentation.removeMonitor(monitor);
            }
            FunnelActivity remaining = activity;
            instrumentation.runOnMainSync(() ->
            {
                if (!remaining.isFinishing())
                {
                    remaining.finish();
                }
            });
        }
    }

    private static void Click(FunnelActivity activity, String text)
    {
        View button = Find(activity, text);
        Require(button instanceof Button && button.isEnabled(), "action is available: " + text);
        button.performClick();
    }

    private static View Find(FunnelActivity activity, String text)
    {
        return Find(activity.getWindow().getDecorView(), text);
    }

    private static View Find(View view, String text)
    {
        if (view instanceof TextView && text.contentEquals(((TextView)view).getText()) && view.getVisibility() == View.VISIBLE)
        {
            return view;
        }
        if (view instanceof ViewGroup)
        {
            ViewGroup group = (ViewGroup)view;
            for (int index = 0; index < group.getChildCount(); index++)
            {
                View found = Find(group.getChildAt(index), text);
                if (found != null)
                {
                    return found;
                }
            }
        }
        return null;
    }

    private static void Require(boolean condition, String message)
    {
        if (!condition)
        {
            throw new AssertionError(message);
        }
    }
}
