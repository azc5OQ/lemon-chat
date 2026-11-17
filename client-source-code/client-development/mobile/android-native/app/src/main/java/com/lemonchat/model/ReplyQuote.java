package com.lemonchat.model;

/** quoted content carried inside the encrypted message */
public final class ReplyQuote
{
    public final String messageId;
    public final String authorPublicKey;
    public final String authorName;
    public final String text;

    public ReplyQuote(String messageId, String authorPublicKey, String authorName, String text)
    {
        this.messageId = messageId;
        this.authorPublicKey = authorPublicKey;
        this.authorName = authorName;
        this.text = text;
    }
}
