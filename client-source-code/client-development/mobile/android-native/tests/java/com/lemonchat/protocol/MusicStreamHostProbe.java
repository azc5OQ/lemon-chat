package com.lemonchat.protocol;

import com.lemonchat.model.Member;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.Collections;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import org.json.JSONArray;
import org.json.JSONObject;

/** checks stream titles from actual server message bodies */
public final class MusicStreamHostProbe
{
    private static int checks;

    public static void main(String[] arguments) throws Exception
    {
        try (ChatSession session = new ChatSession(Runnable::run))
        {
            run(session, () ->
            {
                set(session, "publicKey", "local-key");
                set(session, "authenticationPolicy", new JSONObject().put("is_music_bot_marquee_visible_to_everyone", true));
            });
            JSONObject local = person(1, 0, false).put("public_key", "local-key");
            JSONObject bot = person(2, 3, true).put("is_streaming_song", true).put("song_name", "Already playing.mp3");
            dispatch(session, "client_list", new JSONObject().put("clients", new JSONArray().put(local).put(bot)));
            check(member(session).streamingSong && member(session).songName.equals("Already playing.mp3"), "initial list restores an active bot title");
            check(member(session).musicBot, "title metadata preserves bot identity");
            Member captured = member(session);
            dispatch(session, "start_song_stream", new JSONObject().put("client_id", 2).put("song_name", "Next song.mp3"));
            check(member(session).songName.equals("Next song.mp3"), "track changes replace the title");
            check(captured.songName.equals("Already playing.mp3"), "published member snapshots stay immutable");
            dispatch(session, "client_rename", new JSONObject().put("client_id", 2).put("new_username", "Lemon Radio"));
            check(member(session).songName.equals("Next song.mp3"), "rename preserves the playing title");
            check(member(session).withGroups(Collections.singletonList(3), 0).songName.equals("Next song.mp3"), "group updates preserve the title");
            check(member(session).withProfile("SK", "").songName.equals("Next song.mp3"), "profile updates preserve the title");
            dispatch(session, "stop_song_stream", new JSONObject().put("client_id", 2));
            check(!member(session).streamingSong && member(session).songName.isEmpty(), "pause or stop clears the title");
            dispatch(session, "start_song_stream", new JSONObject().put("client_id", 2).put("song_name", ""));
            check(!member(session).streamingSong, "empty song does not create a marquee");
            dispatch(session, "channel_join", new JSONObject().put("client_id", 2).put("channel_id", 0).put("is_streaming_song", true).put("song_name", "Joined stream.mp3"));
            check(member(session).songName.equals("Joined stream.mp3"), "channel presence restores its supplied stream title");
            dispatch(session, "current_channel_active_microphone_usage", new JSONObject().put("clients", new JSONArray().put(new JSONObject().put("client_id", 2).put("audio_state", 1).put("is_streaming_song", true).put("song_name", "Active snapshot.mp3"))));
            check(member(session).songName.equals("Active snapshot.mp3"), "active audio snapshot refreshes the title");
            run(session, () ->
            {
                Method policy = ChatSession.class.getDeclaredMethod("rememberPolicy", JSONObject.class);
                policy.setAccessible(true);
                policy.invoke(session, new JSONObject().put("is_music_bot_marquee_visible_to_everyone", false));
                Method publish = ChatSession.class.getDeclaredMethod("publish");
                publish.setAccessible(true);
                publish.invoke(session);
            });
            check(member(session).streamingSong, "same channel title remains visible under restricted policy");
            dispatch(session, "channel_join", new JSONObject().put("client_id", 1).put("channel_id", 1));
            check(!member(session).streamingSong, "leaving hides titles restricted to the old channel");
            dispatch(session, "channel_join", new JSONObject().put("client_id", 1).put("channel_id", 0));
            check(!member(session).streamingSong, "returning does not revive an old title without a new snapshot");
            dispatch(session, "start_song_stream", new JSONObject().put("client_id", 2).put("song_name", "Resumed.mp3"));
            check(member(session).streamingSong, "a new same channel event restores the title");
            dispatch(session, "client_going_to_idle_mode", new JSONObject().put("client_id", 2));
            check(!member(session).streamingSong, "idle removes the active stream title");
            dispatch(session, "client_connect", person(2, 0, true));
            check(member(session).songName.isEmpty(), "reused client identifier has no old title");
            dispatch(session, "stop_song_stream", new JSONObject().put("client_id", 900));
            check(session.state().members.size() == 2, "unknown stream sender does not create a member");
            boolean rejected = false;
            try
            {
                dispatch(session, "start_song_stream", new JSONObject().put("client_id", 2).put("song_name", "x".repeat(513)));
            }
            catch (Exception expected)
            {
                rejected = true;
            }
            check(rejected && member(session).songName.isEmpty(), "oversized title is rejected before changing state");
            dispatch(session, "client_disconnect", new JSONObject().put("client_id", 2));
            check(session.state().members.size() == 1, "bot departure removes its title with its row");
        }
        System.out.println("PASS " + checks + " music stream title checks");
    }

    private static JSONObject person(int id, int channel, boolean bot) throws Exception
    {
        return new JSONObject().put("client_id", id).put("channel_id", channel).put("username", bot ? "Lemon Radio" : "Listener").put("public_key", "key-" + id).put("audio_state", 1).put("is_music_bot", bot);
    }

    private static Member member(ChatSession session)
    {
        return session.state().members.stream().filter(value -> value.id == 2).findFirst().get();
    }

    private static void dispatch(ChatSession session, String type, JSONObject body) throws Exception
    {
        run(session, () ->
        {
            Method method = ChatSession.class.getDeclaredMethod("dispatch", String.class, JSONObject.class);
            method.setAccessible(true);
            method.invoke(session, type, body.put("type", type));
        });
    }

    private static void set(ChatSession session, String name, Object value) throws Exception
    {
        Field field = ChatSession.class.getDeclaredField(name);
        field.setAccessible(true);
        field.set(session, value);
    }

    private interface Action
    {
        void run() throws Exception;
    }

    private static void run(ChatSession session, Action action) throws Exception
    {
        FutureTask<Void> task = new FutureTask<>(() ->
        {
            action.run();
            return null;
        });
        session.executor().execute(task);
        task.get(10, TimeUnit.SECONDS);
    }

    private static void check(boolean result, String description)
    {
        if (!result)
        {
            throw new AssertionError(description);
        }
        checks++;
    }
}
