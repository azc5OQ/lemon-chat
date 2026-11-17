package com.lemonchat.model;

/** one expiring invitation into the caller's channel */
public final class CallInvitation
{
    public final String callId;
    public final long connectionId;
    public final long expiresAt;
    public final long expiresAtNanos;
    public final int callerId;
    public final int channelId;
    public final String callerName;
    public final String callerPublicKey;

    public CallInvitation(String callId, long connectionId, long expiresAt, long expiresAtNanos, int callerId, int channelId, String callerName, String callerPublicKey)
    {
        this.callId = callId;
        this.connectionId = connectionId;
        this.expiresAt = expiresAt;
        this.expiresAtNanos = expiresAtNanos;
        this.callerId = callerId;
        this.channelId = channelId;
        this.callerName = callerName;
        this.callerPublicKey = callerPublicKey;
    }

    /** @return whether this invitation has reached its monotonic deadline */
    public boolean expired()
    {
        return System.nanoTime() - expiresAtNanos >= 0;
    }
}
