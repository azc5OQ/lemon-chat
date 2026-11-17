package com.lemonchat.ui;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.text.Spannable;
import android.text.SpannableString;
import android.text.style.BackgroundColorSpan;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import com.lemonchat.model.Attachment;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.model.Member;
import com.lemonchat.model.Reaction;
import com.lemonchat.model.ServerEmoji;
import com.lemonchat.settings.LocalSettings;
import java.text.DateFormat;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** renders authenticated message content using the saved display choices */
final class ChatMessageViews
{
    interface Actions
    {
        void onMessageActions(ChatMessage message);
        void onReaction(ChatMessage message, String emojiId);
        void onOpenAttachment(Attachment attachment);
    }

    private final Context context;
    private final Palette palette;
    private final LocalSettings settings;
    private final AvatarImages avatars;
    private final AttachmentImages pictures;
    private final EmojiImages emojiImages;
    private final Actions actions;
    private final float scale;

    ChatMessageViews(Context context, Palette palette, LocalSettings settings, AvatarImages avatars, AttachmentImages pictures, EmojiImages emojiImages, Actions actions)
    {
        this.context = context;
        this.palette = palette;
        this.settings = settings;
        this.avatars = avatars;
        this.pictures = pictures;
        this.emojiImages = emojiImages;
        this.actions = actions;
        scale = settings.getInt("lemon_chat_display_scale") / 100f;
    }

    // hues (degrees) of the sender colours used when the theme's accent colour cannot be used for names
    private static final float[] SENDER_HUES = {210, 145, 25, 275, 355, 180, 320, 45};

    /**
     * @brief Chooses the colour of a sender's name above a message, so that the name differs from the message
     *        text (text colour) and from the time (secondary colour). The theme's accent colour is used when it
     *        is readable on the background (contrast at least 3:1, the minimum for bold text) and is a colour, not
     *        a grey (saturation at least 0.3; Pocket and Mono use greys). Otherwise the name gets a colour chosen by the sender's
     *        name from SENDER_HUES, made darker on a light background or lighter on a dark one until it reaches
     *        a contrast of 4.5:1; the same sender always gets the same colour.
     *
     * @param sender the sender's name
     * @param background colour behind the name
     *
     * @return the colour of the name
     */
    private int nameColor(String sender, int background)
    {
        // theme accent
        float[] accent = new float[3];
        android.graphics.Color.colorToHSV(palette.accent, accent);
        if (contrast(palette.accent, background) >= 3.0 && accent[1] >= 0.3f)
        {
            return palette.accent;
        }

        // colour of the sender
        boolean isDarkBackground = luminance(background) < 0.3;
        float hue = SENDER_HUES[Math.floorMod(sender == null ? 0 : sender.hashCode(), SENDER_HUES.length)];
        float value = isDarkBackground ? 0.7f : 0.75f;
        int color = android.graphics.Color.HSVToColor(new float[]{hue, isDarkBackground ? 0.45f : 0.8f, value});

        // readable on the background
        for (int step = 0; step < 12 && contrast(color, background) < 4.5; step++)
        {
            value = isDarkBackground ? Math.min(1f, value + 0.05f) : Math.max(0f, value - 0.06f);
            color = android.graphics.Color.HSVToColor(new float[]{hue, isDarkBackground ? 0.45f : 0.8f, value});
        }
        return color;
    }

    /**
     * @brief Computes the contrast ratio of two colours as defined by WCAG 2: (lighter luminance + 0.05) divided
     *        by (darker luminance + 0.05), from 1 (same) to 21 (black on white).
     *
     * @param first one colour
     * @param second the other colour
     *
     * @return the contrast ratio
     */
    private static double contrast(int first, int second)
    {
        double a = luminance(first) + 0.05;
        double b = luminance(second) + 0.05;
        return Math.max(a, b) / Math.min(a, b);
    }

    private static double luminance(int color)
    {
        double[] channels = {android.graphics.Color.red(color) / 255.0, android.graphics.Color.green(color) / 255.0, android.graphics.Color.blue(color) / 255.0};
        for (int index = 0; index < channels.length; index++)
        {
            channels[index] = channels[index] <= 0.03928 ? channels[index] / 12.92 : Math.pow((channels[index] + 0.055) / 1.055, 2.4);
        }
        return 0.2126 * channels[0] + 0.7152 * channels[1] + 0.0722 * channels[2];
    }

    View message(ChatMessage message, boolean privateChat, Member author, List<ServerEmoji> emojis, String search)
    {
        int foreground = message.outgoing ? palette.outgoingText : palette.text;
        int secondary = message.outgoing ? palette.outgoingSecondary : palette.secondary;
        boolean desktopRow = palette.theme.tree;
        boolean showAvatar = settings.getBoolean("lemon_show_message_avatars") && (desktopRow || !message.outgoing);
        LinearLayout row = new LinearLayout(context);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity((message.outgoing && !desktopRow ? Gravity.RIGHT : Gravity.LEFT) | (desktopRow ? Gravity.TOP : Gravity.BOTTOM));
        row.setPadding(0, dp(desktopRow ? 4 : 2), 0, dp(desktopRow ? 4 : 3));

        // tree theme sizes
        boolean isMobile2 = palette.theme.id.equals("mobile-2");
        int avatarSize = !desktopRow ? 32 : isMobile2 ? 20 : 14;
        if (showAvatar)
        {
            String letters = desktopRow ? twoLetters(message.senderName) : initials(message.senderName);
            TextView face = text(letters, desktopRow ? Math.round(avatarSize * 0.4f) : 11, desktopRow && !isMobile2 ? 0xFFFFFFFF : palette.accentText, true);
            face.setGravity(Gravity.CENTER);
            Drawable avatar = avatars.drawable(author);
            int circle = desktopRow && !isMobile2 ? 0xFF6E7D8E : palette.accent;
            face.setBackground(avatar == null ? palette.rounded(circle, dp(avatarSize / 2f)) : avatar);
            if (avatar != null)
            {
                face.setText("");
            }
            LinearLayout.LayoutParams position = new LinearLayout.LayoutParams(dp(avatarSize), dp(avatarSize));
            position.setMarginEnd(dp(8));

            // vertical position
            position.topMargin = !desktopRow ? 0 : dp(isMobile2 ? 6 : 1);
            row.addView(face, position);
            face.setContentDescription(message.senderName + " avatar");
            face.setOnLongClickListener(view ->
            {
                actions.onMessageActions(message);
                return true;
            });
        }
        LinearLayout bubble = new LinearLayout(context);
        bubble.setOrientation(LinearLayout.VERTICAL);
        bubble.setPadding(dp(desktopRow ? 0 : 9), dp(desktopRow ? 0 : 5), dp(9), dp(desktopRow ? 0 : 5));
        bubble.setBackground(palette.messageSurface(message.outgoing, dp(palette.theme.radius)));
        int screenWidth = context.getResources().getDisplayMetrics().widthPixels;
        int screenDp = Math.round(screenWidth / context.getResources().getDisplayMetrics().density);
        int reserved = dp(34 + (palette.theme.sideStrip(settings.getBoolean("lemon_strip_vertical")) ? palette.theme.sideWidth(screenDp) : 0) + (showAvatar ? avatarSize + 8 : 0));
        int available = Math.max(dp(80), palette.theme.tree ? screenWidth - reserved : Math.min((int) (screenWidth * 0.72), screenWidth - reserved));
        String time = DateFormat.getTimeInstance(DateFormat.SHORT).format(new java.util.Date(message.timestamp));
        String receipt = message.outgoing && message.seen && settings.getBoolean("lemon_seen_indicator") ? "  ·  Read" : "";
        TextView stamp = text(time + (message.outgoing ? "  ·  " + message.delivery : "") + receipt, desktopRow ? 11 : 10, secondary, false);
        stamp.setMaxWidth(available);
        if (desktopRow)
        {
            // name beside time
            LinearLayout heading = new LinearLayout(context);
            heading.setOrientation(LinearLayout.HORIZONTAL);
            heading.setGravity(Gravity.CENTER_VERTICAL);
            heading.setPadding(0, 0, 0, dp(2));
            TextView name = text(message.senderName, 13, nameColor(message.senderName, palette.background), true);
            name.setSingleLine(true);
            name.setEllipsize(android.text.TextUtils.TruncateAt.END);
            name.setMaxWidth(Math.max(dp(48), available - dp(80)));
            heading.addView(name, new LinearLayout.LayoutParams(-2, -2));
            stamp.setSingleLine(true);
            stamp.setPadding(dp(8), 0, 0, 0);
            heading.addView(stamp, new LinearLayout.LayoutParams(-2, -2));
            bubble.addView(heading, new LinearLayout.LayoutParams(-1, -2));
        }
        else if (!message.outgoing && !privateChat)
        {
            TextView name = text(message.senderName, 13, nameColor(message.senderName, palette.incoming), true);
            name.setMaxWidth(available);
            name.setPadding(0, 0, 0, dp(2));
            bubble.addView(name);
        }
        if (message.reply != null && settings.getBoolean("lemon_chat_replies"))
        {
            LinearLayout quote = new LinearLayout(context);
            quote.setOrientation(LinearLayout.VERTICAL);
            quote.setPadding(dp(9), dp(7), dp(9), dp(7));
            android.graphics.drawable.GradientDrawable background = palette.rounded(palette.background, dp(5));
            background.setStroke(dp(1), palette.border);
            quote.setBackground(background);
            int quoteWidth = Math.max(dp(48), available - dp(18));
            String authorName = message.reply.authorName.isEmpty() ? "Unknown user" : message.reply.authorName;
            TextView quotedAuthor = text("Reply to " + authorName, 12, palette.text, true);
            quotedAuthor.setMaxWidth(quoteWidth);
            quotedAuthor.setSingleLine(true);
            quotedAuthor.setEllipsize(android.text.TextUtils.TruncateAt.END);
            quotedAuthor.setPadding(0, 0, 0, dp(3));
            quote.addView(quotedAuthor);
            TextView excerpt = text(message.reply.text.isEmpty() ? "Original message" : message.reply.text, 13, palette.text, false);
            excerpt.setMaxLines(4);
            excerpt.setMaxWidth(quoteWidth);
            excerpt.setEllipsize(android.text.TextUtils.TruncateAt.END);
            quote.addView(excerpt);
            LinearLayout.LayoutParams position = new LinearLayout.LayoutParams(-2, -2);
            position.bottomMargin = dp(7);
            bubble.addView(quote, position);
        }
        if (message.attachment != null)
        {
            attachment(bubble, message.attachment, available, foreground);
        }
        if (!message.text.isEmpty())
        {
            TextView body = text(message.text, desktopRow ? 13 : palette.theme.messageTextSize(), foreground, false);
            body.setText(highlight(message.text, search));
            body.setTextIsSelectable(true);
            body.setOnLongClickListener(view ->
            {
                actions.onMessageActions(message);
                return true;
            });
            body.setMaxWidth(available);
            body.setLineSpacing(0, 1);
            if (!settings.getBoolean("lemon_use_theme_chat_formatting"))
            {
                applyFormatting(body, message);
            }
            bubble.addView(body);
        }
        if (settings.getBoolean("lemon_chat_reactions") && !message.reactions.isEmpty())
        {
            android.widget.HorizontalScrollView scroll = new android.widget.HorizontalScrollView(context);
            scroll.setHorizontalScrollBarEnabled(false);
            LinearLayout reactions = new LinearLayout(context);
            reactions.setOrientation(LinearLayout.HORIZONTAL);
            for (Reaction reaction : message.reactions)
            {
                String label = reaction.emojiName;
                for (ServerEmoji emoji : emojis)
                {
                    if (emoji.id.equals(reaction.emojiId) && !emoji.unicode.isEmpty())
                    {
                        label = emoji.unicode;
                        break;
                    }
                }
                Button item = new Button(context);
                item.setAllCaps(false);
                item.setText(label + " " + reaction.authorKeys.size());
                Drawable image = emojiImages.drawable(reaction.emojiId);
                if (image != null)
                {
                    image.setBounds(0, 0, dp(22), dp(22));
                    item.setCompoundDrawables(image, null, null, null);
                    item.setText(Integer.toString(reaction.authorKeys.size()));
                    item.setContentDescription(label + ", " + reaction.authorKeys.size() + " reactions");
                }
                item.setTextSize(12 * scale);
                item.setTextColor(reaction.mine ? palette.accentText : palette.secondary);
                item.setBackground(palette.rounded(reaction.mine ? palette.accent : palette.background, dp(10)));
                item.setOnClickListener(view -> actions.onReaction(message, reaction.emojiId));
                LinearLayout.LayoutParams position = new LinearLayout.LayoutParams(-2, dp(36));
                position.setMarginEnd(dp(5));
                reactions.addView(item, position);
            }
            scroll.addView(reactions);
            bubble.addView(scroll, new LinearLayout.LayoutParams(Math.min(available, dp(250)), -2));
        }
        if (!desktopRow)
        {
            stamp.setGravity(Gravity.RIGHT);
            stamp.setPadding(0, dp(2), 0, 0);
            bubble.addView(stamp);
        }
        bubble.setTooltipText("Hold for message actions");
        bubble.setOnLongClickListener(view ->
        {
            actions.onMessageActions(message);
            return true;
        });
        row.addView(bubble, desktopRow ? new LinearLayout.LayoutParams(0, -2, 1) : new LinearLayout.LayoutParams(-2, -2));
        return row;
    }

    private void attachment(LinearLayout bubble, Attachment attachment, int available, int foreground)
    {
        Drawable picture = attachment.picture ? pictures.drawable(attachment) : null;
        if (picture != null)
        {
            ImageView preview = new ImageView(context);
            preview.setImageDrawable(picture);
            preview.setAdjustViewBounds(true);
            preview.setScaleType(ImageView.ScaleType.FIT_CENTER);
            int width = Math.min(available, Math.round(settings.getInt("lemon_chat_image_max_width") * scale));
            int height = Math.round(settings.getInt("lemon_chat_image_max_height") * scale);
            preview.setMaxWidth(width);
            preview.setMaxHeight(Math.min(height, context.getResources().getDisplayMetrics().heightPixels));
            preview.setContentDescription(attachment.name);
            preview.setOnClickListener(view -> actions.onOpenAttachment(attachment));
            bubble.addView(preview, new LinearLayout.LayoutParams(-2, -2));
        }
        TextView file = text(attachment.name + "\n" + android.text.format.Formatter.formatShortFileSize(context, attachment.size), 14, foreground, true);
        file.setMaxWidth(available);
        file.setPadding(0, dp(6), 0, dp(4));
        file.setOnClickListener(view -> actions.onOpenAttachment(attachment));
        bubble.addView(file);
    }

    private void applyFormatting(TextView view, ChatMessage message)
    {
        if (message.fontSize >= 8 && message.fontSize <= 72)
        {
            view.setTextSize(message.fontSize * scale);
        }
        String family = message.fontFamily.toLowerCase(Locale.ROOT);
        if (family.equals("monospace") || family.equals("courier") || family.equals("courier new"))
        {
            view.setTypeface(Typeface.MONOSPACE);
        }
        else if (family.equals("serif") || family.equals("times") || family.equals("times new roman"))
        {
            view.setTypeface(Typeface.SERIF);
        }
        if (message.fontColor.matches("#[0-9a-fA-F]{6}"))
        {
            int selected = Color.parseColor(message.fontColor);
            int background = message.outgoing ? palette.outgoing : palette.incoming;
            float light = Math.max(Color.luminance(selected), Color.luminance(background));
            float dark = Math.min(Color.luminance(selected), Color.luminance(background));
            view.setTextColor((light + 0.05f) / (dark + 0.05f) >= 3 ? selected : message.outgoing ? palette.outgoingText : palette.text);
        }
    }

    private CharSequence highlight(String text, String query)
    {
        if (query == null || query.isEmpty())
        {
            return text;
        }
        SpannableString result = new SpannableString(text);
        Matcher matches = Pattern.compile(Pattern.quote(query), Pattern.CASE_INSENSITIVE | Pattern.UNICODE_CASE).matcher(text);
        int count = 0;
        while (count++ < 100 && matches.find())
        {
            result.setSpan(new BackgroundColorSpan(palette.accent), matches.start(), matches.end(), Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
        }
        return result;
    }

    private TextView text(String value, int size, int color, boolean bold)
    {
        TextView view = new TextView(context);
        view.setText(value);
        view.setTextSize(size * scale);
        view.setIncludeFontPadding(false);
        view.setTextColor(color);
        view.setTypeface(Typeface.create(bold ? "sans-serif-medium" : "sans-serif", Typeface.NORMAL));
        return view;
    }

    private String initials(String name)
    {
        return name.isEmpty() ? "?" : new String(Character.toChars(name.codePointAt(0))).toUpperCase(Locale.ROOT);
    }

    /**
     * @brief the first two characters of a name in capitals, as the browser themes write them in an avatar
     *
     * @param name sender name
     *
     * @return two characters, one for a name of one character, "?" for an empty name
     */
    private String twoLetters(String name)
    {
        if (name.isEmpty())
        {
            return "?";
        }
        int end = name.offsetByCodePoints(0, Math.min(2, name.codePointCount(0, name.length())));
        return name.substring(0, end).toUpperCase(Locale.ROOT);
    }

    private int dp(float value)
    {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }
}
