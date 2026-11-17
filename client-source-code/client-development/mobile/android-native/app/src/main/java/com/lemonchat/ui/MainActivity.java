package com.lemonchat.ui;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.ActivityNotFoundException;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.text.Editable;
import android.text.InputType;
import android.text.InputFilter;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.BaseAdapter;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import com.lemonchat.crypto.Identity;
import com.lemonchat.crypto.IdentityFile;
import com.lemonchat.model.Channel;
import com.lemonchat.model.Attachment;
import com.lemonchat.model.CallInvitation;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.model.ChannelTree;
import com.lemonchat.model.Group;
import com.lemonchat.model.Member;
import com.lemonchat.model.PokeMessage;
import com.lemonchat.model.ServerIcon;
import com.lemonchat.model.ServerEmoji;
import com.lemonchat.model.SessionSnapshot;
import com.lemonchat.protocol.ServerAddress;
import com.lemonchat.service.ChatService;
import com.lemonchat.service.BackgroundPermissions;
import com.lemonchat.settings.LocalSettings;
import java.text.DateFormat;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.Supplier;

/** renders native conversations without owning the connection */
public final class MainActivity extends UiScaleActivity implements ChatService.Observer
{
    private static final int IMPORT_IDENTITY = 10;
    private static final int EXPORT_IDENTITY = 11;
    private static final int RECORD_AUDIO = 12;
    private static final int NOTIFICATIONS = 13;
    private static final int PICK_ATTACHMENT = 14;
    private static final int SAVE_ATTACHMENT = 15;
    private static final int PICK_MUSIC = 16;
    private RetainedUiState retained;
    private final RetainedUiState.Listener fileListener = this::onFilesChanged;
    private final AttachmentFiles.Listener attachmentListener = this::onAttachmentChanged;
    private Dialog importDialog;
    private Button importConfirm;
    private SharedPreferences preferences;
    private Palette palette;
    private ChatService service;
    private boolean bound;
    private boolean started;
    private int screen;
    private String peerConversation;
    private String renderedConversation;
    private long renderedConnectionId = -1;
    private String renderedScope;
    private int messagesFingerprint;
    private LinearLayout root;
    private LinearLayout header;
    private FrameLayout content;
    private LinearLayout voiceDock;
    private LinearLayout composerDock;
    private LinearLayout navigation;
    private ListView chatList;
    private LinearLayout messageHeader;
    private MessageAdapter messageAdapter;
    private int messageRenderVersion;
    private EditText composer;
    private ImageButton send;
    private TextView composerNotice;
    private TextView voiceLabel;
    private ImageButton microphone;
    private Dialog openDialog;
    private ListView peopleList;
    private ListView channelsList;
    private ChannelTreeView channelTree;
    private ScrollView meScroll;
    private String meFingerprint;
    private Intent pendingNavigation;
    private String presentedCall;
    private LocalSettings localSettings;
    private AvatarImages avatarImages;
    private AttachmentImages attachmentImages;
    private EmojiImages emojiImages;
    private BaseAdapter reactionAdapter;
    private AlertDialog reactionPicker;
    private ConversationMembers conversationMembers;
    // the content of the last build of the toolbar and of the navigation; null builds them again
    private String headerContent;
    private String navigationContent;
    private LinearLayout draftPreview;
    private EditText searchInput;
    private boolean keyboardTalking;
    private boolean searchOpen;
    private long renderedNewestMessage;
    private boolean restoringTheme;
    private boolean resizingUi;
    private Set<Integer> themeCollapsedChannels = new HashSet<>();
    private long themeTreeConnection = -1;

    private final ServiceConnection connection = new ServiceConnection()
    {
        @Override
        public void onServiceConnected(ComponentName name, IBinder binder)
        {
            if (!started)
            {
                return;
            }
            service = ((ChatService.LocalBinder) binder).service();
            service.addObserver(MainActivity.this);
            onFilesChanged();
            onAttachmentChanged();
            if (retained.connectionDraft != null)
            {
                showConnection();
            }
        }

        @Override
        public void onServiceDisconnected(ComponentName name)
        {
            service = null;
            render();
        }
    };

    @Override
    public void onCreate(Bundle saved)
    {
        super.onCreate(saved);
        preferences = getSharedPreferences("preferences", MODE_PRIVATE);
        localSettings = LocalSettings.load(this);
        Object previous = getLastNonConfigurationInstance();
        retained = previous instanceof RetainedUiState ? (RetainedUiState) previous : new RetainedUiState();
        avatarImages = new AvatarImages(getResources(), this::onImagesChanged);
        attachmentImages = new AttachmentImages(getResources(), this::onImagesChanged);
        emojiImages = new EmojiImages(getResources(), this::onImagesChanged, id ->
        {
            if (service != null)
            {
                service.session().requestEmoji(id);
            }
        });
        if (saved != null)
        {
            screen = saved.getInt("screen", 0);
            peerConversation = saved.getString("peer");
            renderedConnectionId = saved.getLong("connection", -1);
            renderedScope = saved.getString("scope");
            pendingNavigation = saved.getParcelable("navigation");
            presentedCall = saved.getString("presented-call");
        }
        else if (treeLayout())
        {
            screen = 2;
        }
        if (saved == null)
        {
            rememberNavigation(getIntent());
        }
        applyPalette();
        if (!palette.theme.hasNavigation() && (saved == null || screen == 0))
        {
            screen = preferences.getBoolean("mobile-chat-visible", true) ? 1 : 2;
        }
        buildShell();
        render();
    }

    @Override
    public void onStart()
    {
        super.onStart();
        started = true;
        meFingerprint = null;
        retained.attach(fileListener);
        retained.attachments.attach(attachmentListener);
        bound = bindService(new Intent(this, ChatService.class), connection, Context.BIND_AUTO_CREATE);
    }

    @Override
    public void onStop()
    {
        started = false;
        retained.detach(fileListener);
        retained.attachments.detach(attachmentListener);
        keyboardTalking = false;
        if (service != null)
        {
            if (!service.musicActive())
            {
                service.setTalking(false);
            }
            service.removeObserver(this);
        }
        if (bound)
        {
            unbindService(connection);
            bound = false;
        }
        service = null;
        super.onStop();
    }

    @Override
    public void onNewIntent(Intent intent)
    {
        super.onNewIntent(intent);
        setIntent(intent);
        rememberNavigation(intent);
        onChanged();
    }

    @Override
    public void onSaveInstanceState(Bundle state)
    {
        if (openDialog instanceof ConnectionWizard)
        {
            ((ConnectionWizard)openDialog).capture();
        }
        state.putInt("screen", screen);
        state.putString("peer", peerConversation);
        preserveDraft();
        state.putLong("connection", renderedConnectionId);
        state.putString("scope", renderedScope);
        state.putParcelable("navigation", pendingNavigation);
        state.putString("presented-call", presentedCall);
        super.onSaveInstanceState(state);
    }

    @Override
    public Object onRetainNonConfigurationInstance()
    {
        preserveDraft();
        if (openDialog instanceof ConnectionWizard)
        {
            ((ConnectionWizard)openDialog).capture();
        }
        return retained;
    }

    @Override
    public void onDestroy()
    {
        if (openDialog != null)
        {
            openDialog.dismiss();
        }
        retained.detach(fileListener);
        retained.attachments.detach(attachmentListener);
        avatarImages.close();
        attachmentImages.close();
        emojiImages.close();
        if (!isChangingConfigurations())
        {
            retained.close();
        }
        super.onDestroy();
    }

    @Override
    public void onChanged()
    {
        if (started)
        {
            if (applySavedUiScale())
            {
                return;
            }
            SessionSnapshot snapshot = state();
            if (snapshot != null && snapshot.connectionId > 0)
            {
                String scope = service.conversationScope();
                boolean scopeChanged = renderedScope != null && !scope.isEmpty() && !renderedScope.equals(scope);
                if (scopeChanged || renderedConnectionId != -1 && renderedConnectionId != snapshot.connectionId)
                {
                    preserveDraft();
                    if (scopeChanged)
                    {
                        retained.drafts.clear();
                        peerConversation = null;
                        retained.attachments.clearDraft();
                        attachmentImages.close();
                    }
                    else
                    {
                        retained.drafts.keySet().removeIf(conversation -> !conversation.startsWith("user:"));
                        if (retained.attachments.owner != null && !retained.attachments.owner.conversation.startsWith("user:"))
                        {
                            retained.attachments.clearDraft();
                        }
                    }
                    retained.pendingConversation = null;
                    retained.pendingText = null;
                    retained.reply = null;
                    retained.pendingReply = null;
                    retained.replyConversation = null;
                    renderedConversation = null;
                    composer = null;
                }
                renderedConnectionId = snapshot.connectionId;
                if (!scope.isEmpty())
                {
                    renderedScope = scope;
                }
            }
            finishPendingDraft();
            openPendingNavigation();
            render();
            restoreResizedLayout();
            showIncomingCall();
            showHistoryConsent();
        }
    }

    @Override
    public void onProblem(String message)
    {
        retained.pendingConversation = null;
        retained.pendingText = null;
        retained.attachments.sending = false;
        if (started)
        {
            showNotice(message);
            render();
        }
    }

    private void applyPalette()
    {
        palette = Palette.load(this);
        setTheme(palette.theme.windowStyle(palette.dark));
        applySystemBars();
    }

    private void applySystemBars()
    {
        Window window = getWindow();
        View decor = window.getDecorView();
        window.setStatusBarColor(Color.TRANSPARENT);
        window.setNavigationBarColor(Color.TRANSPARENT);
        if (Build.VERSION.SDK_INT >= 30)
        {
            window.setDecorFitsSystemWindows(false);
            int appearance = palette.dark || palette.theme.quilt ? 0 : android.view.WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS | android.view.WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;
            android.view.WindowInsetsController controller = window.getInsetsController();
            if (controller != null)
            {
                controller.setSystemBarsAppearance(appearance, android.view.WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS | android.view.WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS);
            }
        }
        else
        {
            int flags = View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN;
            if (!palette.dark && !palette.theme.quilt)
            {
                flags |= View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
            }
            decor.setSystemUiVisibility(flags);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean focused)
    {
        super.onWindowFocusChanged(focused);
        if (focused && palette != null)
        {
            applySystemBars();
            markVisibleConversationRead();
        }
        if (!focused && keyboardTalking)
        {
            keyboardTalking = false;
            if (service != null && !service.musicActive())
            {
                service.setTalking(false);
            }
        }
    }

    private void buildShell()
    {
        root = vertical();
        root.setBackgroundColor(palette.bar);
        root.setTag("theme-root:" + palette.theme.id);
        root.setOnApplyWindowInsetsListener((view, insets) ->
        {
            if (Build.VERSION.SDK_INT >= 30)
            {
                android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.ime());
                view.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            }
            else
            {
                view.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(), insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            }
            return insets;
        });
        header = horizontal();
        header.setTag("theme-header");
        header.setBackground(palette.barSurface());
        header.setPadding(dp(8), dp(4), dp(8), dp(4));
        root.addView(header, new LinearLayout.LayoutParams(-1, dp(palette.theme.toolbarHeight)));
        content = new FrameLayout(this);
        content.setBackground(palette.panelSurface(getResources().getDisplayMetrics().density));

        // The microphone lies on the lower edge of the chat area, drawn over the chat, so that the chat keeps its
        // full height down to the message field.
        FrameLayout chatArea = new FrameLayout(this);
        chatArea.addView(content, new FrameLayout.LayoutParams(-1, -1));
        root.addView(chatArea, new LinearLayout.LayoutParams(-1, 0, 1));
        voiceDock = vertical();
        voiceDock.setTag("theme-voice-dock");
        voiceDock.setGravity(Gravity.CENTER);
        voiceDock.setPadding(dp(12), dp(2), dp(12), dp(4));
        chatArea.addView(voiceDock, new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL));
        composerDock = vertical();
        composerDock.setTag("theme-composer-dock");
        composerDock.setBackgroundColor(palette.background);
        root.addView(composerDock, new LinearLayout.LayoutParams(-1, -2));
        navigation = horizontal();
        navigation.setTag("theme-navigation");
        navigation.setPadding(dp(palette.theme.tree ? 8 : 12), dp(palette.theme.tree ? 2 : 6), dp(palette.theme.tree ? 8 : 12), dp(palette.theme.tree ? 2 : 8));
        navigation.setBackground(palette.barSurface());
        root.addView(navigation, palette.theme.tree ? 1 : root.getChildCount(), new LinearLayout.LayoutParams(-1, dp(palette.theme.navigationHeight)));
        renderedConversation = null;
        composer = null;
        meScroll = null;
        meFingerprint = null;
        setContentView(root);
    }

    private void render()
    {
        if (root == null || isFinishing())
        {
            return;
        }
        avatarImages.update(state());
        emojiImages.update(state());
        if (openDialog instanceof MemberDrawer && openDialog.isShowing())
        {
            ((MemberDrawer) openDialog).render(state());
        }
        if (reactionAdapter != null)
        {
            reactionAdapter.notifyDataSetChanged();
        }
        renderHeader();
        renderNavigation();
        boolean chatAvailable = service != null && state() != null && state().connected && (peerConversation != null || state().channelId >= 0);
        composerDock.setVisibility(screen == 1 && chatAvailable ? View.VISIBLE : View.GONE);
        if (screen == 1 && chatAvailable)
        {
            renderChat();
        }
        else
        {
            preserveDraft();
            renderedConversation = null;
            composer = null;
            View browsing = screen == 0 ? peopleList : screen == 2 ? treeLayout() ? channelTree : channelsList : screen == 3 ? meScroll : null;
            if (browsing == null || browsing.getParent() != content || screen != 3 && (state() == null || !state().connected))
            {
                content.removeAllViews();
            }
            if (screen == 3)
            {
                renderMe();
            }
            else if (state() == null || !state().connected)
            {
                renderWelcome();
            }
            else if (screen == 2)
            {
                renderChannels();
            }
            else
            {
                renderPeople();
            }
        }
        renderVoice();
    }

    private void renderHeader()
    {
        SessionSnapshot snapshot = state();
        Member shownPeer = selectedPeer();
        StringBuilder toolbarText = new StringBuilder().append(screen).append('|').append(System.identityHashCode(palette)).append('|')
            .append(peerConversation).append('|').append(shownPeer == null ? "-" : ConversationMembers.describe(shownPeer)).append('|')
            .append(conversationTitle()).append('|').append(localSettings.getBoolean("lemon_chat_search")).append('|')
            .append(localSettings.getBoolean("lemon_strip_vertical")).append('|').append(canCall(shownPeer)).append('|');
        if (snapshot != null)
        {
            toolbarText.append(snapshot.connected).append(',').append(snapshot.idle).append(',').append(snapshot.username).append(',')
                .append(snapshot.detail).append(',').append(snapshot.channelId).append('|');
            for (Member member : membersInChannel(snapshot.channelId))
            {
                toolbarText.append(ConversationMembers.describe(member));
            }
        }

        // unchanged toolbar
        if (toolbarText.toString().equals(headerContent) && header.getChildCount() > 0)
        {
            return;
        }
        headerContent = toolbarText.toString();
        header.removeAllViews();
        header.getLayoutParams().height = dp(palette.theme.toolbarHeight);
        if (!palette.theme.hasNavigation())
        {
            boolean profilePage = screen == 3;
            ImageButton profile = toolbarButton(profilePage ? "back" : "me", profilePage ? "Back to conversations" : "Your profile and local settings");
            profile.setTag(profilePage ? "theme-back-chat" : "theme-profile");
            profile.setOnClickListener(view -> selectScreen(profilePage ? preferences.getBoolean("mobile-chat-visible", true) ? 1 : 2 : 3));
            header.addView(profile, new LinearLayout.LayoutParams(dp(48), dp(48)));
        }
        else if (screen == 1)
        {
            ImageButton back = toolbarButton("back", "Back to people");
            back.setOnClickListener(view -> selectScreen(0));
            header.addView(back, new LinearLayout.LayoutParams(dp(40), dp(44)));
            Member headerPeer = selectedPeer();
            View face = headerPeer != null ? memberAvatar(headerPeer, 40) : avatar(peerConversation != null ? conversationTitle() : "#", false, 40);
            LinearLayout.LayoutParams facePosition = new LinearLayout.LayoutParams(dp(40), dp(40));
            facePosition.rightMargin = dp(8);
            header.addView(face, facePosition);
            if (peerConversation == null)
            {
                face.setContentDescription("Hold to choose a channel");
                face.setOnLongClickListener(view ->
                {
                    selectScreen(2);
                    return true;
                });
            }
        }
        LinearLayout titleArea = vertical();
        titleArea.setTag("theme-channel-title");
        titleArea.setGravity(Gravity.CENTER_VERTICAL);
        boolean isConnected = state() != null && state().connected;
        String[] titles = {"People", isConnected ? conversationTitle() : "Chat", "Channels", "Me"};
        TextView title = text(titles[screen], screen == 1 ? 17 : 20, palette.barText, true);
        title.setSingleLine();
        title.setEllipsize(android.text.TextUtils.TruncateAt.END);
        titleArea.addView(title);
        String detail = state() == null ? "Your conversations, together" : state().connected ? (state().idle ? "Away · " : "Connected · ") + state().username : state().detail;
        if (screen == 1 && peerConversation != null && (state() == null || !state().connected))
        {
            detail = "Saved conversation · offline";
        }
        if (screen == 1 && state() != null && state().connected)
        {
            Member peer = selectedPeer();
            if (peer != null)
            {
                detail = peer.idle ? "Away · private conversation" : "Online · private conversation";
            }
            else if (peerConversation != null)
            {
                detail = "This person is offline";
            }
            else
            {
                detail = channelMemberSummary();
            }
        }
        TextView subtitle = text(detail, 11, palette.barSecondary, false);
        subtitle.setSingleLine();
        subtitle.setEllipsize(android.text.TextUtils.TruncateAt.END);
        LinearLayout summary = horizontal();
        if (screen == 1 && peerConversation == null && state() != null && state().connected)
        {
            FrameLayout faces = avatarStack(membersInChannel(state().channelId));
            faces.setTag("theme-header-avatars");
            summary.addView(faces, new LinearLayout.LayoutParams(avatarStackWidth(membersInChannel(state().channelId).size()), dp(24)));
        }
        summary.addView(subtitle, new LinearLayout.LayoutParams(-1, -2));
        titleArea.addView(summary);
        titleArea.setOnClickListener(view ->
        {
            if (screen == 1 && peerConversation == null && state() != null && state().connected)
            {
                showMembers();
            }
        });
        if (screen == 1 && peerConversation == null)
        {
            titleArea.setBackground(palette.interactive(Color.TRANSPARENT, dp(4)));
        }
        header.addView(titleArea, new LinearLayout.LayoutParams(0, -1, 1));
        if (palette.theme.tree && screen != 3 && isConnected)
        {
            ImageButton visibility = toolbarButton(screen == 1 ? "eye" : "eye-off", screen == 1 ? "Hide chat" : "Show chat");
            visibility.setTag("theme-chat-toggle");
            visibility.setSelected(screen == 1);
            visibility.setOnClickListener(view -> toggleMobileChat());
            header.addView(visibility, new LinearLayout.LayoutParams(dp(48), dp(48)));
            ImageButton people = toolbarButton("people", "All server members");
            people.setTag("theme-all-members");
            people.setOnClickListener(view -> showAllMembers());
            header.addView(people, new LinearLayout.LayoutParams(dp(48), dp(48)));
        }
        if (screen == 1 && !palette.theme.tree && localSettings.getBoolean("lemon_chat_search"))
        {
            ImageButton search = toolbarButton("search", "Search this conversation");
            search.setOnClickListener(view -> openSearch());
            header.addView(search, new LinearLayout.LayoutParams(dp(40), dp(44)));
        }
        Member peer = selectedPeer();
        if (screen == 1 && canCall(peer))
        {
            ImageButton call = toolbarButton("call", "Call " + peer.name);
            call.setOnClickListener(view -> showCall(peer));
            header.addView(call, new LinearLayout.LayoutParams(dp(40), dp(44)));
        }
        if (screen == 1 && palette.theme.strip == NativeTheme.MOVABLE_STRIP)
        {
            boolean side = localSettings.getBoolean("lemon_strip_vertical");
            ImageButton direction = toolbarButton(side ? "people-top" : "people-side", side ? "Move people above chat" : "Move people to the left");
            direction.setTag("theme-strip-direction");
            direction.setOnClickListener(view -> togglePeopleStrip());
            header.addView(direction, new LinearLayout.LayoutParams(dp(40), dp(44)));
        }
        if (screen != 1 && (!palette.theme.tree || screen == 3))
        {
            ImageButton themes = toolbarButton("theme", "Choose theme");
            themes.setOnClickListener(view -> showThemes());
            header.addView(themes, new LinearLayout.LayoutParams(dp(40), dp(44)));
        }
        ImageButton settings = toolbarButton("settings", "Connection settings");
        settings.setOnClickListener(view ->
        {
            // no guide while connected
            boolean isConnectedNow = service != null && service.snapshot() != null && service.snapshot().connected;
            if (palette.theme.tree || isConnectedNow)
            {
                showMobileTools(view);
            }
            else
            {
                showConnection();
            }
        });
        if (palette.theme.tree)
        {
            settings.setContentDescription("Chat and connection options");
        }
        header.addView(settings, new LinearLayout.LayoutParams(dp(48), dp(48)));
    }

    private void renderNavigation()
    {
        String navigationText = screen + "|" + System.identityHashCode(palette);

        // unchanged navigation
        if (navigationText.equals(navigationContent) && navigation.getChildCount() > 0)
        {
            return;
        }
        navigationContent = navigationText;
        navigation.removeAllViews();
        navigation.setVisibility(palette.theme.hasNavigation() ? View.VISIBLE : View.GONE);
        if (!palette.theme.hasNavigation())
        {
            return;
        }
        int navigationHeight = dp(palette.theme.navigationHeight);
        String[] names = {"People", "Chat", "Channels", "Me"};
        String[] icons = {"people", "chat", "channels", "me"};
        for (int i = 0; i < names.length; i++)
        {
            final int target = i;
            LinearLayout item = vertical();
            item.setGravity(Gravity.CENTER);
            item.setMinimumHeight(dp(48));
            item.setContentDescription(names[i]);
            item.setSelected(screen == i);
            item.setFocusable(true);
            item.setBackground(palette.interactive(Color.TRANSPARENT, dp(8)));
            TextView glyph = text("", 12, palette.text, false);
            glyph.setCompoundDrawablesWithIntrinsicBounds(null, null, null, null);
            glyph.setBackground(new GlyphDrawable(icons[i], screen == i ? palette.accentText : palette.barSecondary));
            FrameLayout iconArea = new FrameLayout(this);
            iconArea.setBackground(palette.rounded(screen == i ? palette.accent : Color.TRANSPARENT, dp(palette.theme.tree ? 5 : 18)));
            int iconSize = dp(palette.theme.tree ? 20 : 23);
            FrameLayout.LayoutParams iconPosition = new FrameLayout.LayoutParams(iconSize, iconSize, Gravity.CENTER);
            iconArea.addView(glyph, iconPosition);
            int iconHeight = dp(palette.theme.tree ? 24 : 29);
            item.addView(iconArea, new LinearLayout.LayoutParams(dp(palette.theme.tree ? 36 : 50), iconHeight));
            TextView label = text(names[i], 11, screen == i ? palette.barText : palette.barSecondary, screen == i);
            label.setTag("theme-nav-label:" + names[i]);
            label.setGravity(Gravity.CENTER);
            label.setSingleLine(true);
            label.setEllipsize(android.text.TextUtils.TruncateAt.END);
            label.setPadding(0, dp(palette.theme.tree ? 2 : 4), 0, 0);
            android.graphics.Paint.FontMetricsInt font = label.getPaint().getFontMetricsInt();
            int itemHeight = Math.max(dp(48), iconHeight + font.bottom - font.top + label.getPaddingTop() + dp(4));
            navigationHeight = Math.max(navigationHeight, itemHeight + navigation.getPaddingTop() + navigation.getPaddingBottom());
            item.addView(label);
            item.setOnClickListener(view -> selectScreen(target));
            navigation.addView(item, new LinearLayout.LayoutParams(0, -1, 1));
        }
        navigation.getLayoutParams().height = navigationHeight;
        navigation.requestLayout();
    }

    private void renderWelcome()
    {
        LinearLayout area = vertical();
        area.setPadding(dp(30), dp(24), dp(30), dp(24));
        area.setGravity(Gravity.CENTER);
        TextView emblem = avatar("L", false, 76);
        area.addView(emblem);
        TextView title = text("Welcome to Lemon Chat", 30, palette.text, true);
        title.setGravity(Gravity.CENTER);
        title.setPadding(0, dp(26), 0, dp(12));
        area.addView(title);
        String description = state() != null && state().status == SessionSnapshot.Status.FAILED ? state().detail : "Connect to your server to see people, messages and voice channels.";
        TextView subtitle = text(description, 16, palette.secondary, false);
        subtitle.setGravity(Gravity.CENTER);
        subtitle.setLineSpacing(dp(5), 1);
        area.addView(subtitle, new LinearLayout.LayoutParams(-1, -2));
        Button connect = button("Connect to a server", true);
        LinearLayout.LayoutParams action = new LinearLayout.LayoutParams(dp(220), dp(52));
        action.topMargin = dp(28);
        area.addView(connect, action);
        connect.setOnClickListener(view -> showConnection());
        if (state() != null && (state().status == SessionSnapshot.Status.CONNECTING || state().status == SessionSnapshot.Status.AUTHENTICATING))
        {
            connect.setText("Cancel connection");
            connect.setOnClickListener(view -> service.disconnect());
            subtitle.setText(state().detail);
        }
        content.addView(area, new FrameLayout.LayoutParams(-1, -1));
    }

    private void renderPeople()
    {
        SessionSnapshot snapshot = state();
        List<Member> members = snapshot != null && snapshot.connected ? snapshot.members : java.util.Collections.emptyList();
        List<Channel> channels = snapshot != null ? snapshot.channels : java.util.Collections.emptyList();
        List<BrowseRow> rows = new ArrayList<>();
        Set<String> onlineConversations = new HashSet<>();
        Map<Integer, String> channelNames = new HashMap<>();
        for (Channel channel : channels)
        {
            channelNames.put(channel.id, channel.name);
        }
        for (Member member : members)
        {
            onlineConversations.add(member.conversationId);
        }
        boolean hasOfflineConversations = false;
        for (String conversation : service.conversationIds())
        {
            List<ChatMessage> messages = service.messages(conversation);
            if (onlineConversations.contains(conversation) || messages.isEmpty())
            {
                continue;
            }
            if (!hasOfflineConversations)
            {
                rows.add(browseSection("recent", "RECENT CONVERSATIONS"));
                hasOfflineConversations = true;
            }
            ChatMessage last = messages.get(messages.size() - 1);
            String title = service.conversationTitle(conversation);
            String preview = (last.outgoing ? "You: " : "") + last.text;
            rows.add(new BrowseRow("recent:" + conversation, true, true, () ->
            {
                LinearLayout row = actionRow(title, preview, "chat", view ->
                {
                    preserveDraft();
                    peerConversation = conversation;
                    selectScreen(1);
                });
                LinearLayout labels = (LinearLayout) row.getChildAt(1);
                TextView detail = (TextView) labels.getChildAt(1);
                detail.setSingleLine();
                detail.setEllipsize(android.text.TextUtils.TruncateAt.END);
                return row;
            }));
        }
        if (snapshot != null && snapshot.connected)
        {
            rows.add(browseSection("online", "ON THIS SERVER"));
        }
        for (Member member : members)
        {
            if (member.id == snapshot.clientId)
            {
                continue;
            }
            List<ChatMessage> history = service.messages(member.conversationId);
            String channelName = channelNames.getOrDefault(member.channelId, "channel");
            String preview = member.musicBot ? "Music bot · " + channelName : member.idle ? "Away" : "In " + channelName;
            if (!member.musicBot && !history.isEmpty())
            {
                ChatMessage latest = history.get(history.size() - 1);
                preview = (latest.outgoing ? "You: " : "") + latest.text;
            }
            String detail = preview;
            rows.add(new BrowseRow("member:" + (member.musicBot ? "bot:" + member.id : member.conversationId), true, !member.musicBot, () ->
            {
                LinearLayout row = personRow(member, detail);
                if (!member.musicBot)
                {
                    row.setOnClickListener(view -> openConversation(member));
                }
                return row;
            }));
        }
        if (members.size() <= 1 && !hasOfflineConversations)
        {
            rows.add(new BrowseRow("empty", false, false, () ->
            {
                LinearLayout block = vertical();
                empty(block, "You’re the first one here", "People will appear here when they connect. You can still explore the channels.");
                return block;
            }));
        }
        peopleList = showBrowseList(peopleList, rows, snapshot != null ? snapshot.connectionId : 0);
    }

    private void renderChannels()
    {
        if (treeLayout())
        {
            renderChannelTree();
            return;
        }
        SessionSnapshot snapshot = state();
        List<BrowseRow> rows = new ArrayList<>();
        Map<Integer, List<Member>> channelMembers = new HashMap<>();
        for (Member member : snapshot.members)
        {
            channelMembers.computeIfAbsent(member.channelId, key -> new ArrayList<>()).add(member);
        }
        rows.add(browseSection("channels", "FIND YOUR ROOM"));
        for (Channel channel : snapshot.channels)
        {
            List<Member> members = channelMembers.getOrDefault(channel.id, java.util.Collections.emptyList());
            boolean current = channel.id == snapshot.channelId;
            rows.add(new BrowseRow("channel:" + channel.id, true, true, () -> channelRow(channel, members, current)));
        }
        channelsList = showBrowseList(channelsList, rows, snapshot.connectionId);
    }

    private boolean treeLayout()
    {
        return NativeTheme.selected(this).tree;
    }

    private void renderChannelTree()
    {
        if (channelTree == null || channelTree.getTag() != palette)
        {
            channelTree = createChannelTree();
            channelTree.setTag(palette);
        }
        channelTree.preferences(localSettings, avatarImages);
        channelTree.render(state());
        if (channelTree.getParent() != content)
        {
            if (channelTree.getParent() instanceof ViewGroup)
            {
                ((ViewGroup) channelTree.getParent()).removeView(channelTree);
            }
            content.removeAllViews();
            content.addView(channelTree, new FrameLayout.LayoutParams(-1, -1));
        }
    }

    private ChannelTreeView createChannelTree()
    {
        return new ChannelTreeView(this, palette, new ChannelTreeView.Actions()
        {
            @Override
            public void onJoin(Channel channel)
            {
                joinChannel(channel);
            }

            @Override
            public void onOpenMember(Member member)
            {
                openConversation(member);
            }

            @Override
            public void onChannelMembers(Channel channel)
            {
                showMembers(channel.id, channel.name);
            }

            @Override
            public void onMemberInfo(Member member)
            {
                showMemberInfo(member);
            }
        });
    }

    private void joinChannel(Channel channel)
    {
        if (service == null || state() == null || !state().connected)
        {
            return;
        }
        if (channel.id == state().channelId)
        {
            peerConversation = null;
            finishChannelSelection();
        }
        else if (channel.passwordProtected)
        {
            showChannelPassword(channel);
        }
        else
        {
            peerConversation = null;
            service.session().joinChannel(channel.id, "");
            finishChannelSelection();
        }
    }

    private void finishChannelSelection()
    {
        if (palette.theme.tree && screen == 2)
        {
            render();
        }
        else
        {
            selectScreen(1);
        }
    }

    private LinearLayout channelRow(Channel channel, List<Member> members, boolean current)
    {
        LinearLayout row = horizontal();
        row.setPadding(dp(12), dp(12), dp(12), dp(12));
        row.setBackground(palette.interactive(current ? palette.selection : palette.surface, dp(palette.theme.radius)));
        TextView symbol = avatar("#", false, 40);
        row.addView(symbol, new LinearLayout.LayoutParams(dp(40), dp(40)));
        LinearLayout labels = vertical();
        labels.setPadding(dp(14), 0, dp(6), 0);
        TextView name = text(channel.name, 17, palette.text, true);
        name.setSingleLine();
        name.setEllipsize(android.text.TextUtils.TruncateAt.END);
        labels.addView(name);
        String detail = current ? "You are here" : members.isEmpty() ? "Quiet for now" : members.size() + (members.size() == 1 ? " person" : " people");
        if (channel.passwordProtected)
        {
            detail += " · password required";
        }
        labels.addView(text(detail, 12, palette.secondary, false));
        row.addView(labels, new LinearLayout.LayoutParams(0, -2, 1));
        if (!members.isEmpty())
        {
            row.addView(avatarStack(members), new LinearLayout.LayoutParams(avatarStackWidth(members.size()), dp(30)));
        }
        row.setOnClickListener(view ->
        {
            if (current)
            {
                peerConversation = null;
                selectScreen(1);
            }
            else if (channel.passwordProtected)
            {
                showChannelPassword(channel);
            }
            else
            {
                peerConversation = null;
                service.session().joinChannel(channel.id, "");
                selectScreen(1);
            }
        });
        return row;
    }


    private BrowseRow browseSection(String key, String title)
    {
        return new BrowseRow("section:" + key, false, false, () ->
        {
            LinearLayout block = vertical();
            section(block, title);
            return block;
        });
    }

    /**
     * updates visible rows while keeping the same item at the top
     *
     * @param list existing list, or null on first display
     * @param rows lightweight row descriptions
     * @param connectionId connection that owns these rows
     * @return the attached list
     */
    private ListView showBrowseList(ListView list, List<BrowseRow> rows, long connectionId)
    {
        if (list == null || list.getTag() != palette)
        {
            list = new ListView(this);
            list.setTag(palette);
            list.setClipToPadding(false);
            list.setPadding(dp(18), dp(4), dp(18), dp(18));
            list.setDivider(null);
            list.setSelector(new android.graphics.drawable.ColorDrawable(Color.TRANSPARENT));
            list.setCacheColorHint(Color.TRANSPARENT);
            list.setAdapter(new BrowseAdapter());
        }
        BrowseAdapter adapter = (BrowseAdapter) list.getAdapter();
        int first = list.getFirstVisiblePosition();
        View firstView = list.getChildAt(0);
        int top = firstView != null ? firstView.getTop() - list.getPaddingTop() : 0;
        String anchor = first < adapter.rows.size() ? adapter.rows.get(first).key : null;
        boolean sameConnection = adapter.connectionId == connectionId;
        adapter.rows = rows;
        adapter.connectionId = connectionId;
        adapter.notifyDataSetChanged();
        if (sameConnection && anchor != null)
        {
            for (int index = 0; index < rows.size(); index++)
            {
                if (anchor.equals(rows.get(index).key))
                {
                    first = index;
                    break;
                }
            }
        }
        else
        {
            first = 0;
            top = 0;
        }
        list.setSelectionFromTop(Math.min(first, Math.max(0, rows.size() - 1)), top);
        if (list.getParent() != content)
        {
            if (list.getParent() instanceof ViewGroup)
            {
                ((ViewGroup) list.getParent()).removeView(list);
            }
            content.removeAllViews();
            content.addView(list, new FrameLayout.LayoutParams(-1, -1));
        }
        return list;
    }

    private static final class BrowseRow
    {
        final String key;
        final boolean card;
        final boolean selectable;
        final Supplier<View> render;

        BrowseRow(String key, boolean card, boolean selectable, Supplier<View> render)
        {
            this.key = key;
            this.card = card;
            this.selectable = selectable;
            this.render = render;
        }
    }

    private final class BrowseAdapter extends BaseAdapter
    {
        List<BrowseRow> rows = new ArrayList<>();
        long connectionId = -1;

        @Override
        public int getCount()
        {
            return rows.size();
        }

        @Override
        public BrowseRow getItem(int position)
        {
            return rows.get(position);
        }

        @Override
        public long getItemId(int position)
        {
            return position;
        }

        @Override
        public boolean areAllItemsEnabled()
        {
            return false;
        }

        @Override
        public boolean isEnabled(int position)
        {
            return rows.get(position).selectable;
        }

        @Override
        public View getView(int position, View recycled, ViewGroup parent)
        {
            FrameLayout container = recycled instanceof FrameLayout ? (FrameLayout) recycled : new FrameLayout(MainActivity.this);
            container.removeAllViews();
            BrowseRow row = rows.get(position);
            container.setPadding(0, 0, 0, row.card ? dp(10) : 0);
            container.addView(row.render.get(), new FrameLayout.LayoutParams(-1, -2));
            return container;
        }
    }

    private void renderChat()
    {
        if (service == null)
        {
            return;
        }
        String conversation = conversationId();
        if (!conversation.equals(renderedConversation))
        {
            preserveDraft();
            content.removeAllViews();
            LinearLayout column = vertical();
            if (localSettings.getBoolean("lemon_chat_search") && (searchOpen || !retained.searchQuery.isEmpty() && conversation.equals(retained.searchConversation)))
            {
                searchInput = input("Search this conversation", false);
                searchInput.setFilters(new InputFilter[]{new InputFilter.LengthFilter(128)});
                searchInput.setText(conversation.equals(retained.searchConversation) ? retained.searchQuery : "");
                searchInput.addTextChangedListener(new TextWatcher()
                {
                    @Override
                    public void beforeTextChanged(CharSequence value, int start, int count, int after)
                    {
                    }

                    @Override
                    public void onTextChanged(CharSequence value, int start, int before, int count)
                    {
                        retained.searchConversation = conversation;
                        retained.searchQuery = value.toString();
                        renderChat();
                    }

                    @Override
                    public void afterTextChanged(Editable value)
                    {
                    }
                });
                column.addView(searchInput, new LinearLayout.LayoutParams(-1, -2));
            }
            else
            {
                searchInput = null;
            }
            chatList = new ListView(this);
            chatList.setDivider(null);
            chatList.setClipToPadding(false);
            chatList.setPadding(dp(8), dp(4), dp(8), dp(6));
            messageHeader = vertical();
            chatList.addHeaderView(messageHeader, null, false);
            messageAdapter = new MessageAdapter();
            chatList.setAdapter(messageAdapter);
            column.addView(chatList, new LinearLayout.LayoutParams(-1, 0, 1));
            composerNotice = text("", 12, palette.secondary, false);
            composerNotice.setPadding(dp(20), dp(6), dp(20), dp(8));
            column.addView(composerNotice);
            LinearLayout composerRow = horizontal();
            composerRow.setPadding(dp(8), dp(2), dp(8), dp(4));
            ImageButton attach = iconButton("attach", "Attach a picture or file", false);
            attach.setOnClickListener(view -> chooseAttachment());
            composerRow.addView(attach, new LinearLayout.LayoutParams(dp(48), dp(48)));
            if (localSettings.getBoolean("lemon_show_mp3_button"))
            {
                ImageButton music = iconButton("music", "Stream a music file to your channel", false);
                music.setOnClickListener(view -> chooseMusic());
                composerRow.addView(music, new LinearLayout.LayoutParams(dp(48), dp(48)));
            }
            composer = input("Message", false);
            composer.setTextSize(palette.theme.messageTextSize());
            composer.setPadding(dp(10), dp(8), dp(10), dp(8));
            composer.setMinimumHeight(dp(44));
            composer.setIncludeFontPadding(false);
            composer.setTag("theme-message-input");
            composer.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
            composer.setSingleLine(false);
            composer.setMinLines(1);
            composer.setMaxLines(4);
            composer.setMaxHeight(dp(124));
            composer.setFilters(new InputFilter[]{new InputFilter.LengthFilter(RetainedUiState.MESSAGE_LIMIT)});
            composer.setSaveEnabled(false);
            composer.setText(retained.drafts.getOrDefault(conversation, ""));
            composer.setSelection(composer.length());
            composerRow.addView(composer, new LinearLayout.LayoutParams(0, -2, 1));
            send = iconButton("send", "Send message", true);
            send.setTag("theme-send");
            LinearLayout.LayoutParams sendPosition = new LinearLayout.LayoutParams(dp(48), dp(48));
            sendPosition.leftMargin = dp(4);
            composerRow.addView(send, sendPosition);
            send.setOnClickListener(view -> sendMessage());
            composer.addTextChangedListener(new TextWatcher()
            {
                @Override
                public void beforeTextChanged(CharSequence value, int start, int count, int after)
                {
                }

                @Override
                public void onTextChanged(CharSequence value, int start, int before, int count)
                {
                    updateComposer();
                }

                @Override
                public void afterTextChanged(Editable value)
                {
                }
            });
            composerDock.removeAllViews();
            composerRow.setTag("theme-composer");
            draftPreview = vertical();
            draftPreview.setPadding(dp(18), dp(4), dp(18), dp(4));
            composerDock.addView(draftPreview, new LinearLayout.LayoutParams(-1, -2));
            composerDock.addView(composerRow);
            LinearLayout body = new LinearLayout(this);
            boolean side = palette.theme.sideStrip(localSettings.getBoolean("lemon_strip_vertical"));
            int width = Math.round(getResources().getDisplayMetrics().widthPixels / getResources().getDisplayMetrics().density);
            int stripWidth = palette.theme.sideWidth(width);
            body.setOrientation(side ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
            conversationMembers = null;
            if (palette.theme.strip != NativeTheme.NO_STRIP)
            {
                conversationMembers = new ConversationMembers(this, palette, localSettings, avatarImages, new ConversationMembers.Actions()
                {
                    @Override
                    public void open(Member member)
                    {
                        openConversation(member);
                    }

                    @Override
                    public void info(Member member)
                    {
                        showMemberInfo(member);
                    }

                    @Override
                    public void allPeople()
                    {
                        if (palette.theme.tree)
                        {
                            showAllMembers();
                        }
                        else
                        {
                            selectScreen(0);
                        }
                    }
                });
                body.addView(conversationMembers, new LinearLayout.LayoutParams(side ? dp(stripWidth) : -1, side ? -1 : -2));
            }
            body.addView(column, new LinearLayout.LayoutParams(side ? 0 : -1, side ? -1 : 0, 1));
            content.addView(body, new FrameLayout.LayoutParams(-1, -1));
            renderedConversation = conversation;
            messagesFingerprint = 0;
            renderedNewestMessage = 0;
        }
        List<ChatMessage> messages = service.messages(conversation);
        if (conversationMembers != null)
        {
            conversationMembers.render(state());
        }
        String search = localSettings.getBoolean("lemon_chat_search") && conversation.equals(retained.searchConversation) ? retained.searchQuery : "";
        int fingerprint = messages.size() + 17 + search.hashCode() + localSettings.asMap().hashCode() + Long.hashCode(avatarImages.revision());
        for (ChatMessage message : messages)
        {
            fingerprint = fingerprint * 31 + Long.hashCode(message.id) + message.delivery.hashCode() + message.text.hashCode();
            fingerprint = fingerprint * 31 + (message.seen ? 1 : 0) + message.reactions.hashCode() + (message.attachment != null ? message.attachment.id.hashCode() : 0);
        }
        if (fingerprint != messagesFingerprint)
        {
            int firstPosition = chatList.getFirstVisiblePosition();
            View firstView = chatList.getChildAt(0);
            int firstTop = firstView == null ? 0 : firstView.getTop();
            long anchorId = firstPosition > 0 && firstPosition <= messageAdapter.rows.size() ? messageAdapter.rows.get(firstPosition - 1).id : Long.MIN_VALUE;
            messageHeader.removeAllViews();
            addConversationStartLine();
            addPublicKeyDetails();
            ArrayList<ChatMessage> visible = new ArrayList<>();
            String query = search.toLowerCase(java.util.Locale.ROOT);
            for (ChatMessage message : messages)
            {
                String searchable = message.text + (message.attachment != null ? " " + message.attachment.name : "") + (message.reply != null ? " " + message.reply.text : "");
                if (query.isEmpty() || searchable.toLowerCase(java.util.Locale.ROOT).contains(query))
                {
                    visible.add(message);
                }
            }
            if (!messages.isEmpty() && visible.isEmpty())
            {
                empty(messageHeader, "No matching messages", "Try a different search.");
            }
            messageRenderVersion++;
            messageAdapter.rows = visible;
            messageAdapter.notifyDataSetChanged();
            messagesFingerprint = fingerprint;
            long newest = messages.isEmpty() ? 0 : messages.get(messages.size() - 1).id;
            ListView renderedList = chatList;
            if (!restoringTheme && localSettings.getBoolean("lemon_auto_scroll") && search.isEmpty() && newest != renderedNewestMessage)
            {
                renderedList.post(() -> renderedList.setSelection(visible.size()));
            }
            else
            {
                int anchor = Math.min(firstPosition, visible.size());
                for (int index = 0; index < visible.size(); index++)
                {
                    if (visible.get(index).id == anchorId)
                    {
                        anchor = index + 1;
                        break;
                    }
                }
                int position = anchor;
                renderedList.post(() -> renderedList.setSelectionFromTop(position, firstTop));
            }
            renderedNewestMessage = newest;
        }
        renderDraftPreview();
        updateComposer();
        markVisibleConversationRead();
    }

    private View messageBubble(ChatMessage message)
    {
        ChatMessageViews views = new ChatMessageViews(this, palette, localSettings, avatarImages, attachmentImages, emojiImages, new ChatMessageViews.Actions()
        {
            @Override
            public void onMessageActions(ChatMessage selected)
            {
                showMessageActions(selected);
            }

            @Override
            public void onReaction(ChatMessage selected, String emojiId)
            {
                if (service != null)
                {
                    service.session().reactToMessage(selected, emojiId);
                }
            }

            @Override
            public void onOpenAttachment(Attachment attachment)
            {
                showAttachment(attachment);
            }
        });
        String search = conversationId().equals(retained.searchConversation) ? retained.searchQuery : "";
        return views.message(message, peerConversation != null, memberForKey(message.authorPublicKey), state() != null ? state().emojis : java.util.Collections.emptyList(), search);
    }

    /** creates only the message rows currently needed by the viewport */
    private final class MessageAdapter extends BaseAdapter
    {
        private List<ChatMessage> rows = java.util.Collections.emptyList();

        @Override
        public int getCount()
        {
            return rows.size();
        }

        @Override
        public ChatMessage getItem(int position)
        {
            return rows.get(position);
        }

        @Override
        public long getItemId(int position)
        {
            return rows.get(position).id;
        }

        @Override
        public boolean hasStableIds()
        {
            return true;
        }

        @Override
        public View getView(int position, View recycled, ViewGroup parent)
        {
            FrameLayout row = recycled instanceof FrameLayout ? (FrameLayout) recycled : new FrameLayout(MainActivity.this);
            ChatMessage message = rows.get(position);
            String binding = messageRenderVersion + ":" + message.id;
            if (!binding.equals(row.getTag()))
            {
                row.removeAllViews();
                row.addView(messageBubble(message), new FrameLayout.LayoutParams(-1, -2));
                row.setTag(binding);
            }
            return row;
        }
    }

    private void updateComposer()
    {
        if (composer == null || send == null)
        {
            return;
        }
        boolean connected = state() != null && state().connected;
        boolean ready = connected && (peerConversation != null ? selectedPeer() != null : state().channelKeysReady);
        composer.setHint(peerConversation != null ? "Message " + conversationTitle() : "Message #" + channelName(state() != null ? state().channelId : -1));
        boolean attached = service != null && retained.attachments.matches(service.conversationScope(), conversationId());
        composer.setHint(attached ? "" : composer.getHint());
        send.setEnabled(ready && (composer.length() > 0 || attached) && retained.pendingConversation == null && !retained.attachments.sending && !retained.attachments.busy);
        send.setAlpha(send.isEnabled() ? 1 : 0.4f);
        String notice = !connected ? "You’re offline. Reconnect to send a message. Your draft is kept here." : peerConversation != null && selectedPeer() == null ? "This person is offline. Your draft is kept here." : !ready ? "Waiting for this channel’s encryption keys…" : "";
        composerNotice.setText(notice);
        composerNotice.setVisibility(notice.isEmpty() ? View.GONE : View.VISIBLE);
    }

    private void sendMessage()
    {
        if (service == null || composer == null || !send.isEnabled())
        {
            return;
        }
        if (retained.attachments.matches(service.conversationScope(), conversationId()))
        {
            sendAttachment();
            return;
        }
        retained.pendingConversation = conversationId();
        retained.pendingText = composer.getText().toString();
        retained.pendingAfterId = 0;
        for (ChatMessage message : service.messages(retained.pendingConversation))
        {
            retained.pendingAfterId = Math.max(retained.pendingAfterId, message.id);
        }
        Member peer = selectedPeer();
        retained.pendingReply = conversationId().equals(retained.replyConversation) && localSettings.getBoolean("lemon_chat_replies") ? retained.reply : null;
        if (peerConversation != null && peer != null)
        {
            service.session().sendDirectMessage(peer.id, retained.pendingText, retained.pendingReply);
        }
        else if (peerConversation == null)
        {
            service.session().sendChannelMessage(retained.pendingText, retained.pendingReply);
        }
        updateComposer();
    }

    private void finishPendingDraft()
    {
        finishPendingAttachment();
        if (retained.pendingConversation == null || service == null)
        {
            return;
        }
        for (ChatMessage message : service.messages(retained.pendingConversation))
        {
            if (message.id > retained.pendingAfterId && message.outgoing && message.text.equals(retained.pendingText))
            {
                if (retained.pendingText.equals(retained.drafts.get(retained.pendingConversation)))
                {
                    retained.drafts.remove(retained.pendingConversation);
                }
                if (composer != null && retained.pendingConversation.equals(renderedConversation) && composer.getText().toString().equals(retained.pendingText))
                {
                    composer.setText("");
                }
                retained.pendingConversation = null;
                retained.pendingText = null;
                if (retained.reply == retained.pendingReply)
                {
                    retained.reply = null;
                    retained.replyConversation = null;
                }
                retained.pendingReply = null;
                break;
            }
        }
    }

    private void onImagesChanged()
    {
        if (reactionAdapter != null)
        {
            reactionAdapter.notifyDataSetChanged();
        }
        if (started && !isDestroyed())
        {
            meFingerprint = null;
            messagesFingerprint = 0;
            headerContent = null;
            if (conversationMembers != null)
            {
                conversationMembers.clearRenderedContent();
            }
            render();
        }
    }

    private Member memberForKey(String key)
    {
        if (state() != null && key != null && !key.isEmpty())
        {
            for (Member member : state().members)
            {
                if (member.publicKey.equals(key))
                {
                    return member;
                }
            }
        }
        return null;
    }

    private void openSearch()
    {
        if (screen != 1 || !localSettings.getBoolean("lemon_chat_search"))
        {
            return;
        }
        preserveDraft();
        searchOpen = true;
        renderedConversation = null;
        render();
        if (searchInput != null)
        {
            searchInput.requestFocus();
            getSystemService(android.view.inputmethod.InputMethodManager.class).showSoftInput(searchInput, android.view.inputmethod.InputMethodManager.SHOW_IMPLICIT);
        }
    }

    private void markVisibleConversationRead()
    {
        if (started && hasWindowFocus() && screen == 1 && peerConversation != null && service != null && state() != null && state().connected)
        {
            service.session().markConversationRead(peerConversation);
        }
    }

    /**
     * @brief adds the line that names the person or the channel at the top of a conversation, with the same words
     * as the browser client: "now talking to user: NAME" or "now talking channel: NAME"
     */
    private void addConversationStartLine()
    {
        String line = (peerConversation != null ? "now talking to user: " : "now talking channel: ") + conversationTitle();
        TextView view = text(line, 11, palette.secondary, false);
        view.setTypeface(Typeface.create("sans-serif", Typeface.ITALIC));
        view.setPadding(dp(6), dp(6), dp(6), dp(6));
        messageHeader.addView(view);
    }

    private void addPublicKeyDetails()
    {
        if (peerConversation == null || localSettings.getBoolean("lemon_is_public_key_info_hidden"))
        {
            return;
        }
        Member peer = selectedPeer();
        String local = service.session().localPublicKey();
        String other = peer == null ? "Available while this person is connected" : peer.publicKey;
        TextView keys = text("Your public key\n" + (local.isEmpty() ? "Available when connected" : local) + "\n\nTheir public key\n" + other, 11, palette.secondary, false);
        keys.setTypeface(Typeface.MONOSPACE);
        keys.setTextIsSelectable(true);
        keys.setPadding(dp(6), dp(8), dp(6), dp(18));
        messageHeader.addView(keys);
    }

    private void showMessageActions(ChatMessage message)
    {
        List<String> names = new ArrayList<>();
        List<Runnable> actions = new ArrayList<>();
        boolean current = state() != null && state().connected && message.connectionId == state().connectionId;
        if (message.attachment == null && current && localSettings.getBoolean("lemon_chat_replies"))
        {
            names.add("Reply");
            actions.add(() ->
            {
                retained.reply = message;
                retained.replyConversation = message.conversationId;
                renderDraftPreview();
                composer.requestFocus();
            });
        }
        if (message.attachment == null && current && localSettings.getBoolean("lemon_chat_reactions"))
        {
            names.add("React");
            actions.add(() -> showReactions(message));
        }
        if (message.attachment != null)
        {
            names.add("Open or save file");
            actions.add(() -> showAttachment(message.attachment));
        }
        if (!message.text.isEmpty())
        {
            names.add("Copy text");
            actions.add(() -> getSystemService(ClipboardManager.class).setPrimaryClip(ClipData.newPlainText("Message", message.text)));
        }

        // delete on this device
        if (service != null)
        {
            names.add("Delete on this device");
            actions.add(() ->
            {
                if (service != null)
                {
                    service.deleteMessageLocally(message);
                }
            });
        }
        if (!names.isEmpty())
        {
            showDialog(new AlertDialog.Builder(this).setTitle("Message").setItems(names.toArray(new String[0]), (dialog, index) -> actions.get(index).run()).setNegativeButton("Cancel", null).create());
        }
    }

    private void showReactions(ChatMessage message)
    {
        if (state() == null || state().emojis.isEmpty())
        {
            showNotice("This server has no available emoji reactions");
            return;
        }
        List<ServerEmoji> emojis = new ArrayList<>(state().emojis);
        long expectedConnection = state().connectionId;
        reactionAdapter = new BaseAdapter()
        {
            @Override
            public void notifyDataSetChanged()
            {
                SessionSnapshot current = state();
                emojis.clear();
                if (current != null && current.connected && current.connectionId == expectedConnection)
                {
                    emojis.addAll(current.emojis);
                }
                else if (reactionPicker != null)
                {
                    reactionPicker.dismiss();
                }
                super.notifyDataSetChanged();
            }

            @Override
            public int getCount()
            {
                return emojis.size();
            }

            @Override
            public Object getItem(int position)
            {
                return emojis.get(position);
            }

            @Override
            public long getItemId(int position)
            {
                return position;
            }

            @Override
            public View getView(int position, View recycled, ViewGroup parent)
            {
                ServerEmoji emoji = emojis.get(position);
                TextView name = text((emoji.unicode.isEmpty() ? "" : emoji.unicode + "  ") + emoji.name, 17, palette.text, false);
                name.setPadding(dp(18), dp(10), dp(18), dp(10));
                android.graphics.drawable.Drawable image = emojiImages.drawable(emoji.id);
                if (image != null)
                {
                    image.setBounds(0, 0, dp(28), dp(28));
                    name.setCompoundDrawables(image, null, null, null);
                    name.setCompoundDrawablePadding(dp(12));
                }
                return name;
            }
        };
        AlertDialog picker = new AlertDialog.Builder(this).setTitle("React to message").setAdapter(reactionAdapter, (dialog, index) ->
        {
            SessionSnapshot current = state();
            if (service != null && current != null && current.connected && current.connectionId == expectedConnection && index >= 0 && index < emojis.size())
            {
                String selected = emojis.get(index).id;
                for (ServerEmoji emoji : current.emojis)
                {
                    if (emoji.id.equals(selected))
                    {
                        service.session().reactToMessage(message, selected);
                        break;
                    }
                }
            }
        }).setNegativeButton("Cancel", null).create();
        BaseAdapter adapter = reactionAdapter;
        reactionPicker = picker;
        picker.setOnDismissListener(dialog ->
        {
            if (reactionAdapter == adapter)
            {
                reactionAdapter = null;
            }
            if (reactionPicker == picker)
            {
                reactionPicker = null;
            }
        });
        showDialog(picker);
    }

    private void renderDraftPreview()
    {
        if (draftPreview == null || service == null)
        {
            return;
        }
        draftPreview.removeAllViews();
        if (retained.reply != null && conversationId().equals(retained.replyConversation) && localSettings.getBoolean("lemon_chat_replies"))
        {
            LinearLayout quote = horizontal();
            TextView excerpt = text("Reply to " + retained.reply.senderName + "\n" + retained.reply.quoteText(), 12, palette.secondary, false);
            excerpt.setMaxLines(3);
            excerpt.setEllipsize(android.text.TextUtils.TruncateAt.END);
            quote.addView(excerpt, new LinearLayout.LayoutParams(0, -2, 1));
            ImageButton remove = iconButton("close", "Cancel reply", false);
            remove.setOnClickListener(view ->
            {
                retained.reply = null;
                retained.replyConversation = null;
                renderDraftPreview();
            });
            quote.addView(remove, new LinearLayout.LayoutParams(dp(40), dp(40)));
            draftPreview.addView(quote);
        }
        if (retained.attachments.matches(service.conversationScope(), conversationId()))
        {
            Attachment attachment = retained.attachments.draft;
            LinearLayout preview = horizontal();
            android.graphics.drawable.Drawable drawable = attachmentImages.drawable(attachment);
            if (drawable != null)
            {
                android.widget.ImageView picture = new android.widget.ImageView(this);
                picture.setImageDrawable(drawable);
                picture.setScaleType(android.widget.ImageView.ScaleType.FIT_CENTER);
                preview.addView(picture, new LinearLayout.LayoutParams(dp(70), dp(54)));
            }
            TextView label = text((retained.attachments.sending ? "Sending " : "") + attachment.name, 12, palette.secondary, false);
            label.setMaxLines(2);
            preview.addView(label, new LinearLayout.LayoutParams(0, -2, 1));
            ImageButton remove = iconButton("close", "Remove attachment", false);
            remove.setEnabled(!retained.attachments.sending);
            remove.setOnClickListener(view -> retained.attachments.clearDraft());
            preview.addView(remove, new LinearLayout.LayoutParams(dp(40), dp(40)));
            draftPreview.addView(preview);
        }
        draftPreview.setVisibility(draftPreview.getChildCount() == 0 ? View.GONE : View.VISIBLE);
    }

    private void chooseAttachment()
    {
        if (service == null || state() == null || !state().connected)
        {
            showNotice("Connect before selecting an attachment");
            return;
        }
        if (retained.attachments.busy || retained.attachments.sending)
        {
            showNotice("Wait for the current file operation to finish");
            return;
        }
        ArrayList<String> choices = new ArrayList<>();
        ArrayList<Runnable> actions = new ArrayList<>();
        if (state().capabilities.pictureUpload)
        {
            choices.add("Picture");
            actions.add(() -> pickAttachment(true, false));
        }
        if (state().capabilities.fileUpload)
        {
            choices.add("File");
            actions.add(() -> pickAttachment(false, false));
        }
        if (choices.isEmpty())
        {
            showNotice("This server does not allow picture or file uploads");
            return;
        }
        showDialog(new AlertDialog.Builder(this).setTitle("Attach").setItems(choices.toArray(new String[0]), (dialog, selected) -> actions.get(selected).run()).create());
    }

    private void pickAttachment(boolean picture, boolean avatar)
    {
        if (service == null || state() == null || !state().connected || retained.attachments.busy || retained.attachments.sending)
        {
            return;
        }
        long limit = avatar ? state().capabilities.avatarMaxBytes : picture ? state().capabilities.pictureMaxBytes : state().capabilities.fileMaxBytes;
        retained.attachments.picker = new AttachmentFiles.Ticket(service.conversationScope(), conversationId(), (int) Math.min(Attachment.MAX_BYTES, limit), picture, avatar);
        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        picker.addCategory(Intent.CATEGORY_OPENABLE);
        picker.setType(picture ? "image/*" : "*/*");
        try
        {
            startPicker(picker, PICK_ATTACHMENT);
        }
        catch (ActivityNotFoundException unavailable)
        {
            retained.attachments.picker = null;
            showNotice("No file picker is available on this device");
        }
    }

    private void onAttachmentChanged()
    {
        if (!started || isFinishing() || isDestroyed())
        {
            return;
        }
        AttachmentFiles files = retained.attachments;
        if (service != null && files.musicUri != null)
        {
            if (files.musicScope != null && files.musicScope.equals(service.conversationScope()))
            {
                service.startMusic(files.musicUri, files.musicName);
            }
            else
            {
                showNotice("The connection changed. Choose the music file again.");
            }
            files.musicUri = null;
            files.musicName = null;
        }
        String notice = files.takeNotice();
        if (notice != null)
        {
            showNotice(notice);
        }
        if (service != null && files.avatarData != null)
        {
            if (files.avatarOwner != null && files.avatarOwner.scope.equals(service.conversationScope()) && state() != null && state().connected)
            {
                service.session().uploadAvatar(files.avatarData);
            }
            else
            {
                showNotice("The connection changed. Choose the avatar again.");
            }
            files.avatarData = null;
            files.avatarOwner = null;
        }
        if (files.openUri != null)
        {
            Intent open = new Intent(Intent.ACTION_VIEW).setDataAndType(files.openUri, files.openMime);
            open.setClipData(ClipData.newRawUri("Attachment", files.openUri));
            open.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            files.openUri = null;
            files.openMime = null;
            try
            {
                startActivity(Intent.createChooser(open, "Open attachment"));
            }
            catch (ActivityNotFoundException unavailable)
            {
                showNotice("No app can open this file. Use Save instead.");
            }
        }
        if (service != null && files.draft != null && files.owner != null && !files.owner.scope.equals(service.conversationScope()))
        {
            files.clearDraft();
            return;
        }
        if (files.draft != null && retained.reply != null && files.owner != null && files.owner.conversation.equals(retained.replyConversation))
        {
            retained.reply = null;
            retained.replyConversation = null;
            showNotice("Attachments are sent separately from quoted replies");
        }
        renderDraftPreview();
        updateComposer();
    }

    private void sendAttachment()
    {
        AttachmentFiles files = retained.attachments;
        if (service == null || !files.matches(service.conversationScope(), conversationId()) || files.sending || files.busy)
        {
            return;
        }
        Member peer = selectedPeer();
        if (peerConversation != null && peer == null)
        {
            return;
        }
        files.sentAfterId = 0;
        for (ChatMessage message : service.messages(conversationId()))
        {
            files.sentAfterId = Math.max(files.sentAfterId, message.id);
        }
        files.sending = true;
        Attachment selected = files.draft;
        if (peer != null)
        {
            service.session().sendDirectAttachment(peer.id, selected.name, selected.mime, selected.data(), selected.picture);
        }
        else
        {
            service.session().sendChannelAttachment(selected.name, selected.mime, selected.data(), selected.picture);
        }
        if (composer != null && composer.length() > 0)
        {
            showNotice("The attachment is sending. Your text is kept ready to send next.");
        }
        renderDraftPreview();
        updateComposer();
    }

    private void finishPendingAttachment()
    {
        AttachmentFiles files = retained.attachments;
        if (service == null || !files.sending || files.owner == null || files.draft == null)
        {
            return;
        }
        for (ChatMessage message : service.messages(files.owner.conversation))
        {
            if (message.id > files.sentAfterId && message.outgoing && message.attachment != null && message.attachment.name.equals(files.draft.name) && message.attachment.size == files.draft.size)
            {
                if ("Sent".equals(message.delivery))
                {
                    files.clearDraft();
                }
                else if (!"Sending".equals(message.delivery))
                {
                    files.sending = false;
                }
                return;
            }
        }
    }

    private void showAttachment(Attachment attachment)
    {
        showDialog(new AlertDialog.Builder(this).setTitle(attachment.name).setItems(new String[]{"Open with another app", "Save a copy"}, (dialog, selected) ->
        {
            if (retained.attachments.busy)
            {
                showNotice("Wait for the current file operation to finish");
                return;
            }
            if (selected == 0)
            {
                retained.attachments.open(getApplicationContext(), attachment);
            }
            else
            {
                retained.attachments.saving = attachment;
                Intent save = new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType(attachment.mime);
                save.putExtra(Intent.EXTRA_TITLE, attachment.name);
                try
                {
                    startPicker(save, SAVE_ATTACHMENT);
                }
                catch (ActivityNotFoundException unavailable)
                {
                    retained.attachments.saving = null;
                    showNotice("No file picker is available on this device");
                }
            }
        }).create());
    }

    private void chooseMusic()
    {
        keyboardTalking = false;
        if (service == null || state() == null || !state().connected)
        {
            showNotice("Connect to a channel before streaming music");
            return;
        }
        if (service.musicActive())
        {
            showDialog(new AlertDialog.Builder(this).setTitle("Streaming music").setMessage(service.musicName()).setNegativeButton("Keep playing", null).setPositiveButton("Stop", (dialog, button) ->
            {
                if (service != null)
                {
                    service.stopMusic();
                }
            }).create());
            return;
        }
        retained.attachments.musicScope = service.conversationScope();
        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("audio/*");
        try
        {
            startPicker(picker, PICK_MUSIC);
        }
        catch (ActivityNotFoundException unavailable)
        {
            showNotice("No file picker is available on this device");
        }
    }

    private void showLocalSettings()
    {
        if (openDialog != null)
        {
            openDialog.dismiss();
        }
        openDialog = LocalSettingsDialog.show(this, service, new LocalSettingsDialog.Callbacks()
        {
            @Override
            public void onSaved()
            {
                localSettings = LocalSettings.load(MainActivity.this);
                if (applySavedUiScale())
                {
                    return;
                }
                rebuildTheme();
            }

            @Override
            public void onDialogReplaced(AlertDialog dialog)
            {
                openDialog = dialog;
            }

            @Override
            public void onAvatar()
            {
                showAvatar();
            }

            @Override
            public void onGroups()
            {
                showGroups();
            }

            @Override
            public void onIdentityImport()
            {
                chooseIdentityFile();
            }

            @Override
            public void onIdentityExport()
            {
                exportIdentity();
            }

            @Override
            public void onLog()
            {
                showLog();
            }

            @Override
            public void onHistory()
            {
                showHistorySettings();
            }
        });
    }

    private void showHistoryConsent()
    {
        if (service == null || service.historyPolicy().configured || service.historyBusy() || isFinishing() || isDestroyed() || !started || openDialog != null && openDialog.isShowing() || state() != null && state().call != null)
        {
            return;
        }
        openDialog = HistoryDialog.show(this, service, true, () ->
        {
            meFingerprint = null;
            render();
        });
    }

    private void showHistorySettings()
    {
        if (service == null)
        {
            return;
        }
        if (openDialog != null)
        {
            openDialog.dismiss();
        }
        openDialog = HistoryDialog.show(this, service, !service.historyPolicy().configured, () ->
        {
            meFingerprint = null;
            render();
        });
    }

    private void showAvatar()
    {
        if (state() == null || !state().connected || !state().capabilities.avatarUpload)
        {
            showNotice("Connect to a server that allows avatars");
            return;
        }
        showDialog(new AlertDialog.Builder(this).setTitle("Your avatar").setItems(new String[]{"Choose a picture", "Remove avatar"}, (dialog, selected) ->
        {
            if (service == null || state() == null || !state().connected)
            {
                return;
            }
            if (selected == 0)
            {
                pickAttachment(true, true);
            }
            else
            {
                service.session().deleteAvatar();
            }
        }).create());
    }

    private void showGroups()
    {
        LinearLayout form = sheet("Server groups");
        if (state() == null || !state().connected)
        {
            label(form, "Connect to see the groups available on this server.");
        }
        else
        {
            int count = 0;
            for (Group group : state().groups)
            {
                if (!group.channelGroup)
                {
                    label(form, group.name);
                    form.addView(text("Group " + group.id + (group.type.isEmpty() ? "" : " · " + group.type), 12, palette.secondary, false));
                    count++;
                }
            }
            if (count == 0)
            {
                label(form, "This server has not provided any server groups.");
            }
        }
        showSheet(form);
    }

    private void showLog()
    {
        LinearLayout form = sheet("Connection log");
        TextView log = text(service == null ? "The service is not connected" : service.connectionLog(), 12, palette.secondary, false);
        log.setTypeface(Typeface.MONOSPACE);
        log.setTextIsSelectable(true);
        form.addView(log);
        showSheet(form);
    }

    private void showBackgroundSettings()
    {
        BackgroundPermissions.Status status = BackgroundPermissions.read(this);
        LinearLayout form = sheet("Background connection");
        label(form, "Lemon connects directly to your server. Allow background activity so messages and calls can arrive while another app is open or the screen is off.");
        label(form, status.batteryExempt ? "Battery optimization: unrestricted" : "Battery optimization: enabled");
        label(form, status.backgroundRestricted ? "Background activity: restricted by Android" : "Background activity: allowed");
        label(form, status.notificationsAllowed ? "Notifications: allowed" : "Notifications: blocked");
        label(form, status.fullScreenCallsAllowed ? "Incoming call screen: allowed" : "Incoming call screen: limited to notifications");
        if (!status.notificationsAllowed)
        {
            Button notifications = button("Allow notifications", false);
            notifications.setOnClickListener(view ->
            {
                openDialog.dismiss();
                if (!BackgroundPermissions.openNotificationSettings(this))
                {
                    showNotice("Notification settings are unavailable on this device");
                }
            });
            centeredAction(form, notifications);
        }
        if (!status.fullScreenCallsAllowed)
        {
            Button calls = button("Allow incoming call screen", false);
            calls.setOnClickListener(view ->
            {
                openDialog.dismiss();
                if (!BackgroundPermissions.openFullScreenCallSettings(this))
                {
                    showNotice("Incoming call settings are unavailable on this device");
                }
            });
            centeredAction(form, calls);
        }
        if (!status.batteryExempt)
        {
            Button request = button("Allow background connection", true);
            request.setOnClickListener(view ->
            {
                openDialog.dismiss();
                if (!BackgroundPermissions.requestBatteryExemption(this))
                {
                    showNotice("Open this app's battery settings to allow background activity");
                }
            });
            centeredAction(form, request);
        }
        Button battery = button("Battery settings", false);
        battery.setOnClickListener(view ->
        {
            openDialog.dismiss();
            if (!BackgroundPermissions.openBatterySettings(this))
            {
                showNotice("Battery settings are unavailable on this device");
            }
        });
        centeredAction(form, battery);
        Button app = button("App and notification settings", false);
        app.setOnClickListener(view ->
        {
            openDialog.dismiss();
            if (!BackgroundPermissions.openAppSettings(this))
            {
                showNotice("App settings are unavailable on this device");
            }
        });
        centeredAction(form, app);
        showSheet(form);
    }

    private void renderVoice()
    {
        boolean hiddenForMobile = palette.theme.id.equals("default-mobile") && localSettings.getBoolean("lemon_hide_mobile_mic");
        boolean show = screen != 3 && !localSettings.getBoolean("lemon_hide_mic") && !hiddenForMobile && state() != null && state().connected;
        voiceDock.setVisibility(show ? View.VISIBLE : View.GONE);
        if (!show)
        {
            return;
        }
        if (microphone == null || microphone.getParent() != voiceDock)
        {
            voiceDock.removeAllViews();
            microphone = iconButton("mic", "Hold to talk in the current channel", false);
            microphone.setTag("theme-microphone");
            voiceDock.addView(microphone, new LinearLayout.LayoutParams(dp(48), dp(48)));
            voiceLabel = text("", 10, palette.secondary, false);
            voiceLabel.setGravity(Gravity.CENTER);
            voiceLabel.setPadding(dp(8), dp(2), dp(8), dp(2));
            voiceLabel.setBackground(palette.rounded(palette.background, dp(8)));
            voiceDock.addView(voiceLabel);
            microphone.setOnTouchListener((view, event) ->
            {
                if (service == null)
                {
                    return false;
                }
                if (event.getActionMasked() == MotionEvent.ACTION_DOWN)
                {
                    if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED)
                    {
                        requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, RECORD_AUDIO);
                    }
                    else
                    {
                        service.setTalking(localSettings.getBoolean("lemon_continuous_mic") ? !service.talking() : true);
                        view.setPressed(true);
                    }
                    return true;
                }
                if (event.getActionMasked() == MotionEvent.ACTION_UP || event.getActionMasked() == MotionEvent.ACTION_CANCEL)
                {
                    if (!localSettings.getBoolean("lemon_continuous_mic") || event.getActionMasked() == MotionEvent.ACTION_CANCEL)
                    {
                        service.setTalking(false);
                    }
                    view.setPressed(false);
                    if (event.getActionMasked() == MotionEvent.ACTION_UP)
                    {
                        view.performClick();
                    }
                    return true;
                }
                return true;
            });
        }
        microphone.setEnabled(service.voiceReady());
        microphone.setAlpha(service.voiceReady() ? 1 : 0.45f);
        microphone.setBackground(palette.microphone(service.talking(), dp(28)));
        microphone.setImageDrawable(new GlyphDrawable("mic", service.talking() ? palette.accentText : palette.theme.quilt ? Color.parseColor("#3d444c") : palette.text));
        microphone.setContentDescription(localSettings.getBoolean("lemon_continuous_mic") ? service.talking() ? "Stop microphone" : "Start microphone" : "Hold to talk in the current channel");
        microphone.setTooltipText(microphone.getContentDescription());
        voiceLabel.setText(service.talking() ? "Talking in " + channelName(state().channelId) : service.voiceReady() ? (localSettings.getBoolean("lemon_continuous_mic") ? "Tap to talk · " : "Hold to talk · ") + channelName(state().channelId) : service.voiceStatus());
        voiceLabel.setVisibility(service.talking() || !service.voiceReady() ? View.VISIBLE : View.GONE);
    }

    private void renderMe()
    {
        Identity identity = service != null ? service.identity() : null;
        SessionSnapshot snapshot = state();
        String fingerprint = preferences.getString("username", "You") + ":" + preferences.getString("endpoint", "") + ":" + palette.theme.id + ":" + palette.dark + ":" + preferences.getBoolean("show-microphone", true) + ":" + (identity == null ? 0 : identity.rsaKeyBits) + ":" + (service != null ? service.identityError() : "") + ":" + (service != null && service.automaticConnectionEnabled()) + ":" + (service != null && service.hasSavedConnection()) + ":" + (snapshot != null && snapshot.connected) + ":" + (snapshot != null && snapshot.idle) + ":" + (snapshot != null && snapshot.idleAllowed) + ":" + (snapshot != null ? snapshot.username : "");
        fingerprint += service != null ? ":" + HistoryDialog.summary(service.historyPolicy()) : "";
        if (meScroll != null && meScroll.getParent() == content && fingerprint.equals(meFingerprint))
        {
            return;
        }
        int previousScroll = meScroll != null ? meScroll.getScrollY() : 0;
        content.removeAllViews();
        LinearLayout list = scrollContent();
        meScroll = (ScrollView) list.getParent();
        meFingerprint = fingerprint;
        LinearLayout profile = vertical();
        profile.setGravity(Gravity.CENTER);
        profile.setPadding(dp(20), dp(22), dp(20), dp(26));
        String name = state() != null && state().connected ? state().username : preferences.getString("username", "You");
        Member me = state() != null ? memberForKey(service.session().localPublicKey()) : null;
        View profilePicture = me != null ? memberAvatar(me, 72) : avatar(name, false, 72);
        profilePicture.setOnClickListener(view -> showAvatar());
        profile.addView(profilePicture);
        TextView title = text(name, 23, palette.text, true);
        title.setPadding(0, dp(14), 0, dp(5));
        profile.addView(title);
        profile.addView(text(identity == null ? "Loading your identity…" : identity.rsaKeyBits + " bit identity · stored on this device", 12, palette.secondary, false));
        list.addView(profile);
        section(list, "MAKE IT YOURS");
        list.addView(actionRow("Theme", palette.theme.name, "theme", view -> showThemes()), rowMargins());
        if (palette.theme.allowsAppearance())
        {
            list.addView(settingRow("Dark appearance", "Remembered for " + palette.theme.name, palette.dark, checked ->
            {
                preferences.edit().putBoolean("theme-dark-" + palette.theme.id, checked).apply();
                rebuildTheme();
            }), rowMargins());
        }
        list.addView(actionRow("Local settings", "Appearance, chat, audio, files and identity", "settings", view -> showLocalSettings()), rowMargins());
        if (service != null)
        {
            list.addView(actionRow("Offline history", HistoryDialog.summary(service.historyPolicy()), "chat", view -> showHistorySettings()), rowMargins());
        }
        if (localSettings.getBoolean("lemon_show_log_button"))
        {
            list.addView(actionRow("Connection log", "Recent connection events", "chat", view -> showLog()), rowMargins());
        }
        section(list, "YOUR IDENTITY");
        list.addView(actionRow("Import identity", "Use your existing .lmn file", "lock", view -> chooseIdentityFile()), rowMargins());
        list.addView(actionRow("Export identity", "Keep a backup or move to another device", "lock", view -> exportIdentity()), rowMargins());
        if (service != null && service.identityError() != null)
        {
            TextView warning = text(service.identityError(), 13, palette.danger, false);
            warning.setPadding(dp(6), dp(12), dp(6), dp(12));
            list.addView(warning);
        }
        section(list, "CONNECTION");

        // guide only while disconnected
        if (snapshot == null || !snapshot.connected)
        {
            list.addView(actionRow("Connection settings", preferences.getString("endpoint", "Choose a server"), "settings", view -> showConnection()), rowMargins());
        }
        if (snapshot != null && snapshot.connected)
        {
            list.addView(actionRow("Administration", "Server settings, groups and permissions", "lock", view -> showAdministration()), rowMargins());
        }
        boolean savedConnection = service != null && service.hasSavedConnection();
        LinearLayout automatic = settingRow("Connect automatically", savedConnection ? "Use your saved server when Lemon opens or your phone restarts. Receive messages and calls while away." : "Connect to a server first to save its address and your identity.", service != null && service.automaticConnectionEnabled(), checked ->
        {
            if (service != null)
            {
                service.setAutomaticConnectionEnabled(checked);
                if (checked && Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
                {
                    requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, NOTIFICATIONS);
                }
            }
        });
        boolean automaticAvailable = savedConnection || service != null && service.automaticConnectionEnabled();
        automatic.setEnabled(automaticAvailable);
        automatic.getChildAt(1).setEnabled(automaticAvailable);
        automatic.setAlpha(automaticAvailable ? 1 : 0.5f);
        list.addView(automatic, rowMargins());
        list.addView(actionRow("Background connection", "Allow messages and calls while the screen is off", "settings", view -> showBackgroundSettings()), rowMargins());
        if (snapshot != null && snapshot.connected && snapshot.idleAllowed)
        {
            String availability = snapshot.idle ? "Return to channel" : "Go away";
            String explanation = snapshot.idle ? "Join your previous channel to listen and speak again" : "Leave channel audio and stay available for messages and calls";
            list.addView(actionRow(availability, explanation, "people", view ->
            {
                if (service != null && state() != null)
                {
                    if (state().idle)
                    {
                        service.leaveIdle();
                    }
                    else
                    {
                        service.enterIdle();
                    }
                }
            }), rowMargins());
        }
        if (state() != null && state().connected)
        {
            Button disconnect = button("Disconnect", false);
            disconnect.setTextColor(palette.danger);
            disconnect.setOnClickListener(view -> service.disconnect());
            LinearLayout.LayoutParams position = new LinearLayout.LayoutParams(dp(180), dp(48));
            position.gravity = Gravity.CENTER_HORIZONTAL;
            position.topMargin = dp(18);
            list.addView(disconnect, position);
        }
        TextView version = text("Lemon Native · early evaluation", 11, palette.secondary, false);
        version.setGravity(Gravity.CENTER);
        version.setPadding(0, dp(26), 0, dp(12));
        list.addView(version);
        ScrollView rendered = meScroll;
        rendered.post(() -> rendered.scrollTo(0, previousScroll));
    }

    private void showConnection()
    {
        if (openDialog instanceof ConnectionWizard && openDialog.isShowing())
        {
            return;
        }
        if (service == null || service.identity() == null)
        {
            showNotice("The client is still starting. Try again in a moment.");
            return;
        }
        if (retained.connectionDraft == null)
        {
            try
            {
                retained.connectionDraft = new ConnectionWizard.Draft(preferences.getString("endpoint", ""), preferences.getString("username", ""), service.serverKeys());
            }
            catch (IllegalArgumentException error)
            {
                retained.connectionDraft = new ConnectionWizard.Draft("", preferences.getString("username", ""), service.serverKeys());
                showNotice("The saved address could not be read. Enter the address and port again.");
            }
        }
        ConnectionWizard.Draft draft = retained.connectionDraft;
        ConnectionWizard wizard = new ConnectionWizard(this, palette, draft, (endpoint, username, keys) ->
        {
            if (service == null || service.identity() == null)
            {
                return "The client is still starting. Try Connect in a moment.";
            }
            service.connect(endpoint, username, keys);
            if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            {
                requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, NOTIFICATIONS);
            }
            return null;
        });
        wizard.setOnDismissListener(dialog ->
        {
            if (!isChangingConfigurations() && retained.connectionDraft == draft)
            {
                retained.connectionDraft = null;
            }
            if (openDialog == wizard)
            {
                openDialog = null;
            }
        });
        showDialog(wizard);
    }

    private void showChannelPassword(Channel channel)
    {
        LinearLayout form = sheet("Join " + channel.name);
        label(form, "Channel password");
        EditText password = input("Password", true);
        form.addView(password);
        Button join = button("Join channel", true);
        centeredAction(form, join);
        join.setOnClickListener(view ->
        {
            if (service != null)
            {
                peerConversation = null;
                service.session().joinChannel(channel.id, password.getText().toString());
                openDialog.dismiss();
                finishChannelSelection();
            }
        });
        showSheet(form);
    }

    private void showMembers()
    {
        if (state() == null)
        {
            return;
        }
        showMembers(state().channelId, channelName(state().channelId));
    }

    private void showMembers(int channelId, String channelName)
    {
        LinearLayout form = sheet(channelName);
        List<BrowseRow> rows = new ArrayList<>();
        int localId = state() != null ? state().clientId : -1;
        for (Member member : membersInChannel(channelId))
        {
            boolean selectable = !member.musicBot && member.id != localId;
            rows.add(new BrowseRow("member:" + member.id, true, selectable, () ->
            {
                LinearLayout row = personRow(member, member.musicBot ? "Music bot" : member.id == localId ? "You" : member.idle ? "Away" : "In this channel");
                if (selectable)
                {
                    row.setOnClickListener(view ->
                    {
                        openDialog.dismiss();
                        openConversation(member);
                    });
                }
                return row;
            }));
        }
        ListView list = new ListView(this);
        list.setDivider(null);
        list.setSelector(new android.graphics.drawable.ColorDrawable(Color.TRANSPARENT));
        BrowseAdapter adapter = new BrowseAdapter();
        adapter.rows = rows;
        list.setAdapter(adapter);
        int height = Math.min(dp(Math.max(1, rows.size()) * 86), getResources().getDisplayMetrics().heightPixels / 2);
        form.addView(list, new LinearLayout.LayoutParams(-1, height));
        showSheet(form);
    }

    private void showMemberInfo(Member member)
    {
        SessionSnapshot snapshot = state();
        if (snapshot == null)
        {
            return;
        }
        LinearLayout form = sheet(member.name);
        form.addView(text(ChannelTree.audioDescription(member), 14, palette.secondary, false));
        Set<Integer> assigned = new HashSet<>(member.groupIds);
        Map<Long, String> iconData = new HashMap<>();
        for (ServerIcon icon : snapshot.icons)
        {
            iconData.put(icon.id, icon.data);
        }
        StringBuilder serverNames = new StringBuilder();
        String role = null;
        boolean unsupported = false;
        for (Group group : snapshot.groups)
        {
            boolean channelRole = group.channelGroup && group.id == member.channelGroupId && !member.idle && member.channelId >= 0;
            if (!channelRole && (group.channelGroup || !assigned.contains(group.id)))
            {
                continue;
            }
            if (channelRole)
            {
                role = group.name;
            }
            else
            {
                if (serverNames.length() > 0)
                {
                    serverNames.append('\n');
                }
                serverNames.append(group.name);
            }
            String data = iconData.get(group.iconId);
            unsupported |= group.iconId >= 0 && data != null && !ServerIconCache.supported(data);
        }
        label(form, "Channel group");
        form.addView(text(role == null ? "No assigned channel group" : role, 16, palette.text, false));
        label(form, "Server groups");
        TextView groups = text(serverNames.length() == 0 ? "No assigned server groups" : serverNames.toString(), 16, palette.text, false);
        groups.setLineSpacing(dp(5), 1);
        form.addView(groups);
        if (service != null && member.id != snapshot.clientId && !member.publicKey.isEmpty())
        {
            label(form, "Playback volume");
            TextView level = text("", 13, palette.secondary, false);
            form.addView(level);
            android.widget.SeekBar volume = new android.widget.SeekBar(this);
            volume.setMax(200);
            volume.setProgress(Math.round(service.peerGain(member.publicKey) * 100));
            level.setText(volume.getProgress() == 0 ? "Muted on this device" : volume.getProgress() + "% on this device");
            volume.setContentDescription("Playback volume for " + member.name);
            volume.setOnSeekBarChangeListener(new android.widget.SeekBar.OnSeekBarChangeListener()
            {
                @Override
                public void onProgressChanged(android.widget.SeekBar bar, int progress, boolean fromUser)
                {
                    level.setText(progress == 0 ? "Muted on this device" : progress + "% on this device");
                }

                @Override
                public void onStartTrackingTouch(android.widget.SeekBar bar)
                {
                }

                @Override
                public void onStopTrackingTouch(android.widget.SeekBar bar)
                {
                    if (service != null)
                    {
                        service.setPeerGain(member.publicKey, bar.getProgress() / 100f);
                    }
                }
            });
            form.addView(volume, new LinearLayout.LayoutParams(-1, dp(44)));
            LinearLayout controls = horizontal();
            Button mute = button("Mute", false);
            Button reset = button("Reset", false);
            mute.setOnClickListener(view ->
            {
                volume.setProgress(0);
                if (service != null)
                {
                    service.setPeerGain(member.publicKey, 0);
                }
            });
            reset.setOnClickListener(view ->
            {
                volume.setProgress(100);
                if (service != null)
                {
                    service.setPeerGain(member.publicKey, 1);
                }
            });
            controls.addView(mute, new LinearLayout.LayoutParams(0, dp(44), 1));
            controls.addView(reset, new LinearLayout.LayoutParams(0, dp(44), 1));
            form.addView(controls);
        }
        if (unsupported)
        {
            TextView note = text("A custom icon uses an image format this client cannot display. The group names above are unchanged.", 13, palette.secondary, false);
            note.setPadding(0, dp(16), 0, 0);
            form.addView(note);
        }
        if (!member.musicBot && member.id != snapshot.clientId)
        {
            Button message = button("Open conversation", true);
            centeredAction(form, message);
            message.setOnClickListener(view ->
            {
                if (state() != null && state().connectionId == snapshot.connectionId)
                {
                    openDialog.dismiss();
                    openConversation(member);
                }
            });
        }
        if (canCall(member))
        {
            Button call = button("Call", false);
            centeredAction(form, call);
            call.setOnClickListener(view ->
            {
                if (currentCallTarget(member, snapshot.connectionId))
                {
                    showCall(member);
                }
            });
        }
        showSheet(form);
    }

    private void chooseIdentityFile()
    {
        if (retained.busy())
        {
            showNotice("Wait for the current identity operation to finish");
            return;
        }
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startPicker(intent, IMPORT_IDENTITY);
    }

    private void exportIdentity()
    {
        if (retained.busy())
        {
            showNotice("Wait for the current identity operation to finish");
            return;
        }
        if (service == null || service.identity() == null)
        {
            showNotice("Your identity is not ready yet");
            return;
        }
        try
        {
            retained.exportData = IdentityFile.export(service.identity());
            Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("application/octet-stream");
            intent.putExtra(Intent.EXTRA_TITLE, "identity.lmn");
            startPicker(intent, EXPORT_IDENTITY);
        }
        catch (RuntimeException error)
        {
            showNotice("The identity could not be exported");
        }
    }

    /**
     * @brief starts a system file picker and keeps the session in its channel while the picker is in front
     *
     * @param intent the picker request
     * @param request the request code that onActivityResult receives
     */
    private void startPicker(Intent intent, int request)
    {
        // keep the channel
        if (service != null)
        {
            service.setPickerOpen(true);
        }

        // open the picker
        try
        {
            startActivityForResult(intent, request);
        }
        catch (RuntimeException error)
        {
            if (service != null)
            {
                service.setPickerOpen(false);
            }
            throw error;
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data)
    {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null)
        {
            if (request == EXPORT_IDENTITY)
            {
                retained.clearExport();
            }
            if (request == PICK_ATTACHMENT)
            {
                retained.attachments.picker = null;
            }
            if (request == SAVE_ATTACHMENT)
            {
                retained.attachments.saving = null;
            }
            return;
        }
        Uri uri = data.getData();
        if (request == IMPORT_IDENTITY)
        {
            if (!retained.readIdentity(getApplicationContext().getContentResolver(), uri, localSettings.getInt("lemon_rsa_key_bits")))
            {
                showNotice("Wait for the current identity operation to finish");
            }
        }
        else if (request == EXPORT_IDENTITY)
        {
            if (!retained.writeIdentity(getApplicationContext().getContentResolver(), uri))
            {
                showNotice("Please choose Export identity again");
            }
        }
        else if (request == PICK_ATTACHMENT)
        {
            if (!retained.attachments.read(getApplicationContext().getContentResolver(), uri))
            {
                showNotice("Please choose the attachment again");
            }
        }
        else if (request == SAVE_ATTACHMENT)
        {
            if (!retained.attachments.save(getApplicationContext().getContentResolver(), uri))
            {
                showNotice("Please choose Save again");
            }
        }
        else if (request == PICK_MUSIC)
        {
            retained.attachments.music(getApplicationContext().getContentResolver(), uri, retained.attachments.musicScope);
        }
    }

    private void onFilesChanged()
    {
        if (!started || isFinishing() || isDestroyed())
        {
            return;
        }
        if (importDialog != null && importDialog.isShowing())
        {
            if (retained.importData == null)
            {
                importDialog.dismiss();
                importDialog = null;
                render();
            }
            else if (importConfirm != null)
            {
                importConfirm.setEnabled(!retained.importInFlight);
            }
        }
        else if (retained.importData != null && service != null)
        {
            confirmImport(retained.importData);
        }
        String notice = retained.takeNotice();
        if (notice != null)
        {
            showNotice(notice);
        }
    }

    private void confirmImport(byte[] bytes)
    {
        if (isFinishing() || isDestroyed())
        {
            return;
        }
        Identity parsed;
        try
        {
            parsed = IdentityFile.parse(bytes, retained.legacyBits);
        }
        catch (RuntimeException error)
        {
            retained.clearImport();
            showNotice(error.getMessage());
            return;
        }
        String prefix = new String(bytes, java.nio.charset.StandardCharsets.UTF_8).replace("\ufeff", "").trim();
        boolean legacy = !prefix.startsWith("{");
        LinearLayout form = sheet("Import your identity");
        TextView explanation = text(legacy ? "This older file does not contain its key size. Choose the size used by the original client." : "This file contains a " + parsed.rsaKeyBits + " bit identity. Importing replaces the identity saved by this app.", 14, palette.secondary, false);
        form.addView(explanation);
        android.widget.Spinner size = new android.widget.Spinner(this);
        String[] sizes = {"2048", "3072", "4096", "6144", "8192"};
        size.setAdapter(new android.widget.ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, sizes));
        for (int index = 0; index < sizes.length; index++)
        {
            if (Integer.parseInt(sizes[index]) == retained.legacyBits)
            {
                size.setSelection(index);
                break;
            }
        }
        size.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener()
        {
            @Override
            public void onItemSelected(android.widget.AdapterView<?> parent, View selected, int position, long id)
            {
                retained.legacyBits = Integer.parseInt(sizes[position]);
            }

            @Override
            public void onNothingSelected(android.widget.AdapterView<?> parent)
            {
            }
        });
        if (legacy)
        {
            label(form, "Original key size");
            form.addView(size, new LinearLayout.LayoutParams(-1, dp(52)));
        }
        Button confirm = button("Import identity", true);
        centeredAction(form, confirm);
        confirm.setOnClickListener(view ->
        {
            if (service == null)
            {
                showNotice("The client is still starting");
                return;
            }
            retained.applyImport(service, Integer.parseInt(sizes[size.getSelectedItemPosition()]));
        });
        showSheet(form);
        importDialog = openDialog;
        importConfirm = confirm;
        confirm.setEnabled(!retained.importInFlight);
        Dialog shown = importDialog;
        shown.setOnDismissListener(dialog ->
        {
            if (started && !isChangingConfigurations() && !retained.importInFlight && retained.importData == bytes)
            {
                retained.clearImport();
            }
            if (importDialog == shown)
            {
                importDialog = null;
                importConfirm = null;
            }
        });
    }

    @Override
    public void onRequestPermissionsResult(int request, String[] permissions, int[] grants)
    {
        super.onRequestPermissionsResult(request, permissions, grants);
        if (request == RECORD_AUDIO)
        {
            boolean allowed = checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED;
            if (service != null)
            {
                service.session().setMicrophoneAvailable(allowed);
            }
            showNotice(allowed ? "Microphone ready. Hold the button to talk." : "Microphone access is off. You can still listen.");
        }
    }

    @Override
    public void onBackPressed()
    {
        if (screen == 1 && searchInput != null)
        {
            preserveDraft();
            retained.searchQuery = "";
            retained.searchConversation = null;
            searchOpen = false;
            renderedConversation = null;
            render();
            return;
        }
        if (!palette.theme.hasNavigation())
        {
            if (screen == 3)
            {
                selectScreen(preferences.getBoolean("mobile-chat-visible", true) ? 1 : 2);
            }
            else
            {
                super.onBackPressed();
            }
        }
        else if (screen != 0)
        {
            selectScreen(0);
        }
        else
        {
            super.onBackPressed();
        }
    }

    private LinearLayout sheet(String title)
    {
        LinearLayout form = vertical();
        form.setPadding(dp(24), dp(18), dp(24), dp(28));
        LinearLayout titleRow = horizontal();
        titleRow.addView(text(title, 23, palette.text, true), new LinearLayout.LayoutParams(0, -2, 1));
        ImageButton close = iconButton("close", "Close", false);
        close.setOnClickListener(view -> openDialog.dismiss());
        titleRow.addView(close, new LinearLayout.LayoutParams(dp(42), dp(42)));
        form.addView(titleRow);
        return form;
    }

    private void showDialog(Dialog dialog)
    {
        if (openDialog != null && openDialog != dialog)
        {
            openDialog.dismiss();
        }
        openDialog = dialog;
        dialog.show();
    }

    private void showSheet(LinearLayout form)
    {
        if (openDialog != null)
        {
            openDialog.dismiss();
        }
        openDialog = new Dialog(this);
        openDialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(false);
        scroll.addView(form);
        scroll.setBackground(palette.rounded(palette.surface, dp(28)));
        openDialog.setContentView(scroll);
        Window window = openDialog.getWindow();
        if (window != null)
        {
            window.setBackgroundDrawableResource(android.R.color.transparent);
            window.setDimAmount(0.35f);
            window.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
            window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
            window.setGravity(Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        }
        openDialog.show();
        if (window != null)
        {
            int width = Math.min(getResources().getDisplayMetrics().widthPixels - dp(16), dp(520));
            form.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
            int maximumHeight = (int) (getResources().getDisplayMetrics().heightPixels * 0.82);
            window.setLayout(width, Math.min(form.getMeasuredHeight(), maximumHeight));
        }
    }

    private void selectScreen(int value)
    {
        if (palette.theme.tree && value == 0)
        {
            showAllMembers();
            return;
        }
        preserveDraft();
        if (service != null)
        {
            service.setTalking(false);
        }
        screen = value;
        if (palette.theme.tree && (value == 1 || value == 2))
        {
            preferences.edit().putBoolean("mobile-chat-visible", value == 1).apply();
        }
        renderedConversation = null;
        render();
    }

    /** switches the mobile workspace without changing the connection or conversation */
    private void toggleMobileChat()
    {
        rememberLayoutForResize();
        retained.mobilePositions.put(screen, retained.layoutPosition);
        screen = screen == 1 ? 2 : 1;
        preferences.edit().putBoolean("mobile-chat-visible", screen == 1).apply();
        renderedConversation = null;
        retained.layoutPosition = retained.mobilePositions.get(screen);
        render();
        restoreResizedLayout();
    }

    private void showAllMembers()
    {
        if (openDialog != null)
        {
            openDialog.dismiss();
        }
        MemberDrawer drawer = new MemberDrawer(this, palette, new MemberDrawer.Actions()
        {
            @Override
            public View row(Member member)
            {
                SessionSnapshot snapshot = state();
                String detail = member.musicBot ? "Music bot" : snapshot != null && member.id == snapshot.clientId ? "You" : member.idle ? "Away" : channelName(member.channelId);
                List<ChatMessage> messages = service == null ? java.util.Collections.emptyList() : service.messages(member.conversationId);
                if (!member.musicBot && !messages.isEmpty())
                {
                    ChatMessage latest = messages.get(messages.size() - 1);
                    detail = (latest.outgoing ? "You: " : "") + latest.text;
                }
                return personRow(member, detail);
            }

            @Override
            public void open(Member member)
            {
                openConversation(member);
            }
        });
        openDialog = drawer;
        drawer.render(state());
        drawer.show();
    }

    private void showMobileTools(View anchor)
    {
        android.widget.PopupMenu menu = new android.widget.PopupMenu(new android.view.ContextThemeWrapper(this, palette.theme.windowStyle(palette.dark)), anchor);
        if (screen == 1 && localSettings.getBoolean("lemon_chat_search"))
        {
            menu.getMenu().add("Search conversation").setOnMenuItemClickListener(item ->
            {
                openSearch();
                return true;
            });
        }
        menu.getMenu().add("Local settings").setOnMenuItemClickListener(item ->
        {
            showLocalSettings();
            return true;
        });
        menu.getMenu().add("Choose theme").setOnMenuItemClickListener(item ->
        {
            showThemes();
            return true;
        });
        if (service != null && service.snapshot().connected)
        {
            menu.getMenu().add("Administration").setOnMenuItemClickListener(item ->
            {
                showAdministration();
                return true;
            });
        }

        // guide only while disconnected
        if (service == null || !service.snapshot().connected)
        {
            menu.getMenu().add("Connection settings").setOnMenuItemClickListener(item ->
            {
                showConnection();
                return true;
            });
        }
        menu.show();
    }

    private void showAdministration()
    {
        if (service == null || !service.snapshot().connected)
        {
            return;
        }
        if (openDialog != null)
        {
            openDialog.dismiss();
        }
        openDialog = AdministrationDialog.show(this, service);
    }

    private void showThemes()
    {
        LinearLayout form = sheet("Choose theme");
        for (NativeTheme theme : NativeTheme.ALL)
        {
            LinearLayout choice = horizontal();
            choice.setPadding(dp(12), dp(11), dp(12), dp(11));
            choice.setBackground(palette.rounded(theme == palette.theme ? palette.selection : Color.TRANSPARENT, dp(8)));
            Palette preview = new Palette(theme, theme.dark(preferences));
            TextView sample = text("Aa", 15, preview.text, true);
            sample.setGravity(Gravity.CENTER);
            sample.setBackground(preview.rounded(preview.background, dp(8)));
            choice.addView(sample, new LinearLayout.LayoutParams(dp(44), dp(44)));
            LinearLayout labels = vertical();
            labels.setPadding(dp(12), 0, 0, 0);
            labels.addView(text(theme.name + (theme == palette.theme ? "  ✓" : ""), 15, palette.text, true));
            labels.addView(text(theme.description, 12, palette.secondary, false));
            choice.addView(labels, new LinearLayout.LayoutParams(0, -2, 1));
            choice.setContentDescription(theme.name + (theme == palette.theme ? ", selected" : ""));
            choice.setOnClickListener(view ->
            {
                openDialog.dismiss();
                selectTheme(theme.id);
            });
            form.addView(choice, new LinearLayout.LayoutParams(-1, -2));
        }
        showSheet(form);
    }

    /**
     * changes views while keeping the service and conversation intact
     *
     * @param id built in theme identifier
     */
    void selectTheme(String id)
    {
        NativeTheme theme = NativeTheme.find(id);
        preferences.edit().putString("theme", theme.id).apply();
        if (theme.tree && screen == 0)
        {
            screen = preferences.getBoolean("mobile-chat-visible", true) ? 1 : 2;
        }
        rebuildTheme();
    }

    private void rebuildTheme()
    {
        preserveDraft();
        ListView previousList = screen == 1 ? chatList : screen == 0 ? peopleList : screen == 2 ? palette.theme.tree ? channelTree : channelsList : null;
        if (screen == 2 && previousList == null)
        {
            previousList = channelTree != null && channelTree.getParent() == content ? channelTree : channelsList;
        }
        int first = previousList != null ? previousList.getFirstVisiblePosition() : 0;
        View firstView = previousList != null ? previousList.getChildAt(0) : null;
        int top = firstView != null ? firstView.getTop() - previousList.getPaddingTop() : 0;
        int scroll = meScroll != null ? meScroll.getScrollY() : 0;
        int selectionStart = composer != null ? composer.getSelectionStart() : 0;
        int selectionEnd = composer != null ? composer.getSelectionEnd() : 0;
        boolean focused = composer != null && composer.hasFocus();
        ChannelTreeView previousTree = channelTree;
        if (previousTree != null && state() != null)
        {
            themeCollapsedChannels = previousTree.collapsedChannels();
            themeTreeConnection = state().connectionId;
        }
        peopleList = null;
        channelsList = null;
        channelTree = null;
        conversationMembers = null;
        microphone = null;
        applyPalette();
        buildShell();
        restoringTheme = true;
        try
        {
            render();
            if (channelTree != null && state() != null && state().connectionId == themeTreeConnection)
            {
                channelTree.restoreCollapsed(themeCollapsedChannels);
            }
        }
        finally
        {
            restoringTheme = false;
        }
        ListView next = screen == 1 ? chatList : screen == 0 ? peopleList : screen == 2 ? treeLayout() ? channelTree : channelsList : null;
        if (next != null)
        {
            next.post(() -> next.setSelectionFromTop(first, top));
        }
        if (meScroll != null)
        {
            ScrollView nextScroll = meScroll;
            nextScroll.post(() -> nextScroll.scrollTo(0, scroll));
        }
        if (composer != null)
        {
            composer.setSelection(Math.max(0, Math.min(selectionStart, composer.length())), Math.max(0, Math.min(selectionEnd, composer.length())));
            if (focused)
            {
                composer.requestFocus();
            }
        }
    }

    private void rememberLayoutForResize()
    {
        preserveDraft();
        RetainedUiState.LayoutPosition position = new RetainedUiState.LayoutPosition();
        position.screen = screen;
        position.conversation = conversationId();
        position.scope = renderedScope;
        position.connection = renderedConnectionId;
        ListView list = screen == 1 ? chatList : screen == 0 ? peopleList : screen == 2 ? channelTree != null ? channelTree : channelsList : null;
        if (list != null)
        {
            position.first = list.getFirstVisiblePosition();
            View first = list.getChildAt(0);
            position.top = first == null ? 0 : (first.getTop() - list.getPaddingTop()) / getResources().getDisplayMetrics().density;
        }
        position.scroll = meScroll == null ? 0 : meScroll.getScrollY() / getResources().getDisplayMetrics().density;
        position.selectionStart = composer == null ? 0 : composer.getSelectionStart();
        position.selectionEnd = composer == null ? 0 : composer.getSelectionEnd();
        ChannelTreeView tree = screen == 2 ? channelTree : null;
        if (tree != null)
        {
            position.collapsed = tree.collapsedChannels();
        }
        retained.layoutPosition = position;
    }

    private boolean applySavedUiScale()
    {
        if (resizingUi)
        {
            return true;
        }
        if (service == null || appliedUiScale() == service.localSettings().getInt("native_ui_scale"))
        {
            return false;
        }
        localSettings = service.localSettings();
        rememberLayoutForResize();
        resizingUi = true;
        recreate();
        return true;
    }

    private void restoreResizedLayout()
    {
        RetainedUiState.LayoutPosition position = retained.layoutPosition;
        if (position == null || service == null)
        {
            return;
        }
        retained.layoutPosition = null;
        if (position.screen != screen || !java.util.Objects.equals(position.scope, renderedScope) || screen == 1 && !java.util.Objects.equals(position.conversation, conversationId()))
        {
            return;
        }
        ChannelTreeView tree = screen == 2 ? channelTree : null;
        if (tree != null && position.connection == renderedConnectionId)
        {
            tree.restoreCollapsed(position.collapsed);
        }
        ListView list = screen == 1 ? chatList : screen == 0 ? peopleList : screen == 2 ? channelTree != null ? channelTree : channelsList : null;
        if (list != null)
        {
            list.post(() -> list.setSelectionFromTop(position.first, dp(position.top)));
        }
        if (meScroll != null)
        {
            ScrollView scroll = meScroll;
            scroll.post(() -> scroll.scrollTo(0, dp(position.scroll)));
        }
        if (composer != null)
        {
            composer.setSelection(Math.max(0, Math.min(position.selectionStart, composer.length())), Math.max(0, Math.min(position.selectionEnd, composer.length())));
        }
    }

    private void togglePeopleStrip()
    {
        if (service == null)
        {
            return;
        }
        Map<String, Object> draft = localSettings.asMap();
        draft.put("lemon_strip_vertical", !localSettings.getBoolean("lemon_strip_vertical"));
        service.saveLocalSettings(draft, error ->
        {
            if (isFinishing() || isDestroyed() || !started)
            {
                return;
            }
            if (error != null)
            {
                showNotice(error);
                return;
            }
            localSettings = LocalSettings.load(this);
            rebuildTheme();
        });
    }

    private void openConversation(Member member)
    {
        if (member.musicBot || state() == null || member.id == state().clientId)
        {
            return;
        }
        preserveDraft();
        peerConversation = member.conversationId;
        selectScreen(1);
    }

    private boolean canCall(Member member)
    {
        SessionSnapshot snapshot = state();
        return member != null && !member.musicBot && member.idle && snapshot != null && snapshot.connected && snapshot.idleAllowed && !snapshot.idle && snapshot.channelId >= 0 && member.id != snapshot.clientId;
    }

    private void showCall(Member member)
    {
        if (!canCall(member))
        {
            return;
        }
        long origin = state().connectionId;
        LinearLayout form = sheet("Call " + member.name);
        TextView description = text("Invite " + member.name + " to join you in " + channelName(state().channelId) + ". They can answer or decline. When they answer, you both use this channel’s voice chat.", 15, palette.secondary, false);
        description.setLineSpacing(dp(4), 1);
        form.addView(description);
        Button call = button("Call", true);
        centeredAction(form, call);
        call.setOnClickListener(view ->
        {
            if (currentCallTarget(member, origin) && service.callMember(member.id))
            {
                openDialog.dismiss();
                showNotice("Calling " + member.name);
            }
            else
            {
                showNotice("This person is no longer available for a call");
            }
        });
        showSheet(form);
    }

    private boolean currentCallTarget(Member target, long origin)
    {
        SessionSnapshot snapshot = state();
        if (snapshot == null || snapshot.connectionId != origin)
        {
            return false;
        }
        for (Member member : snapshot.members)
        {
            if (member.id == target.id && member.publicKey.equals(target.publicKey))
            {
                return canCall(member);
            }
        }
        return false;
    }

    private void rememberNavigation(Intent intent)
    {
        if (intent == null)
        {
            return;
        }
        try
        {
            Intent navigation = new Intent();
            String[] keys = {ChatService.EXTRA_CONVERSATION_ID, ChatService.EXTRA_CONVERSATION_SCOPE, ChatService.EXTRA_CALL_ID};
            boolean present = intent.getBooleanExtra(ChatService.EXTRA_SHOW_CHANNEL, false);
            for (String key : keys)
            {
                String value = intent.getStringExtra(key);
                if (value != null && value.length() <= 128)
                {
                    navigation.putExtra(key, value);
                    present = true;
                }
            }
            if (present)
            {
                navigation.putExtra(ChatService.EXTRA_CONNECTION_ID, intent.getLongExtra(ChatService.EXTRA_CONNECTION_ID, -1));
                navigation.putExtra(ChatService.EXTRA_SHOW_CHANNEL, intent.getBooleanExtra(ChatService.EXTRA_SHOW_CHANNEL, false));
                pendingNavigation = navigation;
            }
        }
        catch (android.os.BadParcelableException | ClassCastException invalid)
        {
            pendingNavigation = null;
        }
    }

    /** opens only notifications belonging to the current server and identity */
    private void openPendingNavigation()
    {
        if (pendingNavigation == null || service == null || service.identity() == null)
        {
            return;
        }
        Intent intent = pendingNavigation;
        pendingNavigation = null;
        long connectionId = intent.getLongExtra(ChatService.EXTRA_CONNECTION_ID, -1);
        String callId = intent.getStringExtra(ChatService.EXTRA_CALL_ID);
        if (intent.getBooleanExtra(ChatService.EXTRA_SHOW_CHANNEL, false))
        {
            if (state() == null || !state().connected || state().connectionId != connectionId)
            {
                showNotice("This call is no longer available");
                return;
            }
            preserveDraft();
            peerConversation = null;
            presentedCall = callId;
            screen = 1;
            renderedConversation = null;
            return;
        }
        if (callId != null)
        {
            CallInvitation call = state() != null ? state().call : null;
            if (call != null && call.callId.equals(callId) && call.connectionId == connectionId && !call.expired())
            {
                launchIncomingCall(call);
            }
            else
            {
                showNotice("This call is no longer available");
            }
            return;
        }
        String conversation = intent.getStringExtra(ChatService.EXTRA_CONVERSATION_ID);
        String scope = intent.getStringExtra(ChatService.EXTRA_CONVERSATION_SCOPE);
        if (!service.canOpenNotification(conversation, scope, connectionId))
        {
            showNotice("This notification belongs to another connection or identity");
            return;
        }
        preserveDraft();
        peerConversation = conversation.startsWith("user:") ? conversation : null;
        screen = 1;
        renderedConversation = null;
        PokeMessage poke = service.consumePoke(conversation);
        if (poke != null)
        {
            LinearLayout form = sheet("Poke from " + poke.senderName);
            TextView message = text(poke.text.isEmpty() ? poke.senderName + " wants your attention." : poke.text, 16, palette.text, false);
            message.setLineSpacing(dp(4), 1);
            form.addView(message);
            Button done = button("Got it", true);
            centeredAction(form, done);
            done.setOnClickListener(view -> openDialog.dismiss());
            showSheet(form);
        }
    }

    private void showIncomingCall()
    {
        CallInvitation call = state() != null ? state().call : null;
        if (call == null)
        {
            presentedCall = null;
        }
        else if (!call.callId.equals(presentedCall) && !call.expired())
        {
            launchIncomingCall(call);
        }
    }

    private void launchIncomingCall(CallInvitation call)
    {
        presentedCall = call.callId;
        Intent intent = new Intent(this, IncomingCallActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        intent.putExtra(ChatService.EXTRA_CALL_ID, call.callId);
        intent.putExtra(ChatService.EXTRA_CONNECTION_ID, call.connectionId);
        startActivity(intent);
    }

    private void preserveDraft()
    {
        if (composer != null && renderedConversation != null)
        {
            retained.storeDraft(renderedConversation, composer.getText().toString());
        }
    }

    private SessionSnapshot state()
    {
        return service != null ? service.snapshot() : null;
    }

    private String conversationId()
    {
        return peerConversation != null ? peerConversation : "channel:" + (state() != null ? state().channelId : -1);
    }

    private String conversationTitle()
    {
        Member peer = selectedPeer();
        return peer != null ? peer.name : peerConversation != null && service != null ? service.conversationTitle(peerConversation) : state() != null && state().channelId >= 0 ? channelName(state().channelId) : "Chat";
    }

    private Member selectedPeer()
    {
        if (state() != null && peerConversation != null)
        {
            for (Member member : state().members)
            {
                if (!member.musicBot && member.conversationId.equals(peerConversation))
                {
                    return member;
                }
            }
        }
        return null;
    }

    private String channelName(int channelId)
    {
        if (state() != null)
        {
            for (Channel channel : state().channels)
            {
                if (channel.id == channelId)
                {
                    return channel.name;
                }
            }
        }
        return "channel";
    }

    private List<Member> membersInChannel(int id)
    {
        List<Member> values = new ArrayList<>();
        if (state() != null)
        {
            for (Member member : state().members)
            {
                if (member.channelId == id)
                {
                    values.add(member);
                }
            }
        }
        return values;
    }

    private String channelMemberSummary()
    {
        List<Member> values = membersInChannel(state().channelId);
        StringBuilder names = new StringBuilder();
        for (int i = 0; i < Math.min(values.size(), 3); i++)
        {
            if (i != 0)
            {
                names.append(", ");
            }
            names.append(values.get(i).id == state().clientId ? "You" : values.get(i).name);
        }
        if (values.size() > 3)
        {
            names.append(" +").append(values.size() - 3);
        }
        return values.size() == 1 ? "Only you are here" : names.toString();
    }

    private FrameLayout avatarStack(List<Member> members)
    {
        FrameLayout stack = new FrameLayout(this);
        for (int i = Math.min(3, members.size()) - 1; i >= 0; i--)
        {
            Member member = members.get(i);
            View face = memberAvatar(member, 20);
            FrameLayout.LayoutParams position = new FrameLayout.LayoutParams(dp(20), dp(20), Gravity.CENTER_VERTICAL | Gravity.LEFT);
            position.leftMargin = dp(i * 23);
            stack.addView(face, position);
        }
        return stack;
    }

    private int avatarStackWidth(int count)
    {
        return dp(Math.min(3, count) * 23 + 3);
    }

    private LinearLayout personRow(Member member, String subtitle)
    {
        LinearLayout row = horizontal();
        row.setOnLongClickListener(view ->
        {
            showMemberInfo(member);
            return true;
        });
        row.setFocusable(!member.musicBot);
        row.setPadding(dp(12), dp(10), dp(12), dp(10));
        row.setBackground(palette.interactive(palette.surface, dp(palette.theme.radius)));
        row.addView(memberAvatar(member, 42), new LinearLayout.LayoutParams(dp(42), dp(42)));
        LinearLayout labels = vertical();
        labels.setPadding(dp(14), 0, dp(8), 0);
        TextView name = text(member.name + (localSettings.getBoolean("lemon_show_flags") ? countryFlag(member.countryCode) : ""), 17, palette.text, true);
        name.setSingleLine();
        labels.addView(name);
        boolean songVisible = member.musicBot && member.streamingSong && !member.songName.isEmpty();
        TextView preview = text(songVisible ? member.songName : subtitle, 13, palette.secondary, false);
        preview.setSingleLine();
        preview.setEllipsize(songVisible ? android.text.TextUtils.TruncateAt.MARQUEE : android.text.TextUtils.TruncateAt.END);
        preview.setMarqueeRepeatLimit(-1);
        preview.setSelected(songVisible);
        preview.setPadding(0, dp(4), 0, 0);
        labels.addView(preview);
        row.addView(labels, new LinearLayout.LayoutParams(0, -2, 1));
        if (!member.musicBot)
        {
            TextView dot = text("●", 11, member.idle ? palette.secondary : Color.parseColor(palette.dark ? "#9DCB82" : "#688D4D"), false);
            dot.setContentDescription(member.idle ? "Away" : "Online");
            row.addView(dot);
        }
        if (canCall(member))
        {
            ImageButton call = iconButton("call", "Call " + member.name, false);
            call.setOnClickListener(view -> showCall(member));
            LinearLayout.LayoutParams position = new LinearLayout.LayoutParams(dp(48), dp(48));
            position.leftMargin = dp(8);
            row.addView(call, position);
        }
        return row;
    }

    private LinearLayout actionRow(String title, String subtitle, String glyph, View.OnClickListener action)
    {
        LinearLayout row = horizontal();
        row.setPadding(dp(16), dp(15), dp(14), dp(15));
        row.setBackground(palette.interactive(palette.surface, dp(palette.theme.radius)));
        TextView icon = text("", 12, palette.text, false);
        icon.setBackground(new GlyphDrawable(glyph, palette.secondary));
        row.addView(icon, new LinearLayout.LayoutParams(dp(22), dp(22)));
        LinearLayout labels = vertical();
        labels.setPadding(dp(14), 0, dp(8), 0);
        labels.addView(text(title, 15, palette.text, true));
        TextView detail = text(subtitle, 12, palette.secondary, false);
        detail.setPadding(0, dp(4), 0, 0);
        labels.addView(detail);
        row.addView(labels, new LinearLayout.LayoutParams(0, -2, 1));
        row.setOnClickListener(action);
        return row;
    }

    private LinearLayout settingRow(String title, String subtitle, boolean value, java.util.function.Consumer<Boolean> changed)
    {
        LinearLayout row = horizontal();
        row.setPadding(dp(16), dp(14), dp(12), dp(14));
        row.setBackground(palette.rounded(palette.surface, dp(palette.theme.radius)));
        LinearLayout labels = vertical();
        labels.addView(text(title, 15, palette.text, true));
        TextView detail = text(subtitle, 12, palette.secondary, false);
        detail.setPadding(0, dp(4), dp(12), 0);
        labels.addView(detail);
        row.addView(labels, new LinearLayout.LayoutParams(0, -2, 1));
        android.widget.Switch toggle = new android.widget.Switch(this);
        toggle.setContentDescription(title);
        toggle.setChecked(value);
        toggle.setThumbTintList(android.content.res.ColorStateList.valueOf(value ? palette.accentText : palette.secondary));
        toggle.setOnCheckedChangeListener((button, checked) ->
        {
            toggle.setThumbTintList(android.content.res.ColorStateList.valueOf(checked ? palette.accentText : palette.secondary));
            changed.accept(checked);
        });
        row.addView(toggle);
        row.setOnClickListener(view -> toggle.setChecked(!toggle.isChecked()));
        return row;
    }

    private LinearLayout scrollContent()
    {
        ScrollView scroll = new ScrollView(this);
        scroll.setClipToPadding(false);
        scroll.setPadding(dp(18), dp(4), dp(18), dp(18));
        LinearLayout list = vertical();
        scroll.addView(list);
        content.addView(scroll, new FrameLayout.LayoutParams(-1, -1));
        return list;
    }

    private void section(LinearLayout parent, String value)
    {
        TextView title = text(value, 10, palette.secondary, true);
        title.setLetterSpacing(0.12f);
        title.setPadding(dp(4), dp(20), dp(4), dp(12));
        parent.addView(title);
    }

    private void empty(LinearLayout parent, String title, String detail)
    {
        LinearLayout block = vertical();
        block.setPadding(dp(28), dp(56), dp(28), dp(38));
        block.setGravity(Gravity.CENTER);
        TextView heading = text(title, 20, palette.text, true);
        heading.setGravity(Gravity.CENTER);
        block.addView(heading);
        TextView description = text(detail, 14, palette.secondary, false);
        description.setGravity(Gravity.CENTER);
        description.setLineSpacing(dp(4), 1);
        description.setPadding(0, dp(12), 0, 0);
        block.addView(description);
        parent.addView(block, new LinearLayout.LayoutParams(-1, -2));
    }

    private void label(LinearLayout parent, String value)
    {
        TextView label = text(value, 12, palette.secondary, true);
        label.setPadding(dp(2), dp(18), dp(2), dp(8));
        parent.addView(label);
    }

    private EditText input(String hint, boolean secret)
    {
        EditText field = new EditText(this);
        field.setTextSize(16);
        field.setTextColor(palette.text);
        field.setHintTextColor(palette.secondary);
        field.setPadding(dp(15), dp(13), dp(15), dp(13));
        field.setMinHeight(dp(50));
        field.setSingleLine(true);
        field.setHint(hint);
        field.setBackground(palette.rounded(palette.input, dp(palette.theme.radius)));
        field.setInputType(secret ? InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD : InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        return field;
    }

    private Button button(String value, boolean primary)
    {
        Button button = new Button(this);
        button.setText(value);
        button.setAllCaps(false);
        button.setTextSize(14);
        button.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        button.setTextColor(primary ? palette.accentText : palette.text);
        button.setMinHeight(0);
        button.setMinimumHeight(0);
        button.setPadding(dp(18), dp(10), dp(18), dp(10));
        button.setBackground(palette.interactive(primary ? palette.accent : palette.surface, dp(palette.theme.radius)));
        return button;
    }

    private ImageButton iconButton(String glyph, String description, boolean primary)
    {
        ImageButton button = new ImageButton(this);
        int color = primary ? palette.accentText : palette.text;
        if (glyph.equals("attach") || glyph.equals("music"))
        {
            android.graphics.drawable.Drawable icon = getDrawable(glyph.equals("attach") ? com.lemonchat.R.drawable.ic_attach : com.lemonchat.R.drawable.ic_music).mutate();
            icon.setTint(color);
            button.setImageDrawable(icon);
        }
        else
        {
            button.setImageDrawable(new GlyphDrawable(glyph, color));
        }
        button.setScaleType(android.widget.ImageView.ScaleType.FIT_CENTER);
        button.setPadding(dp(13), dp(13), dp(13), dp(13));
        button.setBackground(palette.interactive(primary ? palette.accent : Color.TRANSPARENT, dp(28)));
        button.setContentDescription(description);
        return button;
    }

    private ImageButton toolbarButton(String glyph, String description)
    {
        ImageButton button = iconButton(glyph, description, false);
        button.setImageDrawable(new GlyphDrawable(glyph, palette.barText));
        button.setPadding(dp(10), dp(10), dp(10), dp(10));
        return button;
    }

    private View memberAvatar(Member member, int size)
    {
        android.graphics.drawable.Drawable picture = avatarImages.drawable(member);
        if (picture == null)
        {
            return avatar(member.musicBot ? "♫" : member.name, member.musicBot, size);
        }
        android.widget.ImageView view = new android.widget.ImageView(this);
        view.setImageDrawable(picture);
        view.setContentDescription(member.name);
        view.setLayoutParams(new LinearLayout.LayoutParams(dp(size), dp(size)));
        return view;
    }

    static String countryFlag(String code)
    {
        if (code == null || !code.matches("(?i)[a-z]{2}"))
        {
            return "";
        }
        String upper = code.toUpperCase(java.util.Locale.ROOT);
        return " " + new String(Character.toChars(0x1f1e6 + upper.charAt(0) - 'A')) + new String(Character.toChars(0x1f1e6 + upper.charAt(1) - 'A'));
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event)
    {
        if (keyCode == KeyEvent.KEYCODE_F && event.isCtrlPressed() && localSettings.getBoolean("lemon_chat_search"))
        {
            openSearch();
            return true;
        }
        if (keyCode == localSettings.getInt("native_ptt_key_code") && !event.isCtrlPressed() && !event.isAltPressed() && service != null && service.voiceReady() && (!localSettings.getBoolean("lemon_ptt_disable_while_typing") || !(getCurrentFocus() instanceof EditText)))
        {
            if (event.getRepeatCount() == 0 && checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED)
            {
                keyboardTalking = true;
                service.setTalking(true);
            }
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event)
    {
        if (keyboardTalking && keyCode == localSettings.getInt("native_ptt_key_code"))
        {
            keyboardTalking = false;
            if (service != null)
            {
                service.setTalking(false);
            }
            return true;
        }
        return super.onKeyUp(keyCode, event);
    }

    private TextView avatar(String name, boolean bot, int size)
    {
        TextView face = text(bot ? "♫" : initials(name), size * 0.34f, palette.accentText, true);
        face.setGravity(Gravity.CENTER);
        face.setBackground(palette.rounded(palette.accent, dp(size / 2f)));
        face.setLayoutParams(new LinearLayout.LayoutParams(dp(size), dp(size)));
        return face;
    }

    private String initials(String name)
    {
        if (name == null || name.trim().isEmpty())
        {
            return "?";
        }
        String[] words = name.trim().split("\\s+");
        String first = new String(Character.toChars(words[0].codePointAt(0)));
        if (words.length > 1)
        {
            first += new String(Character.toChars(words[words.length - 1].codePointAt(0)));
        }
        return first.toUpperCase(java.util.Locale.ROOT);
    }

    private TextView text(String value, float size, int color, boolean bold)
    {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        view.setIncludeFontPadding(false);
        view.setTypeface(Typeface.create(bold ? "sans-serif-medium" : "sans-serif", Typeface.NORMAL));
        return view;
    }

    private LinearLayout horizontal()
    {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.HORIZONTAL);
        layout.setGravity(Gravity.CENTER_VERTICAL);
        return layout;
    }

    private LinearLayout vertical()
    {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        return layout;
    }

    private LinearLayout.LayoutParams rowMargins()
    {
        LinearLayout.LayoutParams position = new LinearLayout.LayoutParams(-1, -2);
        position.bottomMargin = dp(10);
        return position;
    }

    private void centeredAction(LinearLayout form, Button action)
    {
        LinearLayout.LayoutParams position = new LinearLayout.LayoutParams(dp(190), dp(50));
        position.topMargin = dp(24);
        position.gravity = Gravity.CENTER_HORIZONTAL;
        form.addView(action, position);
    }

    private void showNotice(String message)
    {
        if (!isFinishing() && !isDestroyed())
        {
            Toast.makeText(this, message, Toast.LENGTH_LONG).show();
        }
    }

    private int dp(float value)
    {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }
}
