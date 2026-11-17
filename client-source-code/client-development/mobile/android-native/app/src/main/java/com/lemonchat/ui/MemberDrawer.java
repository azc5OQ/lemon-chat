package com.lemonchat.ui;

import android.app.Dialog;
import android.content.Context;
import android.graphics.Color;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.BaseAdapter;
import android.widget.EditText;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import com.lemonchat.model.Member;
import com.lemonchat.model.SessionSnapshot;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/** shows a searchable copy of the current server member list */
final class MemberDrawer extends Dialog
{
    interface Actions
    {
        View row(Member member);
        void open(Member member);
    }

    private final Actions actions;
    private final EditText search;
    private final BaseAdapter adapter;
    private final List<Member> visible = new ArrayList<>();
    private List<Member> members = Collections.emptyList();
    private int localId = -1;

    MemberDrawer(Context context, Palette palette, Actions actions)
    {
        super(context);
        this.actions = actions;
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        LinearLayout panel = new LinearLayout(context);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setTag("theme-members-drawer");
        panel.setBackgroundColor(palette.surface);
        panel.setPadding(dp(8), dp(8), dp(8), dp(8));
        LinearLayout heading = new LinearLayout(context);
        heading.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = new TextView(context);
        title.setText("Server members");
        title.setTextColor(palette.text);
        title.setTextSize(17);
        title.setPadding(dp(8), 0, 0, 0);
        heading.addView(title, new LinearLayout.LayoutParams(0, -2, 1));
        ImageButton close = new ImageButton(context);
        close.setImageDrawable(new GlyphDrawable("close", palette.text));
        close.setPadding(dp(12), dp(12), dp(12), dp(12));
        close.setBackground(palette.interactive(Color.TRANSPARENT, dp(24)));
        close.setContentDescription("Close member list");
        close.setTag("theme-members-close");
        close.setOnClickListener(view -> dismiss());
        heading.addView(close, new LinearLayout.LayoutParams(dp(48), dp(48)));
        panel.addView(heading);
        search = new EditText(context);
        search.setTag("theme-members-search");
        search.setHint("Search server members");
        search.setSingleLine(true);
        search.setTextSize(15);
        search.setTextColor(palette.text);
        search.setHintTextColor(palette.secondary);
        search.setBackground(palette.rounded(palette.input, dp(8)));
        search.setPadding(dp(12), dp(8), dp(12), dp(8));
        search.setMinimumHeight(dp(48));
        search.setFilters(new android.text.InputFilter[]{new android.text.InputFilter.LengthFilter(128)});
        LinearLayout.LayoutParams searchPosition = new LinearLayout.LayoutParams(-1, -2);
        searchPosition.setMargins(dp(4), dp(8), dp(4), dp(8));
        panel.addView(search, searchPosition);
        adapter = new BaseAdapter()
        {
            @Override
            public int getCount()
            {
                return visible.size();
            }

            @Override
            public Object getItem(int position)
            {
                return visible.get(position);
            }

            @Override
            public long getItemId(int position)
            {
                return visible.get(position).id;
            }

            @Override
            public View getView(int position, View recycled, ViewGroup parent)
            {
                Member member = visible.get(position);
                View row = actions.row(member);
                row.setTag("theme-drawer-member:" + member.id);
                if (!member.musicBot && member.id != localId)
                {
                    row.setOnClickListener(view ->
                    {
                        dismiss();
                        actions.open(member);
                    });
                }
                return row;
            }
        };
        ListView list = new ListView(context);
        list.setTag("theme-members-list");
        list.setDivider(null);
        list.setAdapter(adapter);
        panel.addView(list, new LinearLayout.LayoutParams(-1, 0, 1));
        TextView empty = new TextView(context);
        empty.setText("No matching members");
        empty.setTextColor(palette.secondary);
        empty.setGravity(Gravity.CENTER);
        panel.addView(empty, new LinearLayout.LayoutParams(-1, dp(64)));
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
        setContentView(panel);
        Window window = getWindow();
        if (window != null)
        {
            window.setBackgroundDrawableResource(android.R.color.transparent);
            window.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
            window.setDimAmount(0.35f);
            window.setGravity(Gravity.RIGHT | Gravity.TOP);
            window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE | WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN);
        }
        setOnShowListener(dialog ->
        {
            if (window != null)
            {
                int width = Math.min(dp(360), getContext().getResources().getDisplayMetrics().widthPixels * 9 / 10);
                window.setLayout(width, -1);
                panel.setTranslationX(width);
                panel.animate().translationX(0).setDuration(180).start();
            }
        });
    }

    void render(SessionSnapshot snapshot)
    {
        members = snapshot == null || !snapshot.connected ? Collections.emptyList() : snapshot.members;
        localId = snapshot == null ? -1 : snapshot.clientId;
        filter();
    }

    private void filter()
    {
        String query = search.getText().toString().trim().toLowerCase(Locale.ROOT);
        visible.clear();
        for (Member member : members)
        {
            if (query.isEmpty() || member.name.toLowerCase(Locale.ROOT).contains(query))
            {
                visible.add(member);
            }
        }
        adapter.notifyDataSetChanged();
    }

    private int dp(float value)
    {
        return Math.round(value * getContext().getResources().getDisplayMetrics().density);
    }
}
