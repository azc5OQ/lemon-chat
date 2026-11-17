package com.lemonchat.model;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** immutable message or delivery update for one conversation */
public final class ChatMessage
{
    public final long id;
    public final String conversationId;
    public final int senderId;
    public final String senderName;
    public final String text;
    public final boolean outgoing;
    public final long timestamp;
    public final String delivery;
    public final String authorPublicKey;
    public final long serverId;
    public final long connectionId;
    public final String reactionId;
    public final ReplyQuote reply;
    public final List<Reaction> reactions;
    public final Attachment attachment;
    public final boolean seen;
    public final String fontFamily;
    public final int fontSize;
    public final String fontColor;

    public ChatMessage(long id, String conversationId, int senderId, String senderName, String text, boolean outgoing, long timestamp, String delivery, String authorPublicKey, long serverId)
    {
        this(id, conversationId, senderId, senderName, text, outgoing, timestamp, delivery, authorPublicKey, serverId, -1, "", null, Collections.emptyList(), null, false);
    }

    public ChatMessage(long id, String conversationId, int senderId, String senderName, String text, boolean outgoing, long timestamp, String delivery, String authorPublicKey, long serverId, long connectionId, String reactionId, ReplyQuote reply, List<Reaction> reactions, Attachment attachment, boolean seen)
    {
        this(id, conversationId, senderId, senderName, text, outgoing, timestamp, delivery, authorPublicKey, serverId, connectionId, reactionId, reply, reactions, attachment, seen, "Arial", 12, "#000000");
    }

    public ChatMessage(long id, String conversationId, int senderId, String senderName, String text, boolean outgoing, long timestamp, String delivery, String authorPublicKey, long serverId, long connectionId, String reactionId, ReplyQuote reply, List<Reaction> reactions, Attachment attachment, boolean seen, String fontFamily, int fontSize, String fontColor)
    {
        this.fontFamily = fontFamily != null && fontFamily.length() <= 100 ? fontFamily : "Arial";
        this.fontSize = Math.max(8, Math.min(72, fontSize));
        this.fontColor = fontColor != null && fontColor.matches("#[0-9a-fA-F]{6}") ? fontColor : "#000000";
        this.connectionId = connectionId;
        this.reactionId = reactionId;
        this.reply = reply;
        this.reactions = Collections.unmodifiableList(new ArrayList<>(reactions));
        this.attachment = attachment;
        this.seen = seen;
        this.id = id;
        this.conversationId = conversationId;
        this.senderId = senderId;
        this.senderName = senderName;
        this.text = text;
        this.outgoing = outgoing;
        this.timestamp = timestamp;
        this.delivery = delivery;
        this.authorPublicKey = authorPublicKey;
        this.serverId = serverId;
    }

    /**
     * copies this message after a delivery result
     *
     * @param value user facing delivery state
     * @param confirmedId server message identifier
     * @return updated immutable message
     */
    public ChatMessage withDelivery(String value, long confirmedId)
    {
        return new ChatMessage(id, conversationId, senderId, senderName, text, outgoing, timestamp, value, authorPublicKey, confirmedId, connectionId, reactionId, reply, reactions, attachment, seen, fontFamily, fontSize, fontColor);
    }

    public ChatMessage withReactions(List<Reaction> value)
    {
        return new ChatMessage(id, conversationId, senderId, senderName, text, outgoing, timestamp, delivery, authorPublicKey, serverId, connectionId, reactionId, reply, value, attachment, seen, fontFamily, fontSize, fontColor);
    }

    public ChatMessage withSeen()
    {
        return new ChatMessage(id, conversationId, senderId, senderName, text, outgoing, timestamp, delivery, authorPublicKey, serverId, connectionId, reactionId, reply, reactions, attachment, true, fontFamily, fontSize, fontColor);
    }

    public ChatMessage withText(String value)
    {
        return new ChatMessage(id, conversationId, senderId, senderName, value, outgoing, timestamp, delivery, authorPublicKey, serverId, connectionId, reactionId, reply, reactions, attachment, seen, fontFamily, fontSize, fontColor);
    }

    public ChatMessage withAttachment(Attachment value)
    {
        return new ChatMessage(id, conversationId, senderId, senderName, text, outgoing, timestamp, delivery, authorPublicKey, serverId, connectionId, reactionId, reply, reactions, value, seen, fontFamily, fontSize, fontColor);
    }

    /** @return text that identifies this message in a quoted reply */
    public String quoteText()
    {
        if (!text.isEmpty())
        {
            return text;
        }
        if (attachment != null)
        {
            return (attachment.picture ? "Picture: " : "File: ") + attachment.name;
        }
        return "Original message";
    }
}
