package com.lemonchat.ui;

import android.app.Dialog;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.TextView;
import com.lemonchat.model.AdminSettings;
import com.lemonchat.model.AdminState;
import com.lemonchat.model.SessionSnapshot;
import com.lemonchat.service.ChatService;
import java.lang.reflect.Field;
import java.util.Collections;
import org.json.JSONObject;

/** real administration views with isolated in memory server snapshots */
public final class AdministrationChecks
{
    private int checks;

    /**
     * checks administration views without a live service or connection
     *
     * @param activity visible fixture activity
     * @return completed assertion count
     * @throws Exception when a check or fixture setup fails
     */
    public static int verify(MainActivity activity) throws Exception
    {
        ChatService service = new ChatService();
        AdministrationChecks suite = new AdministrationChecks();
        Field snapshot = ChatService.class.getDeclaredField("snapshot");
        snapshot.setAccessible(true);
        SessionSnapshot original = new SessionSnapshot(SessionSnapshot.Status.DISCONNECTED, "", -1, -1, "", false, Collections.emptyList(), Collections.emptyList(), -1);
        Dialog dialog = null;
        try
        {
            snapshot.set(service, state(original, admin(false, false, null)));
            dialog = AdministrationDialog.show(activity, service);
            View root = dialog.getWindow().getDecorView();
            EditText password = root.findViewWithTag("administration-password");
            suite.check(password != null && !password.isSaveEnabled(), "admin password stays out of saved view state");
            suite.check(password.getImportantForAutofill() == View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS, "admin password is not offered for autofill storage");
            suite.check(find(root, "Groups and permissions").getVisibility() == View.VISIBLE, "delegated group access is offered without administrator login");
            suite.check(root.findViewWithTag("administration-save").getVisibility() == View.GONE, "non administrators cannot edit general settings");
            password.setText("unsent fixture secret");
            dialog.dismiss();
            suite.check(password.getText().length() == 0, "closing the login screen clears its password view");

            AdminSettings settings = new AdminSettings(new JSONObject().put("max_slots", 20).put("used_slots", 2).put("max_channel_count", 100).put("reserved_slots", 2).put("is_typing_indicator_allowed", true));
            snapshot.set(service, state(original, admin(true, false, settings)));
            dialog = AdministrationDialog.show(activity, service);
            root = dialog.getWindow().getDecorView();
            EditText capacity = root.findViewWithTag("max_channel_count");
            Button save = root.findViewWithTag("administration-save");
            suite.check(capacity != null && capacity.getText().toString().equals("100"), "general setting controls show actual server values");
            suite.check(!save.isEnabled(), "unchanged settings cannot be saved");
            suite.check(save.getLayoutParams().width != ViewGroup.LayoutParams.MATCH_PARENT, "save remains a compact centered action");
            capacity.setText("0");
            suite.check(save.isEnabled(), "editing creates a local settings draft");
            save.performClick();
            TextView notice = root.findViewWithTag("administration-notice");
            suite.check(notice.getText().toString().contains("Channel limit"), "invalid capacity has a visible field specific error");
            suite.check(service.adminState().settings.values.get("max_channel_count").equals(100), "invalid draft does not mutate the service snapshot");
            capacity.setText("75");
            ((AdministrationDialog)dialog).onChanged();
            suite.check(capacity.getText().toString().equals("75"), "observer refresh preserves an unsaved draft");
            snapshot.set(service, state(original, admin(true, true, settings)));
            ((AdministrationDialog)dialog).onChanged();
            root = dialog.getWindow().getDecorView();
            EditText replacement = root.findViewWithTag("administration-password");
            EditText repeat = root.findViewWithTag("administration-password-repeat");
            suite.check(replacement != null && repeat != null, "forced password change asks for a replacement and confirmation");
            replacement.setText("first");
            repeat.setText("second");
            find(root, "Change password").performClick();
            suite.check(repeat.getError() != null, "mismatched administrator passwords cannot be submitted");
            snapshot.set(service, original);
            ((AdministrationDialog)dialog).onChanged();
            suite.check(!dialog.isShowing(), "connection replacement closes administration and its secret fields");
        }
        finally
        {
            if (dialog != null)
            {
                dialog.dismiss();
            }
            snapshot.set(service, original);
        }
        return suite.checks;
    }

    private static AdminState admin(boolean administrator, boolean force, AdminSettings settings)
    {
        return new AdminState(987654, administrator, true, false, force, "", "", 1, settings, Collections.emptyList(), Collections.singletonMap("view_group_permissions", true), Collections.emptyMap());
    }

    private static SessionSnapshot state(SessionSnapshot source, AdminState admin)
    {
        return new SessionSnapshot(SessionSnapshot.Status.CONNECTED, "Fixture", 1, 0, "Admin fixture", false, source.channels, source.members, source.groups, admin.connectionId, false, false, null, false, source.icons, source.emojis, source.capabilities, admin);
    }

    private static TextView find(View view, String label)
    {
        if (view instanceof TextView && label.contentEquals(((TextView)view).getText()))
        {
            return (TextView)view;
        }
        if (view instanceof ViewGroup)
        {
            ViewGroup parent = (ViewGroup)view;
            for (int i = 0; i < parent.getChildCount(); i++)
            {
                TextView found = find(parent.getChildAt(i), label);
                if (found != null)
                {
                    return found;
                }
            }
        }
        return null;
    }

    private void check(boolean valid, String message)
    {
        if (!valid)
        {
            throw new AssertionError(message);
        }
        checks++;
    }
}
