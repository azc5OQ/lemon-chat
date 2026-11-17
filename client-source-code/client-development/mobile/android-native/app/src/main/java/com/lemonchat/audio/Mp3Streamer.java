package com.lemonchat.audio;

import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.media.AudioFormat;
import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.net.Uri;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicBoolean;

/** decodes a selected MP3 in bounded chunks and paces encrypted stream input */
final class Mp3Streamer implements AutoCloseable
{
    interface Listener
    {
        boolean onReady();
        boolean onPacket(byte[] packet);
        void onFinished(String error);
    }

    private final Context context;
    private final Uri uri;
    private final Listener listener;
    private final AtomicBoolean stopped = new AtomicBoolean();
    private final Thread worker;
    private final short[] frame = new short[1920];
    private int frameOffset;
    private long sentFrames;
    private long startedAt;

    Mp3Streamer(Context context, Uri uri, Listener listener)
    {
        if (uri == null || !"content".equals(uri.getScheme()))
        {
            throw new IllegalArgumentException("Select an MP3 from the file picker");
        }
        this.context = context.getApplicationContext();
        this.uri = uri;
        this.listener = listener;
        worker = new Thread(this::run, "native-mp3-stream");
    }

    void start()
    {
        worker.start();
    }

    private void run()
    {
        MediaExtractor extractor = new MediaExtractor();
        MediaCodec decoder = null;
        String failure = null;
        try (AssetFileDescriptor file = context.getContentResolver().openAssetFileDescriptor(uri, "r"); OpusCodec encoder = new OpusCodec(true, 48000, 2))
        {
            if (file == null || file.getLength() > 40L * 1024 * 1024)
            {
                throw new IllegalArgumentException("Choose an MP3 no larger than 40 MiB");
            }
            if (file.getLength() >= 0)
            {
                extractor.setDataSource(file.getFileDescriptor(), file.getStartOffset(), file.getLength());
            }
            else
            {
                extractor.setDataSource(file.getFileDescriptor());
            }
            MediaFormat format = null;
            for (int index = 0; index < extractor.getTrackCount(); index++)
            {
                MediaFormat candidate = extractor.getTrackFormat(index);
                if ("audio/mpeg".equals(candidate.getString(MediaFormat.KEY_MIME)))
                {
                    format = candidate;
                    extractor.selectTrack(index);
                    break;
                }
            }
            if (format == null)
            {
                throw new IllegalArgumentException("The selected file is not an MP3");
            }
            format.setInteger(MediaFormat.KEY_PCM_ENCODING, AudioFormat.ENCODING_PCM_16BIT);
            decoder = MediaCodec.createDecoderByType("audio/mpeg");
            decoder.configure(format, null, null, 0);
            decoder.start();
            if (!listener.onReady())
            {
                return;
            }
            startedAt = System.nanoTime();
            PcmResampler resampler = new PcmResampler(format.getInteger(MediaFormat.KEY_SAMPLE_RATE), format.getInteger(MediaFormat.KEY_CHANNEL_COUNT));
            MediaCodec.BufferInfo information = new MediaCodec.BufferInfo();
            boolean inputEnded = false;
            boolean outputEnded = false;
            long compressedBytes = 0;
            long lastOutput = System.nanoTime();
            while (!stopped.get() && !outputEnded)
            {
                if (!inputEnded)
                {
                    int index = decoder.dequeueInputBuffer(10000);
                    if (index >= 0)
                    {
                        ByteBuffer input = decoder.getInputBuffer(index);
                        int size = extractor.readSampleData(input, 0);
                        if (size < 0)
                        {
                            decoder.queueInputBuffer(index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inputEnded = true;
                        }
                        else
                        {
                            compressedBytes += size;
                            if (compressedBytes > 40L * 1024 * 1024)
                            {
                                throw new IllegalArgumentException("The MP3 exceeds 40 MiB");
                            }
                            decoder.queueInputBuffer(index, 0, size, extractor.getSampleTime(), 0);
                            extractor.advance();
                        }
                    }
                }
                int index = decoder.dequeueOutputBuffer(information, 10000);
                if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED)
                {
                    MediaFormat output = decoder.getOutputFormat();
                    if (output.containsKey(MediaFormat.KEY_PCM_ENCODING) && output.getInteger(MediaFormat.KEY_PCM_ENCODING) != AudioFormat.ENCODING_PCM_16BIT)
                    {
                        throw new IllegalArgumentException("The MP3 decoder did not provide 16 bit audio");
                    }
                    resampler = new PcmResampler(output.getInteger(MediaFormat.KEY_SAMPLE_RATE), output.getInteger(MediaFormat.KEY_CHANNEL_COUNT));
                }
                else if (index >= 0)
                {
                    try
                    {
                        if (information.size < 0 || information.size > 262144 || information.size % 2 != 0)
                        {
                            throw new IllegalArgumentException("The MP3 decoder returned an invalid audio chunk");
                        }
                        ByteBuffer output = decoder.getOutputBuffer(index).duplicate().order(ByteOrder.LITTLE_ENDIAN);
                        output.position(information.offset);
                        output.limit(information.offset + information.size);
                        short[] samples = new short[information.size / 2];
                        output.asShortBuffer().get(samples);
                        resampler.accept(samples, samples.length, (left, right) -> sample(left, right, encoder));
                        lastOutput = System.nanoTime();
                        outputEnded = (information.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                    }
                    finally
                    {
                        decoder.releaseOutputBuffer(index, false);
                    }
                }
                if (System.nanoTime() - lastOutput > 10000000000L)
                {
                    throw new IllegalStateException("The MP3 decoder stopped producing audio");
                }
            }
            if (!stopped.get() && frameOffset > 0)
            {
                Arrays.fill(frame, frameOffset, frame.length, (short)0);
                send(encoder);
            }
        }
        catch (Exception error)
        {
            if (!stopped.get())
            {
                failure = error.getMessage() == null ? "The MP3 could not be streamed" : error.getMessage();
            }
        }
        finally
        {
            if (decoder != null)
            {
                try
                {
                    decoder.stop();
                }
                catch (RuntimeException ignored)
                {
                }
                try
                {
                    decoder.release();
                }
                catch (RuntimeException ignored)
                {
                }
            }
            try
            {
                extractor.release();
            }
            catch (RuntimeException ignored)
            {
            }
            listener.onFinished(failure);
        }
    }

    private void sample(short left, short right, OpusCodec encoder) throws Exception
    {
        if (stopped.get())
        {
            throw new InterruptedException();
        }
        frame[frameOffset++] = left;
        frame[frameOffset++] = right;
        if (frameOffset == frame.length)
        {
            send(encoder);
        }
    }

    private void send(OpusCodec encoder) throws Exception
    {
        long deadline = startedAt + sentFrames * 20000000L;
        while (!stopped.get() && System.nanoTime() < deadline)
        {
            Thread.sleep(Math.min(20, Math.max(1, (deadline - System.nanoTime()) / 1000000L)));
        }
        if (stopped.get())
        {
            throw new InterruptedException();
        }
        byte[] packet = encoder.encode(frame, 960);
        if (packet == null || !listener.onPacket(packet))
        {
            throw new IllegalStateException("The connection could not keep up with the music stream");
        }
        sentFrames++;
        frameOffset = 0;
        if (System.nanoTime() - deadline > 200000000L)
        {
            startedAt = System.nanoTime() - sentFrames * 20000000L;
        }
    }

    @Override
    public void close()
    {
        stopped.set(true);
        worker.interrupt();
    }
}
