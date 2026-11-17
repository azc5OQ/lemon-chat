package com.lemonchat.ui;

import android.app.Instrumentation;
import android.content.Context;
import android.text.TextUtils;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import com.lemonchat.model.Channel;
import com.lemonchat.model.Member;
import com.lemonchat.model.SessionSnapshot;
import com.lemonchat.settings.LocalSettings;
import java.util.Collections;
import java.util.concurrent.atomic.AtomicReference;

/** checks scrolling titles and recycled channel rows */
public final class MusicTitleChecks
{
    private int checks;

    public static int verify(Instrumentation instrumentation) throws Exception
    {
        MusicTitleChecks suite = new MusicTitleChecks();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        instrumentation.runOnMainSync(() ->
        {
            try
            {
                suite.views(instrumentation.getTargetContext());
            }
            catch (Throwable error)
            {
                failure.set(error);
            }
        });
        if (failure.get() != null)
        {
            throw new AssertionError("Music title view check failed", failure.get());
        }
        return suite.checks;
    }

    private void views(Context context)
    {
        Channel channel = new Channel(0, -1, "Music", "", false, true);
        Member bot = new Member(2, 0, "Lemon Radio", "", "bot-key", true, false, 1, 0, "").withSong(true, "Artist and a long current song title.mp3");
        ChannelTreeView tree = new ChannelTreeView(context, new Palette(false), new ChannelTreeView.Actions()
        {
            @Override
            public void onJoin(Channel value)
            {
            }

            @Override
            public void onOpenMember(Member member)
            {
                throw new AssertionError("A bot cannot open a direct conversation");
            }

            @Override
            public void onChannelMembers(Channel value)
            {
            }

            @Override
            public void onMemberInfo(Member member)
            {
            }
        });
        tree.preferences(LocalSettings.validate(Collections.emptyMap()), null);
        tree.render(state(channel, bot));
        View row = tree.getAdapter().getView(1, null, tree);
        TextView title = find(row, bot.songName);
        check(title != null && title.getVisibility() == View.VISIBLE, "playing bot has a visible song title");
        check(title.getEllipsize() == TextUtils.TruncateAt.MARQUEE && title.isSelected(), "long titles use native marquee");
        check(title.getMarqueeRepeatLimit() == -1 && title.getMaxLines() == 1, "marquee repeats in one compact line");
        check(!row.hasOnClickListeners(), "music bot row has no direct chat click");
        check(row.getContentDescription().toString().contains(bot.songName), "accessibility includes the whole title");
        tree.render(state(channel, bot));
        row = tree.getAdapter().getView(1, row, tree);
        check(find(row, bot.songName) == title && title.isSelected(), "unchanged state retains the marquee view");
        Member next = bot.withSong(true, "Next song.mp3");
        tree.render(state(channel, next));
        row = tree.getAdapter().getView(1, row, tree);
        check(find(row, next.songName) == title, "track change updates the recycled label");
        tree.render(state(channel, bot.withSong(false, "")));
        row = tree.getAdapter().getView(1, row, tree);
        check(title.getVisibility() == View.GONE && !title.isSelected() && title.getText().length() == 0, "stopping clears and hides the marquee");
        tree.render(state(channel, next));
        row = tree.getAdapter().getView(1, row, tree);
        check(title.getVisibility() == View.VISIBLE && title.isSelected(), "resuming starts the label again");
        row = tree.getAdapter().getView(0, row, tree);
        check(title.getVisibility() == View.GONE && title.getText().length() == 0, "recycling into a channel clears old song text");
        tree.render(null);
        AvatarImages avatars = new AvatarImages(context.getResources(), () ->
        {
        });
        ConversationMembers strip = new ConversationMembers(context, new Palette(NativeTheme.find("bluebell"), false), LocalSettings.validate(Collections.emptyMap()), avatars, new ConversationMembers.Actions()
        {
            @Override
            public void open(Member member)
            {
                throw new AssertionError("A bot cannot open a direct conversation");
            }

            @Override
            public void info(Member member)
            {
            }

            @Override
            public void allPeople()
            {
            }
        });
        try
        {
            strip.render(state(channel, bot));
            TextView stripTitle = find(strip, bot.songName);
            check(stripTitle != null && stripTitle.isSelected() && stripTitle.getEllipsize() == TextUtils.TruncateAt.MARQUEE, "conversation strip also shows a scrolling song title");
            check(!((View)stripTitle.getParent()).hasOnClickListeners(), "bot tile in the strip cannot open chat");
            strip.render(state(channel, next));
            check(find(strip, next.songName) != null && find(strip, bot.songName) == null, "conversation strip replaces old track text");
            strip.render(state(channel, bot.withSong(false, "")));
            check(find(strip, next.songName) == null, "conversation strip removes a stopped song");
            strip.render(null);
        }
        finally
        {
            avatars.close();
        }
    }

    private static SessionSnapshot state(Channel channel, Member member)
    {
        return new SessionSnapshot(SessionSnapshot.Status.CONNECTED, "", 1, 0, "Listener", true, Collections.singletonList(channel), Collections.singletonList(member), 1);
    }

    private static TextView find(View view, String text)
    {
        if (view instanceof TextView && text.contentEquals(((TextView)view).getText()))
        {
            return (TextView)view;
        }
        if (view instanceof ViewGroup)
        {
            ViewGroup parent = (ViewGroup)view;
            for (int index = 0; index < parent.getChildCount(); index++)
            {
                TextView found = find(parent.getChildAt(index), text);
                if (found != null)
                {
                    return found;
                }
            }
        }
        return null;
    }

    private void check(boolean value, String description)
    {
        if (!value)
        {
            throw new AssertionError(description);
        }
        checks++;
    }
}
