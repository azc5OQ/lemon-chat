package com.lemonchat.ui;

import android.content.Context;
import android.graphics.Typeface;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.BaseAdapter;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.Spinner;
import android.widget.TextView;
import com.lemonchat.model.PermissionDefinition;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Consumer;

/** searches permission categories without building a wide editable table */
final class PermissionListView extends LinearLayout
{
    private final Palette palette;
    private final Consumer<PermissionFields.Field> selected;
    private final EditText search;
    private final Spinner categories;
    private final List<PermissionFields.Field> filtered = new ArrayList<>();
    private final Rows adapter = new Rows();
    private boolean channel;
    private PermissionDefinition values;
    private String category = "All categories";
    private boolean editable;
    private int editPower;

    PermissionListView(Context context, Palette palette, Consumer<PermissionFields.Field> selected)
    {
        super(context);
        this.palette = palette;
        this.selected = selected;
        setOrientation(VERTICAL);
        search = new EditText(context);
        search.setTag("admin-permission-search");
        search.setHint("Search permissions");
        search.setTextColor(palette.text);
        search.setHintTextColor(palette.secondary);
        search.setTextSize(15);
        search.setSingleLine(true);
        search.setSaveEnabled(false);
        search.setMinHeight(dp(48));
        search.setPadding(dp(12), dp(8), dp(12), dp(8));
        search.setBackground(palette.rounded(palette.input, dp(10)));
        addView(search, new LayoutParams(-1, -2));
        categories = new Spinner(context);
        categories.setTag("admin-permission-category");
        categories.setContentDescription("Permission category");
        addView(categories, new LayoutParams(-1, dp(48)));
        ListView list = new ListView(context);
        list.setTag("admin-permission-list");
        list.setDivider(null);
        list.setAdapter(adapter);
        list.setOnItemClickListener((parent, view, position, id) -> this.selected.accept(filtered.get(position)));
        addView(list, new LayoutParams(-1, 0, 1));
        TextView empty = text("No permissions match", 14, palette.secondary);
        empty.setPadding(dp(12), dp(16), dp(12), dp(16));
        addView(empty);
        list.setEmptyView(empty);
        search.addTextChangedListener(new TextWatcher()
        {
            @Override
            public void beforeTextChanged(CharSequence value, int start, int count, int after)
            {
            }

            @Override
            public void onTextChanged(CharSequence value, int start, int before, int count)
            {
                filter();
            }

            @Override
            public void afterTextChanged(Editable value)
            {
            }
        });
        categories.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener()
        {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id)
            {
                category = parent.getItemAtPosition(position).toString();
                filter();
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent)
            {
            }
        });
        setCategories();
    }

    void bind(boolean channel, PermissionDefinition values, boolean editable, int editPower)
    {
        boolean changedKind = this.channel != channel;
        this.channel = channel;
        this.values = values;
        this.editable = editable;
        this.editPower = editPower;
        if (changedKind)
        {
            setCategories();
        }
        filter();
    }

    private void setCategories()
    {
        List<String> names = new ArrayList<>();
        names.add("All categories");
        names.addAll(channel ? PermissionFields.CHANNEL_CATEGORIES : PermissionFields.CATEGORIES);
        category = names.get(0);
        categories.setAdapter(new ArrayAdapter<>(getContext(), android.R.layout.simple_spinner_dropdown_item, names));
    }

    private void filter()
    {
        filtered.clear();
        if (values != null)
        {
            for (PermissionFields.Field field : PermissionFields.forGroup(channel))
            {
                if ((field.power ? values.powers.containsKey(field.name) : values.actions.containsKey(field.name)) && (category.equals("All categories") || category.equals(field.category)) && field.matches(search.getText().toString()))
                {
                    filtered.add(field);
                }
            }
        }
        adapter.notifyDataSetChanged();
    }

    static String valueLabel(PermissionFields.Field field, PermissionDefinition values)
    {
        if (field.power)
        {
            return Integer.toString(values.powers.getOrDefault(field.name, 0));
        }
        String decision = values.actions.get(field.name);
        if ("grant".equals(decision))
        {
            return "Enabled";
        }
        if ("deny".equals(decision))
        {
            return "Disabled";
        }
        return field.defaultAllowed ? "Default: enabled" : "Default: disabled";
    }

    private final class Rows extends BaseAdapter
    {
        @Override
        public int getCount()
        {
            return filtered.size();
        }

        @Override
        public Object getItem(int position)
        {
            return filtered.get(position);
        }

        @Override
        public long getItemId(int position)
        {
            return position;
        }

        @Override
        public View getView(int position, View convert, ViewGroup parent)
        {
            PermissionFields.Field field = filtered.get(position);
            LinearLayout row = new LinearLayout(getContext());
            row.setTag("admin-permission:" + field.name);
            row.setOrientation(VERTICAL);
            row.setPadding(dp(12), dp(10), dp(12), dp(10));
            row.setMinimumHeight(dp(64));
            row.setBackground(palette.interactive(palette.surface, dp(8)));
            TextView label = text(field.label, 15, palette.text);
            label.setTypeface(null, Typeface.BOLD);
            row.addView(label);
            int required = values.grants.getOrDefault(field.name, 1000);
            String detail = valueLabel(field, values) + " · Edit power " + required;
            if (!editable || editPower < required)
            {
                detail += " · Read only";
            }
            PermissionDefinition.Flags options = values.flags.get(field.name);
            if (options != null && options.negate)
            {
                detail += " · Negate";
            }
            if (options != null && options.skip)
            {
                detail += " · Skip";
            }
            row.addView(text(detail, 13, palette.secondary));
            return row;
        }
    }

    private TextView text(String value, int size, int color)
    {
        TextView text = new TextView(getContext());
        text.setText(value);
        text.setTextColor(color);
        text.setTextSize(size);
        return text;
    }

    private int dp(int value)
    {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
