package com.lemonchat.protocol;

import com.lemonchat.model.AdminGroup;
import com.lemonchat.model.AdminSettings;
import com.lemonchat.model.AdminState;
import com.lemonchat.model.Member;
import com.lemonchat.model.PermissionDefinition;
import com.lemonchat.model.SessionSnapshot;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** serial administration requests on the authenticated session worker */
final class AdminController
{
    interface Host
    {
        void send(JSONObject message);
        long now();
        long lastSent();
        SessionSnapshot state();
    }

    private final Host host;
    private long connectionId;
    private boolean isAdmin;
    private boolean groupsEnabled;
    private boolean passwordChangeRequired;
    private boolean uncertainWrite;
    private long revision;
    private String status = "";
    private String error = "";
    private AdminSettings settings;
    private List<AdminGroup> groups = Collections.emptyList();
    private Map<String, Boolean> access = Collections.emptyMap();
    private Map<String, Integer> powers = Collections.emptyMap();
    private Pending pending;

    private static final class Pending
    {
        final String action;
        JSONObject body;
        final long deadline;
        boolean sent;
        boolean channelGroup;
        int groupId = -1;
        String name;
        PermissionDefinition permissions;
        final Set<Integer> existing = new HashSet<>();
        Member member;
        boolean add;
        AdminGroup original;
        String requiredAction;

        Pending(String action, JSONObject body, long deadline)
        {
            this.action = action;
            this.body = body;
            this.deadline = deadline;
        }
    }

    AdminController(Host host)
    {
        this.host = host;
    }

    void reset(long connection)
    {
        connectionId = connection;
        isAdmin = false;
        groupsEnabled = false;
        passwordChangeRequired = false;
        uncertainWrite = false;
        pending = null;
        settings = null;
        groups = Collections.emptyList();
        access = Collections.emptyMap();
        powers = Collections.emptyMap();
        status = "";
        error = "";
        revision++;
    }

    AdminState snapshot()
    {
        return new AdminState(connectionId, isAdmin, groupsEnabled, pending != null, passwordChangeRequired, status, error, revision, settings, groups, access, powers);
    }

    void problem(String text)
    {
        error = text;
        status = "";
        revision++;
    }

    void login(String password) throws JSONException
    {
        checkReady(false);
        password(password);
        if (isAdmin)
        {
            load();
            return;
        }
        queue("login", body("admin_password").put("value", password), "Confirming administrator access");
    }

    void load() throws JSONException
    {
        checkReady(false);
        requireAdmin();
        queue("load", body("load_server_settings").put("memory_only", false), "Loading server settings");
    }

    void save(AdminSettings original, Map<String, Object> edited, boolean includeMusic) throws JSONException
    {
        checkReady(true);
        requireAdmin();
        if (original == null || settings != original)
        {
            throw new IllegalArgumentException("Settings changed. Reload them before saving");
        }
        queue("settings", original.changes(edited, includeMusic), "Saving server settings");
    }

    void changePassword(String password) throws JSONException
    {
        checkReady(true);
        requireAdmin();
        password(password);
        queue("password", body("change_admin_password").put("value", password), "Changing the administrator password");
    }

    void createGroup(String name, String type, boolean channelGroup) throws JSONException
    {
        checkReady(true);
        name(name);
        requireGroups(channelGroup ? "create_channel_groups" : "create_groups");
        JSONObject request;
        if (channelGroup)
        {
            request = body("edit_channel_group").put("action", "create").put("name", name).put("icon_id", -1).put("permissions", new JSONObject());
        }
        else
        {
            if (!type.equals("normal") && !type.equals("guest") && !type.equals("moderator") && !type.equals("admin"))
            {
                throw new IllegalArgumentException("Choose an existing group preset");
            }
            request = body("server_settings_add_new_group").put("group_name", name).put("group_type", type);
        }
        queue("create", request, "Creating the group");
        pending.channelGroup = channelGroup;
        pending.name = name;
        for (AdminGroup group : groups)
        {
            if (group.channelGroup == channelGroup)
            {
                pending.existing.add(group.id);
            }
        }
    }

    void renameGroup(AdminGroup original, String name) throws JSONException
    {
        checkReady(true);
        name(name);
        AdminGroup group = current(original, "edit_groups");
        JSONObject request = group.channelGroup ? channelUpdate(group, name, group.permissions) : body("rename_group").put("group_id", group.id).put("group_name", name);
        queue("rename", request, "Renaming the group");
        pending.groupId = group.id;
        pending.channelGroup = group.channelGroup;
        pending.name = name;
        pending.original = group;
        pending.requiredAction = "edit_groups";
    }

    void deleteGroup(AdminGroup original) throws JSONException
    {
        checkReady(true);
        AdminGroup group = current(original, "delete_groups");
        if ((!group.channelGroup && (group.id == 0 || group.isDefaultGuest)) || (group.channelGroup && group.id <= 1))
        {
            throw new IllegalArgumentException("This built in group cannot be deleted");
        }
        JSONObject request = group.channelGroup ? body("edit_channel_group").put("action", "delete").put("group_id", group.id) : body("server_settings_delete_group").put("group_id", group.id);
        queue("delete", request, "Deleting the group");
        pending.groupId = group.id;
        pending.channelGroup = group.channelGroup;
        pending.original = group;
        pending.requiredAction = "delete_groups";
    }

    void permissions(AdminGroup original, PermissionDefinition edited) throws JSONException
    {
        checkReady(true);
        AdminGroup group = current(original, "edit_group_permissions");
        if (group.permissions == null || edited == null || (!group.channelGroup && group.id == 0))
        {
            throw new IllegalArgumentException("These permissions cannot be edited");
        }
        checkPermissionChanges(group, edited);
        JSONObject request = group.channelGroup ? channelUpdate(group, group.name, edited) : body("set_group_permissions").put("group_id", group.id).put("permissions", edited.toJson());
        queue("permissions", request, "Applying group permissions");
        pending.groupId = group.id;
        pending.channelGroup = group.channelGroup;
        pending.permissions = edited;
        pending.original = group;
        pending.requiredAction = "edit_group_permissions";
    }

    void assign(AdminGroup original, Member target, boolean add) throws JSONException
    {
        checkReady(true);
        AdminGroup group = current(original, add ? "assign_groups" : "remove_groups");
        Member member = currentMember(target);
        JSONObject request;
        if (group.channelGroup)
        {
            if (member.channelId < 0 || member.idle)
            {
                throw new IllegalArgumentException("The person must be in a visible channel");
            }
            request = body("moderate_client").put("client_id", member.id).put("identity", member.publicKey).put("channel_id", member.channelId).put("action", "channel_group").put("role", add ? group.id : 0);
        }
        else
        {
            request = body(add ? "add_group_to_client" : "remove_group_from_client").put("client_id", member.id).put("group_id", group.id);
        }
        queue("membership", request, "Changing group membership");
        pending.groupId = group.id;
        pending.channelGroup = group.channelGroup;
        pending.member = member;
        pending.add = add;
        pending.original = group;
        pending.requiredAction = add ? "assign_groups" : "remove_groups";
    }

    void saveGroups() throws JSONException
    {
        checkReady(true);
        if (!groupsEnabled || (!isAdmin && !canSaveGroups()))
        {
            throw new IllegalArgumentException("You cannot save server groups");
        }
        queue("persist", body("save_groups"), "Requesting a group save");
    }

    /** receives only authenticated bodies from the current connection */
    void control(String type, JSONObject message) throws JSONException
    {
        if (message.has("is_permission_system_enabled"))
        {
            groupsEnabled = message.getBoolean("is_permission_system_enabled");
            if (!groupsEnabled)
            {
                groups = Collections.emptyList();
                access = Collections.emptyMap();
                powers = Collections.emptyMap();
            }
            revision++;
        }
        switch (type)
        {
            case "admin_status":
                isAdmin = message.getBoolean("is_admin");
                if (!isAdmin)
                {
                    settings = null;
                    passwordChangeRequired = false;
                    if (pending != null && !pending.action.equals("login"))
                    {
                        finish(false, "Administrator access changed");
                    }
                }
                else if (waiting("login"))
                {
                    finish(true, "Administrator access confirmed");
                }
                revision++;
                break;
            case "force_admin_password_change":
                passwordChangeRequired = true;
                revision++;
                break;
            case "server_settings_values":
                if (isAdmin && !message.optBoolean("memory_only", false))
                {
                    settings = new AdminSettings(message);
                    revision++;
                    if (waiting("load"))
                    {
                        finish(true, "Server settings loaded");
                    }
                }
                break;
            case "server_settings_saved":
                if (waiting("settings"))
                {
                    boolean saved = message.getBoolean("success");
                    finish(saved, saved ? "Server settings saved" : "The server did not save these settings. Reload before trying again");
                    settings = null;
                }
                break;
            case "group_list":
                readGroups(message);
                matchCatalog();
                break;
            case "moderation_result":
                result(message);
                break;
            case "access_denied":
            case "permission_denied":
                if (pending != null && pending.sent)
                {
                    finish(false, message.optString("reason", "The server refused this change"));
                }
                break;
            default:
                break;
        }
    }

    void afterState() throws JSONException
    {
        if (!waiting("membership"))
        {
            return;
        }
        Pending operation = pending;
        Member member;
        try
        {
            member = currentMember(operation.member);
        }
        catch (IllegalArgumentException stale)
        {
            finish(false, "The selected person is no longer connected");
            return;
        }
        boolean matches = operation.channelGroup ? member.channelId == operation.member.channelId && member.channelGroupId == (operation.add ? operation.groupId : 0) : member.groupIds.contains(operation.groupId) == operation.add;
        if (matches)
        {
            accepted();
        }
    }

    boolean tick() throws JSONException
    {
        if (pending == null)
        {
            return false;
        }
        if (host.now() >= pending.deadline)
        {
            String action = pending.action;
            uncertainWrite = !action.equals("login") && !action.equals("load") && !action.equals("persist");
            finish(false, action.equals("login") ? "Administrator access was not confirmed. Check the password and try again" : uncertainWrite ? "No result arrived. Reconnect before making another change" : "No settings reply arrived. Try loading again");
            return true;
        }
        if (!pending.sent && host.now() - host.lastSent() >= 180)
        {
            try
            {
                if (pending.original != null)
                {
                    AdminGroup latest = current(pending.original, pending.requiredAction);
                    if (pending.permissions != null)
                    {
                        checkPermissionChanges(latest, pending.permissions);
                    }
                }
                else if (pending.action.equals("create"))
                {
                    requireGroups(pending.channelGroup ? "create_channel_groups" : "create_groups");
                }
                else if (pending.action.equals("persist") && (!groupsEnabled || !isAdmin && !canSaveGroups()))
                {
                    throw new IllegalArgumentException("Your access changed before the group save request");
                }
            }
            catch (IllegalArgumentException changed)
            {
                finish(false, changed.getMessage());
                return true;
            }
            if (pending.member != null)
            {
                try
                {
                    Member member = currentMember(pending.member);
                    if (pending.channelGroup && member.channelId != pending.member.channelId)
                    {
                        throw new IllegalArgumentException("The person changed channel. Select them again");
                    }
                }
                catch (IllegalArgumentException stale)
                {
                    finish(false, stale.getMessage());
                    return true;
                }
            }
            JSONObject request = pending.body;
            pending.body = null;
            pending.sent = true;
            host.send(request);
            if (pending != null && pending.action.equals("persist"))
            {
                finish(true, "Changes accepted. Save requested. The server does not confirm group persistence");
            }
            return true;
        }
        return false;
    }

    long nextDelay()
    {
        if (pending == null)
        {
            return -1;
        }
        return Math.max(1, pending.sent ? pending.deadline - host.now() : Math.min(pending.deadline - host.now(), host.lastSent() + 180 - host.now()));
    }

    private void result(JSONObject message) throws JSONException
    {
        if (pending == null || !pending.sent)
        {
            return;
        }
        String text = ProtocolJson.string(message, "text", 4096);
        boolean succeeded = message.getBoolean("succeeded");
        if (pending.action.equals("password") && (text.equals("Admin password changed.") || text.contains("admin password was not changed")))
        {
            if (succeeded)
            {
                passwordChangeRequired = false;
            }
            finish(succeeded, text);
        }
        else if (pending.channelGroup && !pending.action.equals("membership") && text.toLowerCase(java.util.Locale.ROOT).contains("channel group"))
        {
            if (!succeeded)
            {
                finish(false, text);
            }
        }
        else if (!succeeded && (pending.channelGroup || pending.action.equals("settings")))
        {
            problem(text);
        }
    }

    private void readGroups(JSONObject message) throws JSONException
    {
        JSONObject rights = message.getJSONObject("access");
        Map<String, Boolean> newAccess = booleans(rights);
        Map<String, Integer> newPowers = new LinkedHashMap<>();
        JSONObject powerValues = rights.optJSONObject("powers");
        if (powerValues != null)
        {
            if (powerValues.length() > 128)
            {
                throw new JSONException("Too many permission powers");
            }
            Iterator<String> names = powerValues.keys();
            while (names.hasNext())
            {
                String name = names.next();
                newPowers.put(name, (int)ProtocolJson.integer(powerValues, name, 0, 1000));
            }
        }
        ArrayList<AdminGroup> newGroups = new ArrayList<>();
        readCatalog(message.getJSONArray("groups"), false, newGroups, newAccess, newPowers);
        readCatalog(message.optJSONArray("channel_groups"), true, newGroups, newAccess, newPowers);
        groups = newGroups;
        access = newAccess;
        powers = newPowers;
        groupsEnabled = true;
        revision++;
    }

    private void readCatalog(JSONArray catalog, boolean channel, List<AdminGroup> result, Map<String, Boolean> global, Map<String, Integer> effective) throws JSONException
    {
        if (catalog == null)
        {
            return;
        }
        if (catalog.length() > (channel ? 128 : 1000))
        {
            throw new JSONException("Too many administration groups");
        }
        Set<Integer> ids = new HashSet<>();
        for (int i = 0; i < catalog.length(); i++)
        {
            JSONObject entry = catalog.getJSONObject(i);
            int id = (int)ProtocolJson.integer(entry, "group_id", 0, channel ? 127 : 999);
            if (!ids.add(id))
            {
                throw new JSONException("Duplicate administration group");
            }
            PermissionDefinition definition = entry.has("permissions") ? PermissionDefinition.fromJson(entry.getJSONObject("permissions")) : null;
            Map<String, Boolean> rights;
            if (channel)
            {
                rights = new LinkedHashMap<>();
                boolean manageable = isAdmin || definition != null && !hasFlags(definition) && definition.powers.getOrDefault("channel_group_protection", 0) <= effective.getOrDefault("group_management_power", 0);
                rights.put("edit_groups", id > 0 && manageable && Boolean.TRUE.equals(global.get("edit_channel_groups")));
                rights.put("edit_group_permissions", rights.get("edit_groups"));
                rights.put("delete_groups", id > 1 && manageable && Boolean.TRUE.equals(global.get("delete_channel_groups")));
                rights.put("assign_groups", Boolean.TRUE.equals(global.get("assign_channel_groups")));
                rights.put("remove_groups", rights.get("assign_groups"));
            }
            else
            {
                rights = booleans(entry.optJSONObject("access"));
            }
            String name = ProtocolJson.string(entry, channel ? "name" : "group_name", 256);
            String type = channel ? "channel" : ProtocolJson.string(entry, "group_type", 32);
            long icon = channel ? ProtocolJson.integer(entry, "icon_id", -2, 9007199254740991L) : entry.optBoolean("has_icon", false) ? ProtocolJson.integer(entry, "group_linked_icon_id", 0, 9007199254740991L) : -1;
            result.add(new AdminGroup(id, name, type, channel, entry.optBoolean("is_default_guest", false), icon, rights, definition));
        }
    }

    private void matchCatalog() throws JSONException
    {
        if (pending == null || !pending.sent)
        {
            return;
        }
        AdminGroup group = snapshot().group(pending.groupId, pending.channelGroup);
        boolean matches = false;
        switch (pending.action)
        {
            case "create":
                for (AdminGroup candidate : groups)
                {
                    if (candidate.channelGroup == pending.channelGroup && !pending.existing.contains(candidate.id) && candidate.name.equals(pending.name))
                    {
                        matches = true;
                        break;
                    }
                }
                break;
            case "delete":
                matches = group == null;
                break;
            case "rename":
                matches = group != null && group.name.equals(pending.name);
                break;
            case "permissions":
                matches = group != null && group.permissions != null && group.permissions.sameValues(pending.permissions);
                break;
            default:
                break;
        }
        if (matches)
        {
            accepted();
        }
    }

    private void accepted() throws JSONException
    {
        if (pending.channelGroup)
        {
            finish(true, pending.action.equals("membership") ? "Channel role updated" : "Channel group change accepted and saved");
        }
        else
        {
            queue("persist", body("save_groups"), "Changes accepted. Requesting a save");
        }
    }

    private AdminGroup current(AdminGroup original, String action)
    {
        AdminGroup current = original == null ? null : snapshot().group(original.id, original.channelGroup);
        if (!groupsEnabled || current == null || !current.name.equals(original.name) || current.iconId != original.iconId || (current.permissions == null) != (original.permissions == null) || (current.permissions != null && !current.permissions.sameValues(original.permissions)))
        {
            throw new IllegalArgumentException("This group changed. Open it again before editing");
        }
        if (!current.allows(action))
        {
            throw new IllegalArgumentException("The server does not allow this group action");
        }
        return current;
    }

    private Member currentMember(Member original)
    {
        if (original != null)
        {
            for (Member member : host.state().members)
            {
                if (member.id == original.id && !member.musicBot && member.publicKey.equals(original.publicKey))
                {
                    return member;
                }
            }
        }
        throw new IllegalArgumentException("The selected person is no longer connected");
    }

    private void checkPermissionChanges(AdminGroup group, PermissionDefinition edited)
    {
        if (!isAdmin && (hasFlags(edited) || hasFlags(group.permissions)))
        {
            throw new IllegalArgumentException("Only administrators can edit flagged permissions");
        }
        if (group.channelGroup)
        {
            for (PermissionDefinition.Flags flags : edited.flags.values())
            {
                if (flags.skip)
                {
                    throw new IllegalArgumentException("Channel roles do not support Skip");
                }
            }
        }
        if (!isAdmin)
        {
            int power = powers.getOrDefault("permission_edit_power", 0);
            Set<String> names = new HashSet<>(group.permissions.actions.keySet());
            names.addAll(group.permissions.powers.keySet());
            names.addAll(edited.actions.keySet());
            names.addAll(edited.powers.keySet());
            for (String name : names)
            {
                Object previous = group.permissions.actions.containsKey(name) ? group.permissions.actions.get(name) : group.permissions.powers.get(name);
                Object next = edited.actions.containsKey(name) ? edited.actions.get(name) : edited.powers.get(name);
                int oldGrant = group.permissions.grants.getOrDefault(name, 1000);
                int newGrant = edited.grants.getOrDefault(name, 1000);
                if ((!java.util.Objects.equals(previous, next) || oldGrant != newGrant) && (power < oldGrant || power < newGrant))
                {
                    throw new IllegalArgumentException("Your permission editing power is too low for " + name);
                }
            }
        }
    }

    private static boolean hasFlags(PermissionDefinition definition)
    {
        for (PermissionDefinition.Flags flags : definition.flags.values())
        {
            if (flags.skip || flags.negate)
            {
                return true;
            }
        }
        return false;
    }

    private static Map<String, Boolean> booleans(JSONObject source) throws JSONException
    {
        LinkedHashMap<String, Boolean> result = new LinkedHashMap<>();
        if (source == null)
        {
            return result;
        }
        if (source.length() > 132)
        {
            throw new JSONException("Too many access fields");
        }
        Iterator<String> keys = source.keys();
        while (keys.hasNext())
        {
            String name = keys.next();
            Object value = source.get(name);
            if (value instanceof Boolean && name.length() <= 100)
            {
                result.put(name, (Boolean)value);
            }
        }
        return result;
    }

    private JSONObject channelUpdate(AdminGroup group, String name, PermissionDefinition permissions) throws JSONException
    {
        if (permissions == null)
        {
            throw new IllegalArgumentException("The channel role's permissions are not visible");
        }
        return body("edit_channel_group").put("action", "update").put("group_id", group.id).put("name", name).put("icon_id", group.iconId).put("permissions", permissions.toJson());
    }

    private void requireGroups(String action)
    {
        if (!groupsEnabled || !Boolean.TRUE.equals(access.get(action)))
        {
            throw new IllegalArgumentException("The server does not allow this group action");
        }
    }

    private boolean canSaveGroups()
    {
        for (String name : new String[] { "create_groups", "edit_groups", "delete_groups", "assign_groups", "remove_groups", "edit_group_permissions", "upload_icons", "delete_icons" })
        {
            if (Boolean.TRUE.equals(access.get(name)))
            {
                return true;
            }
        }
        return false;
    }

    private void requireAdmin()
    {
        if (!isAdmin)
        {
            throw new IllegalArgumentException("Enter the administrator password first");
        }
    }

    private void checkReady(boolean mutation)
    {
        if (pending != null)
        {
            throw new IllegalArgumentException("Wait for the current administration request");
        }
        if (mutation && uncertainWrite)
        {
            throw new IllegalArgumentException("Reconnect before making another change because the previous result is unknown");
        }
    }

    private static void name(String name)
    {
        if (name == null || name.trim().isEmpty() || name.indexOf('\0') >= 0 || name.getBytes(StandardCharsets.UTF_8).length >= 32)
        {
            throw new IllegalArgumentException("Use a group name from 1 to 31 UTF-8 bytes");
        }
    }

    private static void password(String password)
    {
        if (password == null || password.isEmpty() || password.indexOf('\0') >= 0 || password.getBytes(StandardCharsets.UTF_8).length > 50)
        {
            throw new IllegalArgumentException("Use a password from 1 to 50 UTF-8 bytes");
        }
    }

    private static JSONObject body(String type) throws JSONException
    {
        return new JSONObject().put("type", type);
    }

    private void queue(String action, JSONObject request, String text)
    {
        pending = new Pending(action, request, host.now() + 15000);
        status = text;
        error = "";
        revision++;
    }

    private boolean waiting(String action)
    {
        return pending != null && pending.sent && pending.action.equals(action);
    }

    private void finish(boolean success, String text)
    {
        pending = null;
        status = success ? text : "";
        error = success ? "" : text;
        revision++;
    }
}
