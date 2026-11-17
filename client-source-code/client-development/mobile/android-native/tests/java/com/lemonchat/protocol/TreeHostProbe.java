package com.lemonchat.protocol;

import com.lemonchat.model.Channel;
import com.lemonchat.model.ChannelTree;
import com.lemonchat.model.Group;
import com.lemonchat.model.Member;
import com.lemonchat.model.SessionSnapshot;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import org.json.JSONArray;
import org.json.JSONObject;

/** checks tree bounds and the server group messages used by its rows */
public final class TreeHostProbe
{
    private static int checks;

    public static void main(String[] arguments) throws Exception
    {
        hierarchy();
        audioStates();
        groups();
        icons();
        System.out.println("PASS: " + checks + " channel tree and group checks");
    }

    private static void hierarchy()
    {
        List<Channel> channels = Arrays.asList(channel(0, -1), channel(1, 0), channel(2, 1));
        List<Member> people = Arrays.asList(member(1, 0, false, 2), member(2, 2, false, 3), member(3, 1, true, 1));
        SessionSnapshot snapshot = state(channels, people);
        List<ChannelTree.Row> rows = ChannelTree.rows(snapshot, Collections.emptySet());
        check(rows.size() == 6, "every channel and member is represented");
        check(rows.get(0).channel.id == 0 && rows.get(1).member.id == 1 && rows.get(2).channel.id == 1 && rows.get(3).member.id == 3 && rows.get(4).channel.id == 2, "members appear inside their own nested channels");
        check(rows.get(2).people == 0 && rows.get(2).musicBots == 1, "music bots are counted separately");
        check(rows.get(4).depth == 2 && rows.get(5).depth == 3, "nested depth reaches member rows");
        rows = ChannelTree.rows(snapshot, Collections.singleton(1));
        check(rows.size() == 3 && rows.get(2).channel.id == 1, "collapsed descendants do not reappear as roots");
        ArrayList<Channel> deep = new ArrayList<>();
        for (int id = 0; id < 10000; id++)
        {
            deep.add(channel(id, id - 1));
        }
        check(ChannelTree.rows(state(deep, Collections.emptyList()), Collections.emptySet()).size() == 10000, "deep trees do not recurse on the Java stack");
        check(ChannelTree.rows(state(deep, Collections.emptyList()), Collections.singleton(0)).size() == 1, "large collapsed branches stay hidden");
        rows = ChannelTree.rows(state(Arrays.asList(channel(1, 2), channel(2, 1)), Collections.emptyList()), Collections.emptySet());
        check(rows.size() == 2, "cyclic parent data cannot loop or duplicate rows");
        rows = ChannelTree.rows(state(Collections.singletonList(channel(1, 99)), Collections.emptyList()), Collections.emptySet());
        check(rows.size() == 1 && rows.get(0).depth == 0, "missing parents retain the known channel");
        rows = ChannelTree.rows(state(Collections.emptyList(), Collections.singletonList(member(1, -1, false, 4))), Collections.emptySet());
        check(rows.size() == 2 && rows.get(0).channel == null && rows.get(0).member == null, "concealed membership gets no invented channel");
        check(rows.get(0).stableId() != rows.get(1).stableId(), "section and member ids are distinct");
    }

    private static void audioStates()
    {
        check(ChannelTree.audioDescription(member(1, 0, false, 1)).equals("Speaking"), "active transmission is identified");
        check(ChannelTree.audioDescription(member(1, 0, false, 2)).equals("Push to talk ready"), "enabled push to talk does not claim active speech");
        check(ChannelTree.audioDescription(member(1, 0, false, 3)).equals("Microphone off"), "microphone disabled retains reception");
        check(ChannelTree.audioDescription(member(1, 0, false, 4)).equals("Audio off"), "all audio disabled is distinct");
        check(ChannelTree.audioDescription(member(1, 0, true, 1)).equals("Music bot"), "bot audio state does not invent playback status");
        check(ChannelTree.audioDescription(member(1, 0, false, 1).withPresence(0, true, 1)).equals("Idle"), "idle clients are not shown speaking");
    }

    private static void groups() throws Exception
    {
        try (ChatSession session = new ChatSession(Runnable::run))
        {
            JSONObject catalog = new JSONObject().put("groups", new JSONArray().put(serverGroup(2, "Members"))).put("channel_groups", new JSONArray().put(new JSONObject().put("group_id", 1).put("name", "Channel Admin").put("icon_id", -2)));
            dispatch(session, "group_list", catalog);
            check(session.state().groups.size() == 2, "server and channel group catalogs are retained");
            JSONObject person = new JSONObject().put("client_id", 7).put("channel_id", 0).put("username", "Maya").put("public_key", "test-public-key").put("audio_state", 2).put("group_ids", new JSONArray().put(2)).put("channel_group_id", 1);
            dispatch(session, "client_connect", person);
            Member maya = session.state().members.get(0);
            check(maya.groupIds.equals(Collections.singletonList(2)) && maya.channelGroupId == 1, "initial member groups are parsed");
            boolean immutable = false;
            try
            {
                maya.groupIds.add(3);
            }
            catch (UnsupportedOperationException expected)
            {
                immutable = true;
            }
            check(immutable, "published membership is immutable");
            dispatch(session, "group_add", serverGroup(3, "Moderators"));
            dispatch(session, "group_add_to_client", new JSONObject().put("client_id", 7).put("group_id", 3));
            check(session.state().members.get(0).groupIds.equals(Arrays.asList(2, 3)), "live server group assignment is applied");
            dispatch(session, "group_add_to_client", new JSONObject().put("client_id", 7).put("group_id", 3));
            check(session.state().members.get(0).groupIds.size() == 2, "repeated assignments are idempotent");
            dispatch(session, "group_icon_changed", new JSONObject().put("group_id", 3).put("has_icon", true).put("group_linked_icon_id", 8));
            check(group(session, 3).iconId == 8, "live group icon changes are retained");
            dispatch(session, "icon_delete", new JSONObject().put("icon_id", 8));
            check(group(session, 3).iconId == -1, "deleted icon assignments are cleared");
            dispatch(session, "client_rename", new JSONObject().put("client_id", 7).put("new_username", "Maya R"));
            check(session.state().members.get(0).groupIds.size() == 2 && session.state().members.get(0).channelGroupId == 1, "rename preserves group membership");
            dispatch(session, "audio_state_of_single_client", new JSONObject().put("client_id", 7).put("value", 4));
            check(session.state().members.get(0).audioState == 4 && session.state().members.get(0).channelGroupId == 1, "audio updates preserve channel roles");
            dispatch(session, "channel_group_changed", new JSONObject().put("client_id", 7).put("channel_id", 5).put("channel_group_id", 2));
            check(session.state().members.get(0).channelGroupId == 1, "stale channel role events do not change another channel");
            dispatch(session, "channel_join", new JSONObject().put("client_id", 7).put("channel_id", 5).put("channel_group_id", 2).put("audio_state", 2));
            check(session.state().members.get(0).channelId == 5 && session.state().members.get(0).channelGroupId == 2, "channel changes install their own role");
            dispatch(session, "client_going_to_idle_mode", new JSONObject().put("client_id", 7));
            check(session.state().members.get(0).channelGroupId == 0, "idle membership clears the visible channel role");
            dispatch(session, "remove_group_from_client", new JSONObject().put("client_id", 7).put("group_id", 2));
            check(session.state().members.get(0).groupIds.equals(Collections.singletonList(3)), "live membership removal is applied");
            dispatch(session, "group_delete", new JSONObject().put("group_id", 3));
            check(session.state().members.get(0).groupIds.isEmpty() && group(session, 3) == null, "deleting a group clears its memberships");
            boolean rejected = false;
            try
            {
                dispatch(session, "client_connect", new JSONObject(person.toString()).put("group_ids", new JSONArray().put("2")));
            }
            catch (Exception expected)
            {
                rejected = true;
            }
            check(rejected, "string group identifiers are rejected");
            rejected = false;
            try
            {
                dispatch(session, "group_list", new JSONObject().put("groups", new JSONArray().put(serverGroup(2, "A")).put(serverGroup(2, "B"))));
            }
            catch (Exception expected)
            {
                rejected = true;
            }
            check(rejected && group(session, 2).name.equals("Members"), "invalid catalogs do not partially replace state");
        }
    }

    private static void dispatch(ChatSession session, String type, JSONObject value) throws Exception
    {
        Method dispatch = ChatSession.class.getDeclaredMethod("dispatch", String.class, JSONObject.class);
        dispatch.setAccessible(true);
        FutureTask<Void> action = new FutureTask<>(() ->
        {
            dispatch.invoke(session, type, value.put("type", type));
            return null;
        });
        session.executor().execute(action);
        action.get(10, TimeUnit.SECONDS);
    }

    private static void icons() throws Exception
    {
        String image = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/lXcAAAAASUVORK5CYII=";
        try (ChatSession session = new ChatSession(Runnable::run))
        {
            dispatch(session, "icon_list", new JSONObject().put("icons", new JSONArray().put(new JSONObject().put("icon_id", 4).put("base64_icon", image))));
            check(session.state().icons.size() == 1 && session.state().icons.get(0).data.equals(image), "real server icon data URI is preserved");
            dispatch(session, "channel_create", new JSONObject().put("channel_id", 0).put("parent_channel_id", -1).put("channel_name", "General").put("has_channel_icon", true).put("channel_icon_id", 4));
            check(session.state().channels.get(0).iconId == 4, "channel icon assignment is parsed");
            dispatch(session, "channel_icon_changed", new JSONObject().put("channel_id", 0).put("has_channel_icon", false));
            check(session.state().channels.get(0).iconId == -1, "channel icon removal updates the immutable channel");
            dispatch(session, "channel_icon_changed", new JSONObject().put("channel_id", 0).put("has_channel_icon", true).put("channel_icon_id", 4));
            dispatch(session, "icon_delete", new JSONObject().put("icon_id", 4));
            check(session.state().icons.isEmpty() && session.state().channels.get(0).iconId == -1, "deleted icons leave no channel assignment or payload");
            dispatch(session, "icon_add", new JSONObject().put("icon_id", 4).put("base64_icon", "https://example.invalid/icon.png"));
            dispatch(session, "icon_add", new JSONObject().put("icon_id", 5).put("base64_icon", "data:image/png;base64," + "A".repeat(40960)));
            check(session.state().icons.isEmpty(), "remote URL and oversized icon payloads are not retained");
            JSONArray many = new JSONArray();
            for (int index = 0; index < 60; index++)
            {
                many.put(new JSONObject().put("icon_id", index).put("base64_icon", "data:image/png;base64," + "A".repeat(40000)));
            }
            dispatch(session, "icon_list", new JSONObject().put("icons", many));
            int total = session.state().icons.stream().mapToInt(icon -> icon.data.length()).sum();
            check(total <= 2 * 1024 * 1024 && session.state().icons.size() < 60, "icon catalog obeys the aggregate memory limit");
            boolean immutable = false;
            try
            {
                session.state().icons.clear();
            }
            catch (UnsupportedOperationException expected)
            {
                immutable = true;
            }
            check(immutable, "icon snapshots cannot be changed by a view");
        }
    }

    private static JSONObject serverGroup(int id, String name) throws Exception
    {
        return new JSONObject().put("group_id", id).put("group_name", name).put("group_type", "normal").put("has_icon", false).put("group_linked_icon_id", 0);
    }

    private static Group group(ChatSession session, int id)
    {
        return session.state().groups.stream().filter(value -> !value.channelGroup && value.id == id).findFirst().orElse(null);
    }

    private static Channel channel(int id, int parent)
    {
        return new Channel(id, parent, "Channel " + id, "", false, true);
    }

    private static Member member(int id, int channel, boolean bot, int audio)
    {
        return new Member(id, channel, "Person " + id, "", "key-" + id, bot, false, audio, 0, "");
    }

    private static SessionSnapshot state(List<Channel> channels, List<Member> members)
    {
        return new SessionSnapshot(SessionSnapshot.Status.CONNECTED, "", 1, 0, "", true, channels, members, 1);
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
