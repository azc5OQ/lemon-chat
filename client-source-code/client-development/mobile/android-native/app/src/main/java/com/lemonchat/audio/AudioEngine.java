package com.lemonchat.audio;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTrack;
import android.media.MediaRecorder;
import android.media.audiofx.AcousticEchoCanceler;
import android.os.Process;
import android.os.PowerManager;
import android.os.SystemClock;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * captures voice and mixes bounded Opus queues without touching protocol state
 */
final class AudioEngine implements AutoCloseable
{
    interface Listener
    {
        void onEncoded(byte[] packet, long captureId);
        void onError(String message);
        void onFocusChanged(boolean available, boolean permanent);
    }

    private static final int RATE = 48000;
    private static final int FRAME = 960;
    private static final int MAX_SOURCES = 32;
    private static final int MAX_PACKETS = 12;
    private final Context context;
    private final Listener listener;
    private final AudioManager manager;
    private final AudioFocusRequest focus;
    private final PowerManager.WakeLock wakeLock;
    private final Object captureLock = new Object();
    private final Map<Integer, Source> sources = new LinkedHashMap<>();
    private final Map<Integer, Float> gains = new LinkedHashMap<>();
    private volatile String microphoneDevice = "";
    private final Thread captureThread;
    private final Thread playbackThread;
    private volatile boolean running = true;
    private volatile boolean capturing;
    private volatile boolean focused;
    private volatile long playbackGeneration;
    private volatile long captureId;
    private volatile AudioRecord recorder;
    private volatile AudioTrack player;
    private final int previousMode;
    private final AtomicBoolean closed = new AtomicBoolean();
    private long wakeRenewAfter;

    private static final class Source implements AutoCloseable
    {
        final OpusCodec decoder = new OpusCodec(false, RATE, 2);
        final ArrayDeque<byte[]> packets = new ArrayDeque<>();
        short[] pcm;
        int offset;
        long lastPacket;
        long firstPacket;
        int sequence = -1;
        float gain = 1;

        /**
         * releases the decoder and queued samples
         */
        @Override
        public void close()
        {
            decoder.close();
            packets.clear();
            pcm = null;
        }
    }

    /**
     * starts the audio workers after the media socket is authenticated
     *
     * @param context application context
     * @param listener encoded audio and device status callbacks
     */
    AudioEngine(Context context, Listener listener)
    {
        this.context = context.getApplicationContext();
        this.listener = listener;
        manager = (AudioManager)this.context.getSystemService(Context.AUDIO_SERVICE);
        if (manager == null)
        {
            throw new IllegalStateException("Audio is unavailable on this device");
        }
        PowerManager power = (PowerManager)this.context.getSystemService(Context.POWER_SERVICE);
        if (power == null)
        {
            throw new IllegalStateException("Audio power management is unavailable");
        }
        wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "lemonchat:voice");
        wakeLock.setReferenceCounted(false);
        previousMode = manager.getMode();
        AudioAttributes attributes = new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION).setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build();
        focus = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT).setAudioAttributes(attributes).setWillPauseWhenDucked(true).setOnAudioFocusChangeListener(this::handleFocus).build();
        if (manager.requestAudioFocus(focus) != AudioManager.AUDIOFOCUS_REQUEST_GRANTED)
        {
            throw new IllegalStateException("Another app is using audio");
        }
        try
        {
            focused = true;
            manager.setMode(AudioManager.MODE_IN_COMMUNICATION);
            keepAwake();
        }
        catch (RuntimeException failure)
        {
            focused = false;
            releaseWakeLock();
            restoreMode();
            manager.abandonAudioFocusRequest(focus);
            throw failure;
        }
        captureThread = new Thread(this::captureLoop, "native-microphone");
        playbackThread = new Thread(this::playbackLoop, "native-speaker");
        captureThread.start();
        playbackThread.start();
    }

    /**
     * pauses devices during interruptions without resuming a microphone press
     *
     * @param change Android audio focus change
     */
    private synchronized void handleFocus(int change)
    {
        if (!running)
        {
            return;
        }
        if (change < 0)
        {
            focused = false;
            stopCapture();
            clearPlayback();
            releaseWakeLock();
            restoreMode();
            listener.onFocusChanged(false, change == AudioManager.AUDIOFOCUS_LOSS);
        }
        else if (change == AudioManager.AUDIOFOCUS_GAIN)
        {
            try
            {
                manager.setMode(AudioManager.MODE_IN_COMMUNICATION);
                focused = true;
                keepAwake();
                synchronized (sources)
                {
                    sources.notifyAll();
                }
                listener.onFocusChanged(true, false);
            }
            catch (RuntimeException failure)
            {
                focused = false;
                releaseWakeLock();
                restoreMode();
                listener.onError("Audio could not resume");
            }
        }
    }

    /**
     * checks current audio focus without requesting it
     *
     * @return whether Android currently allows playback and capture
     */
    boolean hasFocus()
    {
        return running && focused;
    }

    /**
     * renews the finite CPU lease while voice remains connected
     */
    synchronized void keepAwake()
    {
        long now = SystemClock.elapsedRealtime();
        if (running && focused && (now >= wakeRenewAfter || !wakeLock.isHeld()))
        {
            wakeLock.acquire(60000);
            wakeRenewAfter = now + 20000;
        }
    }

    /**
     * releases the CPU lease after interruption or disconnect
     */
    private synchronized void releaseWakeLock()
    {
        if (wakeLock.isHeld())
        {
            wakeLock.release();
        }
        wakeRenewAfter = 0;
    }

    /**
     * restores the mode only while this app still owns communication mode
     */
    private void restoreMode()
    {
        if (manager.getMode() == AudioManager.MODE_IN_COMMUNICATION)
        {
            manager.setMode(previousMode);
        }
    }

    /**
     * begins capture only after the server grants speaking state
     */
    void startCapture()
    {
        synchronized (captureLock)
        {
            if (running && focused && !capturing)
            {
                captureId++;
                capturing = true;
                captureLock.notifyAll();
            }
        }
    }

    /**
     * stops microphone reads and retires queued packets immediately
     */
    void stopCapture()
    {
        synchronized (captureLock)
        {
            capturing = false;
            captureId++;
            AudioRecord active = recorder;
            if (active != null)
            {
                try
                {
                    active.stop();
                }
                catch (IllegalStateException ignored)
                {
                }
            }
            captureLock.notifyAll();
        }
    }

    /**
     * checks whether an encoded packet belongs to the current press
     *
     * @param id capture generation attached to the packet
     * @return true while this capture remains active
     */
    boolean isCurrentCapture(long id)
    {
        return running && focused && capturing && captureId == id;
    }

    /**
     * queues fresh audio while limiting memory and playback delay
     *
     * @param sender authenticated sender ID
     * @param sequence unsigned sequence from the decrypted packet
     * @param packet Opus data without the sequence bytes
     */
    void receive(int sender, int sequence, byte[] packet)
    {
        if (!running || !focused || packet == null || packet.length < 1 || packet.length > 3838)
        {
            return;
        }
        synchronized (sources)
        {
            Source source = sources.get(sender);
            if (source == null)
            {
                if (sources.size() >= MAX_SOURCES)
                {
                    return;
                }
                source = new Source();
                source.gain = gains.getOrDefault(sender, 1f);
                sources.put(sender, source);
            }
            int difference = (sequence - source.sequence) & 0xffff;
            if (source.sequence >= 0 && (difference == 0 || difference >= 0x8000))
            {
                return;
            }
            source.sequence = sequence;
            source.lastPacket = SystemClock.elapsedRealtime();
            if (source.packets.isEmpty() && source.pcm == null)
            {
                source.firstPacket = source.lastPacket;
            }
            if (source.packets.size() >= MAX_PACKETS)
            {
                source.packets.clear();
                source.pcm = null;
                source.offset = 0;
            }
            source.packets.addLast(packet.clone());
            sources.notifyAll();
        }
    }

    /**
     * removes buffered audio when the channel changes
     */
    void clearPlayback()
    {
        synchronized (sources)
        {
            playbackGeneration++;
            for (Source source : sources.values())
            {
                source.close();
            }
            sources.clear();
            sources.notifyAll();
        }
        AudioTrack active = player;
        if (active != null)
        {
            try
            {
                active.pause();
                active.flush();
                if (running && focused)
                {
                    active.play();
                }
            }
            catch (IllegalStateException ignored)
            {
            }
        }
    }

    /**
     * discards one sender after a stream stops
     *
     * @param sender authenticated sender ID
     */
    void discard(int sender)
    {
        synchronized (sources)
        {
            Source source = sources.remove(sender);
            if (source != null)
            {
                source.close();
            }
        }
    }

    void setMicrophoneDevice(String key)
    {
        if (!microphoneDevice.equals(key))
        {
            stopCapture();
            microphoneDevice = key;
        }
    }

    void setGain(int sender, float gain)
    {
        float bounded = Float.isFinite(gain) ? Math.max(0, Math.min(2, gain)) : 1;
        synchronized (sources)
        {
            if (gains.size() < 1024 || gains.containsKey(sender))
            {
                gains.put(sender, bounded);
            }
            Source source = sources.get(sender);
            if (source != null)
            {
                source.gain = bounded;
            }
        }
    }

    void forgetGain(int sender)
    {
        synchronized (sources)
        {
            gains.remove(sender);
        }
    }

    /**
     * captures complete frames and releases devices between presses
     */
    private void captureLoop()
    {
        Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);
        while (running)
        {
            long id;
            synchronized (captureLock)
            {
                while (running && !capturing)
                {
                    try
                    {
                        captureLock.wait();
                    }
                    catch (InterruptedException ignored)
                    {
                    }
                }
                if (!running)
                {
                    break;
                }
                id = captureId;
            }
            AudioRecord input = null;
            AcousticEchoCanceler echo = null;
            try
            {
                input = openRecorder();
                int samples = input.getSampleRate() / 50;
                short[] pcm = new short[samples];
                synchronized (captureLock)
                {
                    if (!isCurrentCapture(id))
                    {
                        continue;
                    }
                    recorder = input;
                    input.startRecording();
                }
                if (AcousticEchoCanceler.isAvailable())
                {
                    try
                    {
                        echo = AcousticEchoCanceler.create(input.getAudioSessionId());
                        if (echo != null)
                        {
                            echo.setEnabled(true);
                        }
                    }
                    catch (RuntimeException ignored)
                    {
                        if (echo != null)
                        {
                            echo.release();
                            echo = null;
                        }
                    }
                }
                try (OpusCodec encoder = new OpusCodec(true, input.getSampleRate(), 1))
                {
                    int filled = 0;
                    while (isCurrentCapture(id))
                    {
                        int count = input.read(pcm, filled, samples - filled, AudioRecord.READ_BLOCKING);
                        if (count <= 0)
                        {
                            if (isCurrentCapture(id))
                            {
                                throw new IllegalStateException("Microphone recording stopped");
                            }
                            break;
                        }
                        filled += count;
                        if (filled == samples)
                        {
                            byte[] encoded = encoder.encode(pcm, samples);
                            filled = 0;
                            if (encoded != null && isCurrentCapture(id))
                            {
                                listener.onEncoded(encoded, id);
                            }
                        }
                    }
                }
            }
            catch (SecurityException denied)
            {
                if (isCurrentCapture(id))
                {
                    stopCapture();
                    listener.onError("Microphone permission is required");
                }
            }
            catch (RuntimeException failure)
            {
                if (isCurrentCapture(id))
                {
                    stopCapture();
                    listener.onError(failure.getMessage() == null ? "Microphone unavailable" : failure.getMessage());
                }
            }
            finally
            {
                synchronized (captureLock)
                {
                    if (recorder == input)
                    {
                        recorder = null;
                    }
                    if (echo != null)
                    {
                        echo.release();
                    }
                    if (input != null)
                    {
                        input.release();
                    }
                }
            }
        }
    }

    /**
     * opens the best supported mono recording rate
     *
     * @return initialized recorder
     * @throws SecurityException when microphone permission is missing or revoked
     */
    private AudioRecord openRecorder()
    {
        for (int rate : new int[] {48000, 16000, 8000})
        {
            int minimum = AudioRecord.getMinBufferSize(rate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
            if (minimum <= 0)
            {
                continue;
            }
            if (context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED)
            {
                throw new SecurityException("Microphone permission is required");
            }
            AudioRecord candidate;
            try
            {
                candidate = new AudioRecord(MediaRecorder.AudioSource.VOICE_COMMUNICATION, rate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, Math.max(minimum, rate / 10 * 2));
            }
            catch (IllegalArgumentException unsupported)
            {
                continue;
            }
            if (candidate.getState() == AudioRecord.STATE_INITIALIZED)
            {
                if (!microphoneDevice.isEmpty())
                {
                    android.media.AudioDeviceInfo preferred = AudioOptions.findMicrophone(context, microphoneDevice);
                    if (preferred == null || !candidate.setPreferredDevice(preferred))
                    {
                        candidate.release();
                        throw new IllegalStateException("The selected microphone is unavailable. Choose another input in Local settings.");
                    }
                }
                return candidate;
            }
            candidate.release();
        }
        throw new IllegalStateException("No supported microphone format");
    }

    /**
     * mixes all active speakers into one stereo output stream
     */
    private void playbackLoop()
    {
        Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);
        while (running)
        {
            AudioTrack output = null;
            try
            {
                synchronized (sources)
                {
                    while (running && !focused)
                    {
                        sources.wait();
                    }
                }
                if (!running)
                {
                    break;
                }
                long generation = playbackGeneration;
                int minimum = AudioTrack.getMinBufferSize(RATE, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
                if (minimum <= 0)
                {
                    throw new IllegalStateException("Speaker playback is unavailable");
                }
                output = new AudioTrack.Builder().setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION).setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build()).setAudioFormat(new AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT).setSampleRate(RATE).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build()).setBufferSizeInBytes(Math.max(minimum, FRAME * 4 * 3)).setTransferMode(AudioTrack.MODE_STREAM).build();
                if (output.getState() != AudioTrack.STATE_INITIALIZED)
                {
                    throw new IllegalStateException("Speaker playback could not start");
                }
                player = output;
                if (focused && generation == playbackGeneration)
                {
                    output.play();
                }
                int[] mix = new int[FRAME * 2];
                short[] pcm = new short[FRAME * 2];
                while (running && focused && generation == playbackGeneration)
                {
                    Arrays.fill(mix, 0);
                    boolean available = mixFrame(mix);
                    if (!available)
                    {
                        synchronized (sources)
                        {
                            sources.wait(20);
                        }
                        continue;
                    }
                    for (int index = 0; index < pcm.length; index++)
                    {
                        pcm[index] = (short)Math.max(Short.MIN_VALUE, Math.min(Short.MAX_VALUE, mix[index]));
                    }
                    int offset = 0;
                    while (running && focused && generation == playbackGeneration && offset < pcm.length)
                    {
                        int count = output.write(pcm, offset, pcm.length - offset, AudioTrack.WRITE_NON_BLOCKING);
                        if (count < 0)
                        {
                            throw new IllegalStateException("Speaker playback stopped");
                        }
                        if (count == 0)
                        {
                            Thread.sleep(5);
                        }
                        offset += count;
                    }
                }
            }
            catch (InterruptedException ignored)
            {
                break;
            }
            catch (RuntimeException failure)
            {
                if (running)
                {
                    listener.onError(failure.getMessage() == null ? "Speaker unavailable" : failure.getMessage());
                }
                break;
            }
            finally
            {
                player = null;
                if (output != null)
                {
                    output.release();
                }
                synchronized (sources)
                {
                    for (Source source : sources.values())
                    {
                        source.close();
                    }
                    sources.clear();
                }
            }
        }
    }

    /**
     * consumes one playback frame while expiring inactive decoders
     *
     * @param mix stereo accumulator
     * @return true when at least one source contributed samples
     */
    private boolean mixFrame(int[] mix)
    {
        boolean available = false;
        long now = SystemClock.elapsedRealtime();
        synchronized (sources)
        {
            Iterator<Source> iterator = sources.values().iterator();
            while (iterator.hasNext())
            {
                Source source = iterator.next();
                if (now - source.lastPacket > 2000)
                {
                    source.close();
                    iterator.remove();
                    continue;
                }
                if (source.pcm == null && source.packets.size() < 2 && now - source.firstPacket < 40)
                {
                    continue;
                }
                int outputOffset = 0;
                while (outputOffset < mix.length)
                {
                    if (source.pcm == null)
                    {
                        byte[] packet = source.packets.pollFirst();
                        if (packet == null)
                        {
                            break;
                        }
                        source.pcm = source.decoder.decode(packet);
                        source.offset = 0;
                        if (source.pcm == null)
                        {
                            continue;
                        }
                    }
                    int count = Math.min(mix.length - outputOffset, source.pcm.length - source.offset);
                    for (int index = 0; index < count; index++)
                    {
                        mix[outputOffset + index] += Math.round(source.pcm[source.offset + index] * source.gain);
                    }
                    available = true;
                    outputOffset += count;
                    source.offset += count;
                    if (source.offset == source.pcm.length)
                    {
                        source.pcm = null;
                    }
                }
            }
        }
        return available;
    }

    /**
     * stops workers before releasing audio focus
     */
    @Override
    public synchronized void close()
    {
        if (!closed.compareAndSet(false, true))
        {
            return;
        }
        running = false;
        focused = false;
        stopCapture();
        captureThread.interrupt();
        playbackThread.interrupt();
        AudioTrack active = player;
        if (active != null)
        {
            try
            {
                active.pause();
                active.flush();
            }
            catch (IllegalStateException ignored)
            {
            }
        }
        try
        {
            captureThread.join(1000);
            playbackThread.join(1000);
        }
        catch (InterruptedException ignored)
        {
            Thread.currentThread().interrupt();
        }
        releaseWakeLock();
        manager.abandonAudioFocusRequest(focus);
        restoreMode();
    }
}
