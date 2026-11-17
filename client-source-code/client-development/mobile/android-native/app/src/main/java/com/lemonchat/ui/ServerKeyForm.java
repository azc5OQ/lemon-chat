package com.lemonchat.ui;

import android.content.Context;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;
import org.json.JSONObject;

/** edits ordered server keys without exposing their wire representation */
final class ServerKeyForm extends LinearLayout
{
    private static final String[] TYPES = {"aes256", "chacha20", "camellia256", "twofish256", "serpent256", "trivium80"};
    private static final String[] LABELS = {"AES 256", "ChaCha20", "Camellia 256", "Twofish 256", "Serpent 256", "Trivium 80"};
    private final Palette palette;
    private final LinearLayout rows;
    private final List<KeyRow> entries = new ArrayList<>();
    private final Button add;
    private boolean invalidSavedKeys;

    ServerKeyForm(Context context, Palette palette, String saved)
    {
        super(context);
        this.palette = palette;
        setOrientation(VERTICAL);
        rows = new LinearLayout(context);
        rows.setOrientation(VERTICAL);
        addView(rows);
        add = new Button(context);
        add.setText("Add server key");
        add.setAllCaps(false);
        add.setTextColor(palette.text);
        add.setBackground(palette.interactive(palette.background, dp(12)));
        add.setOnClickListener(view -> addRow(0, ""));
        addView(add, new LayoutParams(-1, dp(48)));
        if (!saved.isEmpty())
        {
            try
            {
                for (String line : saved.split("\n"))
                {
                    if (line.trim().isEmpty())
                    {
                        continue;
                    }
                    JSONObject value = new JSONObject(line);
                    int type = -1;
                    for (int i = 0; i < TYPES.length; i++)
                    {
                        if (TYPES[i].equals(value.getString("type")))
                        {
                            type = i;
                        }
                    }
                    if (type < 0 || entries.size() >= 8)
                    {
                        throw new IllegalArgumentException("Unsupported saved keys");
                    }
                    addRow(type, value.getString("password"));
                }
            }
            catch (Exception error)
            {
                invalidSavedKeys = true;
                TextView notice = new TextView(context);
                notice.setText("The saved keys could not be read. Clear them before entering replacement keys.");
                notice.setTextColor(palette.danger);
                addView(notice);
                Button clear = new Button(context);
                clear.setText("Clear saved keys");
                clear.setOnClickListener(view ->
                {
                    rows.removeAllViews();
                    entries.clear();
                    invalidSavedKeys = false;
                    notice.setVisibility(GONE);
                    clear.setVisibility(GONE);
                    add.setEnabled(true);
                });
                addView(clear);
            }
        }
    }

    /**
     * serializes the visible keys in their current order
     *
     * @return the desktop key row format
     */
    String serialize()
    {
        if (invalidSavedKeys)
        {
            throw new IllegalArgumentException("Clear or replace the unreadable server keys first");
        }
        StringBuilder result = new StringBuilder();
        try
        {
            for (KeyRow entry : entries)
            {
                if (result.length() > 0)
                {
                    result.append('\n');
                }
                result.append(new JSONObject().put("type", TYPES[entry.type.getSelectedItemPosition()]).put("password", entry.password.getText().toString()).toString());
            }
        }
        catch (org.json.JSONException error)
        {
            throw new IllegalArgumentException("The server keys could not be prepared", error);
        }
        return result.toString();
    }

    private void addRow(int selected, String secret)
    {
        if (entries.size() >= 8)
        {
            return;
        }
        LinearLayout row = new LinearLayout(getContext());
        row.setOrientation(VERTICAL);
        row.setPadding(0, dp(8), 0, dp(12));
        LinearLayout heading = new LinearLayout(getContext());
        heading.setGravity(Gravity.CENTER_VERTICAL);
        Spinner type = new Spinner(getContext());
        type.setSaveEnabled(false);
        ArrayAdapter<String> adapter = new ArrayAdapter<>(getContext(), android.R.layout.simple_spinner_dropdown_item, LABELS)
        {
            @Override
            public View getView(int position, View convertView, android.view.ViewGroup parent)
            {
                TextView view = (TextView) super.getView(position, convertView, parent);
                view.setTextColor(palette.text);
                return view;
            }
        };
        type.setAdapter(adapter);
        type.setSelection(selected);
        heading.addView(type, new LayoutParams(0, dp(48), 1));
        Button remove = new Button(getContext());
        remove.setText("Remove");
        remove.setAllCaps(false);
        remove.setTextColor(palette.secondary);
        remove.setBackground(palette.interactive(android.graphics.Color.TRANSPARENT, dp(12)));
        heading.addView(remove, new LayoutParams(dp(94), dp(48)));
        row.addView(heading);
        EditText password = new EditText(getContext());
        password.setSaveEnabled(false);
        password.setSingleLine(true);
        password.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        password.setHint("Server key");
        password.setTextColor(palette.text);
        password.setHintTextColor(palette.secondary);
        password.setTextSize(15);
        password.setPadding(dp(14), dp(10), dp(14), dp(10));
        password.setBackground(palette.rounded(palette.background, dp(12)));
        password.setText(secret);
        row.addView(password, new LayoutParams(-1, dp(48)));
        KeyRow entry = new KeyRow(type, password);
        entries.add(entry);
        rows.addView(row);
        remove.setOnClickListener(view ->
        {
            entries.remove(entry);
            rows.removeView(row);
            add.setEnabled(true);
        });
        add.setEnabled(entries.size() < 8);
    }

    private int dp(int value)
    {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }

    private static final class KeyRow
    {
        final Spinner type;
        final EditText password;

        KeyRow(Spinner type, EditText password)
        {
            this.type = type;
            this.password = password;
        }
    }
}
