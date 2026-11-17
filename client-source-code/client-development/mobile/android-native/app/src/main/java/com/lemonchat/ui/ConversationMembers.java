package com.lemonchat.ui;

import android.content.Context;
import android.graphics.drawable.Drawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import com.lemonchat.model.ChannelTree;
import com.lemonchat.model.Group;
import com.lemonchat.model.Member;
import com.lemonchat.model.SessionSnapshot;
import com.lemonchat.settings.LocalSettings;
import java.util.ArrayList;
import java.util.List;
import java.util.HashMap;
import java.util.Map;
import com.lemonchat.R;

/** shows nearby people beside or above a conversation */
final class ConversationMembers extends FrameLayout
{
    interface Actions
    {
        void open(Member member);
        void info(Member member);
        void allPeople();
    }

    private final Palette palette;
    private final LocalSettings settings;
    private final AvatarImages avatars;
    private final Actions actions;
    private final List<Member> members = new ArrayList<>();
    private final boolean vertical;
    private final boolean currentChannelOnly;
    private final LinearLayout strip;
    private final BaseAdapter adapter;
    private final ServerIconCache icons;
    private final Map<Integer, Group> serverGroups = new HashMap<>();
    private final Map<Integer, Group> channelGroups = new HashMap<>();
    private long localId;

    ConversationMembers(Context context, Palette palette, LocalSettings settings, AvatarImages avatars, Actions actions)
    {
        super(context);
        this.palette = palette;
        this.settings = settings;
        this.avatars = avatars;
        this.actions = actions;
        vertical = palette.theme.sideStrip(settings.getBoolean("lemon_strip_vertical"));
        currentChannelOnly = palette.theme.tree || settings.getBoolean("lemon_strip_neighbors");
        setTag("theme-member-strip");
        setContentDescription(vertical ? "Channel people on the left" : "People above the conversation");
        setBackground(vertical ? palette.memberColumnSurface(getResources().getDisplayMetrics().density) : palette.rounded(palette.surface, 0));
        strip = new LinearLayout(context);
        strip.setOrientation(LinearLayout.HORIZONTAL);
        strip.setGravity(Gravity.CENTER_VERTICAL);
        strip.setMinimumHeight(dp(70));
        adapter = new BaseAdapter()
        {
            @Override
            public int getCount()
            {
                return members.size();
            }

            @Override
            public Object getItem(int position)
            {
                return members.get(position);
            }

            @Override
            public long getItemId(int position)
            {
                return members.get(position).id;
            }

            @Override
            public View getView(int position, View recycled, ViewGroup parent)
            {
                return tile(members.get(position));
            }
        };
        icons = new ServerIconCache(getResources(), this::refresh);
        if (vertical)
        {
            ListView list = new ListView(context);
            list.setDivider(null);
            list.setAdapter(adapter);
            addView(list, new LayoutParams(-1, -1));
        }
        else
        {
            HorizontalScrollView scroll = new HorizontalScrollView(context);
            scroll.setHorizontalScrollBarEnabled(false);
            scroll.addView(strip);
            addView(scroll, new LayoutParams(-1, -2));
        }
    }

    // the content of the last build of the tiles; null builds them again at the next render
    private String renderedContent;

    void clearRenderedContent()
    {
        renderedContent = null;
    }

    /**
     * @brief Describes everything about a member that a row, a tile or the toolbar can show, so that two
     *        snapshots can be compared by content (every server update creates new Member objects).
     *
     * @param member the member
     *
     * @return the description
     */
    static String describe(Member member)
    {
        return member.id + "," + member.channelId + "," + member.name + "," + member.musicBot + "," + member.idle + "," + member.audioState + ","
            + member.groupIds + "," + member.channelGroupId + "," + member.countryCode + "," + (member.avatarDataUri == null ? 0 : member.avatarDataUri.hashCode()) + ","
            + member.streamingSong + "," + member.songName + ";";
    }

    void render(SessionSnapshot snapshot)
    {
        members.clear();
        serverGroups.clear();
        channelGroups.clear();
        if (snapshot != null && snapshot.connected)
        {
            localId = snapshot.clientId;
            icons.update(snapshot.icons);
            for (Group group : snapshot.groups)
            {
                (group.channelGroup ? channelGroups : serverGroups).put(group.id, group);
            }
            for (Member member : snapshot.members)
            {
                if (!currentChannelOnly || member.channelId == snapshot.channelId)
                {
                    members.add(member);
                }
            }
        }
        setVisibility(members.isEmpty() ? GONE : VISIBLE);

        // compare the content
        StringBuilder content = new StringBuilder().append(localId).append('|').append(serverGroups.keySet()).append(channelGroups.keySet())
            .append(settings.getBoolean("lemon_audio_availability")).append('|');
        for (Member member : members)
        {
            content.append(describe(member));
        }
        if (content.toString().equals(renderedContent))
        {
            return;
        }
        renderedContent = content.toString();
        refresh();
    }

    @Override
    protected void onDetachedFromWindow()
    {
        icons.close();
        super.onDetachedFromWindow();
    }

    private void refresh()
    {
        if (vertical)
        {
            adapter.notifyDataSetChanged();
            return;
        }
        strip.removeAllViews();
        for (int index = 0; index < Math.min(64, members.size()); index++)
        {
            strip.addView(tile(members.get(index)), new LinearLayout.LayoutParams(dp(76), -2));
        }
        if (members.size() > 64)
        {
            TextView all = label("All people");
            all.setOnClickListener(view -> actions.allPeople());
            strip.addView(all, new LinearLayout.LayoutParams(dp(76), -2));
        }
    }

    private View tile(Member member)
    {
        boolean compact = palette.theme.tree;
        LinearLayout tile = new LinearLayout(getContext());
        tile.setOrientation(compact ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        tile.setGravity(compact ? Gravity.CENTER_VERTICAL : Gravity.CENTER);
        tile.setMinimumHeight(dp(34));
        tile.setPadding(dp(compact ? 1 : 4), dp(compact ? 4 : 5), dp(4), dp(compact ? 4 : 5));
        boolean audioVisible = settings.getBoolean("lemon_audio_availability") || member.audioState == 1 || member.musicBot;
        if (compact)
        {
            ImageView audio = audioIndicator(member);
            audio.setVisibility(audioVisible ? VISIBLE : INVISIBLE);
            LinearLayout.LayoutParams position = new LinearLayout.LayoutParams(dp(18), dp(18));
            position.rightMargin = dp(4);
            tile.addView(audio, position);
        }
        else
        {
            Drawable face = avatars.drawable(member);
            FrameLayout portrait = new FrameLayout(getContext());
            ImageView image = new ImageView(getContext());
            image.setImageDrawable(face != null ? face : new GlyphDrawable(member.musicBot ? "music" : "me", palette.secondary));
            portrait.addView(image, new FrameLayout.LayoutParams(dp(28), dp(28), Gravity.CENTER));
            if (audioVisible)
            {
                portrait.addView(audioIndicator(member), new FrameLayout.LayoutParams(dp(14), dp(14), Gravity.BOTTOM | Gravity.RIGHT));
            }
            tile.addView(portrait, new LinearLayout.LayoutParams(dp(38), dp(30)));
        }
        LinearLayout labels = compact ? new LinearLayout(getContext()) : tile;
        if (compact)
        {
            labels.setOrientation(LinearLayout.VERTICAL);
            tile.addView(labels, new LinearLayout.LayoutParams(0, -2, 1));
        }
        TextView name = label(member.id == localId ? "You" : member.name);
        name.setMaxLines(compact ? 2 : 1);
        name.setTextSize(compact ? 13 : 11);
        name.setGravity(compact ? Gravity.START : Gravity.CENTER);
        name.setTextColor(compact ? palette.text : palette.secondary);
        name.setEllipsize(android.text.TextUtils.TruncateAt.END);
        labels.addView(name, new LinearLayout.LayoutParams(-1, -2));
        if (member.streamingSong)
        {
            TextView song = label(member.songName);
            song.setSingleLine(true);
            song.setEllipsize(android.text.TextUtils.TruncateAt.MARQUEE);
            song.setMarqueeRepeatLimit(-1);
            song.setSelected(true);
            song.setFocusable(false);
            song.setContentDescription("Playing " + member.songName);
            labels.addView(song, new LinearLayout.LayoutParams(-1, -2));
        }
        LinearLayout groups = new LinearLayout(getContext());
        groups.setGravity(compact ? Gravity.START : Gravity.CENTER);
        ArrayList<Group> assigned = new ArrayList<>();
        assigned.add(channelGroups.get(member.channelGroupId));
        for (int id : member.groupIds)
        {
            assigned.add(serverGroups.get(id));
        }
        StringBuilder description = new StringBuilder(member.name + ". " + ChannelTree.audioDescription(member));
        if (member.streamingSong)
        {
            description.append(". Playing ").append(member.songName);
        }
        for (Group group : assigned)
        {
            if (group == null)
            {
                continue;
            }
            description.append(". ").append(group.name);
            if (groups.getChildCount() >= 3 || settings.getBoolean(group.channelGroup ? "lemon_hide_channel_group_icons" : "lemon_hide_server_group_icons"))
            {
                continue;
            }
            Drawable icon = group.channelGroup && group.iconId == -2 ? getContext().getDrawable(R.drawable.ic_channel_admin) : icons.drawable(group.iconId);
            if (icon != null)
            {
                ImageView badge = new ImageView(getContext());
                badge.setImageDrawable(icon);
                badge.setContentDescription(group.name);
                badge.setTooltipText(group.name);
                groups.addView(badge, new LinearLayout.LayoutParams(dp(14), dp(14)));
            }
        }

        // show group pictures
        if (groups.getChildCount() > 0 && !compact)
        {
            labels.addView(groups);
        }
        tile.setContentDescription(description);
        tile.setOnLongClickListener(view ->
        {
            actions.info(member);
            return true;
        });
        if (!member.musicBot && member.id != localId)
        {
            tile.setBackground(palette.interactive(android.graphics.Color.TRANSPARENT, dp(4)));
            tile.setFocusable(true);
            tile.setOnClickListener(view -> actions.open(member));
        }
        return tile;
    }

    private ImageView audioIndicator(Member member)
    {
        ImageView audio = new ImageView(getContext());
        String glyph = member.musicBot ? "music" : member.audioState == 4 ? "speaker-off" : member.audioState == 3 || member.idle ? "mic-off" : member.audioState == 1 ? "mic-active" : "mic";
        audio.setImageDrawable(new GlyphDrawable(glyph, palette.text));
        audio.setBackground(palette.rounded(palette.theme.tree ? android.graphics.Color.TRANSPARENT : palette.surface, dp(9)));
        audio.setPadding(dp(2), dp(2), dp(2), dp(2));
        audio.setContentDescription(ChannelTree.audioDescription(member));
        return audio;
    }

    private TextView label(String value)
    {
        TextView text = new TextView(getContext());
        text.setText(value);
        text.setTextSize(11);
        text.setTextColor(palette.secondary);
        text.setGravity(Gravity.CENTER);
        return text;
    }

    private int dp(int value)
    {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
