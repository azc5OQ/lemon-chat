package com.lemonchat.service;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Person;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.media.AudioAttributes;
import android.media.RingtoneManager;
import android.service.notification.StatusBarNotification;
import com.lemonchat.R;
import com.lemonchat.model.CallInvitation;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.model.PokeMessage;
import com.lemonchat.ui.IncomingCallActivity;
import com.lemonchat.ui.MainActivity;
import java.util.LinkedHashSet;

/** creates private notifications with scoped actions */
final class ChatNotifications
{
    static final String CONNECTION_CHANNEL = "server-connection";
    private static final String MESSAGE_CHANNEL = "messages";
    private static final String CALL_CHANNEL = "channel-calls";
    private final Context context;
    private final NotificationManager manager;
    private final LinkedHashSet<String> conversationTags = new LinkedHashSet<>();
    private final LinkedHashSet<String> receivedMessages = new LinkedHashSet<>();
    private String activeCallTag;

    // messages received while no chat screen was visible, shown on the launcher icon
    private int unreadCount;

    ChatNotifications(Context context)
    {
        this.context = context;
        manager = context.getSystemService(NotificationManager.class);
        manager.createNotificationChannel(new NotificationChannel(CONNECTION_CHANNEL, "Server connection", NotificationManager.IMPORTANCE_LOW));
        manager.createNotificationChannel(new NotificationChannel(MESSAGE_CHANNEL, "Messages and pokes", NotificationManager.IMPORTANCE_DEFAULT));
        NotificationChannel calls = new NotificationChannel(CALL_CHANNEL, "Channel calls", NotificationManager.IMPORTANCE_HIGH);
        calls.setSound(RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE), new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE).setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build());
        calls.enableVibration(true);
        manager.createNotificationChannel(calls);
        for (StatusBarNotification existing : manager.getActiveNotifications())
        {
            String tag = existing.getTag();
            if (tag != null && tag.startsWith("call:"))
            {
                manager.cancel(tag, 2);
            }
            else if (tag != null && tag.startsWith("conversation:"))
            {
                conversationTags.add(tag);
            }
        }
    }

    Notification connection(String detail)
    {
        PendingIntent open = PendingIntent.getActivity(context, 0, new Intent(context, MainActivity.class), flags());
        PendingIntent stop = PendingIntent.getService(context, 1, new Intent(context, ChatService.class).setAction(ChatService.DISCONNECT_ACTION), flags());
        return new Notification.Builder(context, CONNECTION_CHANNEL).setSmallIcon(R.drawable.ic_lemon).setContentTitle("Lemon Native").setContentText(detail).setContentIntent(open).setOngoing(true).setOnlyAlertOnce(true).setCategory(Notification.CATEGORY_SERVICE).addAction(new Notification.Action.Builder(null, "Disconnect", stop).build()).build();
    }

    void message(ChatMessage message, String scope, long connection)
    {
        if (message.outgoing)
        {
            return;
        }
        String receipt = connection + ":" + message.id;
        if (!receivedMessages.add(receipt))
        {
            return;
        }
        while (receivedMessages.size() > 256)
        {
            receivedMessages.remove(receivedMessages.iterator().next());
        }

        // count on the launcher icon
        unreadCount++;
        LauncherIconBadge.apply(context, unreadCount);
        String body = message.conversationId.startsWith("user:") ? "You received a private message" : "You received a channel message";
        showConversation(message.conversationId, scope, connection, "New message", body);
    }

    void poke(PokeMessage poke, String scope)
    {
        String body = poke.text.isEmpty() ? "Poked you" : poke.text;
        showConversation(poke.conversationId, scope, poke.connectionId, poke.senderName + " poked you", body);
    }

    private void showConversation(String conversation, String scope, long connection, String title, String body)
    {
        String tag = "conversation:" + scope + ":" + conversation;
        Intent intent = new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        intent.setData(Uri.parse("lemon-native://message/" + Uri.encode(scope) + "/" + Uri.encode(conversation) + "/" + connection));
        intent.putExtra(ChatService.EXTRA_CONVERSATION_ID, conversation).putExtra(ChatService.EXTRA_CONVERSATION_SCOPE, scope).putExtra(ChatService.EXTRA_CONNECTION_ID, connection);
        PendingIntent open = PendingIntent.getActivity(context, 0, intent, flags());
        Notification publicVersion = new Notification.Builder(context, MESSAGE_CHANNEL).setSmallIcon(R.drawable.ic_lemon).setContentTitle("Lemon Native").setContentText("You have a new notification").build();
        Notification notification = new Notification.Builder(context, MESSAGE_CHANNEL).setSmallIcon(R.drawable.ic_lemon).setContentTitle(title).setContentText(body).setStyle(new Notification.BigTextStyle().bigText(body)).setContentIntent(open).setAutoCancel(true).setCategory(Notification.CATEGORY_MESSAGE).setVisibility(Notification.VISIBILITY_PRIVATE).setPublicVersion(publicVersion).build();
        conversationTags.remove(tag);
        conversationTags.add(tag);
        while (conversationTags.size() > 32)
        {
            String oldest = conversationTags.iterator().next();
            conversationTags.remove(oldest);
            manager.cancel(oldest, 2);
        }
        post(tag, notification);
    }

    void call(CallInvitation call)
    {
        clearCall();
        String tag = "call:" + call.connectionId + ":" + call.callId;
        activeCallTag = tag;
        Intent open = callIntent(call, false);
        PendingIntent content = PendingIntent.getActivity(context, 0, open, flags());
        PendingIntent answer = PendingIntent.getActivity(context, 1, callIntent(call, true), flags());
        Intent declineIntent = new Intent(context, ChatService.class).setAction(ChatService.DECLINE_ACTION).setData(Uri.parse("lemon-native://decline/" + call.connectionId + "/" + Uri.encode(call.callId)));
        declineIntent.putExtra(ChatService.EXTRA_CALL_ID, call.callId).putExtra(ChatService.EXTRA_CONNECTION_ID, call.connectionId);
        PendingIntent decline = PendingIntent.getService(context, 2, declineIntent, flags());
        Notification publicVersion = new Notification.Builder(context, CALL_CHANNEL).setSmallIcon(R.drawable.ic_lemon).setContentTitle("Incoming channel call").setContentText("Open Lemon Native to answer").build();
        Notification.Builder builder = new Notification.Builder(context, CALL_CHANNEL).setSmallIcon(R.drawable.ic_lemon).setContentTitle("Incoming channel call").setContentText(call.callerName + " invites you to their channel").setContentIntent(content).setCategory(Notification.CATEGORY_CALL).setVisibility(Notification.VISIBILITY_PRIVATE).setPublicVersion(publicVersion).setOngoing(true).setTimeoutAfter(Math.max(1, Math.min(60000, (call.expiresAtNanos - System.nanoTime()) / 1000000L)));
        if (Build.VERSION.SDK_INT < 34 || manager.canUseFullScreenIntent())
        {
            builder.setFullScreenIntent(content, true);
        }
        if (Build.VERSION.SDK_INT >= 31)
        {
            builder.setStyle(Notification.CallStyle.forIncomingCall(new Person.Builder().setName(call.callerName).setImportant(true).build(), decline, answer));
        }
        else
        {
            builder.addAction(new Notification.Action.Builder(null, "Decline", decline).build());
            builder.addAction(new Notification.Action.Builder(null, "Answer", answer).build());
        }
        post(tag, builder.build());
    }

    private Intent callIntent(CallInvitation call, boolean answer)
    {
        Intent intent = new Intent(context, IncomingCallActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        intent.setData(Uri.parse("lemon-native://call/" + call.connectionId + "/" + Uri.encode(call.callId) + "/" + answer));
        intent.putExtra(ChatService.EXTRA_CALL_ID, call.callId).putExtra(ChatService.EXTRA_CONNECTION_ID, call.connectionId).putExtra(ChatService.EXTRA_ANSWER_CALL, answer);
        return intent;
    }

    void endCall(CallInvitation call, String reason, String scope)
    {
        String tag = "call:" + call.connectionId + ":" + call.callId;
        manager.cancel(tag, 2);
        if (tag.equals(activeCallTag))
        {
            activeCallTag = null;
        }
        if ("expired".equals(reason))
        {
            Notification notification = new Notification.Builder(context, MESSAGE_CHANNEL).setSmallIcon(R.drawable.ic_lemon).setContentTitle("Missed channel call").setContentText("Open Lemon Native to see who is available").setContentIntent(PendingIntent.getActivity(context, 0, new Intent(context, MainActivity.class), flags())).setAutoCancel(true).setVisibility(Notification.VISIBILITY_PRIVATE).build();
            post("missed-call", notification);
        }
    }

    void clearCall()
    {
        if (activeCallTag != null)
        {
            manager.cancel(activeCallTag, 2);
            activeCallTag = null;
        }
    }

    void clearMessages()
    {
        for (String tag : conversationTags)
        {
            manager.cancel(tag, 2);
        }
        conversationTags.clear();
        manager.cancel("missed-call", 2);

        // plain launcher icon
        unreadCount = 0;
        LauncherIconBadge.apply(context, 0);
    }

    private void post(String tag, Notification notification)
    {
        if (Build.VERSION.SDK_INT >= 33 && context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
        {
            return;
        }
        try
        {
            if (manager.areNotificationsEnabled())
            {
                manager.notify(tag, 2, notification);
            }
        }
        catch (SecurityException error)
        {
            android.util.Log.w("LemonNative", "Notifications are unavailable", error);
        }
    }

    private static int flags()
    {
        return PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE;
    }
}
