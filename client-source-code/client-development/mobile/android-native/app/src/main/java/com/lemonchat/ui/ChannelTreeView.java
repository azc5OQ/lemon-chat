package com.lemonchat.ui;

import android.content.Context;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.content.res.ColorStateList;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.PopupMenu;
import com.lemonchat.R;
import com.lemonchat.model.Channel;
import com.lemonchat.model.ChannelTree;
import com.lemonchat.model.Group;
import com.lemonchat.model.Member;
import com.lemonchat.model.SessionSnapshot;
import com.lemonchat.settings.LocalSettings;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** shows channels and their members using recycled native rows */
final class ChannelTreeView extends ListView
{
    interface Actions
    {
        void onJoin(Channel channel);
        void onOpenMember(Member member);
        void onChannelMembers(Channel channel);
        void onMemberInfo(Member member);
    }

    private final Palette palette;
    private final Actions actions;
    private final Set<Integer> collapsed = new HashSet<>();
    private final Map<Integer, Group> serverGroups = new HashMap<>();
    private final Map<Integer, Group> channelGroups = new HashMap<>();
    private final Rows adapter = new Rows();
    private final ServerIconCache icons;
    private List<ChannelTree.Row> rows = Collections.emptyList();
    private SessionSnapshot snapshot;
    private long connection = -1;
    private int currentChannel = -1;
    private LocalSettings settings;
    private AvatarImages avatars;
    private PopupMenu channelMenu;

    ChannelTreeView(Context context, Palette palette, Actions actions)
    {
        super(context);
        this.palette = palette;
        this.actions = actions;
        settings = LocalSettings.load(context);
        icons = new ServerIconCache(getResources(), adapter::notifyDataSetChanged);
        setDivider(null);
        setDividerHeight(0);
        setClipToPadding(false);
        setPadding(dp(4), dp(4), dp(4), dp(8));
        setBackgroundColor(palette.background);
        setAdapter(adapter);
    }

    @Override
    protected void onAttachedToWindow()
    {
        super.onAttachedToWindow();
        if (snapshot != null)
        {
            icons.update(snapshot.icons);
            adapter.notifyDataSetChanged();
        }
    }

    @Override
    protected void onDetachedFromWindow()
    {
        if (channelMenu != null)
        {
            channelMenu.dismiss();
            channelMenu = null;
        }
        icons.close();
        super.onDetachedFromWindow();
    }

    Set<Integer> collapsedChannels()
    {
        return new HashSet<>(collapsed);
    }

    void restoreCollapsed(Set<Integer> channels)
    {
        collapsed.clear();
        collapsed.addAll(channels);
        render(snapshot);
    }

    /**
     * replaces visible state while retaining the user's collapsed branches
     *
     * @param value immutable server snapshot
     */
    void render(SessionSnapshot value)
    {
        snapshot = value;
        if (value == null)
        {
            icons.close();
            rows = Collections.emptyList();
            adapter.notifyDataSetChanged();
            return;
        }
        HashMap<Integer, Channel> channels = new HashMap<>();
        for (Channel channel : value.channels)
        {
            channels.put(channel.id, channel);
        }
        collapsed.retainAll(channels.keySet());
        if (connection != value.connectionId)
        {
            icons.close();
            collapsed.clear();
            connection = value.connectionId;
            currentChannel = -1;
        }
        if (currentChannel != value.channelId)
        {
            HashSet<Integer> visited = new HashSet<>();
            Channel selected = channels.get(value.channelId);
            while (selected != null && visited.add(selected.id))
            {
                collapsed.remove(selected.id);
                selected = channels.get(selected.parentId);
            }
            currentChannel = value.channelId;
        }
        serverGroups.clear();
        channelGroups.clear();
        for (Group group : value.groups)
        {
            (group.channelGroup ? channelGroups : serverGroups).put(group.id, group);
        }
        icons.update(value.icons);
        List<ChannelTree.Row> available = ChannelTree.rows(value, settings.getBoolean("lemon_channels_flat") ? Collections.emptySet() : collapsed);
        ArrayList<ChannelTree.Row> visible = new ArrayList<>();
        for (ChannelTree.Row row : available)
        {
            if (row.member == null && row.channel == null && settings.getBoolean("lemon_strip_neighbors"))
            {
                continue;
            }
            if (row.member != null && settings.getBoolean("lemon_strip_neighbors") && row.member.channelId != value.channelId)
            {
                continue;
            }
            if (row.member != null && settings.getBoolean("lemon_channels_flat") && collapsed.contains(row.member.channelId))
            {
                continue;
            }
            visible.add(row);
        }
        rows = visible;
        adapter.notifyDataSetChanged();
    }

    void preferences(LocalSettings value, AvatarImages pictures)
    {
        settings = value;
        avatars = pictures;
    }

    private final class Rows extends BaseAdapter
    {
        @Override
        public int getCount()
        {
            return rows.size();
        }

        @Override
        public ChannelTree.Row getItem(int position)
        {
            return rows.get(position);
        }

        @Override
        public long getItemId(int position)
        {
            return getItem(position).stableId();
        }

        @Override
        public boolean hasStableIds()
        {
            return true;
        }

        @Override
        public View getView(int position, View recycled, ViewGroup parent)
        {
            Holder holder = recycled == null ? new Holder() : (Holder) recycled.getTag();
            bind(holder, getItem(position));
            return holder.root;
        }
    }

    private final class Holder
    {
        final LinearLayout root = new LinearLayout(getContext());
        final ImageButton expand = iconButton();
        final ImageView state = image();
        final TextView name = label(14, false);
        final TextView song = label(12, false);
        final LinearLayout indicators = new LinearLayout(getContext());

        Holder()
        {
            root.setOrientation(LinearLayout.HORIZONTAL);
            root.setGravity(Gravity.CENTER_VERTICAL);
            root.setMinimumHeight(dp(40));
            root.setTag(this);
            expand.setPadding(dp(7), dp(12), dp(7), dp(12));
            root.addView(expand, new LinearLayout.LayoutParams(dp(30), dp(40)));
            LinearLayout.LayoutParams statePosition = new LinearLayout.LayoutParams(dp(22), dp(22));
            statePosition.setMarginEnd(dp(6));
            root.addView(state, statePosition);
            name.setPadding(0, dp(4), dp(6), dp(4));
            root.addView(name, new LinearLayout.LayoutParams(0, -2, 1));
            song.setTextColor(palette.secondary);
            song.setEllipsize(TextUtils.TruncateAt.MARQUEE);
            song.setMarqueeRepeatLimit(-1);
            song.setFocusable(false);
            song.setVisibility(GONE);
            root.addView(song, new LinearLayout.LayoutParams(0, -2, 1));
            indicators.setOrientation(LinearLayout.HORIZONTAL);
            indicators.setGravity(Gravity.CENTER_VERTICAL);
            root.addView(indicators, new LinearLayout.LayoutParams(-2, -2));
        }
    }

    private void bind(Holder holder, ChannelTree.Row row)
    {
        int width = getWidth() > 0 ? getWidth() : getResources().getDisplayMetrics().widthPixels;
        int maximumDepth = Math.min(8, Math.max(0, (width - dp(260)) / dp(20)));
        int depth = settings.getBoolean("lemon_channels_flat") ? row.member == null ? 0 : 1 : Math.min(row.depth, maximumDepth);
        holder.root.setPadding(dp(depth * 20 + 4), 0, dp(8), 0);
        holder.root.setOnClickListener(null);
        holder.root.setOnLongClickListener(null);
        holder.root.setOnContextClickListener(null);
        holder.root.setClickable(false);
        holder.root.setLongClickable(false);
        holder.root.setFocusable(false);
        holder.root.setTooltipText(null);
        holder.expand.setOnClickListener(null);
        holder.expand.setVisibility(GONE);
        holder.state.setImageDrawable(null);
        holder.state.setBackground(null);
        holder.state.setPadding(0, 0, 0, 0);
        holder.state.setVisibility(GONE);
        holder.indicators.removeAllViews();
        if (row.member == null || !row.member.streamingSong)
        {
            holder.song.setSelected(false);
            holder.song.setText("");
            holder.song.setVisibility(GONE);
        }
        holder.name.setMaxWidth(Integer.MAX_VALUE);
        holder.name.setLayoutParams(new LinearLayout.LayoutParams(0, -2, 1));
        holder.name.setTextColor(palette.text);
        holder.name.setTypeface(Typeface.DEFAULT, Typeface.NORMAL);
        holder.root.setBackgroundColor(android.graphics.Color.TRANSPARENT);
        if (row.channel != null)
        {
            bindChannel(holder, row);
        }
        else if (row.member != null)
        {
            bindMember(holder, row.member);
        }
        else
        {
            holder.name.setText("Other online people");
            holder.name.setTextColor(palette.secondary);
            holder.root.setContentDescription("Other online people. Channel membership is not visible");
        }
    }

    private void bindChannel(Holder holder, ChannelTree.Row row)
    {
        Channel channel = row.channel;
        boolean current = snapshot != null && channel.id == snapshot.channelId;
        holder.root.setBackground(palette.interactive(current ? palette.accent : android.graphics.Color.TRANSPARENT, dp(4)));
        holder.name.setTextColor(current ? palette.accentText : palette.text);
        holder.name.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        holder.name.setText(channel.name + (settings.getBoolean("lemon_show_channel_people") ? " (" + (row.people + row.musicBots) + ")" : ""));
        holder.expand.setVisibility(row.expandable ? VISIBLE : INVISIBLE);
        boolean expanded = settings.getBoolean("lemon_channels_flat") ? !collapsed.contains(channel.id) : row.expanded;
        holder.expand.setImageDrawable(new GlyphDrawable(expanded ? "down" : "arrow", current ? palette.accentText : palette.secondary));
        holder.expand.setContentDescription((expanded ? "Collapse " : "Expand ") + channel.name);
        holder.expand.setOnClickListener(view ->
        {
            if (!collapsed.add(channel.id))
            {
                collapsed.remove(channel.id);
            }
            render(snapshot);
        });
        holder.expand.setBackground(palette.interactive(android.graphics.Color.TRANSPARENT, dp(4)));
        if (!channel.audioEnabled)
        {
            addIndicator(holder, new GlyphDrawable("speaker-off", palette.secondary), "Voice disabled");
        }
        if (channel.passwordProtected)
        {
            addIndicator(holder, new GlyphDrawable("lock", palette.secondary), "Password protected");
        }
        Drawable custom = settings.getBoolean("lemon_hide_channel_icons") ? null : icons.drawable(channel.iconId);
        if (custom != null)
        {
            addIndicator(holder, custom, "Channel icon");
        }
        if (settings.getBoolean("lemon_show_channel_people") && avatars != null)
        {
            int shown = 0;
            for (Member member : snapshot.members)
            {
                if (member.channelId == channel.id && shown++ < 3)
                {
                    Drawable face = avatars.drawable(member);
                    addIndicator(holder, face != null ? face : new GlyphDrawable(member.musicBot ? "music" : "me", palette.secondary), member.name);
                }
            }
        }
        holder.root.setOnClickListener(view -> actions.onJoin(channel));
        holder.root.setFocusable(true);
        holder.root.setOnLongClickListener(view ->
        {
            showChannelMenu(view, channel);
            return true;
        });
        holder.root.setOnContextClickListener(view ->
        {
            showChannelMenu(view, channel);
            return true;
        });
        String count = row.people + (row.people == 1 ? " person" : " people");
        if (row.musicBots > 0)
        {
            count += ", " + row.musicBots + (row.musicBots == 1 ? " music bot" : " music bots");
        }
        holder.root.setContentDescription(channel.name + ". " + (current ? "You are here. " : "") + count + (channel.passwordProtected ? ". Password protected" : "") + (channel.audioEnabled ? "" : ". Voice disabled"));
    }

    private void bindMember(Holder holder, Member member)
    {
        boolean local = snapshot != null && member.id == snapshot.clientId;
        holder.name.setText(member.name + (settings.getBoolean("lemon_show_flags") ? MainActivity.countryFlag(member.countryCode) : ""));
        holder.name.setTypeface(Typeface.DEFAULT, local ? Typeface.BOLD : Typeface.NORMAL);
        if (member.streamingSong)
        {
            int width = getWidth() > 0 ? getWidth() : getResources().getDisplayMetrics().widthPixels;
            holder.name.setMaxWidth(Math.max(dp(64), width * 2 / 5));
            holder.name.setLayoutParams(new LinearLayout.LayoutParams(-2, -2));
            if (!TextUtils.equals(holder.song.getText(), member.songName))
            {
                holder.song.setText(member.songName);
            }
            holder.song.setContentDescription("Playing " + member.songName);
            holder.song.setVisibility(VISIBLE);
            holder.song.setSelected(true);
        }
        holder.state.setVisibility(VISIBLE);
        boolean speaking = !member.musicBot && !member.idle && member.audioState == 1;
        String icon = member.musicBot ? "music" : member.audioState == 4 ? "speaker-off" : member.audioState == 3 || member.idle ? "mic-off" : member.audioState == 1 ? "mic-active" : "mic";
        int color = speaking ? 0xff10251f : palette.secondary;
        GradientDrawable circle = palette.rounded(speaking ? 0xff4dd7bd : palette.surface, dp(11));
        circle.setStroke(dp(1), speaking ? 0xff75e6cf : palette.border);
        holder.state.setBackground(circle);
        holder.state.setPadding(dp(3), dp(3), dp(3), dp(3));
        holder.state.setImageDrawable(new GlyphDrawable(icon, color));
        holder.state.setContentDescription(ChannelTree.audioDescription(member));
        if (!settings.getBoolean("lemon_audio_availability") && !speaking && !member.musicBot)
        {
            holder.state.setVisibility(INVISIBLE);
        }
        ArrayList<Group> memberships = new ArrayList<>();
        Group role = channelGroups.get(member.channelGroupId);
        if (role != null && !member.idle && member.channelId >= 0)
        {
            memberships.add(role);
        }
        for (int id : member.groupIds)
        {
            Group group = serverGroups.get(id);
            if (group != null)
            {
                memberships.add(group);
            }
        }
        StringBuilder description = new StringBuilder(member.name + (local ? ". You" : "") + ". " + ChannelTree.audioDescription(member));
        if (member.streamingSong)
        {
            description.append(". Playing ").append(member.songName);
        }
        int visibleIcons = 0;
        int inspectedIcons = 0;
        for (Group group : memberships)
        {
            String groupName = (group.channelGroup ? "Channel group: " : "Server group: ") + group.name;
            description.append(". ").append(groupName);
            boolean hidden = settings.getBoolean(group.channelGroup ? "lemon_hide_channel_group_icons" : "lemon_hide_server_group_icons");
            if (hidden || visibleIcons >= 6 || inspectedIcons >= 8 || group.iconId == -1)
            {
                continue;
            }
            inspectedIcons++;
            Drawable drawable = group.channelGroup && group.iconId == -2 ? getContext().getDrawable(R.drawable.ic_channel_admin) : icons.drawable(group.iconId);
            if (drawable != null && visibleIcons < 6)
            {
                ImageView image = addIndicator(holder, drawable, groupName);
                image.setOnClickListener(view -> actions.onMemberInfo(member));
                visibleIcons++;
            }
        }
        if (!member.musicBot && !local)
        {
            holder.root.setBackground(palette.interactive(android.graphics.Color.TRANSPARENT, dp(4)));
            holder.root.setFocusable(true);
            holder.root.setOnClickListener(view -> actions.onOpenMember(member));
        }
        holder.root.setOnLongClickListener(view ->
        {
            actions.onMemberInfo(member);
            return true;
        });
        holder.root.setTooltipText(description.toString());
        holder.root.setContentDescription(description.toString());
    }

    private void showChannelMenu(View anchor, Channel channel)
    {
        if (snapshot == null || !snapshot.connected)
        {
            return;
        }
        if (channelMenu != null)
        {
            channelMenu.dismiss();
        }
        long expectedConnection = snapshot.connectionId;
        channelMenu = new PopupMenu(new android.view.ContextThemeWrapper(getContext(), palette.theme.windowStyle(palette.dark)), anchor);
        channelMenu.getMenu().add("Join").setEnabled(channel.id != snapshot.channelId).setOnMenuItemClickListener(item ->
        {
            if (snapshot != null && snapshot.connected && snapshot.connectionId == expectedConnection)
            {
                actions.onJoin(channel);
            }
            return true;
        });
        channelMenu.getMenu().add("View members").setOnMenuItemClickListener(item ->
        {
            if (snapshot != null && snapshot.connected && snapshot.connectionId == expectedConnection)
            {
                actions.onChannelMembers(channel);
            }
            return true;
        });
        channelMenu.show();
    }

    private ImageView addIndicator(Holder holder, Drawable drawable, String description)
    {
        ImageView image = image();
        image.setImageDrawable(drawable);
        image.setContentDescription(description);
        image.setTooltipText(description);
        LinearLayout.LayoutParams position = new LinearLayout.LayoutParams(dp(20), dp(20));
        position.setMarginStart(dp(4));
        holder.indicators.addView(image, position);
        return image;
    }

    private ImageView image()
    {
        ImageView image = new ImageView(getContext());
        image.setScaleType(ImageView.ScaleType.FIT_CENTER);
        return image;
    }

    private ImageButton iconButton()
    {
        ImageButton button = new ImageButton(getContext());
        button.setPadding(dp(12), dp(12), dp(12), dp(12));
        button.setBackground(new RippleDrawable(ColorStateList.valueOf(palette.border), null, palette.rounded(0xffffffff, dp(24))));
        button.setFocusable(false);
        return button;
    }

    private TextView label(int size, boolean bold)
    {
        TextView view = new TextView(getContext());
        view.setTextColor(palette.text);
        view.setTextSize(size);
        view.setSingleLine(true);
        view.setEllipsize(TextUtils.TruncateAt.END);
        if (bold)
        {
            view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        }
        return view;
    }

    private int dp(int value)
    {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
