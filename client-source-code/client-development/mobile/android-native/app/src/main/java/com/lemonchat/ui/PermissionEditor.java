package com.lemonchat.ui;

import android.content.Context;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import com.lemonchat.model.PermissionDefinition;

/** edits one permission without sending an unfinished group definition */
final class PermissionEditor extends AdminWindow
{
    interface Apply
    {
        String value(String value, int required, PermissionDefinition.Flags flags);
    }

    private final Palette palette;
    private final LinearLayout root;
    private final TextView problem;
    private final Button use;
    private boolean invalidated;

    static PermissionEditor show(Context context, Palette palette, PermissionFields.Field field, PermissionDefinition values, boolean editable, boolean administrator, int editPower, Apply apply)
    {
        PermissionEditor editor = new PermissionEditor(context, palette, field, values, editable, administrator, editPower, apply, new LinearLayout(context));
        editor.show();
        return editor;
    }

    private PermissionEditor(Context context, Palette palette, PermissionFields.Field field, PermissionDefinition values, boolean editable, boolean administrator, int editPower, Apply apply, LinearLayout root)
    {
        super(context, root, 560, 720);
        this.root = root;
        this.palette = palette;
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(18), dp(16), dp(18), dp(12));
        root.setBackground(palette.rounded(palette.surface, dp(18)));
        TextView title = text(field.label, 20, palette.text);
        title.setTag("admin-permission-title");
        root.addView(title);
        ScrollView scroll = new ScrollView(context);
        LinearLayout body = new LinearLayout(context);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(0, dp(14), 0, dp(12));
        scroll.addView(body);
        root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        body.addView(text(field.help.isEmpty() ? powerHelp(field.name) : field.help, 14, palette.secondary));
        label(body, field.power ? "Power" : "Action");
        EditText number = null;
        Spinner action = null;
        if (field.power)
        {
            number = number(values.powers.getOrDefault(field.name, 0), "Permission power");
            number.setEnabled(editable);
            body.addView(number);
        }
        else
        {
            action = new Spinner(context);
            action.setTag("admin-permission-action");
            action.setContentDescription("Permission action");
            String[] choices = field.channel ? new String[]{"Enable", "Disable"} : new String[]{"Enable", "Disable", field.defaultAllowed ? "Default: enabled" : "Default: disabled"};
            action.setAdapter(new ArrayAdapter<>(context, android.R.layout.simple_spinner_dropdown_item, choices));
            String original = values.actions.get(field.name);
            action.setSelection("grant".equals(original) ? 0 : "deny".equals(original) || field.channel ? 1 : 2);
            action.setEnabled(editable);
            body.addView(action, new LinearLayout.LayoutParams(-1, dp(48)));
        }
        label(body, "Required editing power");
        EditText required = number(values.grants.getOrDefault(field.name, 1000), "Required editing power");
        required.setTag("admin-permission-grant");
        required.setEnabled(editable);
        body.addView(required);
        body.addView(text("For example, a requirement of 50 lets someone with editing power 50 change this setting. Power 49 is too low. Group management rights still apply.", 13, palette.secondary));
        PermissionDefinition.Flags flags = values.flags.get(field.name);
        CheckBox negate = flag("Negate", flags != null && flags.negate, editable && administrator);
        negate.setTag("admin-permission-negate");
        body.addView(negate);
        body.addView(text("Negate makes the lowest marked value win over ordinary group values. Disable with Negate blocks an action even when another group enables it.", 13, palette.secondary));
        CheckBox skip = flag("Skip channel group values", flags != null && flags.skip, editable && administrator);
        skip.setTag("admin-permission-skip");
        if (field.skip)
        {
            body.addView(skip);
            body.addView(text("Keep server group values for this setting and ignore its channel group values. Other server groups still combine normally.", 13, palette.secondary));
        }
        if (!administrator)
        {
            body.addView(text("Only an administrator can change Negate or Skip. Your editing power is " + editPower + ".", 13, palette.secondary));
        }
        if (!editable)
        {
            body.addView(text("You can view this setting, but your current access does not allow changing it.", 14, palette.secondary));
        }
        problem = text("", 13, palette.danger);
        problem.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        root.addView(problem);
        LinearLayout footer = new LinearLayout(context);
        footer.setGravity(Gravity.CENTER);
        Button cancel = button("Cancel", false);
        cancel.setOnClickListener(view -> dismiss());
        footer.addView(cancel, new LinearLayout.LayoutParams(0, dp(48), 1));
        use = button("Use value", true);
        use.setTag("admin-permission-use");
        use.setEnabled(editable);
        LinearLayout.LayoutParams position = new LinearLayout.LayoutParams(0, dp(48), 1);
        position.setMarginStart(dp(10));
        footer.addView(use, position);
        root.addView(footer);
        EditText selectedNumber = number;
        Spinner selectedAction = action;
        use.setOnClickListener(view ->
        {
            if (invalidated)
            {
                return;
            }
            try
            {
                String value = field.power ? Integer.toString(PermissionDraft.number(selectedNumber.getText().toString())) : new String[]{"grant", "deny", "default"}[selectedAction.getSelectedItemPosition()];
                int grant = PermissionDraft.number(required.getText().toString());
                if (!administrator && grant > editPower)
                {
                    throw new IllegalArgumentException("Required editing power cannot exceed your own power of " + editPower);
                }
                String error = apply.value(value, grant, new PermissionDefinition.Flags(field.skip && skip.isChecked(), negate.isChecked()));
                if (error == null)
                {
                    dismiss();
                }
                else
                {
                    problem.setText(error);
                }
            }
            catch (IllegalArgumentException error)
            {
                problem.setText(error.getMessage());
            }
        });
    }

    void invalidate(String reason)
    {
        invalidated = true;
        use.setEnabled(false);
        problem.setText(reason);
    }

    private String powerHelp(String name)
    {
        if (name.equals("permission_edit_power"))
        {
            return "This power must reach a setting's required editing power before it can be changed. Permission to edit the group is also needed.";
        }
        if (name.equals("group_management_power"))
        {
            return "This power must reach the group's protection for group management and membership changes. The action's permission is also needed.";
        }
        if (name.equals("group_protection"))
        {
            return "Someone changing this group needs group management power at least this high. Separate permissions still apply.";
        }
        if (name.endsWith("_protection"))
        {
            return "The matching action power must reach this protection. Protection 50 blocks power 49, but power 50 can pass this check. Other permissions still apply.";
        }
        if (name.startsWith("channel_") || name.equals("talk_power") || name.equals("stream_power"))
        {
            return "This power must reach the requirement set on the channel. A requirement of 50 accepts power 50 and rejects power 49. Other permissions still apply.";
        }
        return "This action's power must reach the target's matching protection. Power 50 reaches protection 50 but not 51. The action must also be enabled.";
    }

    private CheckBox flag(String label, boolean checked, boolean enabled)
    {
        CheckBox field = new CheckBox(getContext());
        field.setText(label);
        field.setTextColor(palette.text);
        field.setTextSize(15);
        field.setMinHeight(dp(48));
        field.setChecked(checked);
        field.setEnabled(enabled);
        return field;
    }

    private EditText number(int value, String description)
    {
        EditText field = new EditText(getContext());
        field.setContentDescription(description);
        field.setInputType(InputType.TYPE_CLASS_NUMBER);
        field.setSingleLine(true);
        field.setSaveEnabled(false);
        field.setText(Integer.toString(value));
        field.setTextColor(palette.text);
        field.setTextSize(16);
        field.setMinHeight(dp(48));
        field.setPadding(dp(12), dp(8), dp(12), dp(8));
        field.setBackground(palette.rounded(palette.input, dp(10)));
        return field;
    }

    private void label(LinearLayout parent, String value)
    {
        TextView label = text(value, 14, palette.text);
        label.setPadding(0, dp(16), 0, dp(6));
        parent.addView(label);
    }

    private TextView text(String value, int size, int color)
    {
        TextView text = new TextView(getContext());
        text.setText(value);
        text.setTextColor(color);
        text.setTextSize(size);
        return text;
    }

    private Button button(String label, boolean primary)
    {
        Button button = new Button(getContext());
        button.setText(label);
        button.setAllCaps(false);
        button.setTextColor(primary ? palette.accentText : palette.text);
        button.setBackground(palette.interactive(primary ? palette.accent : palette.surface, dp(10)));
        return button;
    }

    private int dp(int value)
    {
        return Math.round(value * getContext().getResources().getDisplayMetrics().density);
    }
}
