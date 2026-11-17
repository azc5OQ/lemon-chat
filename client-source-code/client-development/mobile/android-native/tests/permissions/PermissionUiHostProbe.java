package com.lemonchat.ui;

import com.lemonchat.model.PermissionDefinition;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;

/** checks isolated permission drafts without Android or a live server */
public final class PermissionUiHostProbe
{
    private static int checks;

    public static void main(String[] arguments) throws Exception
    {
        Map<String, String> actions = new LinkedHashMap<>();
        actions.put("send_channel_messages", "grant");
        actions.put("edit_channel_name", "deny");
        Map<String, Integer> powers = new LinkedHashMap<>();
        powers.put("talk_power", 25);
        Map<String, Integer> grants = new LinkedHashMap<>();
        grants.put("talk_power", 50);
        Map<String, PermissionDefinition.Flags> flags = new LinkedHashMap<>();
        flags.put("edit_channel_name", new PermissionDefinition.Flags(true, true));
        PermissionDefinition original = new PermissionDefinition(actions, powers, grants, flags);
        PermissionDraft draft = new PermissionDraft(original);
        check(!draft.changed(), "a fresh draft preserves the received definition");
        PermissionFields.Field talk = field("talk_power", false);
        draft.update(talk, "64", 75, new PermissionDefinition.Flags(true, false));
        PermissionDefinition updated = draft.value();
        check(original.powers.get("talk_power") == 25 && original.grants.get("talk_power") == 50, "editing never changes the service snapshot");
        check(updated.powers.get("talk_power") == 64 && updated.grants.get("talk_power") == 75, "the power and its edit threshold change together");
        check(updated.actions.equals(original.actions) && updated.flags.get("edit_channel_name").negate, "other actions and flags survive a focused edit");
        check(draft.changed(), "the draft reports an actual edit");
        check(PermissionDefinition.fromJson(updated.toJson()).sameValues(updated), "the complete draft survives wire serialization");
        draft.update(talk, "25", 50, new PermissionDefinition.Flags(false, false));
        check(!draft.changed(), "restoring the original values removes the dirty state");
        for (String value : new String[]{"", "-1", "1.5", "1001", "99999999999", " 50", "50 "})
        {
            reject(() -> draft.update(talk, value, 50, new PermissionDefinition.Flags(false, false)));
            check(!draft.changed(), "invalid input leaves the draft unchanged");
        }
        for (String value : new String[]{"0", "1000"})
        {
            draft.update(talk, value, Integer.parseInt(value), new PermissionDefinition.Flags(false, true));
            check(draft.value().powers.get("talk_power") == Integer.parseInt(value), "power boundaries remain valid");
        }
        PermissionDraft actionDraft = new PermissionDraft(original);
        PermissionFields.Field messages = field("send_channel_messages", false);
        reject(() -> actionDraft.update(messages, "grant", 1000, new PermissionDefinition.Flags(true, false)));
        reject(() -> actionDraft.update(messages, "unknown", 1000, new PermissionDefinition.Flags(false, false)));
        reject(() -> actionDraft.update(messages, "deny", -1, new PermissionDefinition.Flags(false, false)));
        reject(() -> actionDraft.update(messages, "deny", 1001, new PermissionDefinition.Flags(false, false)));
        actionDraft.update(messages, "deny", 1000, new PermissionDefinition.Flags(false, true));
        check(actionDraft.value().actions.get(messages.name).equals("deny") && actionDraft.value().flags.get(messages.name).negate, "Disable with Negate is retained explicitly");
        PermissionDraft absent = new PermissionDraft(new PermissionDefinition(Collections.emptyMap(), Collections.emptyMap(), Collections.emptyMap(), Collections.emptyMap()));
        reject(() -> absent.update(messages, "grant", 1000, new PermissionDefinition.Flags(false, false)));
        PermissionDefinition channelValues = new PermissionDefinition(Collections.singletonMap("edit_channel_name", "deny"), Collections.emptyMap(), Collections.emptyMap(), Collections.emptyMap());
        PermissionDraft channelDraft = new PermissionDraft(channelValues);
        reject(() -> channelDraft.update(field("edit_channel_name", true), "default", 1000, new PermissionDefinition.Flags(false, false)));
        check(!channelDraft.changed(), "channel roles retain their explicit grant or deny value");
        check(field("direct_message_power", false).matches("DIRECT message"), "search handles case and readable labels");
        check(field("view_channel_members", false).matches("own channel"), "search includes the source explanation");
        check(!field("delete_channel", true).skip, "channel roles never offer server Skip");
        check(PermissionFields.SERVER.size() == 84 && PermissionFields.CHANNEL.size() == 23, "both current catalogs are complete");
        System.out.println("PASS " + checks + " permission UI host checks");
    }

    private static PermissionFields.Field field(String name, boolean channel)
    {
        for (PermissionFields.Field field : PermissionFields.forGroup(channel))
        {
            if (field.name.equals(name))
            {
                return field;
            }
        }
        throw new AssertionError("Missing permission " + name);
    }

    private static void reject(Runnable action)
    {
        boolean refused = false;
        try
        {
            action.run();
        }
        catch (IllegalArgumentException expected)
        {
            refused = true;
        }
        check(refused, "invalid edits are refused");
    }

    private static void check(boolean value, String description)
    {
        if (!value)
        {
            throw new AssertionError(description);
        }
        checks++;
    }
}
