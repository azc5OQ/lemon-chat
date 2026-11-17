package com.lemonchat.protocol;

import com.lemonchat.model.CallInvitation;
import com.lemonchat.model.Channel;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.model.Member;
import com.lemonchat.model.PokeMessage;
import com.lemonchat.model.SessionSnapshot;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONObject;

/** deterministic checks for expiring call capabilities and idle state */
public final class CallStateHostProbe
{
    public static void run() throws Exception
    {
        List<String> events = new ArrayList<>();
        List<PokeMessage> pokes = new ArrayList<>();
        ChatSession session = new ChatSession(Runnable::run);
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
                throw new AssertionError(message);
            }

            @Override
            public void onCall(CallInvitation invitation, String reason)
            {
                events.add(reason);
            }

            @Override
            public void onPoke(PokeMessage message)
            {
                pokes.add(message);
            }
        });
        try
        {
            work(session, () ->
            {
                set(session, "status", SessionSnapshot.Status.CONNECTED);
                set(session, "phase", 3);
                set(session, "clientId", 1);
                set(session, "publicKey", "local");
                set(session, "idleAllowed", true);
                set(session, "lastReply", TimeUnit.NANOSECONDS.toMillis(System.nanoTime()));
                set(session, "lastKeepalive", Long.MAX_VALUE / 2);
                set(session, "lastControlSent", Long.MAX_VALUE / 2);
                members(session).put(1, person(1, "local", true));
                members(session).put(2, person(2, "caller", false));
                channels(session).put(0, new Channel(0, -1, "General", "", false, true));
                invoke(session, "receiveCall", new JSONObject().put("caller_client_id", 2));
            });
            CallInvitation first = session.state().call;
            check(first != null && first.callerId == 2 && first.channelId == 0, "known caller creates a scoped invitation");
            session.acceptCall("wrong-token", first.connectionId);
            session.declineCall(first.callId, first.connectionId + 1);
            work(session, () ->
            {
            });
            check(session.state().call == first && session.state().idle, "stale call actions leave the invitation unchanged");
            work(session, () -> invoke(session, "receiveCall", new JSONObject().put("caller_client_id", 2)));
            check(session.state().call == first, "duplicate ringing does not extend the deadline");
            session.leaveIdle();
            work(session, () ->
            {
            });
            check(session.state().call == first && session.state().idle, "opening the app does not answer a pending call");
            session.declineCall(first.callId, first.connectionId);
            work(session, () ->
            {
            });
            check(session.state().call == null && session.state().idle && events.contains("declined"), "decline keeps the client idle");
            work(session, () ->
            {
                set(session, "call", new CallInvitation("expired", 0, Long.MAX_VALUE, System.nanoTime() - 1, 2, 0, "Caller", "caller"));
                invoke(session, "validateCurrentCall");
            });
            check(session.state().call == null && events.contains("expired"), "monotonic expiry ignores a future wall clock deadline");
            work(session, () ->
            {
                set(session, "nextIncomingCall", 0L);
                invoke(session, "receiveCall", new JSONObject().put("caller_client_id", 2));
                members(session).put(2, person(2, "replacement", false));
                invoke(session, "validateCurrentCall");
            });
            check(session.state().call == null && events.contains("cancelled"), "reused caller identifier invalidates the invitation");
            work(session, () ->
            {
                invoke(session, "receivePoke", new JSONObject().put("client_id", 2).put("poke_message", "Please join"));
                invoke(session, "receivePoke", new JSONObject().put("client_id", 2).put("poke_message", "x".repeat(16385)));
                invoke(session, "receivePoke", new JSONObject().put("client_id", 99).put("poke_message", "Unknown sender"));
            });
            check(pokes.size() == 1 && pokes.get(0).senderPublicKey.equals("replacement"), "poke events are bounded and keep the sender identity");
            work(session, () ->
            {
                set(session, "nextIncomingCall", 0L);
                invoke(session, "receiveCall", new JSONObject().put("caller_client_id", 2));
            });
            CallInvitation accepted = session.state().call;
            session.acceptCall(accepted.callId, accepted.connectionId);
            work(session, () ->
            {
                check(get(session, "resumeChannel").equals(0) && Boolean.FALSE.equals(get(session, "desiredIdle")), "accept uses the invitation's validated channel");
            });
            check(session.state().call == null && events.contains("accepted"), "accepted invitation is consumed once");
        }
        finally
        {
            session.close();
        }
    }

    private static Member person(int id, String key, boolean idle)
    {
        return new Member(id, idle ? -1 : 0, key, "", key, false, idle, 3, 0, "");
    }

    @SuppressWarnings("unchecked")
    private static Map<Integer, Member> members(ChatSession session) throws Exception
    {
        return (Map<Integer, Member>) get(session, "members");
    }

    @SuppressWarnings("unchecked")
    private static Map<Integer, Channel> channels(ChatSession session) throws Exception
    {
        return (Map<Integer, Channel>) get(session, "channels");
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

    private static void invoke(ChatSession session, String name, Object... arguments) throws Exception
    {
        Method method = arguments.length == 0 ? ChatSession.class.getDeclaredMethod(name) : ChatSession.class.getDeclaredMethod(name, JSONObject.class);
        method.setAccessible(true);
        method.invoke(session, arguments);
    }

    private static void work(ChatSession session, Action action) throws Exception
    {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        session.executor().execute(() ->
        {
            try
            {
                action.run();
            }
            catch (Throwable error)
            {
                failure.set(error);
            }
            finally
            {
                done.countDown();
            }
        });
        if (!done.await(5, TimeUnit.SECONDS))
        {
            throw new AssertionError("Session worker did not finish");
        }
        if (failure.get() != null)
        {
            throw new AssertionError(failure.get());
        }
    }

    private static void check(boolean result, String message)
    {
        if (!result)
        {
            throw new AssertionError(message);
        }
        System.out.println("PASS " + message);
    }

    private interface Action
    {
        void run() throws Exception;
    }
}
