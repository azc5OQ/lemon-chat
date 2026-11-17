package com.lemonchat.model;

/** bounded image data supplied by the server icon catalog */
public final class ServerIcon
{
    public final long id;
    public final String data;

    public ServerIcon(long id, String data)
    {
        this.id = id;
        this.data = data;
    }
}
