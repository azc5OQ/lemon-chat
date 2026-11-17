package com.lemonchat.model;

/** immutable channel shown in the channel list */
public final class Channel
{
    public final int id;
    public final int parentId;
    public final String name;
    public final String description;
    public final boolean passwordProtected;
    public final boolean audioEnabled;
    public final long iconId;

    public Channel(int id, int parentId, String name, String description, boolean passwordProtected, boolean audioEnabled)
    {
        this(id, parentId, name, description, passwordProtected, audioEnabled, -1);
    }

    public Channel(int id, int parentId, String name, String description, boolean passwordProtected, boolean audioEnabled, long iconId)
    {
        this.id = id;
        this.parentId = parentId;
        this.name = name;
        this.description = description;
        this.passwordProtected = passwordProtected;
        this.audioEnabled = audioEnabled;
        this.iconId = iconId;
    }

    /**
     * copies the channel with its current icon assignment
     *
     * @param value server icon identifier, or minus one when absent
     * @return updated channel
     */
    public Channel withIcon(long value)
    {
        return new Channel(id, parentId, name, description, passwordProtected, audioEnabled, value);
    }
}
