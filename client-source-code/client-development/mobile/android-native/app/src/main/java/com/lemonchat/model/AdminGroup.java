package com.lemonchat.model;

import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;

/** group details and the receiver's allowed management actions */
public final class AdminGroup
{
    public final int id;
    public final String name;
    public final String type;
    public final boolean channelGroup;
    public final boolean isDefaultGuest;
    public final long iconId;
    public final Map<String, Boolean> access;
    public final PermissionDefinition permissions;

    public AdminGroup(int id, String name, String type, boolean channelGroup, boolean isDefaultGuest, long iconId, Map<String, Boolean> access, PermissionDefinition permissions)
    {
        this.id = id;
        this.name = name;
        this.type = type;
        this.channelGroup = channelGroup;
        this.isDefaultGuest = isDefaultGuest;
        this.iconId = iconId;
        this.access = Collections.unmodifiableMap(new LinkedHashMap<>(access));
        this.permissions = permissions;
    }

    public boolean allows(String action)
    {
        return Boolean.TRUE.equals(access.get(action));
    }
}
