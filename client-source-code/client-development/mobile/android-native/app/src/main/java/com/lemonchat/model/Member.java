package com.lemonchat.model;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Base64;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** immutable online identity and channel membership */
public final class Member
{
    public final int id;
    public final int channelId;
    public final String name;
    public final String identityHash;
    public final String publicKey;
    public final boolean musicBot;
    public final boolean idle;
    public final int audioState;
    public final int musicBotEncryption;
    public final String musicBotKeys;
    public final String conversationId;
    public final List<Integer> groupIds;
    public final int channelGroupId;
    public final String countryCode;
    public final String avatarDataUri;
    public final boolean streamingSong;
    public final String songName;

    public Member(int id, int channelId, String name, String identityHash, String publicKey, boolean musicBot, boolean idle, int audioState, int musicBotEncryption, String musicBotKeys)
    {
        this(id, channelId, name, identityHash, publicKey, musicBot, idle, audioState, musicBotEncryption, musicBotKeys, Collections.emptyList(), 0);
    }

    public Member(int id, int channelId, String name, String identityHash, String publicKey, boolean musicBot, boolean idle, int audioState, int musicBotEncryption, String musicBotKeys, List<Integer> groupIds, int channelGroupId)
    {
        this(id, channelId, name, identityHash, publicKey, musicBot, idle, audioState, musicBotEncryption, musicBotKeys, groupIds, channelGroupId, "", "");
    }

    public Member(int id, int channelId, String name, String identityHash, String publicKey, boolean musicBot, boolean idle, int audioState, int musicBotEncryption, String musicBotKeys, List<Integer> groupIds, int channelGroupId, String countryCode, String avatarDataUri)
    {
        this(id, channelId, name, identityHash, publicKey, musicBot, idle, audioState, musicBotEncryption, musicBotKeys, groupIds, channelGroupId, countryCode, avatarDataUri, false, "");
    }

    public Member(int id, int channelId, String name, String identityHash, String publicKey, boolean musicBot, boolean idle, int audioState, int musicBotEncryption, String musicBotKeys, List<Integer> groupIds, int channelGroupId, String countryCode, String avatarDataUri, boolean streamingSong, String songName)
    {
        this.streamingSong = streamingSong && !idle && songName != null && !songName.isEmpty();
        this.songName = this.streamingSong ? songName : "";
        this.countryCode = countryCode;
        this.avatarDataUri = avatarDataUri;
        this.id = id;
        this.channelId = channelId;
        this.name = name;
        this.identityHash = identityHash;
        this.publicKey = publicKey;
        this.musicBot = musicBot;
        this.idle = idle;
        this.audioState = audioState;
        this.musicBotEncryption = musicBotEncryption;
        this.musicBotKeys = musicBotKeys;
        this.conversationId = conversationForKey(publicKey);
        this.groupIds = Collections.unmodifiableList(new ArrayList<>(groupIds));
        this.channelGroupId = channelGroupId;
    }

    /**
     * copies this identity with its current membership
     *
     * @param channel destination channel
     * @param idle whether the member is idle
     * @param audio current audio state
     * @return updated immutable member
     */
    public Member withPresence(int channel, boolean idle, int audio)
    {
        return new Member(id, channel, name, identityHash, publicKey, musicBot, idle, audio, musicBotEncryption, musicBotKeys, groupIds, idle || channel != channelId ? 0 : channelGroupId, countryCode, avatarDataUri, streamingSong, songName);
    }

    /**
     * copies this identity with its current name
     *
     * @param value new display name
     * @return updated immutable member
     */
    public Member withName(String value)
    {
        return new Member(id, channelId, value, identityHash, publicKey, musicBot, idle, audioState, musicBotEncryption, musicBotKeys, groupIds, channelGroupId, countryCode, avatarDataUri, streamingSong, songName);
    }

    /**
     * copies this identity with its current groups
     *
     * @param serverGroups server group identifiers
     * @param channelGroup role in the visible channel
     * @return updated immutable member
     */
    public Member withGroups(List<Integer> serverGroups, int channelGroup)
    {
        return new Member(id, channelId, name, identityHash, publicKey, musicBot, idle, audioState, musicBotEncryption, musicBotKeys, serverGroups, channelGroup, countryCode, avatarDataUri, streamingSong, songName);
    }

    public Member withProfile(String country, String avatar)
    {
        return new Member(id, channelId, name, identityHash, publicKey, musicBot, idle, audioState, musicBotEncryption, musicBotKeys, groupIds, channelGroupId, country, avatar, streamingSong, songName);
    }

    /**
     * replaces the current stream title
     *
     * @param active whether a song is playing
     * @param title current song name
     * @return updated immutable member
     */
    public Member withSong(boolean active, String title)
    {
        return new Member(id, channelId, name, identityHash, publicKey, musicBot, idle, audioState, musicBotEncryption, musicBotKeys, groupIds, channelGroupId, countryCode, avatarDataUri, active, title);
    }

    private static String conversationForKey(String publicKey)
    {
        try
        {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(publicKey.getBytes(StandardCharsets.UTF_8));
            return conversationForDigest(digest);
        }
        catch (NoSuchAlgorithmException error)
        {
            throw new IllegalStateException("SHA-256 is unavailable", error);
        }
    }

    /**
     * maps an offline sender to the same conversation as its public key
     *
     * @param identityHash server supplied base64 SHA256 identity hash
     * @return stable private conversation identifier
     */
    public static String conversationForIdentityHash(String identityHash)
    {
        byte[] digest = Base64.getDecoder().decode(identityHash);
        if (digest.length != 32)
        {
            throw new IllegalArgumentException("Invalid identity hash");
        }
        return conversationForDigest(digest);
    }

    private static String conversationForDigest(byte[] digest)
    {
        StringBuilder value = new StringBuilder("user:");
        for (byte item : digest)
        {
            value.append(Character.forDigit((item >>> 4) & 15, 16));
            value.append(Character.forDigit(item & 15, 16));
        }
        return value.toString();
    }
}
