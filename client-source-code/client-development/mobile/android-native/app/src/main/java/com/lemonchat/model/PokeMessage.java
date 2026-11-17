package com.lemonchat.model;

/** bounded attention message from a known person */
public final class PokeMessage
{
    public final long connectionId;
    public final int senderId;
    public final String senderName;
    public final String senderPublicKey;
    public final String conversationId;
    public final String text;
    public final long timestamp;

    public PokeMessage(long connectionId, Member sender, String text, long timestamp)
    {
        this.connectionId = connectionId;
        this.senderId = sender.id;
        this.senderName = sender.name;
        this.senderPublicKey = sender.publicKey;
        this.conversationId = sender.conversationId;
        this.text = text;
        this.timestamp = timestamp;
    }
}
