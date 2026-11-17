package com.lemonchat.ui;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.Typeface;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.SeekBar;
import com.lemonchat.audio.AudioOptions;
import com.lemonchat.model.ClientCapabilities;
import com.lemonchat.model.SessionSnapshot;
import com.lemonchat.service.ChatService;
import com.lemonchat.settings.LocalSettings;
import java.util.LinkedHashMap;
import java.util.Map;

/** edits local preferences without applying an unfinished draft */
public final class LocalSettingsDialog
{
    public interface Callbacks
    {
        void onSaved();
        void onAvatar();
        void onGroups();
        void onIdentityImport();
        void onIdentityExport();
        void onLog();
        void onHistory();
        default void onDialogReplaced(AlertDialog dialog)
        {
        }
    }

    private final Activity activity;
    private final ChatService service;
    private final Callbacks callbacks;
    private final Palette palette;
    private final Map<String, Object> draft;
    private final Map<String, String> errors = new LinkedHashMap<>();
    private final Map<String, String> numberDrafts = new LinkedHashMap<>();
    private Context viewContext;
    private LinearLayout page;
    private ScrollView scroll;
    private TextView problem;
    private AlertDialog dialog;
    private AlertDialog captureDialog;
    private Button save;
    private Button cancel;
    private int selectedSection;
    private boolean saving;

    public static AlertDialog show(Activity activity, ChatService service, Callbacks callbacks)
    {
        if (activity.isFinishing() || activity.isDestroyed() || service == null)
        {
            return null;
        }
        LocalSettingsDialog settings = new LocalSettingsDialog(activity, service, callbacks);
        settings.open();
        return settings.dialog;
    }

    private LocalSettingsDialog(Activity activity, ChatService service, Callbacks callbacks)
    {
        this.activity = activity;
        this.service = service;
        this.callbacks = callbacks;
        palette = Palette.load(activity);
        draft = LocalSettings.load(activity).asMap();
        build();
    }

    private void build()
    {
        viewContext = activity instanceof UiScaleActivity ? ((UiScaleActivity)activity).previewContext((Integer)draft.get("native_ui_scale"), palette.theme.windowStyle(palette.dark)) : activity;
        LinearLayout root = new LinearLayout(viewContext);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(18), dp(16), dp(18), dp(12));
        root.setBackgroundColor(palette.surface);
        TextView title = text("Local settings", 21, true);
        title.setPadding(0, 0, 0, dp(12));
        root.addView(title);
        Spinner sections = new Spinner(viewContext);
        sections.setAdapter(new ArrayAdapter<>(viewContext, android.R.layout.simple_spinner_dropdown_item, new String[]{"Appearance", "Chat", "Audio", "Files & pictures", "Identity", "Advanced"}));
        sections.setSelection(selectedSection);
        root.addView(sections, new LinearLayout.LayoutParams(-1, dp(48)));
        scroll = new ScrollView(viewContext);
        scroll.setFillViewport(true);
        page = new LinearLayout(viewContext);
        page.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(page);
        root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        problem = text("", 13, false);
        problem.setTextColor(palette.danger);
        problem.setVisibility(View.GONE);
        root.addView(problem);
        LinearLayout footer = new LinearLayout(viewContext);
        footer.setGravity(Gravity.CENTER);
        footer.setPadding(0, dp(12), 0, 0);
        cancel = button("Cancel", false);
        save = button("Save", true);
        LinearLayout.LayoutParams buttonSize = new LinearLayout.LayoutParams(dp(116), dp(48));
        buttonSize.setMarginEnd(dp(12));
        footer.addView(cancel, buttonSize);
        footer.addView(save, new LinearLayout.LayoutParams(dp(116), dp(48)));
        root.addView(footer);
        dialog = new AlertDialog.Builder(viewContext).setView(root).create();
        dialog.setOnDismissListener(window ->
        {
            if (captureDialog != null)
            {
                captureDialog.dismiss();
                captureDialog = null;
            }
        });
        cancel.setOnClickListener(view -> dialog.dismiss());
        save.setOnClickListener(view -> save());
        sections.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener()
        {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id)
            {
                showSection(position);
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent)
            {
            }
        });
    }

    private void open()
    {
        dialog.show();
        Window window = dialog.getWindow();
        if (window != null)
        {
            int width = Math.min(activity.getResources().getDisplayMetrics().widthPixels - dp(20), dp(620));
            int height = activity.getResources().getDisplayMetrics().heightPixels - dp(100);
            window.setLayout(width, height);
            window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
            window.setBackgroundDrawable(palette.rounded(palette.surface, dp(18)));
        }
        showSection(selectedSection);
    }

    private void showSection(int section)
    {
        selectedSection = section;
        page.removeAllViews();
        switch (section)
        {
            case 0:
                uiSize();
                number("lemon_chat_display_scale", "Chat size (%)", "50 to 200. Changes messages, not the channel tree");
                action("Reset chat size to 100%", () ->
                {
                    draft.put("lemon_chat_display_scale", 100);
                    errors.remove("lemon_chat_display_scale");
                    numberDrafts.remove("lemon_chat_display_scale");
                    showSection(0);
                });
                toggle("lemon_show_message_avatars", "Show avatars", "Show a picture or initials beside messages");
                toggle("lemon_use_theme_chat_formatting", "Disable custom message fonts", "Use this app's text style for received messages");
                toggle("lemon_show_flags", "Country flags", "Show the country reported by the server");
                toggle("lemon_hide_server_group_icons", "Hide server group icons", "Group names remain available in member information");
                toggle("lemon_hide_channel_group_icons", "Hide channel group icons", "Hide the member's channel role icon");
                toggle("lemon_hide_channel_icons", "Hide channel icons", "Keep password and audio indicators visible");
                toggle("lemon_show_channel_people", "Show channel member counts", "Include up to three member pictures");
                toggle("lemon_channels_flat", "Flat channel list", "Show every channel at the same depth");
                if (palette.theme.strip == NativeTheme.MOVABLE_STRIP)
                {
                    toggle("lemon_strip_vertical", "People on the side", "Place Bluebell's people strip on the left");
                }
                toggle("lemon_strip_neighbors", "Only my channel", "Show people from your joined channel in the member strip");
                toggle("lemon_audio_availability", "Show who has audio off", "Mark people who cannot receive voice");
                break;
            case 1:
                action("Offline history", () -> openAction(callbacks::onHistory));
                action("Delete all messages", this::confirmDeleteAllMessages);
                action("View server groups", () -> openAction(callbacks::onGroups));
                toggle("lemon_chat_reactions", "Emoji reactions", "Show and send reactions in channel and private chats");
                toggle("lemon_chat_replies", "Quoted replies", "Show quotes and the Reply action");
                toggle("lemon_auto_scroll", "Auto-scroll to new messages", "Turn off to keep your place while reading older messages");
                toggle("lemon_is_public_key_info_hidden", "Hide public keys in private chats", "Hide the public-key information above a conversation");
                toggle("lemon_limit_chat_messages", "Limit messages in each chat", "Remove older entries from this device's view");
                number("lemon_chat_message_limit", "Messages per chat", "Applies when the limit is enabled. The app's total memory limit still applies");
                toggle("lemon_chat_search", "Search chat", "Enable search, including Ctrl+F on a connected keyboard");
                toggle("lemon_seen_indicator", "Read receipts", "Show when the other person has read a private message");
                toggle("lemon_send_seen", "Tell others when you read", "Send a receipt when their private message is visible");
                break;
            case 2:
                choice("lemon_continuous_mic", "Microphone", "Hands free still requires you to start the microphone", new Object[]{false, true}, new String[]{"Push to talk", "Hands free"});
                action("Push-to-talk key: " + KeyEvent.keyCodeToString((Integer) draft.get("native_ptt_key_code")).replace("KEYCODE_", ""), this::captureKey);
                toggle("lemon_ptt_disable_while_typing", "Disable talk key while typing", "Do not activate the microphone from the message field");
                Map<String, String> microphones = AudioOptions.microphoneChoices(activity);
                String selected = (String) draft.get("lemon_mic_device_id");
                if (!microphones.containsKey(selected))
                {
                    microphones.put(selected, "Saved microphone (not connected)");
                }
                choice("lemon_mic_device_id", "Microphone input", "Use the system default or a connected microphone", microphones.keySet().toArray(), microphones.values().toArray(new String[0]));
                toggle("lemon_hide_mic", "Hide touch microphone button", "This changes visibility. It does not mute active recording");
                if (palette.theme.id.equals("default-mobile"))
                {
                    toggle("lemon_hide_mobile_mic", "Hide microphone in Mobile 1", "Keep the button available when you choose another theme");
                }
                toggle("lemon_show_mp3_button", "Show MP3 button", "Stream a music file from this device");
                toggle("lemon_sound_effects", "Sound effects", "Play short sounds for messages and presence changes");
                choice("lemon_audio_connection", "Preferred audio connection", "Both connect to the server. The server decides which transports it allows", new Object[]{"webrtc", "websocket"}, new String[]{"WebRTC DataChannel", "WebSocket"});
                break;
            case 3:
                number("lemon_chat_image_max_width", "Maximum image width", "32 to 4096 pixels");
                number("lemon_chat_image_max_height", "Maximum image height", "32 to 4096 pixels. Pictures keep their proportions");
                attachmentToggle("lemon_block_direct_chat_pictures", "Block private pictures", "Ask the server to stop private pictures before sending them to this device");
                attachmentToggle("lemon_block_direct_chat_files", "Block private files", "Ask the server to reject incoming private files");
                attachmentToggle("lemon_block_channel_chat_pictures", "Block channel pictures", "Other people in the channel can still receive them");
                attachmentToggle("lemon_block_channel_chat_files", "Block channel files", "Other people in the channel can still receive them");
                break;
            case 4:
                action("Change avatar", () -> openAction(callbacks::onAvatar));
                action("Import identity", () -> openAction(callbacks::onIdentityImport));
                action("Export identity", () -> openAction(callbacks::onIdentityExport));
                choice("lemon_rsa_key_bits", "Identity key size", "Default for a new identity or legacy import. Your current identity stays unchanged", new Object[]{"2048", "3072", "4096", "6144", "8192"}, new String[]{"2048 bits", "3072 bits", "4096 bits", "6144 bits", "8192 bits"});
                break;
            case 5:
                toggle("lemon_show_log_button", "App log button", "Show a shortcut to the connection log");
                toggle("native_file_logging", "Write a debug log file", "Keep a bounded log of connection events without private content or keys");
                action("Open connection log", () -> openAction(callbacks::onLog));
                break;
            default:
                break;
        }
        scroll.post(() -> scroll.scrollTo(0, 0));
    }

    private void uiSize()
    {
        LinearLayout heading = row("UI size", "Preview here. Save to resize the app. Smaller sizes also shrink controls");
        TextView value = text(draft.get("native_ui_scale") + "%", 14, true);
        value.setTag("ui-scale-value");
        heading.addView(value);
        SeekBar slider = new SeekBar(viewContext);
        slider.setTag("ui-scale-slider");
        slider.setContentDescription("UI size, 70 to 130 percent");
        slider.setMax(60);
        slider.setProgress((Integer)draft.get("native_ui_scale") - 70);
        slider.setProgressTintList(android.content.res.ColorStateList.valueOf(palette.accent));
        slider.setThumbTintList(android.content.res.ColorStateList.valueOf(palette.accent));
        page.addView(slider, new LinearLayout.LayoutParams(-1, dp(48)));
        slider.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener()
        {
            @Override
            public void onProgressChanged(SeekBar bar, int progress, boolean fromUser)
            {
                if (!saving)
                {
                    draft.put("native_ui_scale", progress + 70);
                    value.setText((progress + 70) + "%");
                }
            }

            @Override
            public void onStartTrackingTouch(SeekBar bar)
            {
            }

            @Override
            public void onStopTrackingTouch(SeekBar bar)
            {
                previewSize();
            }
        });
        action("Reset UI size to 100%", () ->
        {
            if (!saving)
            {
                draft.put("native_ui_scale", 100);
                previewSize();
            }
        });
    }

    private void previewSize()
    {
        if (saving || activity.isFinishing() || activity.isDestroyed() || !dialog.isShowing())
        {
            return;
        }
        int position = scroll.getScrollY();
        dialog.dismiss();
        build();
        open();
        scroll.post(() -> scroll.scrollTo(0, position));
        callbacks.onDialogReplaced(dialog);
    }

    private void toggle(String key, String label, String hint)
    {
        toggle(key, label, hint, true);
    }

    private void attachmentToggle(String key, String label, String hint)
    {
        SessionSnapshot snapshot = service.snapshot();
        boolean supported = true;
        if (snapshot != null && snapshot.connected)
        {
            ClientCapabilities capabilities = snapshot.capabilities;
            switch (key)
            {
                case "lemon_block_direct_chat_pictures":
                    supported = capabilities.blockDirectPictures;
                    break;
                case "lemon_block_direct_chat_files":
                    supported = capabilities.blockDirectFiles;
                    break;
                case "lemon_block_channel_chat_pictures":
                    supported = capabilities.blockChannelPictures;
                    break;
                case "lemon_block_channel_chat_files":
                    supported = capabilities.blockChannelFiles;
                    break;
                default:
                    throw new IllegalArgumentException("Unknown attachment setting");
            }
        }
        toggle(key, label, supported ? hint : "This server does not allow this blocking option. Your saved preference is kept for other servers", supported);
    }

    private void toggle(String key, String label, String hint, boolean enabled)
    {
        LinearLayout row = row(label, hint);
        Switch control = new Switch(viewContext);
        control.setContentDescription(label);
        control.setChecked((Boolean) draft.get(key));
        control.setEnabled(enabled);
        control.setOnCheckedChangeListener((button, checked) -> draft.put(key, checked));
        row.addView(control);
        if (enabled)
        {
            row.setOnClickListener(view -> control.setChecked(!control.isChecked()));
        }
    }

    private void number(String key, String label, String hint)
    {
        LinearLayout row = row(label, hint);
        EditText field = new EditText(viewContext);
        field.setSingleLine(true);
        field.setInputType(InputType.TYPE_CLASS_NUMBER);
        field.setTextColor(palette.text);
        field.setContentDescription(label);
        field.setText(numberDrafts.getOrDefault(key, String.valueOf(draft.get(key))));
        field.setSelectAllOnFocus(true);
        row.addView(field, new LinearLayout.LayoutParams(dp(84), dp(52)));
        field.addTextChangedListener(new TextWatcher()
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
                numberDrafts.put(key, value.toString());
                try
                {
                    Object checked = LocalSettings.validateValue(key, Integer.parseInt(value.toString()));
                    draft.put(key, checked);
                    errors.remove(key);
                }
                catch (IllegalArgumentException invalid)
                {
                    errors.put(key, "Check the value for " + label.toLowerCase());
                }
            }
        });
    }

    private void choice(String key, String label, String hint, Object[] values, String[] labels)
    {
        row(label, hint);
        Spinner select = new Spinner(viewContext);
        select.setContentDescription(label);
        select.setAdapter(new ArrayAdapter<>(viewContext, android.R.layout.simple_spinner_dropdown_item, labels));
        for (int index = 0; index < values.length; index++)
        {
            if (values[index].equals(draft.get(key)))
            {
                select.setSelection(index);
            }
        }
        select.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener()
        {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id)
            {
                draft.put(key, values[position]);
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent)
            {
            }
        });
        page.addView(select, new LinearLayout.LayoutParams(-1, dp(48)));
    }

    private void captureKey()
    {
        AlertDialog capture = new AlertDialog.Builder(viewContext).setTitle("Press your talk key").setMessage("Press a key on a connected keyboard").setNegativeButton("Cancel", null).create();
        captureDialog = capture;
        capture.setOnDismissListener(window ->
        {
            if (captureDialog == capture)
            {
                captureDialog = null;
            }
        });
        capture.setOnKeyListener((window, keyCode, event) ->
        {
            if (keyCode == KeyEvent.KEYCODE_BACK || KeyEvent.isModifierKey(keyCode))
            {
                return false;
            }
            if (event.getAction() == KeyEvent.ACTION_UP && keyCode > 0)
            {
                draft.put("native_ptt_key_code", keyCode);
                capture.dismiss();
                showSection(2);
            }
            return true;
        });
        capture.show();
    }

    private void save()
    {
        if (saving)
        {
            return;
        }
        if (!errors.isEmpty())
        {
            showProblem(errors.values().iterator().next());
            return;
        }
        save.setEnabled(false);
        cancel.setEnabled(false);
        dialog.setCancelable(false);
        dialog.setCanceledOnTouchOutside(false);
        saving = true;
        service.saveLocalSettings(draft, error ->
        {
            if (activity.isFinishing() || activity.isDestroyed() || !dialog.isShowing())
            {
                return;
            }
            save.setEnabled(true);
            cancel.setEnabled(true);
            dialog.setCancelable(true);
            dialog.setCanceledOnTouchOutside(true);
            saving = false;
            if (error != null)
            {
                showProblem(error);
                return;
            }
            dialog.dismiss();
            callbacks.onSaved();
        });
    }

    /**
     * @brief asks before deleting every message on this device (open conversations and the saved history), then
     *        deletes them
     */
    private void confirmDeleteAllMessages()
    {
        // ask first
        new AlertDialog.Builder(viewContext)
            .setTitle("Delete all messages?")
            .setMessage("Delete every channel and private message on this device, including the saved history and saved attachments. This cannot be undone. Other people keep their copies. Files you saved outside the app are kept.")
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Delete", (question, button) -> service.deleteAllMessages(error ->
            {
                // show the result
                if (activity.isFinishing() || activity.isDestroyed())
                {
                    return;
                }
                if (error != null)
                {
                    showProblem(error);
                    return;
                }
                android.widget.Toast.makeText(activity, "All messages deleted", android.widget.Toast.LENGTH_SHORT).show();
            }))
            .show();
    }

    private void openAction(Runnable action)
    {
        dialog.dismiss();
        action.run();
    }

    private void showProblem(String message)
    {
        problem.setText(message);
        problem.setVisibility(View.VISIBLE);
    }

    private LinearLayout row(String label, String hint)
    {
        LinearLayout row = new LinearLayout(viewContext);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(12), 0, dp(12));
        LinearLayout words = new LinearLayout(viewContext);
        words.setOrientation(LinearLayout.VERTICAL);
        words.setPadding(0, 0, dp(12), 0);
        words.addView(text(label, 15, false));
        TextView explanation = text(hint, 12, false);
        explanation.setTextColor(palette.secondary);
        explanation.setPadding(0, dp(5), 0, 0);
        words.addView(explanation);
        row.addView(words, new LinearLayout.LayoutParams(0, -2, 1));
        page.addView(row, new LinearLayout.LayoutParams(-1, -2));
        return row;
    }

    private void action(String label, Runnable action)
    {
        Button button = button(label, false);
        button.setOnClickListener(view -> action.run());
        LinearLayout.LayoutParams position = new LinearLayout.LayoutParams(-1, dp(48));
        position.topMargin = dp(8);
        page.addView(button, position);
    }

    private Button button(String label, boolean primary)
    {
        Button button = new Button(viewContext);
        button.setText(label);
        button.setAllCaps(false);
        button.setTextColor(primary ? palette.accentText : palette.text);
        button.setBackground(palette.rounded(primary ? palette.accent : palette.background, dp(14)));
        return button;
    }

    private TextView text(String value, int size, boolean bold)
    {
        TextView view = new TextView(viewContext);
        view.setText(value);
        view.setTextColor(palette.text);
        view.setTextSize(size);
        if (bold)
        {
            view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        }
        return view;
    }

    private int dp(int value)
    {
        return Math.round(value * viewContext.getResources().getDisplayMetrics().density);
    }
}
