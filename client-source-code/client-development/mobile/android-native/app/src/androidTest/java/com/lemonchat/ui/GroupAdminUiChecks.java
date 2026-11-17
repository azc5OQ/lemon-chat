package com.lemonchat.ui;

import android.app.Activity;
import android.app.Dialog;
import android.graphics.Insets;
import android.graphics.Rect;
import android.os.Build;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.ListView;
import android.widget.Spinner;
import com.lemonchat.model.AdminGroup;
import com.lemonchat.model.AdminState;
import com.lemonchat.model.ClientCapabilities;
import com.lemonchat.model.PermissionDefinition;
import com.lemonchat.model.SessionSnapshot;
import com.lemonchat.service.ChatService;
import java.lang.reflect.Field;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;

/** renders group management against isolated snapshots without starting a service */
public final class GroupAdminUiChecks
{
    private int checks;

    /**
     * checks phone, tablet, search, draft and stale connection behavior
     *
     * @param activity visible fixture activity on the main thread
     * @return completed assertion count
     * @throws Exception when a check or fixture setup fails
     */
    public static int verify(Activity activity) throws Exception
    {
        GroupAdminUiChecks suite = new GroupAdminUiChecks();
        suite.run(activity);
        return suite.checks;
    }

    private void run(Activity activity) throws Exception
    {
        ChatService service = new ChatService();
        PermissionDefinition original = permissions();
        setSnapshot(service, state(7, 1, true, 1000, original));
        GroupsAdminDialog dialog = (GroupsAdminDialog)GroupsAdminDialog.show(activity, service);
        try
        {
            View root = dialog.getWindow().getDecorView();
            ViewGroup viewport = root.findViewWithTag("admin-window-viewport");
            measure(viewport, dp(activity, 360), dp(activity, 720));
            check(root.findViewWithTag("admin-group-selector").getVisibility() == View.VISIBLE, "a phone uses a compact group selector");
            check(root.findViewWithTag("admin-group-list").getVisibility() == View.GONE, "a phone avoids a narrow side list");
            measure(viewport, dp(activity, 1000), dp(activity, 800));
            measure(viewport, dp(activity, 1000), dp(activity, 800));
            View groupList = root.findViewWithTag("admin-group-list");
            View permissionList = root.findViewWithTag("admin-permission-list");
            check(groupList.getVisibility() == View.VISIBLE && root.findViewWithTag("admin-group-selector").getVisibility() == View.GONE, "a tablet shows the group list beside permissions");
            check(bounds(viewport, groupList).right <= bounds(viewport, permissionList).left, "the two tablet panes do not overlap");
            ListView list = (ListView)permissionList;
            check(list.getAdapter().getCount() == 84, "the actual list exposes every server permission and power");
            EditText search = root.findViewWithTag("admin-permission-search");
            search.setText("direct_message");
            check(list.getAdapter().getCount() == 3, "search matches the action and its two powers");
            search.setText("not a permission fixture");
            check(list.getAdapter().getCount() == 0, "search shows an empty result without losing the draft");
            search.setText("send_channel_messages");
            clickRow(list, 0);
            PermissionEditor editor = (PermissionEditor)field(dialog, "child");
            View editorRoot = editor.getWindow().getDecorView();
            ((Spinner)editorRoot.findViewWithTag("admin-permission-action")).setSelection(1);
            ((CheckBox)editorRoot.findViewWithTag("admin-permission-negate")).setChecked(true);
            if (Build.VERSION.SDK_INT >= 30)
            {
                ViewGroup editorViewport = editorRoot.findViewWithTag("admin-window-viewport");
                int keyboard = dp(activity, 300);
                editorViewport.dispatchApplyWindowInsets(new WindowInsets.Builder().setInsets(WindowInsets.Type.systemBars(), Insets.of(0, dp(activity, 24), 0, dp(activity, 24))).setInsets(WindowInsets.Type.ime(), Insets.of(0, 0, 0, keyboard)).build());
                measure(editorViewport, dp(activity, 360), dp(activity, 720));
                check(bounds(editorViewport, editorRoot.findViewWithTag("admin-permission-use")).bottom <= dp(activity, 720) - keyboard, "the focused editor footer stays above the keyboard");
            }
            editorRoot.findViewWithTag("admin-permission-use").performClick();
            check(!editor.isShowing(), "using a value closes only the focused editor");
            check(root.findViewWithTag("admin-permissions-apply").isEnabled(), "the edited group waits for an explicit Apply");
            check(service.adminState().group(2, false).permissions == original && original.actions.get("send_channel_messages").equals("grant"), "editing does not mutate service owned permission values");
            PermissionDefinition.Flags editedFlags = ((PermissionDraft)field(dialog, "draft")).value().flags.get("send_channel_messages");
            check(editedFlags != null && editedFlags.negate, "advanced rules are retained in the local draft");
            Map<String, String> changedActions = new LinkedHashMap<>(original.actions);
            changedActions.put("ban", "grant");
            PermissionDefinition changed = new PermissionDefinition(changedActions, original.powers, original.grants, original.flags);
            setSnapshot(service, state(7, 2, true, 1000, changed));
            dialog.onChanged();
            check(!root.findViewWithTag("admin-permissions-apply").isEnabled(), "a server edit prevents overwriting a stale permission baseline");
            check(((android.widget.TextView)root.findViewWithTag("admin-group-notice")).getText().toString().contains("changed on the server"), "the stale draft explains why Apply is unavailable");
            setSnapshot(service, state(8, 3, true, 1000, original));
            dialog.onChanged();
            check(!root.findViewWithTag("admin-group-create").isEnabled() && !root.findViewWithTag("admin-group-actions").isEnabled(), "a replacement connection disables old group actions");
        }
        finally
        {
            dialog.dismiss();
        }
        check(((java.util.List<?>)field(service, "observers")).isEmpty(), "closing management detaches its service observer");

        setSnapshot(service, state(9, 1, false, 49, original));
        dialog = (GroupsAdminDialog)GroupsAdminDialog.show(activity, service);
        try
        {
            View root = dialog.getWindow().getDecorView();
            ((EditText)root.findViewWithTag("admin-permission-search")).setText("talk_power");
            ListView list = root.findViewWithTag("admin-permission-list");
            clickRow(list, 0);
            Dialog editor = (Dialog)field(dialog, "child");
            View editorRoot = editor.getWindow().getDecorView();
            check(!editorRoot.findViewWithTag("admin-permission-use").isEnabled(), "editing power49 cannot change a setting requiring50");
            check(!editorRoot.findViewWithTag("admin-permission-negate").isEnabled() && !editorRoot.findViewWithTag("admin-permission-skip").isEnabled(), "delegated access cannot change administrator flags");
            setSnapshot(service, state(10, 2, false, 49, original));
            dialog.onChanged();
            check(!editor.isShowing(), "connection replacement closes a child editor");
        }
        finally
        {
            dialog.dismiss();
        }
        setSnapshot(service, state(11, 1, false, 49, null));
        dialog = (GroupsAdminDialog)GroupsAdminDialog.show(activity, service);
        try
        {
            View root = dialog.getWindow().getDecorView();
            check(root.findViewWithTag("admin-permission-list").getParent() instanceof PermissionListView && ((View)root.findViewWithTag("admin-permission-list").getParent()).getVisibility() == View.GONE, "omitted permission data is not presented as editable defaults");
            check(!root.findViewWithTag("admin-permissions-apply").isEnabled(), "omitted permission data cannot be submitted");
        }
        finally
        {
            dialog.dismiss();
        }
        PermissionFields.Field channelAction = PermissionFields.CHANNEL.get(0);
        PermissionDefinition channelValues = new PermissionDefinition(Collections.singletonMap(channelAction.name, "deny"), Collections.emptyMap(), Collections.emptyMap(), Collections.emptyMap());
        PermissionEditor channelEditor = PermissionEditor.show(activity, Palette.load(activity), channelAction, channelValues, true, true, 1000, (value, required, flags) -> null);
        try
        {
            Spinner action = channelEditor.getWindow().getDecorView().findViewWithTag("admin-permission-action");
            check(action.getAdapter().getCount() == 2, "channel actions offer explicit Enable and Disable only");
        }
        finally
        {
            channelEditor.dismiss();
        }
    }

    private static AdminState state(long connection, long revision, boolean admin, int editPower, PermissionDefinition definition)
    {
        Map<String, Boolean> access = new LinkedHashMap<>();
        for (String action : Arrays.asList("edit_groups", "delete_groups", "edit_group_permissions", "assign_groups", "remove_groups", "create_groups", "view_group_members"))
        {
            access.put(action, true);
        }
        AdminGroup group = new AdminGroup(2, "Fixture members", "normal", false, false, -1, access, definition);
        return new AdminState(connection, admin, true, false, false, "", "", revision, null, Collections.singletonList(group), access, Collections.singletonMap("permission_edit_power", editPower));
    }

    private static void setSnapshot(ChatService service, AdminState state) throws Exception
    {
        SessionSnapshot snapshot = new SessionSnapshot(SessionSnapshot.Status.CONNECTED, "", 1, 0, "Fixture", true, Collections.emptyList(), Collections.emptyList(), Collections.emptyList(), state.connectionId, false, false, null, false, Collections.emptyList(), Collections.emptyList(), ClientCapabilities.none(), state);
        Field field = ChatService.class.getDeclaredField("snapshot");
        field.setAccessible(true);
        field.set(service, snapshot);
    }

    private static PermissionDefinition permissions()
    {
        Map<String, String> actions = new LinkedHashMap<>();
        Map<String, Integer> powers = new LinkedHashMap<>();
        Map<String, Integer> grants = new LinkedHashMap<>();
        for (PermissionFields.Field field : PermissionFields.SERVER)
        {
            if (field.power)
            {
                powers.put(field.name, 25);
            }
            else
            {
                actions.put(field.name, field.defaultAllowed ? "grant" : "deny");
            }
            grants.put(field.name, 50);
        }
        return new PermissionDefinition(actions, powers, grants, Collections.emptyMap());
    }

    private static void clickRow(ListView list, int index)
    {
        list.performItemClick(list.getAdapter().getView(index, null, list), index, list.getAdapter().getItemId(index));
    }

    private static Object field(Object object, String name) throws Exception
    {
        Field field = object.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(object);
    }

    private static void measure(View view, int width, int height)
    {
        view.forceLayout();
        view.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY));
        view.layout(0, 0, width, height);
    }

    private static Rect bounds(ViewGroup root, View child)
    {
        Rect result = new Rect(0, 0, child.getWidth(), child.getHeight());
        root.offsetDescendantRectToMyCoords(child, result);
        return result;
    }

    private static int dp(Activity activity, int value)
    {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }

    private void check(boolean value, String message)
    {
        if (!value)
        {
            throw new AssertionError(message);
        }
        checks++;
    }
}
