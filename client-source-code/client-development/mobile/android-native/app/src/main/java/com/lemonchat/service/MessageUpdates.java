package com.lemonchat.service;

import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;

/** keeps older archive completions from replacing newer message updates */
final class MessageUpdates
{
    private final Map<String, Pending> pending = new HashMap<>();
    private final Set<String> received = new LinkedHashSet<>();

    static final class Update
    {
        final String key;
        final Pending owner;
        final long revision;
        final boolean first;
        private boolean queued;

        Update(String key, Pending owner, boolean first)
        {
            this.key = key;
            this.owner = owner;
            this.revision = ++owner.revision;
            this.first = first;
        }
    }

    private static final class Pending
    {
        long revision;
        int writes;
        boolean notify;
    }

    Update begin(String key, boolean known)
    {
        Pending owner = pending.get(key);
        boolean first = owner == null && !known && !received.contains(key);
        received.add(key);
        while (received.size() > 2048)
        {
            received.remove(received.iterator().next());
        }
        if (owner == null)
        {
            owner = new Pending();
            owner.notify = first;
            pending.put(key, owner);
        }
        return new Update(key, owner, first);
    }

    void queued(Update update)
    {
        update.queued = true;
        update.owner.writes++;
    }

    boolean current(Update update)
    {
        return pending.get(update.key) == update.owner && update.owner.revision == update.revision;
    }

    boolean takeNotification(Update update)
    {
        if (!current(update) || !update.owner.notify)
        {
            return false;
        }
        update.owner.notify = false;
        return true;
    }

    void finish(Update update)
    {
        if (update.queued)
        {
            update.queued = false;
            update.owner.writes--;
        }
        if (update.owner.writes == 0)
        {
            pending.remove(update.key, update.owner);
        }
    }

    void clear()
    {
        pending.clear();
        received.clear();
    }
}
