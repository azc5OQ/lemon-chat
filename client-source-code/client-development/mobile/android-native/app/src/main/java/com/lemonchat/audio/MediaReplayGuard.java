package com.lemonchat.audio;

import java.util.HashMap;
import java.util.Map;

/**
 * rejects repeated sender counters independently of playback buffer lifetime
 */
final class MediaReplayGuard
{
    private final Map<Integer, Long> counters = new HashMap<>();

    /**
     * accepts an authenticated packet only once
     *
     * @param sender current channel member ID
     * @param counter unsigned sender packet counter
     * @return whether this packet advances the sender's counter
     */
    boolean accept(int sender, long counter)
    {
        Long previous = counters.get(sender);
        if (previous != null && Long.compareUnsigned(counter, previous) <= 0)
        {
            return false;
        }
        counters.put(sender, counter);
        return true;
    }

    /**
     * drops counters after a sender leaves or is replaced
     *
     * @param sender retired client ID
     */
    void forget(int sender)
    {
        counters.remove(sender);
    }

    /**
     * starts a new authenticated session
     */
    void clear()
    {
        counters.clear();
    }
}
