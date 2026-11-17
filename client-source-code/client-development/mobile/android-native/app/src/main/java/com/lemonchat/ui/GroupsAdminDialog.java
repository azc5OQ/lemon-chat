package com.lemonchat.ui;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.PopupMenu;
import android.widget.Spinner;
import android.widget.TextView;
import com.lemonchat.model.AdminGroup;
import com.lemonchat.model.AdminState;
import com.lemonchat.model.Member;
import com.lemonchat.model.PermissionDefinition;
import com.lemonchat.model.SessionSnapshot;
import com.lemonchat.service.ChatService;
import java.util.ArrayList;
import java.util.List;

/** manages groups from service snapshots with local permission drafts */
public final class GroupsAdminDialog extends AdminWindow implements ChatService.Observer
{
    private final Activity activity;
    private final ChatService service;
    private final Palette palette;
    private final long connectionId;
    private final List<AdminGroup> visibleGroups = new ArrayList<>();
    private final GroupRows groupRows = new GroupRows();
    private AdminState state;
    private Spinner scope;
    private Spinner groups;
    private ListView groupList;
    private LinearLayout detail;
    private TextView title;
    private TextView notice;
    private TextView unavailable;
    private Button create;
    private Button actions;
    private Button reset;
    private Button apply;
    private PermissionListView permissions;
    private PermissionDraft draft;
    private AdminGroup draftGroup;
    private Dialog child;
    private MemberRows memberRows;
    private int selectedId = -1;
    private boolean channelGroups;
    private boolean binding;
    private boolean conflict;
    private boolean pendingPermissions;
    private boolean stale;
    private boolean wide;
    private long shownRevision = -1;
    private String groupNames = "";

    /**
     * opens management for the current connection
     *
     * @param activity visible owner
     * @param service connected state owner
     * @return the owned dialog or null when its activity cannot show it
     */
    public static Dialog show(Activity activity, ChatService service)
    {
        if (activity.isFinishing() || activity.isDestroyed() || service == null)
        {
            return null;
        }
        GroupsAdminDialog dialog = new GroupsAdminDialog(activity, service, new LinearLayout(activity));
        dialog.show();
        return dialog;
    }

    private GroupsAdminDialog(Activity activity, ChatService service, LinearLayout root)
    {
        super(activity, root, 1100, 850);
        this.activity = activity;
        this.service = service;
        palette = Palette.load(activity);
        state = service.adminState();
        connectionId = state.connectionId;
        root.setTag("admin-groups-root");
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(16), dp(12), dp(16), dp(12));
        root.setBackground(palette.rounded(palette.surface, dp(18)));
        LinearLayout header = row();
        header.addView(text("Groups & permissions", 20, true), new LinearLayout.LayoutParams(0, -2, 1));
        Button close = button("Close", false);
        close.setOnClickListener(view -> requestClose());
        header.addView(close, new LinearLayout.LayoutParams(-2, dp(48)));
        root.addView(header);
        LinearLayout selectors = row();
        scope = new Spinner(activity);
        scope.setTag("admin-group-scope");
        scope.setContentDescription("Group type");
        scope.setAdapter(new ArrayAdapter<>(activity, android.R.layout.simple_spinner_dropdown_item, new String[]{"Server groups", "Channel groups"}));
        selectors.addView(scope, new LinearLayout.LayoutParams(0, dp(48), 1));
        create = button("New group", false);
        create.setTag("admin-group-create");
        create.setOnClickListener(view -> nameGroup(null));
        selectors.addView(create, new LinearLayout.LayoutParams(-2, dp(48)));
        root.addView(selectors);
        groups = new Spinner(activity);
        groups.setTag("admin-group-selector");
        groups.setContentDescription("Group to manage");
        root.addView(groups, new LinearLayout.LayoutParams(-1, dp(48)));
        LinearLayout content = row();
        content.setGravity(Gravity.TOP);
        groupList = new ListView(activity);
        groupList.setTag("admin-group-list");
        groupList.setDivider(null);
        groupList.setAdapter(groupRows);
        groupList.setOnItemClickListener((parent, view, position, id) -> select(visibleGroups.get(position)));
        content.addView(groupList, new LinearLayout.LayoutParams(dp(240), -1));
        detail = new LinearLayout(activity);
        detail.setOrientation(LinearLayout.VERTICAL);
        content.addView(detail, new LinearLayout.LayoutParams(0, -1, 1));
        LinearLayout groupHeading = row();
        title = text("", 18, true);
        title.setTag("admin-group-title");
        groupHeading.addView(title, new LinearLayout.LayoutParams(0, -2, 1));
        actions = button("Actions", false);
        actions.setTag("admin-group-actions");
        actions.setOnClickListener(this::groupActions);
        groupHeading.addView(actions, new LinearLayout.LayoutParams(-2, dp(48)));
        detail.addView(groupHeading);
        unavailable = text("", 14, false);
        unavailable.setPadding(0, dp(12), 0, dp(12));
        detail.addView(unavailable);
        permissions = new PermissionListView(activity, palette, this::editPermission);
        detail.addView(permissions, new LinearLayout.LayoutParams(-1, 0, 1));
        root.addView(content, new LinearLayout.LayoutParams(-1, 0, 1));
        notice = text("", 13, false);
        notice.setTag("admin-group-notice");
        notice.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        notice.setPadding(0, dp(8), 0, dp(8));
        root.addView(notice);
        LinearLayout footer = row();
        footer.setGravity(Gravity.CENTER);
        reset = button("Reset draft", false);
        reset.setTag("admin-permissions-reset");
        reset.setOnClickListener(view -> discardThen(this::resetDraft));
        footer.addView(reset, new LinearLayout.LayoutParams(0, dp(48), 1));
        apply = button("Apply changes", true);
        apply.setTag("admin-permissions-apply");
        apply.setOnClickListener(view -> applyPermissions());
        LinearLayout.LayoutParams applyPosition = new LinearLayout.LayoutParams(0, dp(48), 1);
        applyPosition.setMarginStart(dp(10));
        footer.addView(apply, applyPosition);
        root.addView(footer);
        scope.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener()
        {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id)
            {
                if (binding || channelGroups == (position == 1))
                {
                    return;
                }
                boolean requested = position == 1;
                syncSelection();
                discardThen(() ->
                {
                    channelGroups = requested;
                    selectedId = -1;
                    draft = null;
                    groupNames = "";
                    render();
                });
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent)
            {
            }
        });
        groups.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener()
        {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id)
            {
                if (!binding && position >= 0 && position < visibleGroups.size())
                {
                    select(visibleGroups.get(position));
                }
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent)
            {
            }
        });
        root.addOnLayoutChangeListener((view, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) ->
        {
            boolean nextWide = right - left >= dp(680);
            if (wide != nextWide)
            {
                wide = nextWide;
                layoutColumns();
            }
        });
        layoutColumns();
        render();
    }

    @Override
    protected void onStart()
    {
        super.onStart();
        service.addObserver(this, false);
    }

    @Override
    protected void onStop()
    {
        service.removeObserver(this);
        closeChild();
        super.onStop();
    }

    @Override
    public void onBackPressed()
    {
        requestClose();
    }

    @Override
    public void onChanged()
    {
        if (memberRows != null)
        {
            memberRows.refresh();
        }
        AdminState latest = service.adminState();
        SessionSnapshot session = service.snapshot();
        boolean invalid = latest.connectionId != connectionId || session == null || !session.connected || session.connectionId != connectionId;
        if (shownRevision == latest.revision && stale == invalid)
        {
            return;
        }
        stale = invalid;
        state = latest;
        shownRevision = latest.revision;
        if (stale)
        {
            closeChild();
        }
        render();
    }

    @Override
    public void onProblem(String message)
    {
        if (isShowing())
        {
            notice.setText(message);
            notice.setTextColor(palette.danger);
        }
    }

    private void render()
    {
        visibleGroups.clear();
        StringBuilder names = new StringBuilder();
        for (AdminGroup group : state.groups)
        {
            if (group.channelGroup == channelGroups)
            {
                visibleGroups.add(group);
                names.append(group.id).append(':').append(group.name).append('\n');
            }
        }
        if (currentGroup() == null)
        {
            selectedId = visibleGroups.isEmpty() ? -1 : visibleGroups.get(0).id;
            draft = null;
            pendingPermissions = false;
        }
        if (!names.toString().equals(groupNames))
        {
            binding = true;
            List<String> labels = new ArrayList<>();
            for (AdminGroup group : visibleGroups)
            {
                labels.add(group.name);
            }
            groups.setAdapter(new ArrayAdapter<>(activity, android.R.layout.simple_spinner_dropdown_item, labels));
            groupNames = names.toString();
            binding = false;
        }
        syncSelection();
        groupRows.notifyDataSetChanged();
        AdminGroup selected = currentGroup();
        conflict = false;
        if (selected != null && selected.permissions != null)
        {
            if (draft == null || !draft.changed() || pendingPermissions && selected.permissions.sameValues(draft.value()))
            {
                draft = new PermissionDraft(selected.permissions);
                draftGroup = selected;
                pendingPermissions = false;
            }
            else
            {
                conflict = !sameTarget(draftGroup, selected);
            }
        }
        else
        {
            draft = null;
        }
        boolean available = !stale && state.groupsEnabled;
        title.setText(selected == null ? "No group selected" : selected.name);
        create.setEnabled(available && !state.busy && (state.isAdmin || state.allows(channelGroups ? "create_channel_groups" : "create_groups")));
        actions.setEnabled(available && selected != null && !state.busy);
        scope.setEnabled(!stale && !state.busy);
        groups.setEnabled(available && !state.busy);
        groupList.setEnabled(available && !state.busy);
        boolean showPermissions = available && selected != null && draft != null;
        permissions.setVisibility(showPermissions ? View.VISIBLE : View.GONE);
        unavailable.setVisibility(showPermissions ? View.GONE : View.VISIBLE);
        unavailable.setText(stale ? "This connection ended. Close this window and open administration again." : !state.groupsEnabled ? "Group permissions are disabled on this server." : selected == null ? "No groups are available." : "This server did not provide the group's permission values for your account.");
        if (showPermissions)
        {
            permissions.bind(channelGroups, draft.value(), canEdit(selected) && !conflict, editPower());
        }
        reset.setEnabled(draft != null && (draft.changed() || conflict) && !state.busy);
        apply.setEnabled(showPermissions && draft.changed() && canEdit(selected) && !conflict && !state.busy);
        notice.setText(stale ? "Connection changed. No changes can be sent from this window." : conflict ? "This group changed on the server. Reset the draft before editing again." : !state.error.isEmpty() ? state.error : state.busy ? "Waiting for the server…" : draft != null && draft.changed() ? "Changes stay in this draft until you apply them." : state.statusText);
        notice.setTextColor(stale || conflict || !state.error.isEmpty() ? palette.danger : palette.secondary);
        if (child instanceof PermissionEditor && (stale || conflict || !showPermissions || !canEdit(selected)))
        {
            ((PermissionEditor)child).invalidate("The original group or your access changed. Close this editor and check the latest values.");
        }
    }

    private AdminGroup currentGroup()
    {
        return state.group(selectedId, channelGroups);
    }

    private void select(AdminGroup selected)
    {
        if (selected.id == selectedId && selected.channelGroup == channelGroups)
        {
            return;
        }
        syncSelection();
        discardThen(() ->
        {
            selectedId = selected.id;
            channelGroups = selected.channelGroup;
            draft = null;
            pendingPermissions = false;
            render();
        });
    }

    private void syncSelection()
    {
        binding = true;
        scope.setSelection(channelGroups ? 1 : 0);
        for (int index = 0; index < visibleGroups.size(); index++)
        {
            if (visibleGroups.get(index).id == selectedId)
            {
                groups.setSelection(index);
                break;
            }
        }
        binding = false;
    }

    private void layoutColumns()
    {
        groups.setVisibility(wide ? View.GONE : View.VISIBLE);
        groupList.setVisibility(wide ? View.VISIBLE : View.GONE);
        detail.setPadding(wide ? dp(16) : 0, 0, 0, 0);
    }

    private int editPower()
    {
        return state.isAdmin ? 1000 : state.powers.getOrDefault("permission_edit_power", 0);
    }

    private boolean canEdit(AdminGroup group)
    {
        return !stale && group != null && group.id != 0 && group.permissions != null && group.allows("edit_group_permissions");
    }

    private void editPermission(PermissionFields.Field field)
    {
        AdminGroup group = currentGroup();
        if (group == null || draft == null || stale)
        {
            return;
        }
        PermissionDraft editing = draft;
        boolean editable = canEdit(group) && !conflict && !state.busy && editPower() >= editing.original.grants.getOrDefault(field.name, 1000);
        closeChild();
        child = PermissionEditor.show(activity, palette, field, editing.value(), editable, state.isAdmin, editPower(), (value, required, flags) ->
        {
            AdminState latest = service.adminState();
            AdminGroup target = latest.group(group.id, group.channelGroup);
            if (latest.connectionId != connectionId || !sameTarget(group, target) || target.permissions == null || !target.permissions.sameValues(editing.original) || !target.allows("edit_group_permissions"))
            {
                return "The group or your access changed. Close this editor and check the latest values.";
            }
            int power = latest.isAdmin ? 1000 : latest.powers.getOrDefault("permission_edit_power", 0);
            PermissionDefinition.Flags originalFlags = editing.original.flags.get(field.name);
            if (power < editing.original.grants.getOrDefault(field.name, 1000) || !latest.isAdmin && (required > power || flags.negate != (originalFlags != null && originalFlags.negate) || flags.skip != (originalFlags != null && originalFlags.skip)))
            {
                return "Your current editing power or administrator access does not allow that change.";
            }
            editing.update(field, value, required, flags);
            render();
            return null;
        });
    }

    private void applyPermissions()
    {
        AdminState latest = service.adminState();
        AdminGroup target = latest.group(selectedId, channelGroups);
        if (draft == null || latest.connectionId != connectionId || !sameTarget(draftGroup, target) || target.permissions == null || !target.permissions.sameValues(draft.original))
        {
            onChanged();
            notice.setText("The original group changed. Reset the draft before applying changes.");
            return;
        }
        pendingPermissions = true;
        if (!service.saveAdminPermissions(connectionId, selectedId, channelGroups, draft.value()))
        {
            pendingPermissions = false;
            notice.setText("The request could not be submitted. Check your connection and access.");
        }
    }

    private void resetDraft()
    {
        state = service.adminState();
        draft = null;
        pendingPermissions = false;
        render();
    }

    private void groupActions(View anchor)
    {
        AdminGroup group = currentGroup();
        if (group == null)
        {
            return;
        }
        PopupMenu menu = new PopupMenu(activity, anchor);
        menu.getMenu().add("Rename").setEnabled(group.allows("edit_groups"));
        menu.getMenu().add("Connected members").setEnabled(state.isAdmin || state.allows("view_group_members"));
        menu.getMenu().add("Delete").setEnabled(group.allows("delete_groups") && group.id != 0 && !group.isDefaultGuest && (!group.channelGroup || group.id != 1));
        menu.getMenu().add("Request save").setEnabled(!group.channelGroup && canRequestSave());
        menu.setOnMenuItemClickListener(item ->
        {
            String action = item.getTitle().toString();
            if (action.equals("Rename"))
            {
                nameGroup(group);
            }
            else if (action.equals("Connected members"))
            {
                showMembers(group);
            }
            else if (action.equals("Delete"))
            {
                closeChild();
                child = new AlertDialog.Builder(activity).setTitle("Delete " + group.name + "?").setMessage("Members lose this group. Their other group assignments stay.").setNegativeButton("Cancel", null).setPositiveButton("Delete", (dialog, which) -> submitted(sameTarget(group, service.adminState().group(group.id, group.channelGroup)) && service.deleteAdminGroup(connectionId, group.id, group.channelGroup))).show();
            }
            else
            {
                submitted(service.saveAdminGroups(connectionId));
            }
            return true;
        });
        menu.show();
    }

    private void nameGroup(AdminGroup group)
    {
        closeChild();
        boolean channel = group == null ? channelGroups : group.channelGroup;
        LinearLayout form = new LinearLayout(activity);
        form.setOrientation(LinearLayout.VERTICAL);
        form.setPadding(dp(20), dp(8), dp(20), dp(8));
        EditText name = new EditText(activity);
        name.setHint("Group name");
        name.setSingleLine(true);
        name.setSaveEnabled(false);
        name.setText(group == null ? "" : group.name);
        form.addView(name);
        Spinner preset = new Spinner(activity);
        if (group == null && !channel)
        {
            form.addView(text("Starting permissions", 14, false));
            preset.setAdapter(new ArrayAdapter<>(activity, android.R.layout.simple_spinner_dropdown_item, new String[]{"Normal", "Guest", "Moderator", "Admin"}));
            form.addView(preset);
        }
        TextView error = text("", 13, false);
        error.setTextColor(palette.danger);
        form.addView(error);
        AlertDialog prompt = new AlertDialog.Builder(activity).setTitle(group == null ? "New " + (channel ? "channel group" : "server group") : "Rename group").setView(form).setNegativeButton("Cancel", null).setPositiveButton(group == null ? "Create" : "Rename", null).create();
        prompt.setOnShowListener(dialog -> prompt.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(view ->
        {
            String value = name.getText().toString().trim();
            if (value.isEmpty())
            {
                name.setError("Enter a group name");
                return;
            }
            if (group != null && !sameTarget(group, service.adminState().group(group.id, group.channelGroup)))
            {
                error.setText("This group changed. Close this prompt and check its latest name.");
                return;
            }
            boolean accepted = group == null ? service.createAdminGroup(connectionId, value, channel ? "normal" : new String[]{"normal", "guest", "moderator", "admin"}[preset.getSelectedItemPosition()], channel) : service.renameAdminGroup(connectionId, group.id, group.channelGroup, value);
            if (accepted)
            {
                prompt.dismiss();
            }
            else
            {
                error.setText("The request could not be submitted. Check the name, connection and your access.");
            }
        }));
        child = prompt;
        prompt.show();
    }

    private void showMembers(AdminGroup group)
    {
        closeChild();
        LinearLayout root = new LinearLayout(activity);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(16), dp(12), dp(16), dp(12));
        root.setBackground(palette.rounded(palette.surface, dp(18)));
        root.addView(text("Connected members · " + group.name, 19, true));
        root.addView(text(group.channelGroup ? "A channel group applies only in the person's current channel." : "Choose who receives this server group. Offline identities are not listed here.", 13, false));
        ListView list = new ListView(activity);
        list.setDivider(null);
        memberRows = new MemberRows(group);
        list.setAdapter(memberRows);
        root.addView(list, new LinearLayout.LayoutParams(-1, 0, 1));
        Button close = button("Close", false);
        close.setOnClickListener(view -> closeChild());
        root.addView(close, new LinearLayout.LayoutParams(-1, dp(48)));
        child = new AdminWindow(activity, root, 650, 700);
        child.show();
    }

    private void submitted(boolean accepted)
    {
        if (!accepted)
        {
            notice.setText("The request could not be submitted. Check your connection and access.");
            notice.setTextColor(palette.danger);
        }
    }

    private boolean canRequestSave()
    {
        if (state.isAdmin)
        {
            return true;
        }
        for (String permission : new String[]{"create_groups", "edit_groups", "delete_groups", "assign_groups", "remove_groups", "edit_group_permissions", "upload_icons", "delete_icons"})
        {
            if (state.allows(permission))
            {
                return true;
            }
        }
        return false;
    }

    /** rejects prompts whose group definition has changed since it was opened */
    private static boolean sameTarget(AdminGroup expected, AdminGroup current)
    {
        return expected != null && current != null && expected.id == current.id && expected.channelGroup == current.channelGroup && expected.name.equals(current.name) && expected.type.equals(current.type) && expected.iconId == current.iconId && expected.isDefaultGuest == current.isDefaultGuest && (expected.permissions == null ? current.permissions == null : expected.permissions.sameValues(current.permissions));
    }

    private void discardThen(Runnable action)
    {
        if (draft == null || !draft.changed())
        {
            action.run();
            return;
        }
        closeChild();
        child = new AlertDialog.Builder(activity).setTitle("Discard permission edits?").setMessage("These changes have not been sent to the server.").setNegativeButton("Keep editing", null).setPositiveButton("Discard", (dialog, which) -> action.run()).show();
    }

    private void requestClose()
    {
        discardThen(this::dismiss);
    }

    private void closeChild()
    {
        if (child != null)
        {
            Dialog closing = child;
            child = null;
            closing.dismiss();
        }
        memberRows = null;
    }

    private final class GroupRows extends BaseAdapter
    {
        @Override
        public int getCount()
        {
            return visibleGroups.size();
        }

        @Override
        public Object getItem(int position)
        {
            return visibleGroups.get(position);
        }

        @Override
        public long getItemId(int position)
        {
            return visibleGroups.get(position).id;
        }

        @Override
        public View getView(int position, View convert, ViewGroup parent)
        {
            AdminGroup group = visibleGroups.get(position);
            TextView label = text(group.name, 15, group.id == selectedId);
            label.setPadding(dp(12), dp(12), dp(12), dp(12));
            label.setMinHeight(dp(48));
            label.setBackground(palette.interactive(group.id == selectedId ? palette.selection : palette.surface, dp(8)));
            return label;
        }
    }

    private final class MemberRows extends BaseAdapter
    {
        private final AdminGroup target;
        private final List<Member> members = new ArrayList<>();

        MemberRows(AdminGroup target)
        {
            this.target = target;
            refresh();
        }

        void refresh()
        {
            members.clear();
            SessionSnapshot snapshot = service.snapshot();
            if (snapshot != null && snapshot.connectionId == connectionId)
            {
                for (Member member : snapshot.members)
                {
                    if (!member.musicBot && (!target.channelGroup || member.channelId >= 0 && !member.idle))
                    {
                        members.add(member);
                    }
                }
            }
            notifyDataSetChanged();
        }

        @Override
        public int getCount()
        {
            return members.size();
        }

        @Override
        public Object getItem(int position)
        {
            return members.get(position);
        }

        @Override
        public long getItemId(int position)
        {
            return members.get(position).id;
        }

        @Override
        public View getView(int position, View convert, ViewGroup parent)
        {
            Member member = members.get(position);
            boolean assigned = target.channelGroup ? member.channelGroupId == target.id : member.groupIds.contains(target.id);
            LinearLayout row = row();
            row.setPadding(0, dp(6), 0, dp(6));
            row.addView(text(member.name, 15, false), new LinearLayout.LayoutParams(0, -2, 1));
            Button action = button(assigned ? target.channelGroup ? "Set default" : "Remove" : "Assign", false);
            AdminState latest = service.adminState();
            AdminGroup group = latest.group(target.id, target.channelGroup);
            SessionSnapshot session = service.snapshot();
            boolean allowed = group != null && !latest.busy && latest.connectionId == connectionId && (target.channelGroup ? latest.isAdmin || latest.allows("assign_channel_groups") && session != null && member.channelId == session.channelId : group.allows(assigned ? "remove_groups" : "assign_groups"));
            action.setEnabled(allowed && !(target.channelGroup && target.id == 0 && assigned));
            action.setOnClickListener(view ->
            {
                SessionSnapshot current = service.snapshot();
                boolean same = current != null && current.connected && current.connectionId == connectionId;
                boolean found = false;
                if (same)
                {
                    for (Member candidate : current.members)
                    {
                        if (candidate.id == member.id && candidate.publicKey.equals(member.publicKey) && (!target.channelGroup || candidate.channelId == member.channelId))
                        {
                            found = true;
                            break;
                        }
                    }
                }
                submitted(found && sameTarget(target, service.adminState().group(target.id, target.channelGroup)) && (target.channelGroup ? service.assignAdminChannelGroup(connectionId, assigned ? 0 : target.id, member.id) : service.assignAdminGroup(connectionId, target.id, member.id, !assigned)));
            });
            row.addView(action, new LinearLayout.LayoutParams(-2, dp(48)));
            return row;
        }
    }

    private LinearLayout row()
    {
        LinearLayout row = new LinearLayout(activity);
        row.setGravity(Gravity.CENTER_VERTICAL);
        return row;
    }

    private TextView text(String value, int size, boolean bold)
    {
        TextView text = new TextView(activity);
        text.setText(value);
        text.setTextSize(size);
        text.setTextColor(bold ? palette.text : palette.secondary);
        if (bold)
        {
            text.setTypeface(null, Typeface.BOLD);
        }
        return text;
    }

    private Button button(String label, boolean primary)
    {
        Button button = new Button(activity);
        button.setText(label);
        button.setAllCaps(false);
        button.setTextColor(primary ? palette.accentText : palette.text);
        button.setBackground(palette.interactive(primary ? palette.accent : palette.surface, dp(10)));
        return button;
    }

    private int dp(int value)
    {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }
}
