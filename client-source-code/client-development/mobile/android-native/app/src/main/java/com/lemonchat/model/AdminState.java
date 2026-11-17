package com.lemonchat.model;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** connection owned administration state shared with native screens */
public final class AdminState
{
    public final long connectionId;
    public final boolean isAdmin;
    public final boolean groupsEnabled;
    public final boolean busy;
    public final boolean passwordChangeRequired;
    public final String statusText;
    public final String error;
    public final long revision;
    public final AdminSettings settings;
    public final List<AdminGroup> groups;
    public final Map<String, Boolean> access;
    public final Map<String, Integer> powers;

    public AdminState(long connectionId, boolean isAdmin, boolean groupsEnabled, boolean busy, boolean passwordChangeRequired, String statusText, String error, long revision, AdminSettings settings, List<AdminGroup> groups, Map<String, Boolean> access, Map<String, Integer> powers)
    {
        this.connectionId = connectionId;
        this.isAdmin = isAdmin;
        this.groupsEnabled = groupsEnabled;
        this.busy = busy;
        this.passwordChangeRequired = passwordChangeRequired;
        this.statusText = statusText;
        this.error = error;
        this.revision = revision;
        this.settings = settings;
        this.groups = Collections.unmodifiableList(new ArrayList<>(groups));
        this.access = Collections.unmodifiableMap(new LinkedHashMap<>(access));
        this.powers = Collections.unmodifiableMap(new LinkedHashMap<>(powers));
    }

    public static AdminState empty(long connectionId)
    {
        return new AdminState(connectionId, false, false, false, false, "", "", 0, null, Collections.emptyList(), Collections.emptyMap(), Collections.emptyMap());
    }

    public boolean allows(String action)
    {
        return Boolean.TRUE.equals(access.get(action));
    }

    public AdminGroup group(int id, boolean channelGroup)
    {
        for (AdminGroup group : groups)
        {
            if (group.id == id && group.channelGroup == channelGroup)
            {
                return group;
            }
        }
        return null;
    }
}
