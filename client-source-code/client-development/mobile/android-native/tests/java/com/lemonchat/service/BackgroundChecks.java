package com.lemonchat.service;

/** verifies retry pacing without an Android runtime */
public final class BackgroundChecks
{
    public static void main(String[] arguments)
    {
        ReconnectBackoff backoff = new ReconnectBackoff();
        long[] expected = {2000, 4000, 8000, 16000, 32000, 60000};
        for (long delay : expected)
        {
            require(backoff.nextDelay() == delay, "retry progression");
        }
        for (int index = 0; index < 10000; index++)
        {
            require(backoff.nextDelay() == 60000, "repeated failure stays bounded");
        }
        backoff.reset();
        require(backoff.nextDelay() == 2000, "user request resets delay");
        require(backoff.nextDelay() == 4000, "retry progression resumes after reset");
        backoff.reset();
        require(backoff.nextDelay() == 2000, "stable connection resets delay");
        System.out.println("Background retry checks: passed progression, 10000 capped failures and reset");
        verifyMessageUpdates();
    }

    private static void verifyMessageUpdates()
    {
        MessageUpdates updates = new MessageUpdates();
        MessageUpdates.Update original = updates.begin("original", false);
        require(original.first && updates.current(original), "the first message is classified as new");
        updates.queued(original);
        MessageUpdates.Update overflow = updates.begin("original", false);
        require(!overflow.first && updates.current(overflow) && !updates.current(original), "an immediate update replaces the pending original");
        require(!updates.takeNotification(original) && updates.takeNotification(overflow), "the first notification follows the newest update");
        updates.finish(overflow);
        require(!updates.current(original) && !updates.takeNotification(original), "an older save cannot overwrite or notify after overflow");
        updates.finish(original);

        MessageUpdates.Update pending = updates.begin("pending", false);
        updates.queued(pending);
        MessageUpdates.Update later = updates.begin("pending", false);
        updates.queued(later);
        updates.finish(pending);
        require(updates.current(later) && updates.takeNotification(later), "the notification survives completion of an older queued write");
        require(!updates.takeNotification(later), "a message is notified only once");
        updates.finish(later);

        MessageUpdates.Update known = updates.begin("known", true);
        require(!known.first && !updates.takeNotification(known), "an existing message update cannot create a new notification");
        updates.finish(known);
        for (int index = 0; index < 300; index++)
        {
            MessageUpdates.Update message = updates.begin("other:" + index, false);
            updates.takeNotification(message);
            updates.finish(message);
        }
        MessageUpdates.Update old = updates.begin("original", true);
        require(!old.first && !updates.takeNotification(old), "a known update remains quiet after 300 other notifications");
        updates.finish(old);
        MessageUpdates.Update trimmed = updates.begin("original", false);
        require(!trimmed.first && !updates.takeNotification(trimmed), "trimming the visible chat cannot make an old update new again");
        updates.finish(trimmed);

        MessageUpdates.Update stale = updates.begin("same-key", false);
        updates.queued(stale);
        updates.clear();
        MessageUpdates.Update replacement = updates.begin("same-key", false);
        updates.queued(replacement);
        require(!updates.current(stale) && !updates.takeNotification(stale), "scope reset retires old completions");
        updates.finish(stale);
        require(updates.current(replacement) && replacement.first && updates.takeNotification(replacement), "an old completion cannot remove the replacement scope");
        updates.finish(replacement);
        System.out.println("PASS 11 archive update and notification ordering checks");
    }

    private static void require(boolean value, String detail)
    {
        if (!value)
        {
            throw new AssertionError(detail);
        }
    }
}
