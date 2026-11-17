package com.lemonchat.ui;

import android.app.Dialog;
import android.content.Context;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import com.lemonchat.protocol.ServerAddress;

/** walks through connection details before saving or connecting */
final class ConnectionWizard extends Dialog
{
    interface ConnectAction
    {
        String connect(String endpoint, String username, String keys);
    }

    /** keeps unfinished connection details in memory across rotation */
    static final class Draft
    {
        final ServerAddress savedAddress;
        String name;
        String address;
        String port;
        String keys;
        int step;

        Draft(String endpoint, String name, String keys)
        {
            savedAddress = ServerAddress.fromEndpoint(endpoint);
            this.name = name;
            this.keys = keys;
            address = savedAddress.address;
            port = Integer.toString(savedAddress.port);
        }
    }

    private static final String[] TITLES = {"What should people call you?", "Which server are you joining?", "Does the server need passwords?", "Ready to connect"};
    private static final String[] HELP =
    {
        "Choose the name people will see in messages and voice channels. You can change it later.",
        "Ask the person running the server for its address and port. Enter the IP address or hostname below, then their port number.",
        "If the person running the server gave you connection passwords, add them below. Use the key types and order they supplied. If they gave you none, choose Next.",
        "Check your details below. Connect saves this server and opens the chat. You can return to any step with Back."
    };
    private final Palette palette;
    private final Draft draft;
    private final ConnectAction action;
    private TextView progress;
    private ProgressBar progressBar;
    private TextView title;
    private TextView help;
    private TextView notice;
    private LinearLayout page;
    private ScrollView scroll;
    private Button back;
    private Button next;
    private EditText name;
    private EditText address;
    private EditText port;
    private ServerKeyForm keys;

    ConnectionWizard(Context context, Palette palette, Draft draft, ConnectAction action)
    {
        super(context, android.R.style.Theme_Material_Light_NoActionBar);
        this.palette = palette;
        this.draft = draft;
        this.action = action;
    }

    @Override
    protected void onCreate(Bundle saved)
    {
        super.onCreate(saved);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        setCanceledOnTouchOutside(false);
        LinearLayout root = column();
        root.setTag("connection-wizard-card");
        root.setPadding(dp(20), dp(16), dp(20), dp(16));
        root.setBackground(palette.rounded(palette.surface, dp(24)));
        LinearLayout heading = new LinearLayout(getContext());
        heading.setGravity(Gravity.CENTER_VERTICAL);
        progress = text("", 13, palette.secondary, false);
        progress.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        heading.addView(progress, new LinearLayout.LayoutParams(0, -2, 1));
        Button cancel = button("Cancel", false);
        cancel.setOnClickListener(view -> dismiss());
        heading.addView(cancel, new LinearLayout.LayoutParams(-2, dp(48)));
        root.addView(heading);
        progressBar = new ProgressBar(getContext(), null, android.R.attr.progressBarStyleHorizontal);
        progressBar.setMax(TITLES.length);
        progressBar.setProgressTintList(android.content.res.ColorStateList.valueOf(palette.accent));
        root.addView(progressBar, new LinearLayout.LayoutParams(-1, dp(4)));
        scroll = new ScrollView(getContext());
        scroll.setTag("connection-wizard-body");
        scroll.setFillViewport(true);
        LinearLayout body = column();
        body.setPadding(0, dp(22), 0, dp(18));
        title = text("", 24, palette.text, true);
        if (android.os.Build.VERSION.SDK_INT >= 28)
        {
            title.setAccessibilityHeading(true);
        }
        body.addView(title);
        help = text("", 15, palette.secondary, false);
        help.setPadding(0, dp(12), 0, dp(20));
        help.setLineSpacing(dp(3), 1);
        body.addView(help);
        page = column();
        body.addView(page);
        notice = text("", 14, palette.danger, false);
        notice.setPadding(0, dp(12), 0, 0);
        notice.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        body.addView(notice);
        scroll.addView(body);
        root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        LinearLayout navigation = new LinearLayout(getContext());
        navigation.setGravity(Gravity.CENTER_VERTICAL);
        back = button("Back", false);
        back.setOnClickListener(view -> previous());
        navigation.addView(back, new LinearLayout.LayoutParams(0, dp(50), 1));
        next = button("Next", true);
        next.setOnClickListener(view -> advance());
        LinearLayout.LayoutParams nextPosition = new LinearLayout.LayoutParams(0, dp(50), 1);
        nextPosition.leftMargin = dp(12);
        navigation.addView(next, nextPosition);
        root.addView(navigation);
        WizardViewport viewport = new WizardViewport(root);
        setContentView(viewport);
        Window window = getWindow();
        if (window != null)
        {
            window.setBackgroundDrawableResource(android.R.color.transparent);
            window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
            window.setGravity(Gravity.CENTER);
            window.setDimAmount(0.4f);
            window.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
            if (Build.VERSION.SDK_INT >= 30)
            {
                window.setDecorFitsSystemWindows(false);
                viewport.setOnApplyWindowInsetsListener((view, insets) ->
                {
                    android.graphics.Insets edges = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout() | WindowInsets.Type.ime());
                    view.setPadding(edges.left + dp(8), edges.top + dp(8), edges.right + dp(8), edges.bottom + dp(8));
                    return insets;
                });
            }
            window.setLayout(WindowManager.LayoutParams.MATCH_PARENT, WindowManager.LayoutParams.MATCH_PARENT);
        }
        renderStep();
    }

    /** fits the card above the keyboard while its question body scrolls */
    private final class WizardViewport extends FrameLayout
    {
        private final View card;

        WizardViewport(View card)
        {
            super(ConnectionWizard.this.getContext());
            this.card = card;
            setTag("connection-wizard-viewport");
            setPadding(dp(8), dp(8), dp(8), dp(8));
            addView(card, new FrameLayout.LayoutParams(0, 0, Gravity.CENTER));
        }

        @Override
        protected void onMeasure(int widthSpec, int heightSpec)
        {
            FrameLayout.LayoutParams position = (FrameLayout.LayoutParams)card.getLayoutParams();
            position.width = Math.min(dp(520), Math.max(0, MeasureSpec.getSize(widthSpec) - getPaddingLeft() - getPaddingRight()));
            position.height = Math.min(dp(620), Math.max(0, MeasureSpec.getSize(heightSpec) - getPaddingTop() - getPaddingBottom()));
            super.onMeasure(widthSpec, heightSpec);
        }
    }

    /** copies visible passwords into the draft before rotation */
    void capture()
    {
        if (keys != null)
        {
            try
            {
                draft.keys = keys.serialize();
            }
            catch (IllegalArgumentException ignored)
            {
                // keep unreadable saved keys until the user replaces them
            }
        }
    }

    @Override
    public void onBackPressed()
    {
        if (draft.step > 0)
        {
            previous();
        }
        else
        {
            dismiss();
        }
    }

    /** returns to the previous question while keeping entered passwords */
    private void previous()
    {
        capture();
        if (draft.step > 0)
        {
            draft.step--;
            renderStep();
        }
    }

    /** validates this step and connects only from the final review */
    private void advance()
    {
        notice.setText("");
        if (draft.step == 0)
        {
            String value = draft.name.trim();
            String error = null;
            if (value.isEmpty())
            {
                error = "Enter your name";
            }
            else if (value.codePointCount(0, value.length()) > 50)
            {
                error = "Use 50 characters or fewer";
            }
            else if (value.getBytes(java.nio.charset.StandardCharsets.UTF_8).length >= 100 || value.indexOf('\0') >= 0)
            {
                error = "Choose a shorter name without control characters";
            }
            if (error != null)
            {
                name.setError(error);
                name.requestFocus();
                return;
            }
        }
        if (draft.step == 1)
        {
            int selectedPort;
            try
            {
                selectedPort = ServerAddress.parsePort(draft.port);
            }
            catch (IllegalArgumentException error)
            {
                port.setError(error.getMessage());
                port.requestFocus();
                return;
            }
            try
            {
                draft.savedAddress.endpoint(draft.address, selectedPort);
            }
            catch (IllegalArgumentException error)
            {
                address.setError(error.getMessage());
                address.requestFocus();
                return;
            }
        }
        if (draft.step == 2)
        {
            try
            {
                draft.keys = keys.serialize();
            }
            catch (IllegalArgumentException error)
            {
                notice.setText(error.getMessage());
                return;
            }
        }
        if (draft.step < TITLES.length - 1)
        {
            draft.step++;
            renderStep();
            return;
        }
        String error = action.connect(draft.savedAddress.endpoint(draft.address, ServerAddress.parsePort(draft.port)), draft.name.trim(), draft.keys);
        if (error == null)
        {
            dismiss();
        }
        else
        {
            notice.setText(error);
        }
    }

    /** displays one question and its current draft values */
    private void renderStep()
    {
        View focused = getCurrentFocus();
        android.view.inputmethod.InputMethodManager keyboard = getContext().getSystemService(android.view.inputmethod.InputMethodManager.class);
        if (focused != null && keyboard != null)
        {
            keyboard.hideSoftInputFromWindow(focused.getWindowToken(), 0);
        }
        name = null;
        address = null;
        port = null;
        keys = null;
        page.removeAllViews();
        progress.setText("Step " + (draft.step + 1) + " of " + TITLES.length);
        progressBar.setProgress(draft.step + 1);
        title.setText(TITLES[draft.step]);
        help.setText(HELP[draft.step]);
        notice.setText("");
        back.setVisibility(draft.step == 0 ? View.INVISIBLE : View.VISIBLE);
        next.setText(draft.step == TITLES.length - 1 ? "Connect" : "Next");
        if (draft.step == 0)
        {
            label("Your name");
            name = input("Name", draft.name, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_WORDS, value -> draft.name = value);
            page.addView(name);
        }
        else if (draft.step == 1)
        {
            label("Server address");
            address = input("chat.example.org or 192.168.1.10", draft.address, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI, value -> draft.address = value);
            page.addView(address);
            label("Port");
            port = input("1111", draft.port, InputType.TYPE_CLASS_NUMBER, value -> draft.port = value);
            page.addView(port);
        }
        else if (draft.step == 2)
        {
            keys = new ServerKeyForm(getContext(), palette, draft.keys);
            page.addView(keys);
            TextView detail = text("These are connection passwords from the server owner. Your identity passphrase and the server's admin password do not belong here.", 13, palette.secondary, false);
            detail.setPadding(0, dp(18), 0, 0);
            page.addView(detail);
        }
        else
        {
            summary("Your name", draft.name.trim());
            summary("Server address", draft.address.trim());
            summary("Port", draft.port.trim());
            int count = draft.keys.isEmpty() ? 0 : draft.keys.split("\n").length;
            summary("Connection passwords", count == 0 ? "None" : count + (count == 1 ? " password added" : " passwords added"));
        }
        scroll.post(() -> scroll.scrollTo(0, 0));
    }

    private void summary(String label, String value)
    {
        label(label);
        TextView field = text(value, 17, palette.text, false);
        field.setTextIsSelectable(true);
        page.addView(field);
    }

    private void label(String value)
    {
        TextView label = text(value, 13, palette.secondary, true);
        label.setPadding(0, dp(14), 0, dp(8));
        page.addView(label);
    }

    private EditText input(String hint, String value, int type, java.util.function.Consumer<String> changed)
    {
        EditText input = new EditText(getContext());
        input.setSaveEnabled(false);
        input.setSingleLine(true);
        input.setInputType(type);
        input.setTextSize(16);
        input.setTextColor(palette.text);
        input.setHintTextColor(palette.secondary);
        input.setHint(hint);
        input.setText(value);
        input.setPadding(dp(14), dp(12), dp(14), dp(12));
        input.setMinHeight(dp(52));
        input.setBackground(palette.rounded(palette.input, dp(12)));
        input.addTextChangedListener(new TextWatcher()
        {
            @Override
            public void beforeTextChanged(CharSequence value, int start, int count, int after)
            {
            }

            @Override
            public void onTextChanged(CharSequence value, int start, int before, int count)
            {
            }

            @Override
            public void afterTextChanged(Editable value)
            {
                changed.accept(value.toString());
            }
        });
        return input;
    }

    private Button button(String label, boolean primary)
    {
        Button button = new Button(getContext());
        button.setText(label);
        button.setAllCaps(false);
        button.setTextSize(15);
        button.setTextColor(primary ? palette.accentText : palette.text);
        button.setBackground(palette.interactive(primary ? palette.accent : palette.surface, dp(14)));
        return button;
    }

    private TextView text(String value, int size, int color, boolean bold)
    {
        TextView text = new TextView(getContext());
        text.setText(value);
        text.setTextSize(size);
        text.setTextColor(color);
        text.setTypeface(Typeface.create(bold ? "sans-serif-medium" : "sans-serif", Typeface.NORMAL));
        return text;
    }

    private LinearLayout column()
    {
        LinearLayout layout = new LinearLayout(getContext());
        layout.setOrientation(LinearLayout.VERTICAL);
        return layout;
    }

    private int dp(int value)
    {
        return Math.round(value * getContext().getResources().getDisplayMetrics().density);
    }
}
