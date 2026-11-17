package com.lemonchat.protocol;

import com.lemonchat.model.Member;
import java.util.ArrayList;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.Map;
import org.json.JSONObject;

/** owns bounded avatar requests independently of visible screens */
final class AvatarLoader
{
    interface Host
    {
        Member member(int id);
        void update(Member member);
        void send(JSONObject message);
    }

    private static final int MAX_CHARACTERS = 2 * 1024 * 1024;
    private static final int MAX_AVATAR_CHARACTERS = 123000;
    private final Host host;
    private final Map<Integer, Record> records = new LinkedHashMap<>();
    private final Map<Integer, Pending> pending = new LinkedHashMap<>();
    private boolean enabled;
    private long order;
    private long nextSendAt;
    private int characters;

    AvatarLoader(Host host)
    {
        this.host = host;
    }

    void setEnabled(boolean value)
    {
        enabled = value;
        if (!enabled)
        {
            pending.clear();
        }
    }

    void reset()
    {
        records.clear();
        pending.clear();
        characters = 0;
        order = 0;
        nextSendAt = 0;
        enabled = false;
    }

    void replaceMembers(Collection<Member> members)
    {
        java.util.Set<Integer> present = new java.util.HashSet<>();
        for (Member member : members)
        {
            present.add(member.id);
            memberChanged(member);
        }
        for (int id : new ArrayList<>(records.keySet()))
        {
            if (!present.contains(id))
            {
                memberRemoved(id);
            }
        }
    }

    void memberChanged(Member member)
    {
        if (member.musicBot || member.publicKey.isEmpty())
        {
            memberRemoved(member.id);
            return;
        }
        Record previous = records.get(member.id);
        if (previous == null || !previous.key.equals(member.publicKey))
        {
            memberRemoved(member.id);
            if (records.size() < 10000)
            {
                records.put(member.id, new Record(member.id, member.publicKey, ++order));
            }
            return;
        }
        if (!previous.data.equals(member.avatarDataUri))
        {
            host.update(member.withProfile(member.countryCode, previous.data));
        }
    }

    void memberRemoved(int id)
    {
        Record removed = records.remove(id);
        if (removed != null)
        {
            characters -= removed.data.length();
        }
    }

    /** marks visible cache misses without forgetting known empty avatars */
    void hint(int[] ids)
    {
        long now = java.util.concurrent.TimeUnit.NANOSECONDS.toMillis(System.nanoTime());
        for (int index = 0; index < Math.min(ids.length, 100); index++)
        {
            Record record = records.get(ids[index]);
            if (record != null)
            {
                record.touched = ++order;
                if (now < record.hintAfter)
                {
                    continue;
                }
                if (record.evicted && !record.priority)
                {
                    record.complete = false;
                    record.attempts = 0;
                }
                if (!record.complete)
                {
                    record.priority = true;
                    record.hintAfter = now + 30000;
                }
            }
        }
    }

    void changed(int id)
    {
        Record record = records.get(id);
        if (record != null)
        {
            clearData(record);
            record.revision++;
            record.complete = false;
            record.priority = true;
            record.evicted = false;
            record.attempts = 0;
        }
    }

    void received(int id, String data)
    {
        Pending request = pending.remove(id);
        Record record = records.get(id);
        if (request == null || record == null || !request.matches(record))
        {
            return;
        }
        if (!data.isEmpty() && !ChatFeatures.imageUri(data, 92160))
        {
            record.complete = true;
            return;
        }
        while (characters - record.data.length() + data.length() > MAX_CHARACTERS && record.priority)
        {
            Record oldest = null;
            for (Record candidate : records.values())
            {
                if (candidate != record && !candidate.data.isEmpty() && (oldest == null || candidate.touched < oldest.touched))
                {
                    oldest = candidate;
                }
            }
            if (oldest == null)
            {
                break;
            }
            clearData(oldest);
            oldest.evicted = true;
        }
        record.complete = true;
        if (characters - record.data.length() + data.length() <= MAX_CHARACTERS)
        {
            characters += data.length() - record.data.length();
            record.data = data;
            record.evicted = false;
            record.touched = ++order;
            Member member = host.member(id);
            if (member != null && member.publicKey.equals(record.key))
            {
                host.update(member.withProfile(member.countryCode, data));
            }
        }
        else
        {
            record.evicted = true;
        }
        record.priority = false;
    }

    void tick(long now)
    {
        if (!enabled)
        {
            return;
        }
        for (Pending request : new ArrayList<>(pending.values()))
        {
            if (now >= request.deadline)
            {
                pending.remove(request.id);
                Record record = records.get(request.id);
                if (record != null && request.matches(record) && record.attempts >= 2)
                {
                    record.complete = true;
                }
            }
        }
        if (now < nextSendAt)
        {
            return;
        }
        int sent = 0;
        while (sent < 4 && pending.size() < 16)
        {
            Record record = nextRecord();
            if (record == null)
            {
                break;
            }
            record.attempts++;
            pending.put(record.id, new Pending(record, now + 10000));
            JSONObject message = new JSONObject();
            try
            {
                message.put("type", "request_avatar_for_client");
                message.put("client_id", record.id);
            }
            catch (org.json.JSONException error)
            {
                throw new IllegalArgumentException(error);
            }
            host.send(message);
            sent++;
        }
        if (sent > 0)
        {
            nextSendAt = now + 250;
        }
    }

    long nextDelay(long now)
    {
        if (!enabled)
        {
            return -1;
        }
        long delay = Long.MAX_VALUE;
        if (pending.size() < 16 && nextRecord() != null)
        {
            delay = Math.max(1, nextSendAt - now);
        }
        for (Pending request : pending.values())
        {
            delay = Math.min(delay, Math.max(1, request.deadline - now));
        }
        return delay == Long.MAX_VALUE ? -1 : delay;
    }

    private Record nextRecord()
    {
        Record first = null;
        for (Record record : records.values())
        {
            if (!record.complete && !pending.containsKey(record.id) && (record.priority || characters <= MAX_CHARACTERS - MAX_AVATAR_CHARACTERS))
            {
                if (record.priority)
                {
                    return record;
                }
                if (first == null)
                {
                    first = record;
                }
            }
        }
        return first;
    }

    private void clearData(Record record)
    {
        characters -= record.data.length();
        record.data = "";
        Member member = host.member(record.id);
        if (member != null && member.publicKey.equals(record.key))
        {
            host.update(member.withProfile(member.countryCode, ""));
        }
    }

    private static final class Record
    {
        final int id;
        final String key;
        String data = "";
        int revision;
        int attempts;
        long touched;
        long hintAfter;
        boolean complete;
        boolean priority;
        boolean evicted;

        Record(int id, String key, long touched)
        {
            this.id = id;
            this.key = key;
            this.touched = touched;
        }
    }

    private static final class Pending
    {
        final int id;
        final String key;
        final int revision;
        final long deadline;

        Pending(Record record, long deadline)
        {
            id = record.id;
            key = record.key;
            revision = record.revision;
            this.deadline = deadline;
        }

        boolean matches(Record record)
        {
            return key.equals(record.key) && revision == record.revision;
        }
    }
}
