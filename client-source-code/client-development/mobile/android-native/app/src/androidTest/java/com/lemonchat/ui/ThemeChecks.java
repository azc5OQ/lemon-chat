package com.lemonchat.ui;

import android.app.Instrumentation;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.os.SystemClock;
import android.view.View;
import android.view.ViewGroup;
import android.view.MotionEvent;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.ListView;
import android.widget.RadioGroup;
import android.widget.SeekBar;
import com.lemonchat.model.Attachment;
import com.lemonchat.model.Channel;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.model.Group;
import com.lemonchat.model.Member;
import com.lemonchat.model.SessionSnapshot;
import com.lemonchat.model.ServerEmoji;
import com.lemonchat.service.ChatService;
import com.lemonchat.service.ConnectionRestartJob;
import com.lemonchat.storage.HistoryPolicy;
import com.lemonchat.storage.HistoryMaintenanceJob;
import com.lemonchat.settings.LocalSettings;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.atomic.AtomicReference;

/** renders the real activity with isolated in memory conversation state */
public final class ThemeChecks
{
    private int checks;
    private Instrumentation instrumentation;
    private MainActivity activity;
    private ChatService service;
    private Object session;
    private Member bot;

    public static int verify(Instrumentation instrumentation) throws Exception
    {
        ThemeChecks suite = new ThemeChecks();
        suite.instrumentation = instrumentation;
        suite.run();
        return suite.checks;
    }

    private void run() throws Exception
    {
        Context context = instrumentation.getTargetContext();
        SharedPreferences preferences = context.getSharedPreferences("preferences", Context.MODE_PRIVATE);
        SharedPreferences history = context.getSharedPreferences(HistoryPolicy.PREFERENCES, Context.MODE_PRIVATE);
        SharedPreferences local = context.getSharedPreferences("local-settings", Context.MODE_PRIVATE);
        Map<String, ?> original = new HashMap<>(preferences.getAll());
        Map<String, ?> originalHistory = new HashMap<>(history.getAll());
        Map<String, ?> originalLocal = new HashMap<>(local.getAll());
        try
        {
            preferences.edit().putString("theme", "default-mobile").putBoolean("mobile-chat-visible", true).putBoolean("automatic_connection", false).putBoolean("reconnect_suppressed", true).putBoolean("connection_requested", false).putBoolean("saved_connection_valid", false).commit();
            SharedPreferences.Editor appearance = preferences.edit();
            for (NativeTheme theme : NativeTheme.ALL)
            {
                appearance.remove("theme-dark-" + theme.id);
            }
            appearance.commit();
            history.edit().putInt("retention_days", -1).putBoolean("erase_pending", false).commit();
            local.edit().clear().putBoolean("lemon_use_theme_chat_formatting", true).putBoolean("lemon_hide_mic", false).putBoolean("lemon_hide_mobile_mic", false).putBoolean("lemon_strip_vertical", false).putBoolean("lemon_auto_scroll", false).commit();
            activity = (MainActivity)instrumentation.startActivitySync(new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
            await(() -> field(activity, "service") != null && !Boolean.TRUE.equals(field(field(activity, "service"), "identityLoading")), "service initialized");
            onMain(() ->
            {
                service = (ChatService)field(activity, "service");
                session = service.session();
                fixture();
                activity.onChanged();
                openChat();
            });
            settled();
            onMain(() ->
            {
                EditText input = input();
                input.setText("a draft kept while changing themes");
                input.setSelection(2, 7);
                RetainedUiState retained = (RetainedUiState)field(activity, "retained");
                retained.attachments.draft = new Attachment("theme-draft", "notes.txt", "text/plain", new byte[]{1, 2, 3}, false);
                retained.attachments.owner = new AttachmentFiles.Ticket("theme-fixture", "channel:0", 1024, false, false);
            });
            for (NativeTheme theme : NativeTheme.ALL)
            {
                onMain(() -> activity.selectTheme(theme.id));
                settled();
                onMain(() -> verifyChat(theme));
                screenshot(theme.id + "-chat");
                verifyEmptyComposer(theme);
                onMain(() ->
                {
                    if (theme.hasNavigation())
                    {
                        navigation().getChildAt(2).performClick();
                    }
                    else
                    {
                        find("theme-chat-toggle").performClick();
                    }
                });
                settled();
                onMain(() -> verifyChannels(theme));
                screenshot(theme.id + "-channels");
                if (theme.tree)
                {
                    verifyMobileWorkspace(theme);
                }
                onMain(this::openChat);
                settled();
                onMain(() -> input().setSelection(2, 7));
            }
            onMain(() ->
            {
                activity.selectTheme("simple");
                navigation().getChildAt(3).performClick();
            });
            settled();
            onMain(() ->
            {
                View appearanceRow = description(activity.getWindow().getDecorView(), "Dark appearance");
                check(appearanceRow instanceof android.widget.Switch, "Simple exposes its appearance choice");
                appearanceRow.performClick();
            });
            settled();
            onMain(() ->
            {
                navigation().getChildAt(1).performClick();
                check(Palette.load(activity).dark, "Simple appearance toggle applies dark mode");
                check(input().getCurrentTextColor() == Palette.load(activity).text, "rebuilt input uses selected appearance");
                activity.selectTheme("mono");
                check(!Palette.load(activity).dark, "Mono Light keeps its named appearance");
                activity.selectTheme("simple");
                check(Palette.load(activity).dark, "appearance is remembered for its own theme");
                check(input().getText().toString().equals("a draft kept while changing themes"), "appearance changes keep the draft");
            });
            onMain(() -> activity.selectTheme("bluebell"));
            settled();
            onMain(() -> find("theme-strip-direction").performClick());
            await(() -> find("theme-member-strip") != null && "Channel people on the left".contentEquals(find("theme-member-strip").getContentDescription()), "bluebell moves people left");
            onMain(() ->
            {
                check(field(activity, "service") == service && service.session() == session, "direction change preserves service and session");
                check(input().getText().toString().equals("a draft kept while changing themes"), "direction change preserves draft");
                find("theme-strip-direction").performClick();
            });
            await(() -> find("theme-member-strip") != null && "People above the conversation".contentEquals(find("theme-member-strip").getContentDescription()), "bluebell restores top strip");
            onMain(() ->
            {
                ListView messages = (ListView)field(activity, "chatList");
                messages.setSelectionFromTop(10, 0);
            });
            settled();
            final int[] anchor = new int[1];
            onMain(() ->
            {
                anchor[0] = ((ListView)field(activity, "chatList")).getFirstVisiblePosition();
                activity.selectTheme("mono-dark");
            });
            settled();
            onMain(() -> check(((ListView)field(activity, "chatList")).getFirstVisiblePosition() == anchor[0], "theme switch preserves scroll anchor"));
            onMain(() -> verifyHistoryChoice(history));
            onMain(() -> verifyLargeStrip());
            verifyUiSize(local);
            verifyLiveReactionPicker();
            onMain(() -> checks += GroupAdminUiChecks.verify(activity));
            onMain(() -> checks += AdministrationChecks.verify(activity));
        }
        finally
        {
            try
            {
                if (activity != null)
                {
                    onMain(() -> activity.finish());
                    instrumentation.waitForIdleSync();
                }
                context.stopService(new Intent(context, ChatService.class));
                if (service != null)
                {
                    await(() -> Boolean.TRUE.equals(field(service, "destroyed")), "test service stopped");
                }
            }
            finally
            {
                restore(preferences, original);
                restore(history, originalHistory);
                restore(local, originalLocal);
                HistoryMaintenanceJob.schedule(context, HistoryPolicy.load(context));
                if (Boolean.TRUE.equals(original.get("saved_connection_valid")) && !Boolean.TRUE.equals(original.get("reconnect_suppressed")) && (Boolean.TRUE.equals(original.get("connection_requested")) || Boolean.TRUE.equals(original.get("automatic_connection"))))
                {
                    ConnectionRestartJob.schedule(context);
                }
            }
        }
    }

    private void fixture() throws Exception
    {
        Member local = new Member(1, 0, "You", "local", "local-key", false, false, 3, 0, "", Collections.emptyList(), 1);
        Member maya = new Member(2, 0, "Maya", "maya", "maya-key", false, false, 1, 0, "", Collections.singletonList(2), 1);
        Member sam = new Member(3, 1, "Sam", "sam", "sam-key", false, false, 4, 0, "");
        bot = new Member(4, 0, "Lemon Radio", "bot", "bot-key", true, false, 1, 0, "");
        SessionSnapshot snapshot = new SessionSnapshot(SessionSnapshot.Status.CONNECTED, "Connected", 1, 0, "You", true,
            Arrays.asList(new Channel(0, -1, "General", "", false, true), new Channel(1, 0, "Weekend plans", "", false, true), new Channel(2, 0, "Music", "", false, true)),
            Arrays.asList(local, maya, sam, bot), Arrays.asList(new Group(1, "Channel admin", "admin", true, -2), new Group(2, "Moderators", "normal", false, -1)), 73);
        set(service, "snapshot", snapshot);
        set(service, "activeScope", "theme-fixture");
        set(service, "voiceReady", true);
        set(service, "voiceStatus", "Ready");
        set(session, "published", snapshot);
        @SuppressWarnings("unchecked")
        Map<String, ArrayList<ChatMessage>> conversations = (Map<String, ArrayList<ChatMessage>>)field(service, "conversations");
        ArrayList<ChatMessage> messages = new ArrayList<>();
        String[] text = {"Anyone up for a walk this afternoon?", "Yes, let's meet at the park.", "Sounds good. See you there!"};
        for (int index = 0; index < 40; index++)
        {
            messages.add(new ChatMessage(index + 1, "channel:0", index % 2 == 0 ? 2 : 1, index % 2 == 0 ? "Maya" : "You", text[index % text.length], index % 2 != 0, 1794736800000L + index * 60000, "sent", index % 2 == 0 ? "maya-key" : "local-key", index + 1));
        }
        conversations.put("channel:0", messages);
        conversations.put(maya.conversationId, new ArrayList<>(Collections.singletonList(new ChatMessage(101, maya.conversationId, 2, "Maya", "See you at the park", false, 1794736800000L, "received", "maya-key", 101))));
    }

    private void verifyChat(NativeTheme theme) throws Exception
    {
        View root = (View)field(activity, "root");
        View header = find("theme-header");
        View nav = navigation();
        View compose = find("theme-composer");
        check(root.getTag().equals("theme-root:" + theme.id), theme.name + " selected in actual activity");
        check(field(activity, "service") == service && service.session() == session && service.snapshot().connectionId == 73, theme.name + " preserves live objects");
        check(input().getText().toString().equals("a draft kept while changing themes"), theme.name + " keeps draft");
        check(input().getSelectionStart() == 2 && input().getSelectionEnd() == 7, theme.name + " keeps text selection");
        RetainedUiState retained = (RetainedUiState)field(activity, "retained");
        check(retained.attachments.draft != null && retained.attachments.draft.name.equals("notes.txt") && retained.attachments.matches("theme-fixture", "channel:0"), theme.name + " keeps attachment owner");
        check(header.getHeight() > 0 && compose.getHeight() > 0 && input().getWidth() > dp(60), theme.name + " lays out usable content");
        ViewGroup faces = (ViewGroup)find("theme-header-avatars");
        check(faces != null && faces.getChildCount() == 3, theme.name + " displays the channel avatars");
        for (int index = 0; index < faces.getChildCount(); index++)
        {
            View face = faces.getChildAt(index);
            check(face.getTop() >= 0 && face.getBottom() <= faces.getHeight() && face.getLeft() >= 0 && face.getRight() <= faces.getWidth(), theme.name + " does not clip an avatar");
            if (index > 0)
            {
                check(face.getRight() < faces.getChildAt(index - 1).getLeft(), theme.name + " separates the avatar circles");
            }
        }
        verifyNavigation(theme, header, nav, compose);
        verifyMicrophone(root, compose);
        View strip = find("theme-member-strip");
        if (theme.strip == NativeTheme.NO_STRIP)
        {
            check(strip == null, theme.name + " leaves conversation space open");
        }
        else
        {
            check(strip != null && strip.isShown(), theme.name + " shows member strip");
            View botTile = description(strip, "Lemon Radio.");
            check(botTile != null && !botTile.isClickable() && !botTile.performClick(), theme.name + " bot cannot open chat");
            if (theme.tree)
            {
                check(find("theme-chat-channel-tree") == null, theme.name + " shows only current members beside chat");
                check(description(strip, "Sam.") == null, theme.name + " excludes other channels from the side strip");
                check(strip.getWidth() >= dp(76) && strip.getWidth() <= dp(110), theme.name + " bounds phone member column");
                check(botTile instanceof LinearLayout && ((LinearLayout)botTile).getOrientation() == LinearLayout.HORIZONTAL, theme.name + " uses compact member rows");
                if (activity.getResources().getConfiguration().fontScale <= 1.1f)
                {
                    check(botTile.getHeight() <= dp(56), theme.name + " keeps short member rows compact");
                }
            }
            else if (theme.strip == NativeTheme.MOVABLE_STRIP)
            {
                check(strip.getLayoutParams().height == ViewGroup.LayoutParams.WRAP_CONTENT, "Bluebell measures its horizontal strip");
                View group = description(strip, "Channel admin");
                check(group != null && y(group) + group.getHeight() <= y(strip) + strip.getHeight(), "Bluebell group icon is not clipped");
            }
        }
    }

    private void verifyChannels(NativeTheme theme) throws Exception
    {
        View root = (View)field(activity, "root");
        check(theme.tree == (field(activity, "channelTree") != null), theme.name + " uses its channel layout");
        verifyMicrophone(root, null);
        View send = find("theme-send");
        check(send == null || !send.isShown(), theme.name + " does not show a chat composer on Channels");
        View content = (View)field(activity, "content");
        if (theme.quilt)
        {
            check(content.getBackground() instanceof ThemePanelDrawable, "Pocket uses native quilted panel");
        }
    }

    private void verifyMobileWorkspace(NativeTheme theme) throws Exception
    {
        onMain(() ->
        {
            ChannelTreeView tree = (ChannelTreeView)field(activity, "channelTree");
            View content = (View)field(activity, "content");
            check(tree.getWidth() == content.getWidth(), theme.name + " hidden chat gives the hierarchy the full width");
            check(!navigation().isShown() && "Show chat".contentEquals(find("theme-chat-toggle").getContentDescription()), theme.name + " keeps one workspace with an eye toggle");
            View general = description(tree, "General.");
            check(general != null && general.getBackground() instanceof android.graphics.drawable.RippleDrawable, theme.name + " channel rows show touch feedback");
            general.performLongClick();
            android.widget.PopupMenu menu = (android.widget.PopupMenu)field(tree, "channelMenu");
            check(menu != null && menu.getMenu().size() == 2, theme.name + " opens a real anchored channel menu");
            check("Join".contentEquals(menu.getMenu().getItem(0).getTitle()) && !menu.getMenu().getItem(0).isEnabled(), theme.name + " does not offer rejoining the current channel");
            menu.dismiss();
            find("theme-all-members").performClick();
        });
        instrumentation.waitForIdleSync();
        SystemClock.sleep(220);
        onMain(() ->
        {
            MemberDrawer drawer = (MemberDrawer)field(activity, "openDialog");
            View decor = drawer.getWindow().getDecorView();
            View panel = decor.findViewWithTag("theme-members-drawer");
            check(drawer.isShowing() && drawer.getWindow().getAttributes().gravity == (android.view.Gravity.RIGHT | android.view.Gravity.TOP), theme.name + " opens members from the right");
            check(panel.getTranslationX() == 0 && panel.getWidth() < activity.getResources().getDisplayMetrics().widthPixels, theme.name + " drawer leaves the workspace visible");
            ListView list = decor.findViewWithTag("theme-members-list");
            check(list.getAdapter().getCount() == 4, theme.name + " drawer includes every visible server member");
            ((EditText)decor.findViewWithTag("theme-members-search")).setText("Sam");
            check(list.getAdapter().getCount() == 1 && ((Member)list.getAdapter().getItem(0)).channelId == 1, theme.name + " searches members in another channel");
            ((EditText)decor.findViewWithTag("theme-members-search")).setText("Lemon");
        });
        instrumentation.waitForIdleSync();
        onMain(() ->
        {
            MemberDrawer drawer = (MemberDrawer)field(activity, "openDialog");
            View decor = drawer.getWindow().getDecorView();
            View botRow = decor.findViewWithTag("theme-drawer-member:4");
            check(botRow != null && !botRow.isClickable() && !botRow.performClick(), theme.name + " drawer cannot open a music bot conversation");
            ((EditText)decor.findViewWithTag("theme-members-search")).setText("Maya");
        });
        instrumentation.waitForIdleSync();
        screenshot(theme.id + "-member-drawer");
        onMain(() ->
        {
            MemberDrawer drawer = (MemberDrawer)field(activity, "openDialog");
            View decor = drawer.getWindow().getDecorView();
            View maya = decor.findViewWithTag("theme-drawer-member:2");
            check(maya != null && maya.getBackground() instanceof android.graphics.drawable.RippleDrawable, theme.name + " member rows show touch feedback");
            check(textView(maya, "See you at the park") != null, theme.name + " drawer previews the latest direct message");
            maya.performClick();
            check(!drawer.isShowing() && field(activity, "peerConversation").equals(service.snapshot().members.get(1).conversationId), theme.name + " human selection opens their private conversation");
            check(field(activity, "service") == service && service.session() == session, theme.name + " drawer navigation preserves the connection");
            RetainedUiState retained = (RetainedUiState)field(activity, "retained");
            check("a draft kept while changing themes".equals(retained.drafts.get("channel:0")), theme.name + " opening a private conversation keeps the channel draft");
            find("theme-chat-toggle").performClick();
        });
        settled();
        onMain(() ->
        {
            ChannelTreeView tree = (ChannelTreeView)field(activity, "channelTree");
            description(tree, "General.").performClick();
            check(Integer.valueOf(2).equals(field(activity, "screen")), theme.name + " joining preserves hidden chat");
            find("theme-chat-toggle").performClick();
        });
        settled();
        onMain(() ->
        {
            check(field(activity, "peerConversation") == null && input().getText().toString().equals("a draft kept while changing themes"), theme.name + " eye restores the channel draft");
            find("theme-chat-toggle").performClick();
        });
        settled();
    }

    private void verifyEmptyComposer(NativeTheme theme) throws Exception
    {
        RetainedUiState retained = (RetainedUiState)field(activity, "retained");
        Attachment attachment = retained.attachments.draft;
        AttachmentFiles.Ticket owner = retained.attachments.owner;
        int previous = find("theme-composer-dock").getHeight();
        onMain(() ->
        {
            retained.attachments.draft = null;
            retained.attachments.owner = null;
            input().setText("");
            activity.onChanged();
        });
        settled();
        onMain(() ->
        {
            check(find("theme-composer-dock").getHeight() < previous, theme.name + " collapses the empty draft");
            check(!((View)field(activity, "draftPreview")).isShown(), theme.name + " removes empty attachment space");
            check(input().getLineCount() == 1, theme.name + " returns to a single line input");
        });
        screenshot(theme.id + "-empty");
        onMain(() ->
        {
            retained.attachments.draft = attachment;
            retained.attachments.owner = owner;
            input().setText("a draft kept while changing themes");
            input().setSelection(2, 7);
            activity.onChanged();
        });
        settled();
    }

    private void verifyNavigation(NativeTheme theme, View header, View nav, View compose)
    {
        if (!theme.hasNavigation())
        {
            check(!nav.isShown() && ((ViewGroup)nav).getChildCount() == 0, theme.name + " has no page navigation bar");
            check(find("theme-profile") != null, theme.name + " keeps profile and local settings in its toolbar");
            check(find("theme-chat-toggle") != null && find("theme-all-members") != null, theme.name + " has separate chat visibility and member controls");
            check(find("theme-chat-toggle").getBackground() instanceof android.graphics.drawable.RippleDrawable, theme.name + " toolbar reacts to touch and keyboard focus");
            return;
        }
        String[] labels = {"People", "Chat", "Channels", "Me"};
        LinearLayout navigation = (LinearLayout)nav;
        check(navigation.getChildCount() == 4, theme.name + " has all navigation targets");
        for (int index = 0; index < labels.length; index++)
        {
            check(labels[index].contentEquals(navigation.getChildAt(index).getContentDescription()), theme.name + " navigation order " + index);
            View target = navigation.getChildAt(index);
            TextView label = target.findViewWithTag("theme-nav-label:" + labels[index]);
            check(target.getHeight() >= dp(48) && target.getWidth() >= dp(48), theme.name + " keeps a usable navigation target");
            check(label != null && label.getLineCount() == 1, theme.name + " keeps the navigation label on one line");
            check(label.getLayout() != null && label.getLayout().getEllipsisCount(0) == 0, theme.name + " shows the complete navigation label");
        }
        check(theme.tree ? y(nav) >= y(header) + header.getHeight() && y(nav) < y(compose) : y(nav) >= y(compose) + compose.getHeight(), theme.name + " positions navigation correctly");
    }

    private void verifyMicrophone(View root, View compose)
    {
        View microphone = find("theme-microphone");
        check(microphone != null && microphone.isShown(), "microphone stays available");
        check(Math.abs((x(microphone) + microphone.getWidth() / 2) - (x(root) + root.getWidth() / 2)) <= dp(2), "microphone is centered");
        if (compose != null)
        {
            View send = find("theme-send");
            check(send != null && send != microphone && send.isShown(), "send remains separate from microphone");
            check(y(microphone) + microphone.getHeight() <= y(compose), "microphone stays above composer");
            check(x(send) + send.getWidth() <= x(root) + root.getWidth(), "send stays inside screen");
        }
    }

    private void screenshot(String name) throws Exception
    {
        onMain(() ->
        {
            View root = (View)field(activity, "root");
            Object dialog = field(activity, "openDialog");
            if (dialog instanceof MemberDrawer && ((MemberDrawer)dialog).isShowing())
            {
                root = ((MemberDrawer)dialog).getWindow().getDecorView();
            }
            Bitmap bitmap = Bitmap.createBitmap(root.getWidth(), root.getHeight(), Bitmap.Config.ARGB_8888);
            root.draw(new android.graphics.Canvas(bitmap));
            File directory = activity.getExternalFilesDir("theme-checks");
            if (directory == null)
            {
                throw new IllegalStateException("No screenshot directory");
            }
            try (FileOutputStream output = new FileOutputStream(new File(directory, name + ".png")))
            {
                bitmap.compress(Bitmap.CompressFormat.PNG, 100, output);
            }
            finally
            {
                bitmap.recycle();
            }
        });
    }

    private void verifyHistoryChoice(SharedPreferences preferences)
    {
        Map<String, ?> saved = new HashMap<>(preferences.getAll());
        HistoryPolicy before = service.historyPolicy();
        boolean[] completed = {false};
        AlertDialog dialog = HistoryDialog.show(activity, service, true, () -> completed[0] = true);
        try
        {
            RadioGroup choices = dialog.getWindow().getDecorView().findViewWithTag("history-retention-choices");
            check(choices != null && choices.getCheckedRadioButtonId() == choices.getChildAt(0).getId(), "history prompt initially selects Don't save");
            check(choices.getChildCount() == 6, "history prompt offers all retention choices");
            check(dialog.getButton(AlertDialog.BUTTON_POSITIVE).getText().toString().equalsIgnoreCase("Continue"), "history prompt requires explicit continuation");
            choices.check(choices.getChildAt(4).getId());
            check(service.historyPolicy() == before && preferences.getAll().equals(saved) && !completed[0], "choosing a duration alone does not grant consent");
        }
        finally
        {
            dialog.dismiss();
        }
        dialog = HistoryDialog.show(activity, service, false, () -> completed[0] = true);
        try
        {
            check(dialog.getButton(AlertDialog.BUTTON_NEUTRAL) != null && dialog.getButton(AlertDialog.BUTTON_NEUTRAL).isShown(), "saved history can be cleared early from settings");
        }
        finally
        {
            dialog.dismiss();
        }
    }

    private void verifyLiveReactionPicker() throws Exception
    {
        SessionSnapshot original = service.snapshot();
        AlertDialog[] opened = new AlertDialog[1];
        try
        {
            onMain(() ->
            {
                ServerEmoji first = new ServerEmoji("1", "First", "☺", "");
                set(service, "snapshot", withEmojis(original, Collections.singletonList(first), original.connectionId));
                java.lang.reflect.Method open = MainActivity.class.getDeclaredMethod("showReactions", ChatMessage.class);
                open.setAccessible(true);
                open.invoke(activity, service.messages("channel:0").get(0));
                AlertDialog picker = (AlertDialog)field(activity, "reactionPicker");
                opened[0] = picker;
                android.widget.BaseAdapter adapter = (android.widget.BaseAdapter)field(activity, "reactionAdapter");
                check(picker.isShowing() && adapter.getCount() == 1, "reaction picker shows the server catalog");
                ServerEmoji second = new ServerEmoji("2", "Second", "♥", "");
                set(service, "snapshot", withEmojis(original, Arrays.asList(first, second), original.connectionId));
                adapter.notifyDataSetChanged();
                check(adapter.getCount() == 2 && ((ServerEmoji)adapter.getItem(1)).id.equals("2"), "open reaction picker accepts new server entries");
                set(service, "snapshot", withEmojis(original, Collections.singletonList(second), original.connectionId));
                adapter.notifyDataSetChanged();
                check(adapter.getCount() == 1 && ((ServerEmoji)adapter.getItem(0)).id.equals("2"), "open reaction picker removes deleted entries");
                set(service, "snapshot", withEmojis(original, Collections.singletonList(first), original.connectionId + 1));
                adapter.notifyDataSetChanged();
                check(!picker.isShowing(), "connection replacement closes the stale reaction picker");
            });
            await(() -> field(activity, "reactionAdapter") == null && field(activity, "reactionPicker") == null, "reaction picker dismiss callback");
            onMain(() -> check(field(activity, "reactionAdapter") == null && field(activity, "reactionPicker") == null, "dismiss callback clears the stale picker references"));
        }
        finally
        {
            onMain(() ->
            {
                if (opened[0] != null && opened[0].isShowing())
                {
                    opened[0].dismiss();
                }
                set(service, "snapshot", original);
            });
        }
    }

    private SessionSnapshot withEmojis(SessionSnapshot value, java.util.List<ServerEmoji> emojis, long connection)
    {
        return new SessionSnapshot(value.status, value.detail, value.clientId, value.channelId, value.username, value.channelKeysReady, value.channels, value.members, value.groups, connection, value.idle, value.idleAllowed, value.call, value.retryableFailure, value.icons, emojis, value.capabilities);
    }

    private void verifyUiSize(SharedPreferences preferences) throws Exception
    {
        Configuration platform = new Configuration(activity.getResources().getConfiguration());
        Configuration original = new Configuration(platform);
        Configuration small = UiScaleActivity.scaledConfiguration(platform, 75);
        Configuration large = UiScaleActivity.scaledConfiguration(platform, 130);
        check(platform.equals(original), "UI scaling does not mutate Android configuration");
        check(small.densityDpi == Math.round(platform.densityDpi * .75f), "75 percent scales app density once");
        check(large.densityDpi == Math.round(platform.densityDpi * 1.3f), "130 percent scales app density once");
        check(small.fontScale == platform.fontScale && large.fontScale == platform.fontScale, "UI scaling preserves Android font accessibility");
        check(UiScaleActivity.scaledConfiguration(platform, 100).equals(platform), "UI size reset restores original configuration");
        check(UiScaleActivity.scaledConfiguration(platform, 1).densityDpi == UiScaleActivity.scaledConfiguration(platform, 70).densityDpi, "UI size has a lower bound");
        check(UiScaleActivity.scaledConfiguration(platform, 500).densityDpi == large.densityDpi, "UI size has an upper bound");
        check(UiScaleActivity.class.isAssignableFrom(IncomingCallActivity.class), "incoming calls use the same app size context");
        Map<String, ?> saved = new HashMap<>(preferences.getAll());
        MainActivity before = activity;
        onMain(() ->
        {
            java.lang.reflect.Method show = MainActivity.class.getDeclaredMethod("showLocalSettings");
            show.setAccessible(true);
            show.invoke(activity);
        });
        instrumentation.waitForIdleSync();
        AlertDialog first = (AlertDialog)field(activity, "openDialog");
        onMain(() ->
        {
            View decor = first.getWindow().getDecorView();
            EditText chatSize = (EditText)description(decor, "Chat size (%)");
            chatSize.setText("110");
            SeekBar slider = decor.findViewWithTag("ui-scale-slider");
            check(slider != null && slider.getMax() == 60 && slider.getProgress() == 30, "UI size slider opens at 100 percent");
            slide(slider, 75);
        });
        await(() -> field(activity, "openDialog") != first, "settings preview rebuilt after release");
        instrumentation.waitForIdleSync();
        onMain(() ->
        {
            AlertDialog preview = (AlertDialog)field(activity, "openDialog");
            View decor = preview.getWindow().getDecorView();
            SeekBar slider = decor.findViewWithTag("ui-scale-slider");
            check(slider.getProgress() == 5, "UI size slider selects 75 percent");
            check(slider.getResources().getConfiguration().densityDpi == small.densityDpi, "settings preview actually uses smaller native views");
            check(slider.getResources().getConfiguration().fontScale == platform.fontScale, "settings preview retains system font scale");
            check(((EditText)description(decor, "Chat size (%)")).getText().toString().equals("110"), "preview retains other unsaved fields");
            check(activity == before && field(activity, "service") == service && service.session() == session, "preview does not recreate the activity or connection");
            check(activity.getResources().getConfiguration().densityDpi == platform.densityDpi, "preview does not resize the underlying app");
            check(preferences.getAll().equals(saved), "preview does not save settings");
            textView(decor, "Reset UI size to 100%").performClick();
        });
        instrumentation.waitForIdleSync();
        onMain(() ->
        {
            AlertDialog reset = (AlertDialog)field(activity, "openDialog");
            View decor = reset.getWindow().getDecorView();
            SeekBar slider = decor.findViewWithTag("ui-scale-slider");
            check(slider.getProgress() == 30 && slider.getResources().getConfiguration().densityDpi == platform.densityDpi, "Reset restores actual preview dimensions");
            check(((EditText)description(decor, "Chat size (%)")).getText().toString().equals("110"), "Reset preserves unrelated draft settings");
            slide(slider, 75);
        });
        instrumentation.waitForIdleSync();
        onMain(() ->
        {
            AlertDialog preview = (AlertDialog)field(activity, "openDialog");
            textView(preview.getWindow().getDecorView(), "Cancel").performClick();
            check(!preview.isShowing() && preferences.getAll().equals(saved), "Cancel discards UI size and other draft edits");
            check(activity.appliedUiScale() == 100 && service.localSettings().getInt("native_ui_scale") == 100, "Cancel keeps the saved app size");
            check(input().getText().toString().equals("a draft kept while changing themes"), "UI size preview preserves the message draft");
        });
    }

    private void slide(SeekBar slider, int percent)
    {
        float left = slider.getPaddingLeft();
        float width = slider.getWidth() - slider.getPaddingLeft() - slider.getPaddingRight();
        float point = left + width * (percent - 70) / 60f;
        long time = SystemClock.uptimeMillis();
        MotionEvent down = MotionEvent.obtain(time, time, MotionEvent.ACTION_DOWN, point, slider.getHeight() / 2f, 0);
        MotionEvent up = MotionEvent.obtain(time, time + 50, MotionEvent.ACTION_UP, point, slider.getHeight() / 2f, 0);
        try
        {
            slider.dispatchTouchEvent(down);
            slider.dispatchTouchEvent(up);
        }
        finally
        {
            down.recycle();
            up.recycle();
        }
    }

    private static TextView textView(View view, String value)
    {
        if (view instanceof TextView && value.contentEquals(((TextView)view).getText()))
        {
            return (TextView)view;
        }
        if (view instanceof ViewGroup)
        {
            ViewGroup group = (ViewGroup)view;
            for (int index = 0; index < group.getChildCount(); index++)
            {
                TextView found = textView(group.getChildAt(index), value);
                if (found != null)
                {
                    return found;
                }
            }
        }
        return null;
    }

    private void verifyLargeStrip() throws Exception
    {
        Configuration configuration = new Configuration(activity.getResources().getConfiguration());
        configuration.fontScale = 1.8f;
        Context largeText = activity.createConfigurationContext(configuration);
        ConversationMembers members = new ConversationMembers(largeText, new Palette(NativeTheme.find("bluebell"), true), LocalSettings.load(activity), (AvatarImages)field(activity, "avatarImages"), new ConversationMembers.Actions()
        {
            @Override
            public void open(Member member)
            {
            }

            @Override
            public void info(Member member)
            {
            }

            @Override
            public void allPeople()
            {
            }
        });
        members.render(service.snapshot());
        members.measure(View.MeasureSpec.makeMeasureSpec(activity.getResources().getDisplayMetrics().widthPixels, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(dp(300), View.MeasureSpec.AT_MOST));
        members.layout(0, 0, members.getMeasuredWidth(), members.getMeasuredHeight());
        View group = description(members, "Channel admin");
        check(group != null && group.getMeasuredHeight() > 0, "large text strip retains actual group icon");
        int bottom = group.getHeight();
        View child = group;
        while (child != members && child.getParent() instanceof View)
        {
            bottom += child.getTop();
            child = (View)child.getParent();
        }
        check(bottom <= members.getMeasuredHeight(), "large Android text does not clip strip icons");
        check(members.getMeasuredHeight() >= dp(70), "horizontal strip retains its usable minimum height");
    }

    private void settled() throws Exception
    {
        instrumentation.waitForIdleSync();
        await(() -> find("theme-header").getWidth() > 0 && (!find("theme-navigation").isShown() || find("theme-navigation").getHeight() > 0), "views measured");
        SystemClock.sleep(40);
        instrumentation.waitForIdleSync();
    }

    private void await(Condition condition, String label) throws Exception
    {
        long deadline = SystemClock.elapsedRealtime() + 15000;
        boolean[] ready = new boolean[1];
        while (SystemClock.elapsedRealtime() < deadline)
        {
            onMain(() -> ready[0] = condition.ready());
            if (ready[0])
            {
                return;
            }
            SystemClock.sleep(25);
        }
        throw new AssertionError("Timed out: " + label);
    }

    private void openChat()
    {
        if (navigation().isShown())
        {
            navigation().getChildAt(1).performClick();
        }
        else
        {
            View back = find("theme-back-chat");
            if (back != null)
            {
                back.performClick();
            }
            View toggle = find("theme-chat-toggle");
            if (toggle != null && "Show chat".contentEquals(toggle.getContentDescription()))
            {
                toggle.performClick();
            }
        }
    }

    private LinearLayout navigation()
    {
        return (LinearLayout)find("theme-navigation");
    }

    private EditText input()
    {
        return (EditText)find("theme-message-input");
    }

    private View find(String tag)
    {
        return activity.getWindow().getDecorView().findViewWithTag(tag);
    }

    private static View description(View view, String prefix)
    {
        if (view.getContentDescription() != null && view.getContentDescription().toString().startsWith(prefix))
        {
            return view;
        }
        if (view instanceof ViewGroup)
        {
            ViewGroup group = (ViewGroup)view;
            for (int index = 0; index < group.getChildCount(); index++)
            {
                View found = description(group.getChildAt(index), prefix);
                if (found != null)
                {
                    return found;
                }
            }
        }
        return null;
    }

    private static int x(View view)
    {
        int[] position = new int[2];
        view.getLocationOnScreen(position);
        return position[0];
    }

    private static int y(View view)
    {
        int[] position = new int[2];
        view.getLocationOnScreen(position);
        return position[1];
    }

    private int dp(int value)
    {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }

    private static Object field(Object target, String name) throws Exception
    {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(target);
    }

    private static void set(Object target, String name, Object value) throws Exception
    {
        Field field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(target, value);
    }

    private static void restore(SharedPreferences preferences, Map<String, ?> values)
    {
        SharedPreferences.Editor editor = preferences.edit().clear();
        for (Map.Entry<String, ?> entry : values.entrySet())
        {
            Object value = entry.getValue();
            if (value instanceof String)
            {
                editor.putString(entry.getKey(), (String)value);
            }
            else if (value instanceof Boolean)
            {
                editor.putBoolean(entry.getKey(), (Boolean)value);
            }
            else if (value instanceof Integer)
            {
                editor.putInt(entry.getKey(), (Integer)value);
            }
            else if (value instanceof Long)
            {
                editor.putLong(entry.getKey(), (Long)value);
            }
            else if (value instanceof Float)
            {
                editor.putFloat(entry.getKey(), (Float)value);
            }
            else if (value instanceof Set)
            {
                @SuppressWarnings("unchecked")
                Set<String> strings = (Set<String>)value;
                editor.putStringSet(entry.getKey(), strings);
            }
        }
        if (!editor.commit())
        {
            throw new AssertionError("Could not restore test preferences");
        }
    }

    private void onMain(Action action) throws Exception
    {
        AtomicReference<Throwable> failure = new AtomicReference<>();
        instrumentation.runOnMainSync(() ->
        {
            try
            {
                action.run();
            }
            catch (Throwable error)
            {
                failure.set(error);
            }
        });
        Throwable error = failure.get();
        if (error != null)
        {
            throw new AssertionError("Theme UI check failed", error);
        }
    }

    private void check(boolean condition, String label)
    {
        if (!condition)
        {
            throw new AssertionError(label);
        }
        checks++;
    }

    private interface Action
    {
        void run() throws Exception;
    }

    private interface Condition
    {
        boolean ready() throws Exception;
    }
}
