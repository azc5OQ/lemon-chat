package com.lemonchat.audio;

import android.content.ContentValues;
import android.content.Context;
import android.net.Uri;
import android.provider.MediaStore;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.UUID;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

/** verifies actual Android MP3 decoding with a disposable media store record */
public final class AudioChecks
{
    public static int verify(Context context) throws Exception
    {
        checkQueuedPreferences(context);
        ContentValues values = new ContentValues();
        values.put(MediaStore.Audio.Media.DISPLAY_NAME, "lemon-native-check-" + UUID.randomUUID() + ".mp3");
        values.put(MediaStore.Audio.Media.MIME_TYPE, "audio/mpeg");
        values.put(MediaStore.Audio.Media.RELATIVE_PATH, "Music/LemonNativeChecks");
        Uri uri = context.getContentResolver().insert(MediaStore.Audio.Media.EXTERNAL_CONTENT_URI, values);
        require(uri != null, "test MP3 could not be created");
        try
        {
            try (InputStream input = context.getAssets().open("sounds/message_received.mp3"); OutputStream output = context.getContentResolver().openOutputStream(uri))
            {
                byte[] buffer = new byte[4096];
                int count;
                while ((count = input.read(buffer)) >= 0)
                {
                    output.write(buffer, 0, count);
                }
            }
            AtomicInteger packets = new AtomicInteger();
            AtomicLong energy = new AtomicLong();
            AtomicReference<String> failure = new AtomicReference<>();
            CountDownLatch finished = new CountDownLatch(1);
            long start = System.nanoTime();
            try (OpusCodec decoder = new OpusCodec(false, 48000, 2); Mp3Streamer stream = new Mp3Streamer(context, uri, new Mp3Streamer.Listener()
            {
                @Override
                public boolean onReady()
                {
                    return true;
                }

                @Override
                public boolean onPacket(byte[] packet)
                {
                    short[] samples = decoder.decode(packet);
                    if (samples == null || samples.length != 1920)
                    {
                        failure.set("MP3 Opus output could not be decoded");
                        return false;
                    }
                    for (short sample : samples)
                    {
                        energy.addAndGet((long)sample * sample);
                    }
                    packets.incrementAndGet();
                    return true;
                }

                @Override
                public void onFinished(String error)
                {
                    if (error != null)
                    {
                        failure.set(error);
                    }
                    finished.countDown();
                }
            }))
            {
                stream.start();
                require(finished.await(15, TimeUnit.SECONDS), "MP3 stream did not finish");
                require(failure.get() == null, "MP3 stream failed: " + failure.get());
                require(packets.get() > 5 && energy.get() > 100000, "MP3 stream was silent");
                require(System.nanoTime() - start >= Math.max(0, packets.get() - 2) * 20000000L, "MP3 packets were sent without pacing");
            }
        }
        finally
        {
            context.getContentResolver().delete(uri, null, null);
        }
        return 13;
    }

    private static void checkQueuedPreferences(Context context) throws Exception
    {
        try (VoiceController voice = new VoiceController(context, (connectionId, message) -> true, (connectionId, status, ready, talking) ->
        {
        }))
        {
            voice.applyOptions(new AudioOptions("selected-microphone", "websocket", false));
            voice.setTransportPreference("webrtc");
            AtomicReference<?> saved = (AtomicReference<?>)read(voice, "requestedOptions");
            AudioOptions options = (AudioOptions)saved.get();
            require(options.microphoneDevice.equals("selected-microphone") && !options.soundEffects, "transport change overwrote queued microphone settings");
            require(options.transport.equals("webrtc"), "queued transport preference was not applied");
            require(read(voice, "rtcTimer") == null && read(voice, "healthTimer") == null, "inactive audio started periodic polling");
            AudioWorker worker = (AudioWorker)read(voice, "worker");
            ThreadPoolExecutor executor = (ThreadPoolExecutor)read(worker, "executor");
            ScheduledThreadPoolExecutor timer = (ScheduledThreadPoolExecutor)read(voice, "timer");
            require(executor.getPoolSize() == 0 && timer.getPoolSize() == 0, "inactive configuration started audio workers");
            CountDownLatch running = new CountDownLatch(1);
            CountDownLatch release = new CountDownLatch(1);
            worker.execute(() ->
            {
                running.countDown();
                try
                {
                    release.await(3, TimeUnit.SECONDS);
                }
                catch (InterruptedException error)
                {
                    Thread.currentThread().interrupt();
                }
            });
            require(running.await(2, TimeUnit.SECONDS), "audio worker did not start");
            AtomicInteger stale = new AtomicInteger();
            Method dispatch = VoiceController.class.getDeclaredMethod("dispatch", Runnable.class);
            dispatch.setAccessible(true);
            write(voice, "requestedConnectionId", 8L);
            require((Boolean)dispatch.invoke(voice, (Runnable)stale::incrementAndGet), "test callback was not queued");
            write(voice, "frameCounter", 13579L);
            write(voice, "receivedCounter", 24680L);
            write(voice, "counterConnectionId", 8L);
            voice.onDisconnected();
            CountDownLatch retired = new CountDownLatch(1);
            worker.executeRetirement(retired::countDown);
            release.countDown();
            require(retired.await(3, TimeUnit.SECONDS) && stale.get() == 0, "retired connection executed an old audio callback");
            require((Long)read(voice, "frameCounter") == 13579L && (Long)read(voice, "receivedCounter") == 24680L && (Long)read(voice, "counterConnectionId") == 8L, "idle teardown reset same-session media counters");
            timer.schedule(() ->
            {
            }, 1, TimeUnit.MILLISECONDS);
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(8);
            while ((executor.getPoolSize() != 0 || timer.getPoolSize() != 0) && System.nanoTime() < deadline)
            {
                Thread.sleep(20);
            }
            require(executor.getPoolSize() == 0 && timer.getPoolSize() == 0 && timer.getQueue().isEmpty(), "idle audio workers did not retire");
        }
    }

    private static Object read(Object object, String name) throws Exception
    {
        Field field = object.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(object);
    }

    private static void write(Object object, String name, Object value) throws Exception
    {
        Field field = object.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(object, value);
    }

    private static void require(boolean condition, String message)
    {
        if (!condition)
        {
            throw new AssertionError(message);
        }
    }
}
