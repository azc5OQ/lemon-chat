package com.lemonchat;

import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import androidx.activity.OnBackPressedCallback;
import androidx.appcompat.app.AppCompatActivity;
import com.google.android.material.button.MaterialButton;
import com.google.android.material.textfield.TextInputEditText;
import com.google.android.material.textfield.TextInputLayout;
import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.function.Consumer;
import org.json.JSONObject;

/** guides connection setup without changing the current connection before confirmation */
public final class ConnectionWizardActivity extends AppCompatActivity
{
    static final int ADVANCED_SETTINGS = RESULT_FIRST_USER;
    private State state;
    private LinearLayout content;
    private TextView error;
    private boolean resumed;
    private boolean renderedSaving;

    private static final class State
    {
        final ConnectionDraft draft;
        final Handler main = new Handler(Looper.getMainLooper());
        WeakReference<ConnectionWizardActivity> owner = new WeakReference<>(null);
        boolean saving;
        boolean saved;
        String failure;

        State(ConnectionDraft draft)
        {
            this.draft = draft;
        }

        void complete(boolean success)
        {
            saving = false;
            saved = success;
            failure = success ? null : "Your settings could not be saved. Please try again.";
            ConnectionWizardActivity current = owner.get();
            if (current != null && current.resumed)
            {
                current.saved();
            }
        }
    }

    @Override
    protected void onCreate(Bundle saved)
    {
        super.onCreate(saved);
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE | WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN);
        Object retained = getLastCustomNonConfigurationInstance();
        if (retained instanceof State)
        {
            state = (State)retained;
        }
        else
        {
            try
            {
                ChatSettings.attachContext(this);
                ChatSettings settings = ChatSettings.getInstance();
                if (!settings.loadPersistedState())
                {
                    throw new IllegalStateException("identity unavailable");
                }
                state = new State(new ConnectionDraft(new JSONObject(settings.getJsonSettings())));
            }
            catch (Exception unavailable)
            {
                new android.app.AlertDialog.Builder(this).setTitle("Settings unavailable").setMessage("Your saved identity could not be opened. Return to the app to resolve it before connecting.").setPositiveButton("Back", (dialog, which) ->
                {
                    finish();
                }).setOnCancelListener(dialog ->
                {
                    finish();
                }).show();
                return;
            }
        }
        state.owner = new WeakReference<>(this);
        getOnBackPressedDispatcher().addCallback(this, new OnBackPressedCallback(true)
        {
            @Override
            public void handleOnBackPressed()
            {
                back();
            }
        });
        render();
    }

    @Override
    public Object onRetainCustomNonConfigurationInstance()
    {
        return state;
    }

    @Override
    protected void onResume()
    {
        super.onResume();
        resumed = true;
        if (state != null)
        {
            state.owner = new WeakReference<>(this);
            saved();
        }
    }

    @Override
    protected void onPause()
    {
        resumed = false;
        super.onPause();
    }

    @Override
    protected void onDestroy()
    {
        if (state != null)
        {
            if (state.owner.get() == this)
            {
                state.owner.clear();
            }
            if (!isChangingConfigurations() && !state.saving)
            {
                state.draft.clear();
            }
        }
        super.onDestroy();
    }

    private void saved()
    {
        if (state.saved)
        {
            setResult(RESULT_OK);
            finish();
        }
        else if (!state.saving && (renderedSaving || content == null))
        {
            render();
        }
    }

    private void back()
    {
        if (state == null || state.saving)
        {
            return;
        }
        if (state.draft.step == 0)
        {
            finish();
        }
        else
        {
            state.draft.step--;
            state.failure = null;
            render();
        }
    }

    private void next()
    {
        if (state.saving)
        {
            return;
        }
        String problem = state.draft.validate(state.draft.step);
        if (problem != null)
        {
            error.setText(problem);
            return;
        }
        if (state.draft.step < 3)
        {
            state.draft.step++;
            state.failure = null;
            render();
            return;
        }
        try
        {
            String patch = state.draft.patch().toString();
            State pending = state;
            pending.saving = true;
            render();
            new Thread(() ->
            {
                boolean success = ChatSettings.getInstance().saveJsonSettings(patch);
                pending.main.post(() ->
                {
                    pending.complete(success);
                });
            }, "lemon-connection-save").start();
        }
        catch (Exception invalid)
        {
            state.saving = false;
            state.failure = invalid instanceof IllegalArgumentException ? invalid.getMessage() : "Your settings could not be saved. Please try again.";
            render();
        }
    }

    private void render()
    {
        renderedSaving = state.saving;
        View focused = getCurrentFocus();
        if (focused != null)
        {
            android.view.inputmethod.InputMethodManager keyboard = getSystemService(android.view.inputmethod.InputMethodManager.class);
            if (keyboard != null)
            {
                keyboard.hideSoftInputFromWindow(focused.getWindowToken(), 0);
            }
        }
        ConnectionDraft draft = state.draft;
        LinearLayout root = column();
        root.setPadding(dp(24), dp(24), dp(24), dp(16));
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS);
        int width = Math.min(getResources().getDisplayMetrics().widthPixels - dp(48), dp(560));
        TextView progress = text("Step " + (draft.step + 1) + " of 4", 14, false);
        root.addView(progress, new LinearLayout.LayoutParams(width, -2));
        ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(4);
        bar.setProgress(draft.step + 1);
        LinearLayout.LayoutParams barSize = new LinearLayout.LayoutParams(width, dp(6));
        barSize.topMargin = dp(12);
        barSize.bottomMargin = dp(20);
        root.addView(bar, barSize);
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        content = column();
        content.setSaveEnabled(false);
        scroll.addView(content, new ScrollView.LayoutParams(-1, -2));
        root.addView(scroll, new LinearLayout.LayoutParams(width, 0, 1));
        String[] titles = {"What should we call you?", "Where are your friends?", "Does the server need a password?", "Ready to connect?"};
        String[] descriptions = {"Choose the name people will see in your chats. You can change it later.", "Ask the person hosting your chat for its address. Keep the default port unless they give you another one.", "Most servers do not need one. If yours does, use the password and type shared by its administrator.", "Check these details. Nothing is changed until you tap Connect."};
        content.addView(text(titles[draft.step], 27, true));
        TextView description = text(descriptions[draft.step], 16, false);
        description.setPadding(0, dp(12), 0, dp(24));
        content.addView(description);
        if (draft.step == 0)
        {
            field("Your name", draft.name, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_WORDS, value ->
            {
                draft.name = value;
            });
        }
        else if (draft.step == 1)
        {
            field("IP address or hostname", draft.host, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI, value ->
            {
                draft.host = value;
            });
            field("Port", draft.port, InputType.TYPE_CLASS_NUMBER, value ->
            {
                draft.port = value;
            });
            content.addView(text("For example: 192.168.1.20 or chat.example.com", 14, false));
        }
        else if (draft.step == 2)
        {
            renderKeys();
        }
        else
        {
            content.addView(text("Your name", 14, false));
            content.addView(text(draft.name, 20, true));
            TextView address = text("Server\n" + draft.host + "\nPort " + draft.port, 18, false);
            address.setPadding(0, dp(24), 0, dp(24));
            content.addView(address);
            ArrayList<String> types = new ArrayList<>();
            for (ConnectionDraft.Key key : draft.keys)
            {
                if (MetadataKey.encode(key.original, key.type, key.password) != null)
                {
                    types.add(MetadataKey.LABELS.get(MetadataKey.TYPES.indexOf(key.type)));
                }
            }
            content.addView(text(types.isEmpty() ? "No connection password" : "Connection passwords\n" + String.join(" → ", types), 16, false));
        }
        error = text(state.failure == null ? "" : state.failure, 14, false);
        error.setTextColor(color(com.google.android.material.R.attr.colorError));
        error.setPadding(0, dp(16), 0, dp(8));
        content.addView(error);
        LinearLayout buttons = new LinearLayout(this);
        buttons.setGravity(Gravity.CENTER_VERTICAL);
        MaterialButton back = button(draft.step == 0 ? "Cancel" : "Back", this::back);
        MaterialButton next = button(state.saving ? "Saving…" : draft.step == 3 ? "Connect" : "Next", this::next);
        LinearLayout.LayoutParams left = new LinearLayout.LayoutParams(0, dp(52), 1);
        left.rightMargin = dp(12);
        buttons.addView(back, left);
        buttons.addView(next, new LinearLayout.LayoutParams(0, dp(52), 1));
        root.addView(buttons, new LinearLayout.LayoutParams(width, -2));
        MaterialButton advanced = button("Advanced settings", () ->
        {
            setResult(ADVANCED_SETTINGS);
            finish();
        });
        advanced.setBackgroundTintList(ColorStateList.valueOf(android.graphics.Color.TRANSPARENT));
        advanced.setTextColor(color(com.google.android.material.R.attr.colorPrimary));
        root.addView(advanced);
        setContentView(root);
        HybridWindowInsets.apply(this, root);
        if (state.saving)
        {
            enabled(root, false);
        }
    }

    private void renderKeys()
    {
        ConnectionDraft draft = state.draft;
        if (draft.keys.isEmpty())
        {
            content.addView(text("No password added. Tap Next if the server is open to everyone.", 16, false));
        }
        for (ConnectionDraft.Key key : new ArrayList<>(draft.keys))
        {
            View row = getLayoutInflater().inflate(R.layout.key_field_row, content, false);
            EditText password = row.findViewById(R.id.key_input);
            password.setSaveEnabled(false);
            password.setText(key.password);
            watch(password, value ->
            {
                key.password = value;
            });
            Spinner type = row.findViewById(R.id.key_type);
            ArrayList<String> labels = new ArrayList<>(MetadataKey.LABELS);
            int selected = MetadataKey.TYPES.indexOf(key.type);
            if (selected < 0)
            {
                selected = labels.size();
                labels.add("Unsupported saved type");
            }
            ArrayAdapter<String> choices = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, labels);
            choices.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
            type.setSaveEnabled(false);
            type.setAdapter(choices);
            type.setSelection(selected);
            type.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener()
            {
                @Override
                public void onItemSelected(AdapterView<?> parent, View view, int position, long id)
                {
                    key.type = position < MetadataKey.TYPES.size() ? MetadataKey.TYPES.get(position) : "";
                }

                @Override
                public void onNothingSelected(AdapterView<?> parent)
                {
                }
            });
            row.findViewById(R.id.remove_key).setOnClickListener(view ->
            {
                draft.keys.remove(key);
                key.password = "";
                render();
            });
            content.addView(row);
        }
        content.addView(button("Add a password", () ->
        {
            draft.keys.add(new ConnectionDraft.Key(null));
            render();
        }));
        TextView note = text("If your host gave you several passwords, add them in the same order.", 14, false);
        note.setPadding(0, dp(12), 0, 0);
        content.addView(note);
    }

    private void field(String label, String value, int inputType, Consumer<String> changed)
    {
        TextInputLayout wrapper = (TextInputLayout)getLayoutInflater().inflate(R.layout.connection_wizard_field, content, false);
        wrapper.setHint(label);
        TextInputEditText input = wrapper.findViewById(R.id.connection_wizard_input);
        input.setInputType(inputType);
        input.setSingleLine(true);
        input.setSaveEnabled(false);
        input.setText(value);
        watch(input, changed);
        LinearLayout.LayoutParams size = new LinearLayout.LayoutParams(-1, -2);
        size.bottomMargin = dp(16);
        content.addView(wrapper, size);
    }

    private void watch(EditText input, Consumer<String> changed)
    {
        input.addTextChangedListener(new TextWatcher()
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
    }

    private LinearLayout column()
    {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        return layout;
    }

    private TextView text(String value, int size, boolean bold)
    {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color(android.R.attr.textColorPrimary));
        if (bold)
        {
            view.setTypeface(null, Typeface.BOLD);
        }
        return view;
    }

    private MaterialButton button(String label, Runnable action)
    {
        MaterialButton button = new MaterialButton(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setOnClickListener(view ->
        {
            action.run();
        });
        return button;
    }

    private void enabled(View view, boolean value)
    {
        view.setEnabled(value);
        if (view instanceof android.view.ViewGroup)
        {
            android.view.ViewGroup group = (android.view.ViewGroup)view;
            for (int index = 0; index < group.getChildCount(); index++)
            {
                enabled(group.getChildAt(index), value);
            }
        }
    }

    private int color(int attribute)
    {
        TypedValue value = new TypedValue();
        getTheme().resolveAttribute(attribute, value, true);
        return value.resourceId == 0 ? value.data : getColor(value.resourceId);
    }

    private int dp(int value)
    {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
