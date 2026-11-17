package com.lemonchat.model;

/** shared limits for queued and retained messages */
public final class MessageBudget
{
    public static final int MAX_MESSAGES = 500;
    public static final int MAX_METADATA_BYTES = 2 * 1024 * 1024;
    public static final int MAX_ATTACHMENT_BYTES = 8 * 1024 * 1024;
    public static final int MAX_TOTAL_BYTES = MAX_METADATA_BYTES + MAX_ATTACHMENT_BYTES;

    private MessageBudget()
    {
    }

    /**
     * counts text and retained metadata without copying attachment content
     *
     * @param message message to account for
     * @return conservative metadata size in bytes
     */
    public static long metadataBytes(ChatMessage message)
    {
        long bytes = 256 + stringBytes(message.conversationId) + stringBytes(message.senderName) + stringBytes(message.text) + stringBytes(message.delivery) + stringBytes(message.authorPublicKey) + stringBytes(message.reactionId) + stringBytes(message.fontFamily) + stringBytes(message.fontColor);
        if (message.reply != null)
        {
            bytes += 64 + stringBytes(message.reply.messageId) + stringBytes(message.reply.authorPublicKey) + stringBytes(message.reply.authorName) + stringBytes(message.reply.text);
        }
        for (Reaction reaction : message.reactions)
        {
            bytes += 64 + stringBytes(reaction.emojiId) + stringBytes(reaction.emojiName);
            for (String author : reaction.authorKeys)
            {
                bytes += stringBytes(author);
            }
        }
        if (message.attachment != null)
        {
            bytes += 64 + stringBytes(message.attachment.id) + stringBytes(message.attachment.name) + stringBytes(message.attachment.mime);
        }
        return bytes;
    }

    /**
     * counts retained attachment content without copying it
     *
     * @param message message to account for
     * @return attachment size in bytes
     */
    public static long attachmentBytes(ChatMessage message)
    {
        return message.attachment == null ? 0 : message.attachment.size;
    }

    /**
     * counts the complete retained message
     *
     * @param message message to account for
     * @return combined metadata and attachment size in bytes
     */
    public static long messageBytes(ChatMessage message)
    {
        return metadataBytes(message) + attachmentBytes(message);
    }

    private static long stringBytes(String value)
    {
        return value == null ? 0 : 40 + value.length() * 2L;
    }
}
