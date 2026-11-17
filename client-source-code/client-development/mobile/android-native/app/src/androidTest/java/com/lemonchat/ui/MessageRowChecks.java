package com.lemonchat.ui;

import android.app.Instrumentation;
import android.content.Context;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;
import com.lemonchat.model.Attachment;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.model.ReplyQuote;
import com.lemonchat.settings.LocalSettings;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

/** checks actual message row geometry and attachment quotes */
public final class MessageRowChecks
{
    private int checks;

    public static int verify(Instrumentation instrumentation) throws Exception
    {
        MessageRowChecks test = new MessageRowChecks();
        Throwable[] failure = new Throwable[1];
        instrumentation.runOnMainSync(() ->
        {
            try
            {
                test.run(instrumentation.getTargetContext());
            }
            catch (Throwable error)
            {
                failure[0] = error;
            }
        });
        if (failure[0] != null)
        {
            throw new AssertionError("Message row checks failed", failure[0]);
        }
        return test.checks;
    }

    private void run(Context context)
    {
        Runnable changed = () ->
        {
        };
        AvatarImages avatars = new AvatarImages(context.getResources(), changed);
        AttachmentImages images = new AttachmentImages(context.getResources(), changed);
        EmojiImages emojis = new EmojiImages(context.getResources(), changed, id ->
        {
        });
        ChatMessageViews.Actions actions = new ChatMessageViews.Actions()
        {
            @Override
            public void onMessageActions(ChatMessage message)
            {
            }

            @Override
            public void onReaction(ChatMessage message, String emojiId)
            {
            }

            @Override
            public void onOpenAttachment(Attachment attachment)
            {
            }
        };
        try
        {
            LocalSettings defaults = LocalSettings.validate(Collections.emptyMap());
            check(defaults.getBoolean("lemon_show_message_avatars"), "avatars are enabled by default");
            Map<String, Object> disabled = new HashMap<>();
            disabled.put("lemon_show_message_avatars", false);
            for (String theme : new String[]{"default-mobile", "mobile-2"})
            {
                for (boolean outgoing : new boolean[]{false, true})
                {
                    ChatMessage message = message(outgoing, new ReplyQuote("b", "peer", "Quoted author", "Quoted line\nSecond quoted line"), null);
                    ChatMessageViews renderer = new ChatMessageViews(context, new Palette(NativeTheme.find(theme), true), defaults, avatars, images, emojis, actions);
                    ViewGroup row = (ViewGroup)renderer.message(message, false, null, Collections.emptyList(), "");
                    layout(context, row);
                    check(row.getChildCount() == 2, theme + " shows the " + (outgoing ? "outgoing" : "incoming") + " avatar");
                    ViewGroup bubble = (ViewGroup)row.getChildAt(1);
                    check(bubble.getChildAt(0) instanceof LinearLayout && ((LinearLayout)bubble.getChildAt(0)).getOrientation() == LinearLayout.HORIZONTAL, theme + " puts sender and time on one header row");
                    ViewGroup header = (ViewGroup)bubble.getChildAt(0);
                    check(header.getChildCount() == 2 && ((TextView)header.getChildAt(0)).getText().toString().equals("Maya") && !((TextView)header.getChildAt(1)).getText().toString().isEmpty(), theme + " header contains sender and timestamp");
                    ViewGroup quote = (ViewGroup)bubble.getChildAt(1);
                    TextView body = (TextView)bubble.getChildAt(2);
                    check(header.getBottom() <= quote.getTop() && quote.getBottom() < body.getTop(), theme + " separates the header, quote and message body");
                    check(quote.getMeasuredWidth() > 0 && quote.getRight() <= bubble.getWidth() - bubble.getPaddingRight() && body.getRight() <= bubble.getWidth() - bubble.getPaddingRight(), theme + " quote and body remain inside the measured row");
                    check(row.getChildAt(0).getTop() <= header.getTop() + bubble.getTop(), theme + " avatar stays beside the start of the message");
                    ViewGroup hidden = (ViewGroup)new ChatMessageViews(context, new Palette(NativeTheme.find(theme), true), LocalSettings.validate(disabled), avatars, images, emojis, actions).message(message, false, null, Collections.emptyList(), "");
                    check(hidden.getChildCount() == 1, theme + " respects an explicit avatar opt-out");
                }
            }
            ReplyQuote empty = new ReplyQuote("b", "peer", "", "");
            View row = new ChatMessageViews(context, new Palette(false), defaults, avatars, images, emojis, actions).message(message(false, empty, null), true, null, Collections.emptyList(), "");
            layout(context, row);
            check(find(row, "Original message") != null && find(row, "Reply to Unknown user") != null, "an empty received quote remains understandable");
            android.graphics.Bitmap bitmap = android.graphics.Bitmap.createBitmap(2, 2, android.graphics.Bitmap.Config.ARGB_8888);
            java.io.ByteArrayOutputStream png = new java.io.ByteArrayOutputStream();
            bitmap.eraseColor(android.graphics.Color.RED);
            bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, png);
            bitmap.recycle();
            Attachment picture = new Attachment("picture", "photo.png", "image/png", png.toByteArray(), true);
            row = new ChatMessageViews(context, new Palette(NativeTheme.find("mobile-2"), true), defaults, avatars, images, emojis, actions).message(message(false, new ReplyQuote("b", "peer", "Maya", "Picture: earlier.png"), picture), false, null, Collections.emptyList(), "");
            layout(context, row);
            check(find(row, "Picture: earlier.png") != null && find(row, "Message body") != null, "a picture message keeps its quoted description and body");
            ViewGroup pictureBubble = (ViewGroup)((ViewGroup)row).getChildAt(1);
            View picturePreview = pictureBubble.getChildAt(2);
            check(picturePreview instanceof android.widget.ImageView && picturePreview.getTop() > pictureBubble.getChildAt(1).getBottom(), "a valid picture preview starts below its quoted reply");
            for (String theme : new String[]{"mono", "mono-dark", "simple", "pocket", "bluebell"})
            {
                ViewGroup messenger = (ViewGroup)new ChatMessageViews(context, new Palette(NativeTheme.find(theme), true), defaults, avatars, images, emojis, actions).message(message(true, null, null), true, null, Collections.emptyList(), "");
                ViewGroup bubble = (ViewGroup)messenger.getChildAt(0);
                check(messenger.getChildCount() == 1 && bubble.getChildAt(bubble.getChildCount() - 1) instanceof TextView && ((TextView)bubble.getChildAt(bubble.getChildCount() - 1)).getText().toString().contains("Sent"), theme + " keeps its existing outgoing bubble and footer");
            }
        }
        finally
        {
            avatars.close();
            images.close();
            emojis.close();
        }
    }

    private ChatMessage message(boolean outgoing, ReplyQuote quote, Attachment attachment)
    {
        return new ChatMessage(1, "channel:0", 7, "Maya", "Message body", outgoing, 100000, "Sent", "peer", 2, 1, "a", quote, Collections.emptyList(), attachment, false);
    }

    private void layout(Context context, View view)
    {
        int width = Math.round(240 * context.getResources().getDisplayMetrics().density);
        view.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
        view.layout(0, 0, width, view.getMeasuredHeight());
    }

    private TextView find(View view, String value)
    {
        if (view instanceof TextView && ((TextView)view).getText().toString().equals(value))
        {
            return (TextView)view;
        }
        if (view instanceof ViewGroup)
        {
            ViewGroup group = (ViewGroup)view;
            for (int index = 0; index < group.getChildCount(); index++)
            {
                TextView found = find(group.getChildAt(index), value);
                if (found != null)
                {
                    return found;
                }
            }
        }
        return null;
    }

    private void check(boolean condition, String message)
    {
        if (!condition)
        {
            throw new AssertionError(message);
        }
        checks++;
    }
}
