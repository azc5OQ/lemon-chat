package com.lemonchat.server;

import android.app.Activity;
import android.annotation.SuppressLint;
import android.app.AlertDialog;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.InputFilter;
import android.text.InputType;
import android.text.TextWatcher;
import android.text.method.PasswordTransformationMethod;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import java.io.File;
import java.lang.ref.WeakReference;
import java.util.function.Consumer;

/** guides new hosts without changing an existing server configuration */
public final class SetupActivity extends Activity
{
    private static final int INK = Color.rgb(35, 42, 30);
    private static final int MUTED = Color.rgb(95, 103, 88);
    private static final int GREEN = Color.rgb(66, 101, 27);
    private State state;
    private LinearLayout body;
    private TextView heading;
    private TextView progressText;
    private TextView problem;
    private ProgressBar progress;
    private Button back;
    private Button next;
    private boolean resumed;
    private OnBackInvokedCallback backCallback;

    @Override
    public void onCreate(Bundle saved)
    {
        super.onCreate(saved);
        boolean retained = getLastNonConfigurationInstance() instanceof State;
        state = retained ? (State)getLastNonConfigurationInstance() : new State();
        if (!retained)
        {
            state.remoteAccess = HostingSettings.IsFunnelEnabled(this);
            if (state.remoteAccess)
            {
                state.draft.transport = "websocket";
            }
            state.draft.webPage = HostingSettings.WebPage(this);
            state.draft.webPagePort = Integer.toString(HostingSettings.WebPagePort(this));
            state.draft.isPageConfigEmbedded = HostingSettings.IsPageConfigEmbedded(this);
        }
        state.owner = new WeakReference<>(this);
        if (ServerSettings.Exists(getFilesDir()) && !state.saved && !state.busy)
        {
            setResult(RESULT_CANCELED);
            finish();
            return;
        }
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        FrameLayout root = new FrameLayout(this);
        root.setFitsSystemWindows(true);
        root.setBackgroundColor(Color.rgb(250, 250, 245));
        root.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS);
        LinearLayout page = Column();
        root.addView(page, new FrameLayout.LayoutParams(-1, -1, Gravity.CENTER_HORIZONTAL));
        root.addOnLayoutChangeListener((view, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) ->
        {
            int width = Math.min(right - left, Dp(560));
            if (width > 0 && page.getLayoutParams().width != width)
            {
                page.getLayoutParams().width = width;
                page.requestLayout();
            }
        });
        LinearLayout header = Column();
        header.setPadding(Dp(24), Dp(20), Dp(24), Dp(12));
        page.addView(header);
        Label(header, "LEMON CHAT SERVER", 12, true).setTextColor(GREEN);
        progressText = Label(header, "", 14, false);
        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(SetupDraft.STEPS);
        progress.setProgressTintList(ColorStateList.valueOf(GREEN));
        LinearLayout.LayoutParams bar = new LinearLayout.LayoutParams(-1, Dp(4));
        bar.topMargin = Dp(10);
        header.addView(progress, bar);
        ScrollView scroll = new ScrollView(this);
        scroll.setClipToPadding(false);
        scroll.setPadding(Dp(24), Dp(8), Dp(24), Dp(16));
        body = Column();
        scroll.addView(body);
        page.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        LinearLayout footer = Column();
        footer.setPadding(Dp(24), Dp(8), Dp(24), Dp(20));
        problem = Label(footer, "", 14, false);
        problem.setTextColor(Color.rgb(159, 47, 38));
        problem.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        LinearLayout actions = new LinearLayout(this);
        actions.setGravity(Gravity.CENTER_VERTICAL);
        footer.addView(actions, new LinearLayout.LayoutParams(-1, -2));
        back = Button("Back", false, this::GoBack);
        next = Button("Next", true, this::GoNext);
        LinearLayout.LayoutParams previous = new LinearLayout.LayoutParams(0, Dp(50), 1);
        previous.rightMargin = Dp(12);
        actions.addView(back, previous);
        actions.addView(next, new LinearLayout.LayoutParams(0, Dp(50), 2));
        page.addView(footer);
        setContentView(root);
        if (Build.VERSION.SDK_INT >= 33)
        {
            backCallback = this::GoBack;
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, backCallback);
        }
        Render();
    }

    @Override
    public Object onRetainNonConfigurationInstance()
    {
        return state;
    }

    @Override
    public void onResume()
    {
        super.onResume();
        resumed = true;
        Complete();
    }

    @Override
    public void onPause()
    {
        resumed = false;
        super.onPause();
    }

    @Override
    public void onDestroy()
    {
        if (Build.VERSION.SDK_INT >= 33 && backCallback != null)
        {
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(backCallback);
            backCallback = null;
        }
        if (state != null && state.owner.get() == this)
        {
            state.owner.clear();
            if (!isChangingConfigurations())
            {
                state.disposed = true;
                state.draft.ClearPasswords();
            }
        }
        super.onDestroy();
    }

    @Override
    @SuppressLint("GestureBackNavigation")
    public void onBackPressed()
    {
        // older Android versions use this fallback
        GoBack();
    }

    private void Render()
    {
        if (body == null)
        {
            return;
        }
        SetupDraft draft = state.draft;
        body.removeAllViews();
        ((ScrollView)body.getParent()).scrollTo(0, 0);
        progressText.setText("Step " + (draft.step + 1) + " of " + SetupDraft.STEPS);
        progress.setProgress(draft.step + 1);
        back.setText(draft.step == 0 ? "Cancel" : "Back");
        next.setText(draft.step == SetupDraft.STEPS - 1 ? "Start server" : "Next");
        String[] titles = {"Start with nearby friends", "Choose your admin password", "Who can join?", "How many people?", "What can people do?", "Web page for other devices", "Ready to host"};
        heading = Label(body, titles[draft.step], 25, true);
        if (Build.VERSION.SDK_INT >= 28)
        {
            heading.setAccessibilityHeading(true);
        }
        if (draft.step == 0)
        {
            Label(body, "Keep this phone and your friends on the same Wi-Fi network, or let them join your phone's hotspot.", 16, false);
            LinearLayout card = Card(body);
            Label(card, "This phone hosts the server", 17, true);
            Label(card, "After setup, the server screen shows the address your friends enter in their Lemon Chat client.", 15, false);
            Label(body, state.remoteAccess ? "Remote access is already enabled on this phone. That choice is kept. You can review it later in Settings → Advanced." : "This setup starts with your local network. You can configure remote access later in Settings → Advanced.", 14, false);
        }
        else if (draft.step == 1)
        {
            Label(body, "This password lets you manage the server from a connected Lemon Chat client. Keep it to yourself. The optional join password and its encryption type come next.", 16, false);
            EditText admin = Field(body, "Admin password", draft.admin, true, false, value -> draft.admin = value);
            EditText confirmation = Field(body, "Repeat admin password", draft.confirmation, true, false, value -> draft.confirmation = value);
            ShowPasswords(body, admin, confirmation);
            Label(body, "In the client, use the lock icon in the upper toolbar to sign in as administrator.", 14, false);
        }
        else if (draft.step == 2)
        {
            Label(body, "A join password is separate from your admin password. Share it only with the people you want to invite.", 16, false);
            CheckBox required = Check(body, "Require a join password", draft.requireJoin);
            LinearLayout password = Column();
            body.addView(password);
            EditText join = Field(password, "Join password", draft.join, true, false, value -> draft.join = value);
            ShowPasswords(password, join);
            JoinCipher(password, draft);
            Label(password, "Friends enter the same password and choose the same type under Server keys in their client.", 14, false);
            password.setVisibility(draft.requireJoin ? View.VISIBLE : View.GONE);
            TextView open = Label(body, "Without a join password, anyone who can reach the server can connect.", 15, false);
            open.setVisibility(draft.requireJoin ? View.GONE : View.VISIBLE);
            required.setOnCheckedChangeListener((button, checked) ->
            {
                draft.requireJoin = checked;
                password.setVisibility(checked ? View.VISIBLE : View.GONE);
                open.setVisibility(checked ? View.GONE : View.VISIBLE);
            });
        }
        else if (draft.step == 3)
        {
            Label(body, "Choose how many clients can connect at once. Pick a preset or enter your own limit below. Sixteen is a good starting point for a few friends.", 16, false);
            LinearLayout custom = Column();
            EditText capacity = Field(custom, "Custom slot count", draft.slots, false, true, value -> draft.slots = value);
            LinearLayout suggestions = new LinearLayout(this);
            suggestions.setGravity(Gravity.CENTER);
            body.addView(suggestions, new LinearLayout.LayoutParams(-1, Dp(48)));
            for (int count : SetupDraft.SLOT_PRESETS)
            {
                Button choice = Button(Integer.toString(count), false, () -> capacity.setText(Integer.toString(count)));
                LinearLayout.LayoutParams size = new LinearLayout.LayoutParams(0, -1, 1);
                size.setMargins(Dp(3), 0, Dp(3), 0);
                suggestions.addView(choice, size);
            }
            custom.setPadding(0, Dp(20), 0, 0);
            body.addView(custom);
            Label(body, "For a larger server, type a value such as 500. Any whole number from 1 to 100,000 is allowed.", 14, false);
            Label(body, "More slots reserve more memory. You can change this later while the server is stopped.", 14, false);
        }
        else if (draft.step == SetupDraft.STEP_FEATURES)
        {
            Label(body, "Choose what people can do on your server. You can change each choice later in Settings → Advanced while the server is stopped.", 16, false);
            for (int index = 0; index < ServerFeatures.KEYS.length; index++)
            {
                int feature = index;
                CheckBox option = Check(body, ServerFeatures.LABELS[index], draft.features.values[index]);
                option.setOnCheckedChangeListener((button, checked) -> draft.features.values[feature] = checked);
                Label(body, ServerFeatures.NOTES[index], 14, false);
            }
        }
        else if (draft.step == SetupDraft.STEP_WEB_PAGE)
        {
            WebPageChoice(body, draft);
        }
        else
        {
            Label(body, "Your choices are saved on this phone when you start. The next screen shows the server status and connection address.", 16, false);
            LinearLayout review = Card(body);
            Label(review, draft.webPage.equals(HostingSettings.PAGE_FUNNEL) ? "Internet through Tailscale Funnel" : "Local network", 17, true);
            Label(review, draft.slots + " client slots", 15, false);
            Label(review, "Admin password set", 15, false);
            Label(review, draft.requireJoin ? "Join password required · " + SetupDraft.JOIN_CIPHER_NAMES.get(SetupDraft.JOIN_CIPHERS.indexOf(draft.joinCipher)) : "No join password", 15, false);
            Label(review, FeatureSummary(draft.features), 15, false);
            Label(review, WebPageSummary(draft), 15, false);
            TextView voice = Label(review, "", 15, false);
            Runnable mediaSummary = () -> voice.setText(!draft.features.IsEnabled("is_voice_chat_active") ? "Voice chat off"
                : draft.transport.equals("disabled") ? "Voice connections disabled" : "Voice chat available");
            mediaSummary.run();
            Advanced(body, draft, mediaSummary);
            Label(body, "Keep the server running while friends are connected. Android will show a hosting notification.", 14, false);
        }
        Busy();
    }

    /**
     * selects the type saved with the connection password
     *
     * @param parent password section
     * @param draft current setup choices
     */
    private void JoinCipher(LinearLayout parent, SetupDraft draft)
    {
        Label(parent, "Password type", 15, true);
        Spinner types = new Spinner(this);
        types.setContentDescription("Password type");
        types.setSaveEnabled(false);
        types.setPadding(Dp(8), 0, Dp(8), 0);
        types.setBackgroundTintList(ColorStateList.valueOf(GREEN));
        ArrayAdapter<String> choices = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, SetupDraft.JOIN_CIPHER_NAMES);
        choices.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        types.setAdapter(choices);
        types.setSelection(SetupDraft.JOIN_CIPHERS.indexOf(draft.joinCipher));
        LinearLayout.LayoutParams spacing = new LinearLayout.LayoutParams(-1, Dp(52));
        spacing.bottomMargin = Dp(12);
        parent.addView(types, spacing);
        Label(parent, "AES-256 is the default. This choice encrypts the connection with your join password. It does not change the admin password.", 14, false);
        TextView compatibility = Label(parent, "Trivium-80 is a compatibility option with an 80-bit key and a fixed IV.", 14, false);
        compatibility.setVisibility(draft.joinCipher.equals("trivium80") ? View.VISIBLE : View.GONE);
        types.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener()
        {
            @Override
            public void onItemSelected(AdapterView<?> source, View view, int position, long identifier)
            {
                if (position >= 0 && position < SetupDraft.JOIN_CIPHERS.size())
                {
                    draft.joinCipher = SetupDraft.JOIN_CIPHERS.get(position);
                    compatibility.setVisibility(draft.joinCipher.equals("trivium80") ? View.VISIBLE : View.GONE);
                }
            }

            @Override
            public void onNothingSelected(AdapterView<?> source)
            {
            }
        });
    }

    private void Advanced(LinearLayout parent, SetupDraft draft, Runnable changed)
    {
        Button expand = Button(draft.advanced ? "Hide advanced settings" : "Advanced settings", false, () ->
        {
        });
        LinearLayout.LayoutParams spacing = new LinearLayout.LayoutParams(-1, Dp(48));
        spacing.topMargin = Dp(16);
        parent.addView(expand, spacing);
        LinearLayout advanced = Column();
        parent.addView(advanced);
        advanced.setVisibility(draft.advanced ? View.VISIBLE : View.GONE);
        expand.setOnClickListener(view ->
        {
            draft.advanced = !draft.advanced;
            advanced.setVisibility(draft.advanced ? View.VISIBLE : View.GONE);
            expand.setText(draft.advanced ? "Hide advanced settings" : "Advanced settings");
        });
        Field(advanced, "Port", draft.port, false, true, value -> draft.port = value);
        Label(advanced, "Usually leave this at 1111. Choose another port only if it is already in use.", 14, false);
        Label(advanced, "Audio connection", 16, true);
        RadioGroup modes = new RadioGroup(this);
        advanced.addView(modes);
        String[] values = {"webrtc", "auto", "websocket", "disabled"};
        String[] labels = {"WebRTC (default)", "WebRTC with WebSocket fallback", "WebSocket only", "Audio connections disabled"};
        for (int index = 0; index < values.length; index++)
        {
            RadioButton mode = new RadioButton(this);
            mode.setId(View.generateViewId());
            mode.setText(labels[index]);
            mode.setTextColor(INK);
            mode.setTextSize(15);
            mode.setMinHeight(Dp(48));
            mode.setTag(values[index]);
            mode.setContentDescription("audio-mode");
            modes.addView(mode);
            mode.setChecked(values[index].equals(draft.transport));
        }
        modes.setOnCheckedChangeListener((group, selected) ->
        {
            View selectedMode = group.findViewById(selected);
            if (selectedMode != null)
            {
                draft.transport = (String)selectedMode.getTag();
                changed.run();
            }
        });
        if (draft.webPage.equals(HostingSettings.PAGE_FUNNEL))
        {
            Label(advanced, "Tailscale Funnel uses WebSocket audio, because Funnel does not forward UDP. This choice applies again when Funnel is switched off.", 14, false);
        }
    }

    /**
     * @brief Shows the choice of how other devices get the browser page: not at all, over HTTP in the local network
     *        (with its port and whether the page contains the connection details), or over HTTPS through Funnel.
     *
     * @param parent the step's content
     * @param draft current setup choices
     *
     * @return void
     */
    private void WebPageChoice(LinearLayout parent, SetupDraft draft)
    {
        Label(parent, "Friends can use the Lemon Chat app, or open the chat in a web browser. This phone can offer that web page.", 16, false);
        RadioGroup choices = new RadioGroup(this);
        parent.addView(choices);
        LinearLayout local = Column();
        parent.addView(local);
        Field(local, "Web page port", draft.webPagePort, false, true, value -> draft.webPagePort = value);
        CheckBox embedded = Check(local, "Visitors connect without typing the address", draft.isPageConfigEmbedded);
        embedded.setOnCheckedChangeListener((button, checked) -> draft.isPageConfigEmbedded = checked);
        Label(local, "The page then contains the chat port and the join password, so anyone who can open the page can connect.", 14, false);
        TextView funnel = Label(parent, "After the start, the server screen asks you to sign in to Tailscale. Enable HTTPS and Funnel in your Tailscale account. Anyone on the internet can then open the page; your join password still applies.", 14, false);
        Label(parent, "Web browsers allow the microphone only on HTTPS pages. On the HTTP page, visitors can read and write but not talk.", 14, false);
        Runnable visibility = () ->
        {
            local.setVisibility(draft.webPage.equals(HostingSettings.PAGE_HTTP) ? View.VISIBLE : View.GONE);
            funnel.setVisibility(draft.webPage.equals(HostingSettings.PAGE_FUNNEL) ? View.VISIBLE : View.GONE);
        };
        String[] values = {HostingSettings.PAGE_NONE, HostingSettings.PAGE_HTTP, HostingSettings.PAGE_FUNNEL};
        String[] labels = {"Do not offer the web page", "Offer it in the local network (HTTP)", "Offer it on the internet through Tailscale Funnel (HTTPS)"};
        for (int index = 0; index < values.length; index++)
        {
            RadioButton choice = new RadioButton(this);
            choice.setId(View.generateViewId());
            choice.setText(labels[index]);
            choice.setTextColor(INK);
            choice.setTextSize(15);
            choice.setMinHeight(Dp(48));
            choice.setTag(values[index]);
            choices.addView(choice);
            choice.setChecked(values[index].equals(draft.webPage));
        }
        choices.setOnCheckedChangeListener((group, selected) ->
        {
            View chosen = group.findViewById(selected);
            if (chosen != null)
            {
                draft.webPage = (String)chosen.getTag();
                if (draft.webPage.equals(HostingSettings.PAGE_FUNNEL))
                {
                    draft.transport = "websocket";
                }
                visibility.run();
            }
        });
        visibility.run();
    }

    private static String FeatureSummary(ServerFeatures features)
    {
        StringBuilder enabled = new StringBuilder();
        for (int index = 0; index < ServerFeatures.KEYS.length; index++)
        {
            if (features.values[index])
            {
                enabled.append(enabled.length() == 0 ? "" : ", ").append(ServerFeatures.LABELS[index]);
            }
        }
        return enabled.length() == 0 ? "All features off" : "On: " + enabled;
    }

    private static String WebPageSummary(SetupDraft draft)
    {
        if (draft.webPage.equals(HostingSettings.PAGE_HTTP))
        {
            return "Web page over HTTP on port " + draft.webPagePort;
        }
        return draft.webPage.equals(HostingSettings.PAGE_FUNNEL) ? "Web page over HTTPS through Tailscale Funnel" : "No web page for other devices";
    }

    private void GoNext()
    {
        if (state.busy)
        {
            return;
        }
        String error = state.draft.Problem(state.draft.step);
        if (error != null)
        {
            problem.setText(error);
            problem.setVisibility(View.VISIBLE);
            problem.announceForAccessibility(error);
            return;
        }
        HideKeyboard();
        state.failure = null;
        if (state.draft.step < SetupDraft.STEPS - 1)
        {
            state.draft.step++;
            Render();
            return;
        }
        state.Start(getApplicationContext());
        Busy();
    }

    private void GoBack()
    {
        if (state == null || state.busy)
        {
            return;
        }
        HideKeyboard();
        if (state.draft.step > 0)
        {
            state.draft.step--;
            state.failure = null;
            Render();
        }
        else
        {
            new AlertDialog.Builder(this).setTitle("Leave setup?").setMessage("Your unfinished choices have not been saved.").setNegativeButton("Keep setting up", null).setPositiveButton("Leave", (dialog, which) -> finish()).show();
        }
    }

    private void HideKeyboard()
    {
        getSystemService(InputMethodManager.class).hideSoftInputFromWindow(next.getWindowToken(), 0);
        body.clearFocus();
    }

    private void Complete()
    {
        if (!resumed || isFinishing() || state == null)
        {
            return;
        }
        if (state.saved)
        {
            setResult(RESULT_OK);
            finish();
            return;
        }
        Busy();
    }

    private void Busy()
    {
        if (next == null)
        {
            return;
        }
        next.setEnabled(!state.busy);
        back.setEnabled(!state.busy);
        Enable(body, !state.busy);
        if (state.busy)
        {
            next.setText("Saving…");
        }
        else
        {
            next.setText(state.draft.step == SetupDraft.STEPS - 1 ? "Start server" : "Next");
        }
        problem.setText(state.failure == null ? "" : state.failure);
        problem.setVisibility(state.failure == null ? View.GONE : View.VISIBLE);
    }

    private void Enable(View view, boolean enabled)
    {
        view.setEnabled(enabled && !(state.draft.webPage.equals(HostingSettings.PAGE_FUNNEL) && "audio-mode".equals(view.getContentDescription())));
        if (view instanceof android.view.ViewGroup)
        {
            android.view.ViewGroup group = (android.view.ViewGroup)view;
            for (int index = 0; index < group.getChildCount(); index++)
            {
                Enable(group.getChildAt(index), enabled);
            }
        }
    }

    private LinearLayout Column()
    {
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        return column;
    }

    private TextView Label(LinearLayout parent, String text, int size, boolean bold)
    {
        TextView label = new TextView(this);
        label.setText(text);
        label.setTextSize(size);
        label.setTextColor(bold ? INK : MUTED);
        label.setLineSpacing(Dp(3), 1);
        if (bold)
        {
            label.setTypeface(null, Typeface.BOLD);
        }
        LinearLayout.LayoutParams spacing = new LinearLayout.LayoutParams(-1, -2);
        spacing.bottomMargin = Dp(12);
        parent.addView(label, spacing);
        return label;
    }

    private LinearLayout Card(LinearLayout parent)
    {
        LinearLayout card = Column();
        card.setPadding(Dp(18), Dp(18), Dp(18), Dp(4));
        card.setBackground(Background(Color.WHITE, Color.rgb(222, 226, 212)));
        LinearLayout.LayoutParams spacing = new LinearLayout.LayoutParams(-1, -2);
        spacing.setMargins(0, Dp(8), 0, Dp(20));
        parent.addView(card, spacing);
        return card;
    }

    private Button Button(String text, boolean primary, Runnable action)
    {
        Button button = new Button(this);
        button.setAllCaps(false);
        button.setText(text);
        button.setTextSize(15);
        button.setTypeface(null, Typeface.BOLD);
        button.setTextColor(primary ? Color.WHITE : GREEN);
        button.setBackground(Background(primary ? GREEN : Color.WHITE, primary ? GREEN : Color.rgb(215, 224, 203)));
        button.setOnClickListener(view -> action.run());
        return button;
    }

    private CheckBox Check(LinearLayout parent, String text, boolean checked)
    {
        CheckBox box = new CheckBox(this);
        box.setText(text);
        box.setTextSize(16);
        box.setTextColor(INK);
        box.setMinHeight(Dp(52));
        box.setChecked(checked);
        parent.addView(box, new LinearLayout.LayoutParams(-1, -2));
        return box;
    }

    private EditText Field(LinearLayout parent, String label, String value, boolean password, boolean numeric, Consumer<String> changed)
    {
        Label(parent, label, 15, true);
        EditText field = new EditText(this);
        field.setSingleLine(true);
        field.setTextSize(16);
        field.setTextColor(INK);
        field.setPadding(Dp(14), Dp(12), Dp(14), Dp(12));
        field.setBackground(Background(Color.WHITE, Color.rgb(205, 214, 192)));
        field.setInputType(numeric ? InputType.TYPE_CLASS_NUMBER : InputType.TYPE_CLASS_TEXT | (password ? InputType.TYPE_TEXT_VARIATION_PASSWORD : InputType.TYPE_TEXT_FLAG_CAP_SENTENCES));
        field.setFilters(new InputFilter[]{new InputFilter.LengthFilter(numeric ? 6 : 255)});
        field.setSaveEnabled(false);
        field.setText(value);
        field.setContentDescription(label);
        field.addTextChangedListener(new TextWatcher()
        {
            @Override
            public void beforeTextChanged(CharSequence text, int start, int count, int after)
            {
            }

            @Override
            public void onTextChanged(CharSequence text, int start, int before, int count)
            {
                changed.accept(text.toString());
            }

            @Override
            public void afterTextChanged(Editable text)
            {
            }
        });
        LinearLayout.LayoutParams spacing = new LinearLayout.LayoutParams(-1, -2);
        spacing.bottomMargin = Dp(20);
        parent.addView(field, spacing);
        return field;
    }

    private void ShowPasswords(LinearLayout parent, EditText... fields)
    {
        CheckBox show = Check(parent, "Show passwords", false);
        show.setOnCheckedChangeListener((button, checked) ->
        {
            for (EditText field : fields)
            {
                field.setTransformationMethod(checked ? null : PasswordTransformationMethod.getInstance());
                field.setSelection(field.length());
            }
        });
    }

    private GradientDrawable Background(int fill, int border)
    {
        GradientDrawable shape = new GradientDrawable();
        shape.setColor(fill);
        shape.setCornerRadius(Dp(12));
        shape.setStroke(Dp(1), border);
        return shape;
    }

    private int Dp(int value)
    {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private static final class State
    {
        private final SetupDraft draft = new SetupDraft();
        private WeakReference<SetupActivity> owner = new WeakReference<>(null);
        private boolean busy;
        private boolean saved;
        private boolean disposed;
        private boolean remoteAccess;
        private String failure;

        private void Start(android.content.Context context)
        {
            File directory = context.getFilesDir();
            if (busy || saved || disposed)
            {
                return;
            }
            busy = true;
            SetupDraft pending = draft.Copy();
            new Thread(() ->
            {
                String error = null;
                try
                {
                    if (ServerService.active)
                    {
                        throw new IllegalStateException("Stop the server before creating settings.");
                    }
                    ServerSettings.Create(directory, pending);
                    HostingSettings.SetWebPage(context, pending.webPage, Integer.parseInt(pending.webPagePort), pending.isPageConfigEmbedded);
                }
                catch (Exception failed)
                {
                    error = failed.getMessage() == null ? "The settings could not be saved. Try again." : failed.getMessage();
                }
                finally
                {
                    pending.ClearPasswords();
                }
                String result = error;
                new Handler(Looper.getMainLooper()).post(() ->
                {
                    busy = false;
                    saved = result == null;
                    failure = result;
                    if (saved || disposed)
                    {
                        draft.ClearPasswords();
                    }
                    SetupActivity activity = owner.get();
                    if (activity != null && !disposed)
                    {
                        activity.Complete();
                    }
                });
            }, "lemon-server-first-setup").start();
        }
    }
}
