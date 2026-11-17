package com.lemonchat.ui;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import com.lemonchat.model.AdminSettings;
import com.lemonchat.model.AdminState;
import com.lemonchat.service.ChatService;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** administrator login and general settings for the current server */
public final class AdministrationDialog extends AdminWindow implements ChatService.Observer
{
    private final Activity activity;
    private final ChatService service;
    private final Palette palette;
    private final long connectionId;
    private final LinearLayout content;
    private final TextView notice;
    private final Button save;
    private final Button reload;
    private final Button groups;
    private final Map<String, Object> draft = new LinkedHashMap<>();
    private AdminSettings original;
    private AdminState state;
    private boolean changed;
    private boolean passwordMode;
    private boolean saving;
    private long savingRevision;
    private boolean binding;
    private boolean shownAdmin;
    private boolean shownPasswordMode;
    private boolean includeMusic = true;
    private String section = "General";
    private Dialog child;

    public static Dialog show(Activity activity, ChatService service)
    {
        if (service == null || !service.snapshot().connected || activity.isFinishing() || activity.isDestroyed())
        {
            return null;
        }
        AdministrationDialog dialog = new AdministrationDialog(activity, service, new LinearLayout(activity));
        dialog.show();
        return dialog;
    }

    private AdministrationDialog(Activity activity, ChatService service, LinearLayout root)
    {
        super(activity, root, 800, 880);
        this.activity = activity;
        this.service = service;
        palette = Palette.load(activity);
        state = service.adminState();
        connectionId = state.connectionId;
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(16), dp(12), dp(16), dp(12));
        root.setBackground(palette.rounded(palette.surface, dp(14)));
        root.setTag("administration-root");
        LinearLayout header = row();
        TextView title = text("Server administration", 19, palette.text);
        title.setTypeface(null, Typeface.BOLD);
        header.addView(title, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        Button close = button("Close", this::requestClose);
        close.setTag("administration-close");
        header.addView(close);
        root.addView(header);
        notice = text("", 13, palette.secondary);
        notice.setPadding(0, dp(8), 0, dp(8));
        notice.setTag("administration-notice");
        root.addView(notice);
        groups = button("Groups and permissions", () ->
        {
            child = GroupsAdminDialog.show(activity, service);
        });
        root.addView(groups);
        ScrollView scroll = new ScrollView(activity);
        scroll.setFillViewport(false);
        content = column();
        content.setPadding(0, dp(12), 0, dp(12));
        scroll.addView(content);
        root.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        LinearLayout footer = row();
        footer.setGravity(Gravity.CENTER);
        reload = button("Reload", this::requestReload);
        reload.setTag("administration-reload");
        save = button("Save changes", this::save);
        save.setTag("administration-save");
        footer.addView(reload);
        footer.addView(save);
        root.addView(footer);
        setOnCancelListener(dialog -> clearPasswords(content));
        setOnDismissListener(dialog ->
        {
            service.removeObserver(this);
            clearPasswords(content);
            if (child != null)
            {
                child.dismiss();
                child = null;
            }
            draft.clear();
        });
    }

    @Override
    public void show()
    {
        super.show();
        service.addObserver(this, false);
        refresh(true);
    }

    @Override
    public void dismiss()
    {
        clearPasswords(content);
        service.removeObserver(this);
        super.dismiss();
    }

    @Override
    public void onChanged()
    {
        refresh(false);
    }

    @Override
    public void onProblem(String message)
    {
        if (isShowing())
        {
            notice.setText(message);
            notice.setTextColor(palette.danger);
        }
    }

    @Override
    public void onBackPressed()
    {
        requestClose();
    }

    private void refresh(boolean force)
    {
        if (!isShowing())
        {
            return;
        }
        AdminState latest = service.adminState();
        if (!service.snapshot().connected || latest.connectionId != connectionId)
        {
            dismiss();
            return;
        }
        state = latest;
        notice.setText(state.error.isEmpty() ? state.statusText : state.error);
        notice.setTextColor(state.error.isEmpty() ? palette.secondary : palette.danger);
        groups.setVisibility(state.groupsEnabled ? View.VISIBLE : View.GONE);
        groups.setEnabled(!state.busy);
        if (saving && !state.busy && state.revision > savingRevision && (state.statusText.equals("Server settings saved") || !state.error.isEmpty()))
        {
            saving = false;
            if (state.error.isEmpty())
            {
                changed = false;
                draft.clear();
                original = null;
                service.loadAdministration(connectionId);
            }
        }
        boolean nextPassword = passwordMode || state.passwordChangeRequired;
        boolean rebuild = force || state.isAdmin != shownAdmin || nextPassword != shownPasswordMode;
        if (state.settings != null && original != state.settings && !changed)
        {
            original = state.settings;
            draft.clear();
            draft.putAll(original.values);
            rebuild = true;
        }
        if (state.isAdmin && !shownAdmin && state.settings == null && !state.busy)
        {
            service.loadAdministration(connectionId);
        }
        shownAdmin = state.isAdmin;
        shownPasswordMode = nextPassword;
        if (rebuild)
        {
            render();
        }
        save.setVisibility(state.isAdmin && !nextPassword ? View.VISIBLE : View.GONE);
        reload.setVisibility(state.isAdmin && !nextPassword ? View.VISIBLE : View.GONE);
        save.setEnabled(!state.busy && original != null && state.settings == original && changed);
        reload.setEnabled(!state.busy);
        setInputsEnabled(content, !state.busy);
    }

    private void render()
    {
        binding = true;
        clearPasswords(content);
        content.removeAllViews();
        if (!state.isAdmin || shownPasswordMode)
        {
            renderPassword(state.isAdmin);
        }
        else if (original == null)
        {
            content.addView(text("Load this server's settings to inspect and edit them.", 15, palette.secondary));
        }
        else
        {
            TextView usage = text(original.usedSlots + " of " + original.maxSlots + " connection slots in use · " + original.connectionPasswordCount + " connection passwords", 13, palette.secondary);
            content.addView(usage);
            List<String> sections = new ArrayList<>();
            for (AdminSettings.Field field : AdminSettings.FIELDS)
            {
                if (original.values.containsKey(field.key) && !sections.contains(field.section))
                {
                    sections.add(field.section);
                }
            }
            Spinner selector = spinner(sections);
            selector.setTag("administration-section");
            selector.setSelection(Math.max(0, sections.indexOf(section)));
            content.addView(selector);
            selector.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener()
            {
                @Override
                public void onItemSelected(AdapterView<?> parent, View view, int position, long id)
                {
                    String selected = sections.get(position);
                    if (!section.equals(selected))
                    {
                        section = selected;
                        render();
                    }
                }

                @Override
                public void onNothingSelected(AdapterView<?> parent)
                {
                }
            });
            for (AdminSettings.Field field : AdminSettings.FIELDS)
            {
                if (field.section.equals(section) && original.values.containsKey(field.key))
                {
                    addField(field);
                }
            }
            CheckBox music = check("Save music bot playlists with these settings", includeMusic);
            music.setOnCheckedChangeListener((button, checked) -> includeMusic = checked);
            content.addView(music);
            TextView note = text("Connection passwords are preserved. Group edits are managed separately.", 12, palette.secondary);
            note.setPadding(0, dp(12), 0, dp(8));
            content.addView(note);
            content.addView(button("Change administrator password", () ->
            {
                passwordMode = true;
                refresh(true);
            }));
        }
        binding = false;
    }

    private void renderPassword(boolean replacement)
    {
        content.addView(text(replacement ? "Choose a new administrator password for this server." : "Enter this server's administrator password to edit its general settings. Group managers can use their existing permissions above.", 15, palette.text));
        EditText password = password(replacement ? "New administrator password" : "Administrator password");
        password.setTag("administration-password");
        content.addView(password);
        EditText repeat = replacement ? password("Repeat new password") : null;
        if (repeat != null)
        {
            repeat.setTag("administration-password-repeat");
            content.addView(repeat);
        }
        content.addView(text("The password is used for this request only. It is not saved on this device.", 12, palette.secondary));
        content.addView(button(replacement ? "Change password" : "Log in", () ->
        {
            String value = password.getText().toString();
            if (repeat != null && !value.equals(repeat.getText().toString()))
            {
                repeat.setError("Passwords do not match");
                return;
            }
            if (value.isEmpty() || value.indexOf('\0') >= 0 || value.getBytes(java.nio.charset.StandardCharsets.UTF_8).length > 50)
            {
                password.setError("Use 1 to 50 UTF-8 bytes");
                return;
            }
            boolean submitted = replacement ? service.changeAdminPassword(connectionId, value) : service.adminLogin(connectionId, value);
            if (submitted)
            {
                password.setText("");
                if (repeat != null)
                {
                    repeat.setText("");
                }
                if (replacement)
                {
                    passwordMode = false;
                }
            }
            else
            {
                onProblem("The connection changed. Open administration again");
            }
        }));
        if (replacement && !state.passwordChangeRequired)
        {
            content.addView(button("Back to settings", () ->
            {
                passwordMode = false;
                refresh(true);
            }));
        }
    }

    private void addField(AdminSettings.Field field)
    {
        LinearLayout row = column();
        row.setPadding(0, dp(12), 0, dp(8));
        if (field.kind == AdminSettings.TOGGLE)
        {
            CheckBox control = check(field.label, Boolean.TRUE.equals(draft.get(field.key)));
            control.setTag(field.key);
            control.setOnCheckedChangeListener((button, value) -> edit(field.key, value));
            row.addView(control);
        }
        else
        {
            row.addView(text(field.label, 15, palette.text));
            if (field.kind == AdminSettings.CHOICE)
            {
                Spinner control = spinner(field.choices);
                control.setTag(field.key);
                control.setSelection(Math.max(0, field.choices.indexOf(String.valueOf(draft.get(field.key)))));
                control.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener()
                {
                    @Override
                    public void onItemSelected(AdapterView<?> parent, View view, int position, long id)
                    {
                        edit(field.key, field.choices.get(position));
                    }

                    @Override
                    public void onNothingSelected(AdapterView<?> parent)
                    {
                    }
                });
                row.addView(control);
            }
            else
            {
                EditText control = input();
                control.setTag(field.key);
                control.setInputType(field.kind == AdminSettings.NUMBER ? InputType.TYPE_CLASS_NUMBER : InputType.TYPE_CLASS_TEXT);
                control.setText(String.valueOf(draft.get(field.key)));
                control.addTextChangedListener(new TextWatcher()
                {
                    @Override
                    public void beforeTextChanged(CharSequence value, int start, int count, int after)
                    {
                    }

                    @Override
                    public void onTextChanged(CharSequence value, int start, int before, int count)
                    {
                        edit(field.key, value.toString());
                    }

                    @Override
                    public void afterTextChanged(Editable value)
                    {
                    }
                });
                row.addView(control);
            }
        }
        row.addView(text(field.help, 12, palette.secondary));
        content.addView(row);
    }

    private void edit(String key, Object value)
    {
        if (!binding && !String.valueOf(draft.get(key)).equals(String.valueOf(value)))
        {
            draft.put(key, value);
            changed = true;
            save.setEnabled(!state.busy && state.settings == original);
        }
    }

    private void save()
    {
        Map<String, Object> values = new LinkedHashMap<>(draft);
        try
        {
            for (AdminSettings.Field field : AdminSettings.FIELDS)
            {
                if (field.kind == AdminSettings.NUMBER && values.containsKey(field.key))
                {
                    String value = String.valueOf(values.get(field.key));
                    if (!value.matches("[0-9]+"))
                    {
                        throw new IllegalArgumentException(field.label + " must be a whole number");
                    }
                    values.put(field.key, Long.parseLong(value));
                }
            }
            original.changes(values, includeMusic);
            savingRevision = state.revision;
            saving = service.saveServerSettings(connectionId, original, values, includeMusic);
            if (!saving)
            {
                onProblem("The connection changed. Open administration again");
            }
        }
        catch (IllegalArgumentException | org.json.JSONException invalid)
        {
            onProblem(invalid.getMessage());
        }
    }

    private void requestReload()
    {
        if (changed)
        {
            child = new AlertDialog.Builder(activity).setTitle("Discard these edits?").setMessage("Reload gets the current settings from the server.").setNegativeButton("Keep editing", null).setPositiveButton("Reload", (dialog, which) -> reload()).show();
        }
        else
        {
            reload();
        }
    }

    private void reload()
    {
        if (service.loadAdministration(connectionId))
        {
            changed = false;
        }
    }

    private void requestClose()
    {
        if (changed && !saving)
        {
            child = new AlertDialog.Builder(activity).setTitle("Discard these edits?").setNegativeButton("Keep editing", null).setPositiveButton("Discard", (dialog, which) -> dismiss()).show();
        }
        else
        {
            dismiss();
        }
    }

    private EditText password(String hint)
    {
        EditText control = input();
        control.setHint(hint);
        control.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        control.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS);
        control.setSaveEnabled(false);
        control.setFreezesText(false);
        return control;
    }

    private static void clearPasswords(View view)
    {
        if (view instanceof EditText && (((EditText)view).getInputType() & InputType.TYPE_TEXT_VARIATION_PASSWORD) == InputType.TYPE_TEXT_VARIATION_PASSWORD)
        {
            ((EditText)view).setText("");
        }
        if (view instanceof ViewGroup)
        {
            ViewGroup parent = (ViewGroup)view;
            for (int i = 0; i < parent.getChildCount(); i++)
            {
                clearPasswords(parent.getChildAt(i));
            }
        }
    }

    private static void setInputsEnabled(View view, boolean enabled)
    {
        view.setEnabled(enabled);
        if (view instanceof ViewGroup)
        {
            ViewGroup parent = (ViewGroup)view;
            for (int i = 0; i < parent.getChildCount(); i++)
            {
                setInputsEnabled(parent.getChildAt(i), enabled);
            }
        }
    }

    private EditText input()
    {
        EditText input = new EditText(activity);
        input.setTextColor(palette.text);
        input.setHintTextColor(palette.secondary);
        input.setSingleLine(true);
        input.setTextSize(15);
        input.setSaveEnabled(false);
        input.setMinHeight(dp(48));
        input.setBackgroundTintList(ColorStateList.valueOf(palette.accent));
        return input;
    }

    private CheckBox check(String label, boolean checked)
    {
        CheckBox check = new CheckBox(activity);
        check.setText(label);
        check.setTextColor(palette.text);
        check.setButtonTintList(ColorStateList.valueOf(palette.accent));
        check.setChecked(checked);
        check.setMinHeight(dp(48));
        return check;
    }

    private Spinner spinner(List<String> choices)
    {
        Spinner result = new Spinner(activity);
        ArrayAdapter<String> adapter = new ArrayAdapter<String>(activity, android.R.layout.simple_spinner_item, choices)
        {
            @Override
            public View getView(int position, View recycled, ViewGroup parent)
            {
                TextView view = (TextView)super.getView(position, recycled, parent);
                view.setTextColor(palette.text);
                view.setMinHeight(dp(48));
                view.setGravity(Gravity.CENTER_VERTICAL);
                return view;
            }
        };
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        result.setAdapter(adapter);
        return result;
    }

    private Button button(String label, Runnable action)
    {
        Button button = new Button(activity);
        button.setText(label);
        button.setAllCaps(false);
        button.setTextColor(palette.accent);
        button.setBackgroundTintList(ColorStateList.valueOf(palette.surface));
        button.setOnClickListener(view -> action.run());
        return button;
    }

    private TextView text(String value, int size, int color)
    {
        TextView text = new TextView(activity);
        text.setText(value);
        text.setTextColor(color);
        text.setTextSize(size);
        return text;
    }

    private LinearLayout column()
    {
        LinearLayout column = new LinearLayout(activity);
        column.setOrientation(LinearLayout.VERTICAL);
        return column;
    }

    private LinearLayout row()
    {
        LinearLayout row = new LinearLayout(activity);
        row.setGravity(Gravity.CENTER_VERTICAL);
        return row;
    }

    private int dp(int value)
    {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }
}
