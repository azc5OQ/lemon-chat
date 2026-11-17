package com.lemonchat.model;

/** immutable server group or channel role received from the server */
public final class Group
{
    public final int id;
    public final String name;
    public final String type;
    public final boolean channelGroup;
    public final long iconId;

    public Group(int id, String name, String type, boolean channelGroup, long iconId)
    {
        this.id = id;
        this.name = name;
        this.type = type;
        this.channelGroup = channelGroup;
        this.iconId = iconId;
    }

    /**
     * copies the group with its current icon assignment
     *
     * @param value server icon id, or a negative built in value
     * @return updated immutable group
     */
    public Group withIcon(long value)
    {
        return new Group(id, name, type, channelGroup, value);
    }
}
