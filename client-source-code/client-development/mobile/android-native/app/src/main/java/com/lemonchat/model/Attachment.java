package com.lemonchat.model;

/** authenticated attachment bytes kept within the client's transfer limit */
public final class Attachment
{
    public static final int MAX_BYTES = 4 * 1024 * 1024;
    public final String id;
    public final String name;
    public final String mime;
    public final long size;
    public final boolean picture;
    private final byte[] content;

    public Attachment(String id, String name, String mime, byte[] content, boolean picture)
    {
        if (content == null || content.length > MAX_BYTES)
        {
            throw new IllegalArgumentException("Attachments must be at most 4 MiB");
        }
        this.id = id;
        this.name = name;
        this.mime = mime;
        this.size = content.length;
        this.picture = picture;
        this.content = content.clone();
    }

    /** @return a copy of the authenticated file bytes */
    public byte[] data()
    {
        return content.clone();
    }
}
