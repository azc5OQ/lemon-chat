package com.lemonchat.protocol;

import com.lemonchat.model.CallInvitation;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.model.Member;
import com.lemonchat.model.SessionSnapshot;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.Map;
import java.util.concurrent.FutureTask;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

/** checks quiet idle scheduling and authenticated policy replacement */
public final class TimerPolicyHostProbe
{
    private static int checks;

    public static void run() throws Exception
    {
        ChatSession session = new ChatSession(Runnable::run);
        try
        {
            runOnWorker(session, () ->
            {
                require(delay(session) < 0 && get(session, "tickTask") == null, "a disconnected session has no periodic timer");
                set(session, "status", SessionSnapshot.Status.CONNECTED);
                set(session, "phase", 3);
                set(session, "clientId", 1);
                set(session, "idleAllowed", true);
                long timestamp = TimeUnit.NANOSECONDS.toMillis(System.nanoTime());
                set(session, "lastKeepalive", timestamp);
                set(session, "lastReply", timestamp);
                Map<Integer, Member> members = members(session);
                members.put(1, new Member(1, -1, "You", "", "local-key", false, true, 3, 0, ""));
                invoke(session, "publish");
                require(!session.sendControl(9, new JSONObject().put("type", "microphone_usage_request")), "audio controls from a replaced connection are rejected before queuing");
                require(delay(session) > 110000 && delay(session) <= 120000, "quiet idle waits for the supported two minute heartbeat");
                require(((Number)invoke(session, "heartbeatInterval")).longValue() == 120000, "idle heartbeat matches server policy");
                set(session, "call", new CallInvitation("call", 0, System.currentTimeMillis() + 2000, System.nanoTime() + TimeUnit.SECONDS.toNanos(2), 2, 0, "Caller", "peer-key"));
                require(delay(session) > 0 && delay(session) <= 2000, "call expiry advances the quiet idle deadline");
                set(session, "call", null);
                set(session, "desiredIdle", Boolean.FALSE);
                require(delay(session) == 100, "foreground resume wakes presence processing immediately");
                set(session, "desiredIdle", null);
                Map<Long, ChatMessage> pending = pending(session);
                pending.put(1L, new ChatMessage(1, "user:" + "a".repeat(64), 2, "Peer", "text", true, System.currentTimeMillis(), "Sending", "key", -1));
                require(delay(session) == 100, "pending delivery retains its short deadline checks");
                pending.clear();
                set(session, "lastReply", timestamp - 200000);
                require(delay(session) == 1, "an expired idle liveness deadline runs immediately");
                set(session, "connectionCheckEnforced", false);
                require(delay(session) > 110000, "disabled timeout enforcement does not close a quiet idle connection");
                set(session, "lastReply", timestamp);
                members.put(1, members.get(1).withPresence(0, false, 3));
                require(((Number)invoke(session, "heartbeatInterval")).longValue() == 10000, "active heartbeat matches the browser interval");
                members.put(1, members.get(1).withPresence(-1, true, 3));
                JSONObject policy = new JSONObject().put("type", "authentication_status").put("is_idle_mode_allowed", true).put("is_audio_over_websocket_allowed", true).put("is_voice_chat_active", true).put("stun_port", 3478);
                set(session, "authenticationPolicy", policy);
                Method remember = ChatSession.class.getDeclaredMethod("rememberPolicy", JSONObject.class);
                remember.setAccessible(true);
                remember.invoke(session, new JSONObject().put("type", "server_policy").put("is_voice_chat_active", false).put("stun_port", 5349).put("is_connection_check_enforced", true));
                require(!policy.getBoolean("is_voice_chat_active") && policy.getInt("stun_port") == 5349 && policy.getBoolean("is_audio_over_websocket_allowed"), "media resume keeps new policy fields and unchanged permissions");
                require((Boolean)get(session, "connectionCheckEnforced"), "live timeout policy changes are applied");
                return null;
            });
            runOnWorker(session, () ->
            {
                ScheduledFuture<?> timer = (ScheduledFuture<?>)get(session, "tickTask");
                require(timer != null && timer.getDelay(TimeUnit.MILLISECONDS) > 100000, "idle has one long deadline instead of repeated polling");
                set(session, "desiredIdle", Boolean.FALSE);
                invoke(session, "scheduleTick");
                timer = (ScheduledFuture<?>)get(session, "tickTask");
                require(timer != null && timer.getDelay(TimeUnit.MILLISECONDS) <= 100, "new foreground work advances the existing idle timer");
                set(session, "desiredIdle", null);
                invoke(session, "cancelTick");
                return null;
            });
        }
        finally
        {
            session.close();
        }
        ScheduledThreadPoolExecutor worker = (ScheduledThreadPoolExecutor)get(session, "worker");
        require(worker.awaitTermination(5, TimeUnit.SECONDS), "closing cancels a long idle deadline without retaining the worker");
        System.out.println("PASS " + checks + " idle timer and policy checks");
    }

    private static void runOnWorker(ChatSession session, java.util.concurrent.Callable<Void> action) throws Exception
    {
        FutureTask<Void> task = new FutureTask<>(action);
        session.executor().execute(task);
        task.get(10, TimeUnit.SECONDS);
    }

    private static long delay(ChatSession session) throws Exception
    {
        return ((Number)invoke(session, "nextTickDelay")).longValue();
    }

    private static Object invoke(ChatSession session, String name) throws Exception
    {
        Method method = ChatSession.class.getDeclaredMethod(name);
        method.setAccessible(true);
        return method.invoke(session);
    }

    private static Object get(ChatSession session, String name) throws Exception
    {
        Field field = ChatSession.class.getDeclaredField(name);
        field.setAccessible(true);
        return field.get(session);
    }

    private static void set(ChatSession session, String name, Object value) throws Exception
    {
        Field field = ChatSession.class.getDeclaredField(name);
        field.setAccessible(true);
        field.set(session, value);
    }

    @SuppressWarnings("unchecked")
    private static Map<Integer, Member> members(ChatSession session) throws Exception
    {
        return (Map<Integer, Member>)get(session, "members");
    }

    @SuppressWarnings("unchecked")
    private static Map<Long, ChatMessage> pending(ChatSession session) throws Exception
    {
        return (Map<Long, ChatMessage>)get(session, "pendingMessages");
    }

    private static void require(boolean condition, String description)
    {
        if (!condition)
        {
            throw new AssertionError(description);
        }
        checks++;
    }
}
