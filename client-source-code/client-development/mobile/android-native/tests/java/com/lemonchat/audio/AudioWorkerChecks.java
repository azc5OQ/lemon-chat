package com.lemonchat.audio;

import java.lang.reflect.Field;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** checks queue bounds, reserved teardown and idle thread retirement */
public final class AudioWorkerChecks
{
    public static void main(String[] arguments) throws Exception
    {
        AudioWorker worker = new AudioWorker();
        Field field = AudioWorker.class.getDeclaredField("executor");
        field.setAccessible(true);
        ThreadPoolExecutor executor = (ThreadPoolExecutor)field.get(worker);
        require(executor.getPoolSize() == 0, "inactive audio started a thread");
        CountDownLatch running = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        AtomicInteger completed = new AtomicInteger();
        worker.execute(() ->
        {
            running.countDown();
            await(release);
            completed.incrementAndGet();
        });
        require(running.await(2, TimeUnit.SECONDS), "audio worker did not start");
        for (int index = 0; index < 127; index++)
        {
            worker.execute(completed::incrementAndGet);
        }
        require(executor.getQueue().size() == 127, "normal queue size changed");
        boolean refused = false;
        try
        {
            worker.execute(completed::incrementAndGet);
        }
        catch (RejectedExecutionException expected)
        {
            refused = true;
        }
        require(refused, "audio queue accepted more than 128 normal jobs");
        CountDownLatch retired = new CountDownLatch(1);
        worker.executeRetirement(retired::countDown);
        require(executor.getQueue().size() == 128, "saturated audio refused teardown");
        release.countDown();
        require(retired.await(3, TimeUnit.SECONDS), "queued teardown did not execute");
        require(completed.get() == 128, "normal tasks were not serialized before teardown");
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(8);
        while (executor.getPoolSize() != 0 && System.nanoTime() < deadline)
        {
            Thread.sleep(20);
        }
        require(executor.getPoolSize() == 0, "idle audio thread did not retire");
        CountDownLatch restarted = new CountDownLatch(1);
        worker.execute(restarted::countDown);
        require(restarted.await(2, TimeUnit.SECONDS), "retired audio worker could not restart");
        CountDownLatch closed = new CountDownLatch(1);
        worker.executeRetirement(closed::countDown);
        worker.close();
        require(closed.await(2, TimeUnit.SECONDS), "shutdown discarded accepted teardown");
        require(executor.awaitTermination(2, TimeUnit.SECONDS), "audio worker did not shut down");
        refused = false;
        try
        {
            worker.execute(() ->
            {
            });
        }
        catch (RejectedExecutionException expected)
        {
            refused = true;
        }
        require(refused, "closed audio worker accepted a job");
        System.out.println("Audio worker checks: 12 passed");
    }

    private static void await(CountDownLatch latch)
    {
        try
        {
            if (!latch.await(5, TimeUnit.SECONDS))
            {
                throw new AssertionError("test gate timed out");
            }
        }
        catch (InterruptedException failure)
        {
            Thread.currentThread().interrupt();
            throw new AssertionError(failure);
        }
    }

    private static void require(boolean condition, String message)
    {
        if (!condition)
        {
            throw new AssertionError(message);
        }
    }
}
