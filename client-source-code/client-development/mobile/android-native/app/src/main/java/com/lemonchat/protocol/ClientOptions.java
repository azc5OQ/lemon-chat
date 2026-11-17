package com.lemonchat.protocol;

/** immutable local choices applied without replacing the connection */
public final class ClientOptions
{
    public final boolean reactions;
    public final boolean replies;
    public final boolean sendReceipts;
    public final boolean showReceipts;
    public final boolean blockDirectPictures;
    public final boolean blockDirectFiles;
    public final boolean blockChannelPictures;
    public final boolean blockChannelFiles;
    public final String audioTransport;

    public ClientOptions(boolean reactions, boolean replies, boolean sendReceipts, boolean showReceipts, boolean blockDirectPictures, boolean blockDirectFiles, boolean blockChannelPictures, boolean blockChannelFiles, String audioTransport)
    {
        this.reactions = reactions;
        this.replies = replies;
        this.sendReceipts = sendReceipts;
        this.showReceipts = showReceipts;
        this.blockDirectPictures = blockDirectPictures;
        this.blockDirectFiles = blockDirectFiles;
        this.blockChannelPictures = blockChannelPictures;
        this.blockChannelFiles = blockChannelFiles;
        this.audioTransport = "webrtc".equals(audioTransport) ? "webrtc" : "websocket";
    }

    public static ClientOptions defaults()
    {
        return new ClientOptions(false, true, true, true, false, false, false, false, "webrtc");
    }
}
