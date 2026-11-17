package com.lemonchat.protocol;

import com.lemonchat.model.Member;
import java.util.ArrayDeque;
import java.util.Collection;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** channel key ownership and membership rules, confined to the session worker */
final class ChannelKeyState
{
    String current;
    String previous;
    int maintainer = -1;
    long generation = -1;
    long waitingSince;
    boolean rotate;
    private boolean reuseEnabled = true;
    private boolean reuseJoin = true;
    private boolean reuseLeave = true;
    private int joinThreshold = 50;
    private int leaveThreshold = 100;
    private final ArrayDeque<Member> recipients = new ArrayDeque<>();

    /** discards keys when the local channel or connection changes */
    void reset()
    {
        current = null;
        previous = null;
        maintainer = -1;
        generation = -1;
        rotate = false;
        recipients.clear();
        waitingSince = System.nanoTime();
    }

    /** applies the server's current key reuse policy */
    void readPolicy(JSONObject message, int localId)
    {
        boolean oldEnabled = reuseEnabled;
        boolean oldJoin = reuseJoin;
        boolean oldLeave = reuseLeave;
        int oldJoinThreshold = joinThreshold;
        int oldLeaveThreshold = leaveThreshold;
        reuseEnabled = message.optBoolean("is_channel_key_reuse_enabled", reuseEnabled);
        reuseJoin = message.optBoolean("is_channel_key_reuse_on_join_allowed", reuseJoin);
        reuseLeave = message.optBoolean("is_channel_key_reuse_on_leave_allowed", reuseLeave);
        joinThreshold = threshold(message, "channel_key_reuse_join_min_clients", joinThreshold);
        leaveThreshold = threshold(message, "channel_key_reuse_leave_min_clients", leaveThreshold);
        if (oldEnabled != reuseEnabled || oldJoin != reuseJoin || oldLeave != reuseLeave || oldJoinThreshold != joinThreshold || oldLeaveThreshold != leaveThreshold)
        {
            refresh(localId);
        }
    }

    private int threshold(JSONObject message, String name, int fallback)
    {
        int value = message.optInt(name, fallback);
        return value >= 30 && value <= 1000000 ? value : fallback;
    }

    /** marks existing keys stale while preserving one set for messages already in flight */
    void refresh(int localId)
    {
        if (current != null)
        {
            previous = current;
        }
        current = null;
        rotate = maintainer >= 0 && maintainer == localId;
        recipients.clear();
        waitingSince = System.nanoTime();
    }

    /** applies an authenticated maintainer assignment */
    void assign(int id, long revision, int localId)
    {
        if (maintainer == id && generation == revision)
        {
            return;
        }
        maintainer = id;
        generation = revision;
        refresh(localId);
    }

    /** updates key rotation or delivery after a member arrives, leaves or changes identity */
    void memberChanged(Member old, Member next, Collection<Member> members, int channelId, int localId)
    {
        int id = next != null ? next.id : (old != null ? old.id : -1);
        boolean wasMember = isMember(old, channelId);
        boolean isMember = isMember(next, channelId);
        boolean changedIdentity = old != null && next != null && !old.publicKey.equals(next.publicKey);
        if (id < 0 || id == localId || (!wasMember && !isMember))
        {
            return;
        }
        if (wasMember && (!isMember || changedIdentity))
        {
            recipients.removeIf(member -> member.id == id);
            if (id == maintainer)
            {
                maintainer = -1;
                refresh(localId);
                return;
            }
            if (changedIdentity || !mayReuse(false, members, channelId))
            {
                refresh(localId);
            }
        }
        if (isMember && (!wasMember || changedIdentity))
        {
            if (changedIdentity || current == null || !mayReuse(true, members, channelId))
            {
                refresh(localId);
            }
            else if (maintainer == localId)
            {
                queue(next, channelId, localId);
            }
        }
    }

    private boolean mayReuse(boolean joining, Collection<Member> members, int channelId)
    {
        int count = 0;
        for (Member member : members)
        {
            if (isMember(member, channelId))
            {
                count++;
            }
        }
        return reuseEnabled && (joining ? reuseJoin : reuseLeave) && count >= (joining ? joinThreshold : leaveThreshold);
    }

    private boolean isMember(Member member, int channelId)
    {
        return member != null && member.channelId == channelId && !member.idle && !member.musicBot;
    }

    /** installs fresh locally generated keys and queues their recipients */
    void installGenerated(String keys, Collection<Member> members, int channelId, int localId) throws JSONException
    {
        validate(keys);
        current = keys;
        rotate = false;
        recipients.clear();
        for (Member member : members)
        {
            queue(member, channelId, localId);
        }
    }

    private void queue(Member member, int channelId, int localId)
    {
        if (member.id != localId && isMember(member, channelId) && !member.publicKey.isEmpty())
        {
            recipients.removeIf(queued -> queued.id == member.id);
            recipients.addLast(member);
        }
    }

    /** takes one recipient so key delivery does not monopolize the worker */
    Member nextRecipient()
    {
        return recipients.pollFirst();
    }

    /** accepts keys only from the exact current channel maintainer */
    boolean accept(int sender, int channelId, JSONObject body) throws JSONException
    {
        if (sender != maintainer || maintainer < 0 || !"channel_keys_from_maintainer".equals(body.optString("type")) || ProtocolJson.integer(body, "channel_id", 0, Integer.MAX_VALUE) != channelId || ProtocolJson.integer(body, "maintainer_generation", 0, 9007199254740991L) != generation)
        {
            return false;
        }
        String keys = ProtocolJson.string(body, "value", 10000);
        validate(keys);
        if (current != null)
        {
            previous = current;
        }
        current = keys;
        rotate = false;
        return true;
    }

    /** rejects malformed content keys before they reach native crypto or audio */
    static void validate(String text) throws JSONException
    {
        JSONArray keys = ProtocolJson.object("{\"keys\":" + text + "}", 10100).getJSONArray("keys");
        if (keys.length() != 4)
        {
            throw new JSONException("Expected four content keys");
        }
        for (int i = 0; i < keys.length(); i++)
        {
            JSONObject key = keys.getJSONObject(i);
            validateBytes(key.getJSONArray("key_bytes"), 32);
            validateBytes(key.getJSONArray("iv_bytes"), 16);
        }
    }

    private static void validateBytes(JSONArray bytes, int count) throws JSONException
    {
        if (bytes.length() != count)
        {
            throw new JSONException("Invalid content key length");
        }
        for (int i = 0; i < count; i++)
        {
            Object value = bytes.get(i);
            if (!(value instanceof Number) || ((Number) value).doubleValue() != ((Number) value).intValue() || ((Number) value).intValue() < 0 || ((Number) value).intValue() > 255)
            {
                throw new JSONException("Invalid content key byte");
            }
        }
    }
}
