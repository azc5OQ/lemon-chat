package com.lemonchat.model;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** immutable session state passed to the main thread */
public final class SessionSnapshot
{
    public enum Status
    {
        DISCONNECTED,
        CONNECTING,
        AUTHENTICATING,
        CONNECTED,
        FAILED
    }

    public final Status status;
    public final String detail;
    public final boolean connected;
    public final int clientId;
    public final int channelId;
    public final String username;
    public final boolean channelKeysReady;
    public final List<Channel> channels;
    public final List<Member> members;
    public final List<Group> groups;
    public final long connectionId;
    public final boolean idle;
    public final boolean idleAllowed;
    public final CallInvitation call;
    public final boolean retryableFailure;
    public final List<ServerIcon> icons;
    public final List<ServerEmoji> emojis;
    public final ClientCapabilities capabilities;
    public final AdminState admin;

    public SessionSnapshot(Status status, String detail, int clientId, int channelId, String username, boolean channelKeysReady, List<Channel> channels, List<Member> members, long connectionId)
    {
        this(status, detail, clientId, channelId, username, channelKeysReady, channels, members, Collections.emptyList(), connectionId);
    }

    public SessionSnapshot(Status status, String detail, int clientId, int channelId, String username, boolean channelKeysReady, List<Channel> channels, List<Member> members, List<Group> groups, long connectionId)
    {
        this(status, detail, clientId, channelId, username, channelKeysReady, channels, members, groups, connectionId, false, false, null);
    }

    public SessionSnapshot(Status status, String detail, int clientId, int channelId, String username, boolean channelKeysReady, List<Channel> channels, List<Member> members, List<Group> groups, long connectionId, boolean idle, boolean idleAllowed, CallInvitation call)
    {
        this(status, detail, clientId, channelId, username, channelKeysReady, channels, members, groups, connectionId, idle, idleAllowed, call, false);
    }

    public SessionSnapshot(Status status, String detail, int clientId, int channelId, String username, boolean channelKeysReady, List<Channel> channels, List<Member> members, List<Group> groups, long connectionId, boolean idle, boolean idleAllowed, CallInvitation call, boolean retryableFailure)
    {
        this(status, detail, clientId, channelId, username, channelKeysReady, channels, members, groups, connectionId, idle, idleAllowed, call, retryableFailure, Collections.emptyList());
    }

    public SessionSnapshot(Status status, String detail, int clientId, int channelId, String username, boolean channelKeysReady, List<Channel> channels, List<Member> members, List<Group> groups, long connectionId, boolean idle, boolean idleAllowed, CallInvitation call, boolean retryableFailure, List<ServerIcon> icons)
    {
        this(status, detail, clientId, channelId, username, channelKeysReady, channels, members, groups, connectionId, idle, idleAllowed, call, retryableFailure, icons, Collections.emptyList(), ClientCapabilities.none());
    }

    public SessionSnapshot(Status status, String detail, int clientId, int channelId, String username, boolean channelKeysReady, List<Channel> channels, List<Member> members, List<Group> groups, long connectionId, boolean idle, boolean idleAllowed, CallInvitation call, boolean retryableFailure, List<ServerIcon> icons, List<ServerEmoji> emojis, ClientCapabilities capabilities)
    {
        this(status, detail, clientId, channelId, username, channelKeysReady, channels, members, groups, connectionId, idle, idleAllowed, call, retryableFailure, icons, emojis, capabilities, AdminState.empty(connectionId));
    }

    public SessionSnapshot(Status status, String detail, int clientId, int channelId, String username, boolean channelKeysReady, List<Channel> channels, List<Member> members, List<Group> groups, long connectionId, boolean idle, boolean idleAllowed, CallInvitation call, boolean retryableFailure, List<ServerIcon> icons, List<ServerEmoji> emojis, ClientCapabilities capabilities, AdminState admin)
    {
        this.admin = admin;
        this.emojis = Collections.unmodifiableList(new ArrayList<>(emojis));
        this.capabilities = capabilities;
        this.status = status;
        this.detail = detail;
        this.connected = status == Status.CONNECTED;
        this.clientId = clientId;
        this.channelId = channelId;
        this.username = username;
        this.channelKeysReady = channelKeysReady;
        this.channels = Collections.unmodifiableList(new ArrayList<>(channels));
        this.members = Collections.unmodifiableList(new ArrayList<>(members));
        this.groups = Collections.unmodifiableList(new ArrayList<>(groups));
        this.connectionId = connectionId;
        this.idle = idle;
        this.idleAllowed = idleAllowed;
        this.call = call;
        this.retryableFailure = retryableFailure;
        this.icons = Collections.unmodifiableList(new ArrayList<>(icons));
    }
}
