package com.lemonchat.service;

/** bounds repeated connection attempts without accumulating pending work */
public final class ReconnectBackoff
{
    private int failures;

    /** @return the next delay in milliseconds, capped at one minute */
    public long nextDelay()
    {
        long delay = Math.min(60000L, 2000L << Math.min(failures, 5));
        failures = Math.min(failures + 1, 6);
        return delay;
    }

    /** starts a new retry sequence after a stable connection or user request */
    public void reset()
    {
        failures = 0;
    }
}
