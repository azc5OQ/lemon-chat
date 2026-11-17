package com.lemonchat.protocol;

import com.lemonchat.crypto.Identity;
import com.lemonchat.model.AdminGroup;
import com.lemonchat.model.AdminSettings;
import com.lemonchat.model.AdminState;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.model.Member;
import com.lemonchat.model.PermissionDefinition;
import com.lemonchat.model.SessionSnapshot;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.BooleanSupplier;
import org.json.JSONArray;
import org.json.JSONObject;

/** administration state checks and optional disposable server round trips */
public final class AdminHostChecks
{
    private static int passed;

    public static void main(String[] arguments) throws Exception
    {
        modelChecks();
        controllerChecks();
        if (arguments.length == 1)
        {
            live(Path.of(arguments[0]));
        }
        System.out.println("PASS " + passed + " administration checks");
    }

    private static void modelChecks() throws Exception
    {
        PermissionDefinition definition = PermissionDefinition.fromJson(new JSONObject("{\"kick\":\"grant\",\"kick_power\":50,\"grants\":{\"kick\":30},\"flags\":{\"kick\":{\"negate\":true}}}"));
        check(definition.sameValues(PermissionDefinition.fromJson(definition.toJson())), "permissions preserve actions, powers, grants and flags");
        rejects(() -> definition.actions.put("kick", "deny"), "permission snapshots are immutable");
        rejects(() -> PermissionDefinition.fromJson(new JSONObject("{\"kick\":true}")), "boolean permission rejected");
        rejects(() -> PermissionDefinition.fromJson(new JSONObject("{\"kick_power\":1001}")), "excessive power rejected");
        rejects(() -> PermissionDefinition.fromJson(new JSONObject("{\"kick_power\":0.5}")), "fractional power rejected");
        rejects(() -> PermissionDefinition.fromJson(new JSONObject("{\"kick\":\"allow\"}")), "unknown action value rejected");
        rejects(() -> PermissionDefinition.fromJson(new JSONObject("{\"kick\":\"grant\",\"flags\":{\"kick\":{\"skip\":1}}}")), "numeric permission flag rejected");
        rejects(() -> PermissionDefinition.fromJson(new JSONObject("{\"kick\":\"grant\",\"grants\":{\"missing\":2}}")), "unknown grant field rejected");
        AdminSettings settings = new AdminSettings(settings());
        Map<String, Object> edited = new LinkedHashMap<>(settings.values);
        edited.put("max_channel_count", 120);
        JSONObject change = settings.changes(edited, false);
        check(change.length() == 3 && change.getInt("max_channel_count") == 120 && !change.has("connection_password_count"), "settings send only changed editable fields");
        edited.put("reserved_slots", 20);
        rejects(() -> settings.changes(edited, false), "reserved slots cannot consume every slot");
        edited.remove("reserved_slots");
        edited.put("max_channel_count", 0);
        rejects(() -> settings.changes(edited, false), "zero channel capacity rejected before send");
        edited.put("max_channel_count", 3.5);
        rejects(() -> settings.changes(edited, false), "fractional channel capacity rejected before send");
    }

    private static void controllerChecks() throws Exception
    {
        Host host = new Host();
        AdminController controller = new AdminController(host);
        controller.reset(7);
        rejects(controller::load, "general settings require confirmed administrator access");
        controller.login("test password");
        check(controller.snapshot().busy && host.sent.isEmpty(), "password request waits for the session rate limit");
        controller.tick();
        check(host.sent.size() == 1 && host.sent.get(0).optString("type").equals("admin_password"), "login uses the actual administrator request");
        controller.control("admin_status", new JSONObject().put("is_admin", true));
        check(controller.snapshot().isAdmin && !controller.snapshot().busy, "administrator reply confirms access");
        controller.control("force_admin_password_change", new JSONObject());
        check(controller.snapshot().passwordChangeRequired, "forced password replacement is retained");
        controller.changePassword("replacement");
        send(host, controller);
        controller.control("moderation_result", new JSONObject().put("succeeded", true).put("text", "Another action finished."));
        check(controller.snapshot().busy, "unrelated moderation result cannot confirm a password change");
        controller.control("moderation_result", new JSONObject().put("succeeded", true).put("text", "Admin password changed."));
        check(!controller.snapshot().busy && !controller.snapshot().passwordChangeRequired, "actual password acknowledgement ends replacement");
        controller.load();
        send(host, controller);
        controller.control("server_settings_values", settings().put("memory_only", true));
        check(controller.snapshot().busy && controller.snapshot().settings == null, "usage reply cannot replace editable settings");
        controller.control("server_settings_values", settings());
        check(controller.snapshot().settings != null && !controller.snapshot().busy, "full settings reply completes loading");
        AdminSettings original = controller.snapshot().settings;
        controller.save(original, Collections.singletonMap("max_channel_count", 120), false);
        send(host, controller);
        controller.control("server_policy", new JSONObject().put("is_permission_system_enabled", true));
        check(controller.snapshot().busy, "policy broadcast does not confirm settings persistence");
        controller.control("server_settings_saved", new JSONObject().put("success", false));
        check(!controller.snapshot().busy && !controller.snapshot().error.isEmpty(), "failed settings save remains a failure");
        controller.control("group_list", catalog("Helpers", "grant"));
        AdminGroup before = controller.snapshot().group(2, false);
        controller.renameGroup(before, "New helpers");
        send(host, controller);
        controller.control("group_list", catalog("Helpers", "grant"));
        check(controller.snapshot().busy, "unrelated catalog does not acknowledge a rename");
        controller.control("group_list", catalog("New helpers", "grant"));
        send(host, controller);
        check(!controller.snapshot().busy && controller.snapshot().statusText.contains("Save requested") && host.sent.get(host.sent.size() - 1).getString("type").equals("save_groups"), "matching server group change requests persistence without claiming its confirmation");
        rejects(() -> controller.renameGroup(before, "Other"), "stale permission baseline rejected");
        controller.reset(8);
        check(!controller.snapshot().isAdmin && controller.snapshot().groups.isEmpty() && controller.snapshot().settings == null, "connection replacement clears all administrator state");
        controller.control("group_list", catalog("Helpers", "grant"));
        check(!controller.snapshot().isAdmin && controller.snapshot().group(2, false).allows("edit_groups"), "delegated rights do not require administrator login");
        controller.renameGroup(controller.snapshot().group(2, false), "Delegated edit");
        send(host, controller);
        check(host.sent.get(host.sent.size() - 1).getString("type").equals("rename_group"), "delegated manager can submit permitted edits");
        host.time += 16000;
        controller.tick();
        check(!controller.snapshot().busy && controller.snapshot().error.contains("Reconnect"), "unknown mutation result requires a new connection");
        rejects(() -> controller.renameGroup(controller.snapshot().group(2, false), "Retry"), "late acknowledgements cannot be reused for another mutation");
        controller.reset(9);
        controller.control("group_list", catalog("Helpers", "grant"));
        controller.renameGroup(controller.snapshot().group(2, false), "Queued edit");
        controller.control("group_list", catalog("Changed elsewhere", "grant"));
        int beforeStale = host.sent.size();
        send(host, controller);
        check(host.sent.size() == beforeStale && controller.snapshot().error.contains("changed"), "remote catalog change cancels a queued edit before sending");
        controller.reset(9);
        controller.login("wrong");
        send(host, controller);
        host.time += 16000;
        controller.tick();
        check(controller.snapshot().error.contains("not confirmed") && !controller.snapshot().isAdmin, "missing password reply does not invent a rejection reason");
        controller.login("retry");
        check(controller.snapshot().busy, "password timeout permits another deliberate attempt");
        controller.reset(10);
        controller.control("group_list", catalog("Helpers", "grant"));
        Member selected = member("first");
        host.members = Collections.singletonList(selected);
        controller.assign(controller.snapshot().group(2, false), selected, true);
        host.members = Collections.singletonList(member("replacement"));
        int count = host.sent.size();
        send(host, controller);
        check(host.sent.size() == count && !controller.snapshot().error.isEmpty(), "queued membership action rejects a reused client slot");
    }

    private static void live(Path credentials) throws Exception
    {
        JSONObject fixture = new JSONObject(Files.readString(credentials));
        try (ChatSession session = new ChatSession(Runnable::run))
        {
            session.setListener(new ChatSession.Listener()
            {
                @Override
                public void onState(SessionSnapshot state)
                {
                }

                @Override
                public void onMessage(ChatMessage message)
                {
                }

                @Override
                public void onError(String message)
                {
                    System.out.println("SESSION " + message);
                }
            });
            session.connect(fixture.getString("endpoint"), "Native admin check", Identity.create(2048), "");
            await(() -> session.state().connected && !session.state().admin.groups.isEmpty(), "real login receives bounded group definitions and access");
            long connection = session.state().connectionId;
            check(session.adminLogin(connection, fixture.getString("admin_password")), "real administrator login queued");
            await(() -> session.state().admin.isAdmin && !session.state().admin.busy, "real administrator status confirms login");
            session.loadAdministration(connection);
            await(() -> session.state().admin.settings != null && !session.state().admin.busy, "real general settings load");
            AdminSettings original = session.state().admin.settings;
            boolean previous = (Boolean)original.values.get("is_typing_indicator_allowed");
            session.saveServerSettings(connection, original, Collections.singletonMap("is_typing_indicator_allowed", !previous), false);
            await(() -> session.state().admin.statusText.equals("Server settings saved"), "real settings save acknowledgement");
            session.loadAdministration(connection);
            await(() -> session.state().admin.settings != null && !session.state().admin.busy, "reload edited server setting");
            check(Boolean.valueOf(!previous).equals(session.state().admin.settings.values.get("is_typing_indicator_allowed")), "edited setting matches actual server reply");
            session.saveServerSettings(connection, session.state().admin.settings, Collections.singletonMap("is_typing_indicator_allowed", previous), false);
            await(() -> session.state().admin.statusText.equals("Server settings saved"), "original fixture setting restored");
            String groupName = "Native check " + Long.toHexString(System.nanoTime()).substring(0, 8);
            session.createAdminGroup(connection, groupName, "normal", false);
            await(() -> !session.state().admin.busy && find(session.state().admin, groupName, false) != null, "real server group creation accepted");
            AdminGroup group = find(session.state().admin, groupName, false);
            check(group.permissions.actions.size() == 58 && group.permissions.powers.size() == 26, "actual current server permission catalog has 58 actions and 26 powers");
            Map<String, String> actions = new LinkedHashMap<>(group.permissions.actions);
            actions.put("send_pokes", "deny");
            PermissionDefinition edited = new PermissionDefinition(actions, group.permissions.powers, group.permissions.grants, group.permissions.flags);
            session.saveAdminPermissions(connection, group, edited);
            await(() -> !session.state().admin.busy && session.state().admin.group(group.id, false).permissions.sameValues(edited), "real permission replacement accepted");
            session.renameAdminGroup(connection, session.state().admin.group(group.id, false), groupName + "x");
            await(() -> !session.state().admin.busy && session.state().admin.group(group.id, false).name.endsWith("x"), "real group rename accepted");
            session.deleteAdminGroup(connection, session.state().admin.group(group.id, false));
            await(() -> !session.state().admin.busy && session.state().admin.group(group.id, false) == null, "temporary server group deleted");
            session.createAdminGroup(connection, groupName, "normal", true);
            await(() -> !session.state().admin.busy && find(session.state().admin, groupName, true) != null, "real channel role creation saved");
            AdminGroup role = find(session.state().admin, groupName, true);
            check(role.permissions.actions.size() == 11 && role.permissions.powers.size() == 12, "actual channel role catalog has 11 actions and 12 powers");
            session.deleteAdminGroup(connection, role);
            await(() -> !session.state().admin.busy && session.state().admin.group(role.id, true) == null, "temporary channel role deleted");
            session.disconnect();
            await(() -> !session.state().connected && !session.state().admin.isAdmin && session.state().admin.groups.isEmpty(), "real disconnect removes administrator access and cached settings");
            check(!session.adminLogin(connection, "ignored"), "disconnected administration call refused");
        }
    }

    private static AdminGroup find(AdminState state, String name, boolean channel)
    {
        for (AdminGroup group : state.groups)
        {
            if (group.channelGroup == channel && group.name.equals(name))
            {
                return group;
            }
        }
        return null;
    }

    private static void await(BooleanSupplier condition, String message) throws Exception
    {
        long deadline = System.nanoTime() + 30_000_000_000L;
        while (!condition.getAsBoolean() && System.nanoTime() < deadline)
        {
            Thread.sleep(25);
        }
        check(condition.getAsBoolean(), message);
    }

    private static JSONObject settings() throws Exception
    {
        return new JSONObject().put("max_slots", 20).put("used_slots", 3).put("max_channel_count", 100).put("reserved_slots", 2).put("connection_password_count", 3).put("is_typing_indicator_allowed", true);
    }

    private static JSONObject catalog(String name, String action) throws Exception
    {
        JSONObject rights = new JSONObject().put("edit_groups", true).put("delete_groups", true).put("edit_group_permissions", true).put("assign_groups", true).put("remove_groups", true);
        JSONObject group = new JSONObject().put("group_id", 2).put("group_name", name).put("group_type", "normal").put("has_icon", false).put("access", rights).put("permissions", new JSONObject().put("send_pokes", action));
        return new JSONObject().put("groups", new JSONArray().put(group)).put("channel_groups", new JSONArray()).put("access", new JSONObject(rights.toString()).put("powers", new JSONObject().put("permission_edit_power", 1000)));
    }

    private static Member member(String key)
    {
        return new Member(3, 0, "Person", "", key, false, false, 3, 0, "");
    }

    private static void send(Host host, AdminController controller) throws Exception
    {
        host.time += 200;
        controller.tick();
    }

    private static void check(boolean value, String text)
    {
        if (!value)
        {
            throw new AssertionError(text);
        }
        passed++;
        System.out.println("PASS " + text);
    }

    private interface Action
    {
        void run() throws Exception;
    }

    private static void rejects(Action action, String text) throws Exception
    {
        boolean rejected = false;
        try
        {
            action.run();
        }
        catch (Exception expected)
        {
            rejected = true;
        }
        check(rejected, text);
    }

    private static final class Host implements AdminController.Host
    {
        long time = 1000;
        long sentAt;
        List<Member> members = Collections.emptyList();
        final List<JSONObject> sent = new ArrayList<>();

        @Override
        public void send(JSONObject message)
        {
            sent.add(message);
            sentAt = time;
        }

        @Override
        public long now()
        {
            return time;
        }

        @Override
        public long lastSent()
        {
            return sentAt;
        }

        @Override
        public SessionSnapshot state()
        {
            return new SessionSnapshot(SessionSnapshot.Status.CONNECTED, "", 1, 0, "Admin", false, Collections.emptyList(), members, 7);
        }
    }
}
