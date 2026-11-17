package com.lemonchat.model;

/** server policy that determines which local choices can take effect */
public final class ClientCapabilities
{
    public final boolean blockDirectPictures;
    public final boolean blockDirectFiles;
    public final boolean blockChannelPictures;
    public final boolean blockChannelFiles;
    public final boolean avatarUpload;
    public final boolean fileUpload;
    public final boolean pictureUpload;
    public final int avatarMaxBytes;
    public final long fileMaxBytes;
    public final long pictureMaxBytes;

    public ClientCapabilities(boolean blockDirectPictures, boolean blockDirectFiles, boolean blockChannelPictures, boolean blockChannelFiles, boolean avatarUpload, boolean fileUpload, boolean pictureUpload, int avatarMaxBytes, long fileMaxBytes, long pictureMaxBytes)
    {
        this.blockDirectPictures = blockDirectPictures;
        this.blockDirectFiles = blockDirectFiles;
        this.blockChannelPictures = blockChannelPictures;
        this.blockChannelFiles = blockChannelFiles;
        this.avatarUpload = avatarUpload;
        this.fileUpload = fileUpload;
        this.pictureUpload = pictureUpload;
        this.avatarMaxBytes = avatarMaxBytes;
        this.fileMaxBytes = fileMaxBytes;
        this.pictureMaxBytes = pictureMaxBytes;
    }

    public static ClientCapabilities none()
    {
        return new ClientCapabilities(false, false, false, false, false, false, false, 0, 0, 0);
    }
}
