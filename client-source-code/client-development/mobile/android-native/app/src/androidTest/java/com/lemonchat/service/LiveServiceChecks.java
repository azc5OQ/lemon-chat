package com.lemonchat.service;

import android.app.Instrumentation;
import android.app.Dialog;
import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.AccessibilityServiceInfo;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.net.Uri;
import android.os.SystemClock;
import android.service.notification.StatusBarNotification;
import android.text.InputType;
import android.view.View;
import android.view.ViewGroup;
import android.view.accessibility.AccessibilityNodeInfo;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import com.lemonchat.crypto.Identity;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.model.CallInvitation;
import com.lemonchat.model.Member;
import com.lemonchat.model.SessionSnapshot;
import com.lemonchat.protocol.ChatSession;
import com.lemonchat.storage.HistoryPolicy;
import com.lemonchat.ui.MainActivity;
import com.lemonchat.ui.IncomingCallActivity;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Predicate;

/** exercises a real service and server inside the isolated verification app */
public final class LiveServiceChecks
{
    private Instrumentation instrumentation;
    private Context context;
    private MainActivity activity;
    private ChatService service;
    private ChatSession peer;
    private final List<ChatMessage> received = new CopyOnWriteArrayList<>();
    private int checks;

    /**
     * checks messaging and background calls through a disposable server
     *
     * @param instrumentation Android test runner
     * @param endpoint isolated test server address
     * @return completed assertion count
     * @throws Exception when a check fails
     */
    public static int verify(Instrumentation instrumentation, String endpoint) throws Exception
    {
        LiveServiceChecks suite = new LiveServiceChecks();
        suite.instrumentation = instrumentation;
        suite.context = instrumentation.getTargetContext();
        if (!suite.context.getPackageName().equals("com.lemonchat.nativeclient.checks"))
        {
            throw new IllegalStateException("Live checks require the isolated verification app");
        }
        suite.run(endpoint);
        return suite.checks;
    }

    private void run(String endpoint) throws Exception
    {
        context.getSharedPreferences("local-settings", Context.MODE_PRIVATE).edit().putBoolean("lemon_sound_effects", false).commit();
        HistoryPolicy.save(context, -1, false);
        try
        {
            activity = launch();
            await(() -> onMain(() -> field(activity, "service") != null), "activity binds its service");
            service = onMain(() -> (ChatService)field(activity, "service"));
            await(() -> onMain(() -> service.identity() != null), "identity loads on the phone");
            connectThroughForm(endpoint);
            await(this::ready, "native service authenticates and receives channel keys");
            checkSavedConnectionForm(endpoint);
            SessionSnapshot initial = state();
            check(initial.channels.size() >= 3, "saved channels reach the phone");
            await(() -> !state().groups.isEmpty(), "server groups reach the phone");
            await(() -> state().members.stream().anyMatch(member -> member.musicBot), "music bot appears in the service member list");
            Member bot = state().members.stream().filter(member -> member.musicBot).findFirst().get();
            check(onMain(() -> !service.callMember(bot.id)), "music bot cannot receive calls");

            peer = new ChatSession(Runnable::run);
            peer.setListener(new ChatSession.Listener()
            {
                @Override
                public void onState(SessionSnapshot snapshot)
                {
                }

                @Override
                public void onMessage(ChatMessage message)
                {
                    received.add(message);
                }

                @Override
                public void onError(String message)
                {
                    android.util.Log.w("LemonLiveChecks", message);
                }
            });
            peer.connect(endpoint, "Phone Test Peer", Identity.create(2048), "");
            await(() -> peer.state().connected && peer.state().channelKeysReady && ready(), "second phone session receives channel keys");
            await(() -> state().members.stream().anyMatch(member -> member.id == peer.state().clientId), "service sees the second session");
            Member other = state().members.stream().filter(member -> member.id == peer.state().clientId).findFirst().get();
            peer.sendChannelMessage("physical phone channel message");
            await(() -> messages("channel:" + state().channelId).stream().anyMatch(message -> message.text.equals("physical phone channel message") && !message.outgoing), "service decrypts an incoming channel message");
            service.session().sendDirectMessage(other.id, "physical phone direct message");
            await(() -> received.stream().anyMatch(message -> message.text.equals("physical phone direct message") && !message.outgoing), "second session decrypts the service direct message");
            ChatMessage original = received.stream().filter(message -> message.text.equals("physical phone direct message") && !message.outgoing).findFirst().get();
            peer.sendDirectMessage(state().clientId, "physical phone quoted reply", original);
            await(() -> messages(other.conversationId).stream().anyMatch(message -> message.text.equals("physical phone quoted reply") && message.reply != null), "service retains the encrypted quoted reply");
            byte[] attachment = "phone attachment bytes".getBytes(StandardCharsets.UTF_8);
            peer.sendDirectAttachment(state().clientId, "phone-check.txt", "text/plain", attachment, false);
            await(() -> messages(other.conversationId).stream().anyMatch(message -> message.attachment != null && Arrays.equals(message.attachment.data(), attachment)), "service receives the complete encrypted file");
            await(() -> onMain(() -> service.voiceReady()), "separate audio connection becomes ready");

            long connection = state().connectionId;
            onMain(() ->
            {
                activity.finishAndRemoveTask();
                return null;
            });
            await(() -> onMain(() -> activity.isDestroyed()), "removing the task destroys its activity");
            await(() -> state().connected && state().idle, "service stays connected and enters idle without its activity");
            check(state().connectionId == connection, "background transition keeps the original connection");
            check(!onMain(() -> service.voiceReady()), "idle releases the audio connection");
            peer.sendDirectMessage(state().clientId, "message while activity is gone");
            await(() -> messages(other.conversationId).stream().anyMatch(message -> message.text.equals("message while activity is gone")), "service receives a private message without UI");
            await(() -> notification("messages") != null, "background message posts an Android notification");
            check(!notification("messages").extras.getCharSequence(Notification.EXTRA_TEXT, "").toString().contains("message while activity is gone"), "message notification hides private content");
            int localId = state().clientId;
            await(() -> peer.state().members.stream().anyMatch(member -> member.id == localId && member.idle), "caller sees the idle recipient");
            peer.callMember(state().clientId);
            await(() -> state().call != null && notification("channel-calls") != null, "idle service receives a call and posts its notification");
            sendAction(notification("channel-calls"), "Decline");
            await(() -> state().call == null && notification("channel-calls") == null, "notification Decline clears the real call");
            check(state().idle, "declining leaves the client idle");
            // allow the incoming call limit to expire
            SystemClock.sleep(5500);
            peer.callMember(state().clientId);
            await(() -> state().call != null && notification("channel-calls") != null, "another call can arrive while idle");
            sendAction(notification("channel-calls"), "Answer");
            await(() -> ready() && !state().idle && state().channelId == peer.state().channelId, "notification Answer joins the caller channel");
            service.session().sendChannelMessage("message after answering");
            await(() -> received.stream().anyMatch(message -> message.text.equals("message after answering") && !message.outgoing), "encrypted chat works after answering");
            activity = launch();
            await(() -> onMain(() -> field(activity, "service") != null), "reopened activity binds the service");
            check(onMain(() -> field(activity, "service") == service), "reopened activity uses the existing service object");
            check(state().connectionId == connection, "reopening does not replace the connection");
            check(messages(other.conversationId).stream().anyMatch(message -> message.text.equals("message while activity is gone")), "messages survive activity destruction");
        }
        finally
        {
            if (peer != null)
            {
                peer.close();
            }
            if (service != null)
            {
                onMain(() ->
                {
                    service.disconnect();
                    return null;
                });
            }
            if (activity != null)
            {
                onMain(() ->
                {
                    activity.finishAndRemoveTask();
                    return null;
                });
            }
        }
    }

    private MainActivity launch()
    {
        return (MainActivity)instrumentation.startActivitySync(new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP));
    }

    private void connectThroughForm(String endpoint) throws Exception
    {
        URI target = URI.create(endpoint);
        String host = target.getHost();
        String address = host.startsWith("[") ? host.substring(1, host.length() - 1) : host;
        boolean secure = "wss".equals(target.getScheme());
        String port = Integer.toString(target.getPort() < 0 ? secure ? 443 : 80 : target.getPort());
        onMain(() ->
        {
            Dialog dialog = openConnectionForm();
            View root = dialog.getWindow().getDecorView();
            EditText nameField = input(root, "Name");
            check(nameField != null, "connection wizard begins with a name");
            nameField.setText("Moto Native");
            press(root, "Next");
            EditText addressField = input(root, "chat.example.org or 192.168.1.10");
            EditText portField = input(root, "1111");
            CheckBox secureField = find(root, CheckBox.class, value -> value.getText().toString().equals("Use TLS (secure connection)"));
            check(addressField != null && portField != null && secureField == null, "server step needs only address and port");
            check((portField.getInputType() & InputType.TYPE_MASK_CLASS) == InputType.TYPE_CLASS_NUMBER, "port field requests the numeric keyboard");
            check(!addressField.getHint().toString().contains("://") && !addressField.getText().toString().contains("://") && !portField.getText().toString().contains("://"), "connection fields do not show protocol prefixes");
            addressField.setText(address);
            portField.setText("0");
            press(root, "Next");
            check(dialog.isShowing() && portField.getError() != null, "the wizard refuses an invalid port");
            portField.setText(port);
            press(root, "Next");
            check(find(root, Button.class, value -> value.getText().toString().equals("Add server key")) != null, "the next step explains optional server passwords");
            press(root, "Next");
            check(find(root, android.widget.TextView.class, value -> value.getText().toString().equals("Ready to connect")) != null, "the final step reviews connection details");
            press(root, "Connect");
            check(!dialog.isShowing(), "Connect submits and closes the wizard");
            return null;
        });
    }

    private void checkSavedConnectionForm(String endpoint) throws Exception
    {
        URI target = URI.create(endpoint);
        String host = target.getHost();
        String address = host.startsWith("[") ? host.substring(1, host.length() - 1) : host;
        boolean secure = "wss".equals(target.getScheme());
        String port = Integer.toString(target.getPort() < 0 ? secure ? 443 : 80 : target.getPort());
        onMain(() ->
        {
            Dialog dialog = openConnectionForm();
            View root = dialog.getWindow().getDecorView();
            EditText nameField = input(root, "Name");
            check(nameField != null && nameField.getText().toString().equals("Moto Native"), "reopening restores the saved display name");
            press(root, "Next");
            EditText addressField = input(root, "chat.example.org or 192.168.1.10");
            EditText portField = input(root, "1111");
            CheckBox secureField = find(root, CheckBox.class, value -> value.getText().toString().equals("Use TLS (secure connection)"));
            check(addressField != null && addressField.getText().toString().equals(address), "reopening restores the saved address without a prefix");
            check(portField != null && portField.getText().toString().equals(port), "reopening restores the saved port separately");
            check(secureField == null, "reopening keeps transport selection automatic");
            dialog.dismiss();
            return null;
        });
    }

    private static void press(View root, String label)
    {
        Button button = find(root, Button.class, value -> value.getText().toString().equals(label));
        if (button == null || !button.performClick())
        {
            throw new AssertionError("Wizard button unavailable: " + label);
        }
    }

    private Dialog openConnectionForm() throws Exception
    {
        Method show = MainActivity.class.getDeclaredMethod("showConnection");
        show.setAccessible(true);
        show.invoke(activity);
        Dialog dialog = (Dialog)field(activity, "openDialog");
        if (dialog == null || !dialog.isShowing())
        {
            throw new AssertionError("Connection sheet did not open");
        }
        return dialog;
    }

    private static EditText input(View root, String hint)
    {
        return find(root, EditText.class, value -> value.getHint() != null && value.getHint().toString().equals(hint));
    }

    private static <T extends View> T find(View root, Class<T> type, Predicate<T> matches)
    {
        if (type.isInstance(root) && matches.test(type.cast(root)))
        {
            return type.cast(root);
        }
        if (root instanceof ViewGroup)
        {
            ViewGroup group = (ViewGroup)root;
            for (int index = 0; index < group.getChildCount(); index++)
            {
                T found = find(group.getChildAt(index), type, matches);
                if (found != null)
                {
                    return found;
                }
            }
        }
        return null;
    }

    private SessionSnapshot state() throws Exception
    {
        return onMain(() -> service.snapshot());
    }

    private boolean ready() throws Exception
    {
        SessionSnapshot snapshot = state();
        return snapshot != null && snapshot.connected && snapshot.channelKeysReady;
    }

    private List<ChatMessage> messages(String conversation) throws Exception
    {
        return onMain(() -> service.messages(conversation));
    }

    private Notification notification(String channel)
    {
        for (StatusBarNotification item : context.getSystemService(NotificationManager.class).getActiveNotifications())
        {
            if (item.getNotification().getChannelId().equals(channel))
            {
                return item.getNotification();
            }
        }
        return null;
    }

    /**
     * clicks the current call action in the system notification panel
     *
     * @param notification posted call notification
     * @param requestedAction Answer or Decline
     * @throws Exception when the action is missing or cannot be clicked
     */
    private void sendAction(Notification notification, String requestedAction) throws Exception
    {
        CallInvitation call = state().call;
        check(call != null, "notification action belongs to a current call");
        boolean answer = "Answer".equals(requestedAction);
        if (!answer && !"Decline".equals(requestedAction))
        {
            throw new IllegalArgumentException("Unknown test call action");
        }
        Intent intent;
        PendingIntent expected;
        int flags = PendingIntent.FLAG_NO_CREATE | PendingIntent.FLAG_IMMUTABLE;
        if (answer)
        {
            intent = new Intent(context, IncomingCallActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
            intent.setData(Uri.parse("lemon-native://call/" + call.connectionId + "/" + Uri.encode(call.callId) + "/true"));
            expected = PendingIntent.getActivity(context, 1, intent, flags);
        }
        else
        {
            intent = new Intent(context, ChatService.class).setAction(ChatService.DECLINE_ACTION);
            intent.setData(Uri.parse("lemon-native://decline/" + call.connectionId + "/" + Uri.encode(call.callId)));
            expected = PendingIntent.getService(context, 2, intent, flags);
        }
        Notification.Action selected = null;
        StringBuilder labels = new StringBuilder();
        if (notification.actions != null)
        {
            for (Notification.Action action : notification.actions)
            {
                if (labels.length() > 0)
                {
                    labels.append(", ");
                }
                labels.append(action.title);
                if (expected != null && expected.equals(action.actionIntent))
                {
                    selected = action;
                }
            }
        }
        Bundle details = new Bundle();
        details.putString("stream", "Call action lookup: requested=" + requestedAction + " locale=" + context.getResources().getConfiguration().getLocales().toLanguageTags() + " titles=[" + labels + "] matched=" + (selected != null) + "\n");
        instrumentation.sendStatus(0, details);
        check(selected != null && selected.title != null && selected.title.length() > 0, "call notification provides " + requestedAction + " for the current call");
        String title = selected.title.toString();
        String caller = call.callerName;
        AccessibilityServiceInfo access = instrumentation.getUiAutomation().getServiceInfo();
        access.flags |= AccessibilityServiceInfo.FLAG_INCLUDE_NOT_IMPORTANT_VIEWS | AccessibilityServiceInfo.FLAG_REPORT_VIEW_IDS | AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS;
        instrumentation.getUiAutomation().setServiceInfo(access);
        check(instrumentation.getUiAutomation().performGlobalAction(AccessibilityService.GLOBAL_ACTION_NOTIFICATIONS), "system notification shade opens for " + title);
        long deadline = SystemClock.elapsedRealtime() + 10000;
        boolean reported = false;
        while (SystemClock.elapsedRealtime() < deadline)
        {
            AccessibilityNodeInfo root = instrumentation.getUiAutomation().getRootInActiveWindow();
            if (root != null)
            {
                try
                {
                    if (!reported)
                    {
                        List<AccessibilityNodeInfo> callers = root.findAccessibilityNodeInfosByText(caller);
                        List<AccessibilityNodeInfo> actions = root.findAccessibilityNodeInfosByText(title);
                        Bundle uiDetails = new Bundle();
                        uiDetails.putString("stream", "Notification UI: package=" + root.getPackageName() + " caller matches=" + callers.size() + " action matches=" + actions.size() + "\n");
                        instrumentation.sendStatus(0, uiDetails);
                        for (AccessibilityNodeInfo node : callers)
                        {
                            node.recycle();
                        }
                        for (AccessibilityNodeInfo node : actions)
                        {
                            node.recycle();
                        }
                        reported = true;
                    }
                    if ("com.android.systemui".equals(String.valueOf(root.getPackageName())) && clickNotificationAction(root, caller, title))
                    {
                        check(true, "system notification " + title + " button is pressed");
                        return;
                    }
                }
                finally
                {
                    root.recycle();
                }
            }
            SystemClock.sleep(100);
        }
        throw new AssertionError("The system notification did not expose " + title + " for the test caller");
    }

    private boolean clickNotificationAction(AccessibilityNodeInfo root, String caller, String title)
    {
        List<AccessibilityNodeInfo> matches = root.findAccessibilityNodeInfosByText(caller);
        try
        {
            for (AccessibilityNodeInfo match : matches)
            {
                AccessibilityNodeInfo parent = match.getParent();
                for (int level = 0; parent != null && level < 8; level++)
                {
                    AccessibilityNodeInfo next = null;
                    try
                    {
                        if (clickActionText(parent, title))
                        {
                            return true;
                        }
                        next = parent.getParent();
                    }
                    finally
                    {
                        parent.recycle();
                    }
                    parent = next;
                }
                if (parent != null)
                {
                    parent.recycle();
                }
            }
            return false;
        }
        finally
        {
            for (AccessibilityNodeInfo match : matches)
            {
                match.recycle();
            }
        }
    }

    private boolean clickActionText(AccessibilityNodeInfo parent, String title)
    {
        List<AccessibilityNodeInfo> actions = parent.findAccessibilityNodeInfosByText(title);
        try
        {
            for (AccessibilityNodeInfo action : actions)
            {
                boolean matches = title.equalsIgnoreCase(String.valueOf(action.getText())) || title.equalsIgnoreCase(String.valueOf(action.getContentDescription()));
                if (!matches || !action.isVisibleToUser() || !action.isEnabled())
                {
                    continue;
                }
                if (action.isClickable() && action.performAction(AccessibilityNodeInfo.ACTION_CLICK))
                {
                    return true;
                }
                AccessibilityNodeInfo container = action.getParent();
                for (int level = 0; container != null && level < 2; level++)
                {
                    AccessibilityNodeInfo next = null;
                    try
                    {
                        if (container.isClickable() && container.isEnabled() && container.performAction(AccessibilityNodeInfo.ACTION_CLICK))
                        {
                            return true;
                        }
                        next = container.getParent();
                    }
                    finally
                    {
                        container.recycle();
                    }
                    container = next;
                }
                if (container != null)
                {
                    container.recycle();
                }
            }
            return false;
        }
        finally
        {
            for (AccessibilityNodeInfo action : actions)
            {
                action.recycle();
            }
        }
    }

    private void await(Callable<Boolean> condition, String description) throws Exception
    {
        long deadline = SystemClock.elapsedRealtime() + 45000;
        while (!condition.call() && SystemClock.elapsedRealtime() < deadline)
        {
            SystemClock.sleep(50);
        }
        check(condition.call(), description);
    }

    private void check(boolean passed, String description)
    {
        if (!passed)
        {
            throw new AssertionError(description);
        }
        checks++;
        Bundle progress = new Bundle();
        progress.putString("stream", "PASS " + description + "\n");
        instrumentation.sendStatus(0, progress);
    }

    private <T> T onMain(Callable<T> action) throws Exception
    {
        if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper())
        {
            return action.call();
        }
        AtomicReference<T> result = new AtomicReference<>();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        instrumentation.runOnMainSync(() ->
        {
            try
            {
                result.set(action.call());
            }
            catch (Throwable error)
            {
                failure.set(error);
            }
        });
        if (failure.get() != null)
        {
            throw new AssertionError("Main thread action failed", failure.get());
        }
        return result.get();
    }

    private static Object field(Object owner, String name) throws Exception
    {
        Field field = owner.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(owner);
    }
}
