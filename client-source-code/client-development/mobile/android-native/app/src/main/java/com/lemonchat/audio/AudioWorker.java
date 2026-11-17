package com.lemonchat.audio;

import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.Semaphore;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/** bounds audio work and lets its thread retire when no media work remains */
final class AudioWorker implements AutoCloseable
{
    private final Semaphore normal = new Semaphore(128);
    private final ThreadPoolExecutor executor = new ThreadPoolExecutor(0, 1, 5, TimeUnit.SECONDS, new ArrayBlockingQueue<>(129), task -> new Thread(task, "native-audio-transport"));

    void execute(Runnable action)
    {
        submit(action, normal);
    }

    void executeRetirement(Runnable action)
    {
        executor.execute(action);
    }

    private void submit(Runnable action, Semaphore slots)
    {
        if (!slots.tryAcquire())
        {
            throw new RejectedExecutionException("The audio worker is full");
        }
        try
        {
            executor.execute(() ->
            {
                try
                {
                    action.run();
                }
                finally
                {
                    slots.release();
                }
            });
        }
        catch (RejectedExecutionException refused)
        {
            slots.release();
            throw refused;
        }
    }

    @Override
    public void close()
    {
        executor.shutdown();
    }
}
