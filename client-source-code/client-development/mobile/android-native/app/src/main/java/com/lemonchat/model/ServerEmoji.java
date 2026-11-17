package com.lemonchat.model;

/** an actual entry in the connected server's emoji catalog */
public final class ServerEmoji
{
    public final String id;
    public final String name;
    public final String unicode;
    public final String dataUri;

    public ServerEmoji(String id, String name, String unicode, String dataUri)
    {
        this.id = id;
        this.name = name;
        this.unicode = unicode;
        this.dataUri = dataUri;
    }
}
