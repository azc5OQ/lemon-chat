package com.lemonchat.service;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;

/** keeps a requested foreground connection scheduled while the screen sleeps */
public final class BackgroundConnectionLease implements AutoCloseable
{
    private static final long TIMEOUT = 60000;
    private static final long RENEW_AFTER = 20000;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final PowerManager.WakeLock lock;
    private final Runnable renew = this::renew;
    private boolean active;
    private boolean closed;

    /**
     * creates an inactive CPU lease without opening audio devices
     *
     * @param context service or application context
     */
    public BackgroundConnectionLease(Context context)
    {
        PowerManager power = context.getApplicationContext().getSystemService(PowerManager.class);
        lock = power == null ? null : power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "lemonchat:connection");
        if (lock != null)
        {
            lock.setReferenceCounted(false);
        }
    }

    /**
     * holds the lease only during an admitted connection or handshake
     *
     * @param wanted false during disconnect, shutdown or offline retry waits
     * @return whether the requested lease state was applied
     */
    public synchronized boolean setActive(boolean wanted)
    {
        if (closed)
        {
            return !wanted;
        }
        if (!wanted)
        {
            active = false;
            handler.removeCallbacks(renew);
            release();
            return true;
        }
        if (active && lock != null && lock.isHeld())
        {
            return true;
        }
        active = true;
        return acquire();
    }

    /** @return whether Android currently holds this CPU lease */
    public synchronized boolean isHeld()
    {
        return lock != null && lock.isHeld();
    }

    private synchronized void renew()
    {
        if (active && !closed)
        {
            acquire();
        }
    }

    private boolean acquire()
    {
        handler.removeCallbacks(renew);
        try
        {
            if (lock == null)
            {
                active = false;
                return false;
            }
            lock.acquire(TIMEOUT);
            handler.postDelayed(renew, RENEW_AFTER);
            return true;
        }
        catch (RuntimeException failure)
        {
            active = false;
            release();
            android.util.Log.w("LemonNative", "Background connection CPU lease is unavailable", failure);
            return false;
        }
    }

    private void release()
    {
        if (lock != null && lock.isHeld())
        {
            try
            {
                lock.release();
            }
            catch (RuntimeException failure)
            {
                android.util.Log.w("LemonNative", "Background connection CPU lease was already released", failure);
            }
        }
    }

    /** releases the lease and prevents delayed renewal after service teardown */
    @Override
    public synchronized void close()
    {
        active = false;
        closed = true;
        handler.removeCallbacks(renew);
        release();
    }
}
