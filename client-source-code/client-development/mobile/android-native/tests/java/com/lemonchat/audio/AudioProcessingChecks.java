package com.lemonchat.audio;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** checks music resampling and the real stereo Opus encoder */
public final class AudioProcessingChecks
{
    public static void main(String[] arguments) throws Exception
    {
        for (int rate : new int[] {8000, 16000, 22050, 44100, 48000, 96000})
        {
            short[] input = new short[rate / 10 * 2];
            for (int index = 0; index < input.length / 2; index++)
            {
                input[index * 2] = (short)(Math.sin(2 * Math.PI * 440 * index / rate) * 10000);
                input[index * 2 + 1] = (short)-input[index * 2];
            }
            List<Short> complete = new ArrayList<>();
            List<Short> chunked = new ArrayList<>();
            PcmResampler first = new PcmResampler(rate, 2);
            PcmResampler second = new PcmResampler(rate, 2);
            first.accept(input, input.length, (left, right) ->
            {
                complete.add(left);
                complete.add(right);
            });
            for (int offset = 0; offset < input.length; offset += 74)
            {
                short[] part = Arrays.copyOfRange(input, offset, Math.min(input.length, offset + 74));
                second.accept(part, part.length, (left, right) ->
                {
                    chunked.add(left);
                    chunked.add(right);
                });
            }
            require(complete.equals(chunked), "decoder chunk boundaries changed resampling");
            require(Math.abs(complete.size() - 9600) <= 12, "resampled duration changed");
            for (int offset = 0; offset < complete.size(); offset += 2)
            {
                require(Math.abs(complete.get(offset) + complete.get(offset + 1)) <= 1, "stereo channels were mixed");
            }
        }
        try (OpusCodec encoder = new OpusCodec(true, 48000, 2); OpusCodec decoder = new OpusCodec(false, 48000, 2))
        {
            long energy = 0;
            long difference = 0;
            for (int frame = 0; frame < 12; frame++)
            {
                short[] input = new short[1920];
                for (int index = 0; index < 960; index++)
                {
                    input[index * 2] = (short)(Math.sin(2 * Math.PI * 440 * (frame * 960 + index) / 48000) * 10000);
                    input[index * 2 + 1] = (short)(Math.sin(2 * Math.PI * 880 * (frame * 960 + index) / 48000) * 10000);
                }
                short[] pcm = decoder.decode(encoder.encode(input, 960));
                require(pcm != null && pcm.length == 1920, "stereo packet duration changed");
                for (int index = 0; index < pcm.length; index += 2)
                {
                    energy += (long)pcm[index] * pcm[index];
                    difference += Math.abs(pcm[index] - pcm[index + 1]);
                }
            }
            require(energy > 100000000 && difference > 100000, "stereo Opus lost the music signal");
            require(encoder.encode(new short[960], 960) == null, "stereo encoder read a short buffer");
        }
        System.out.println("PASS: six music resampling rates, chunk continuity, channel separation, stereo Opus and PCM bounds");
    }

    private static void require(boolean value, String message)
    {
        if (!value)
        {
            throw new AssertionError(message);
        }
    }
}
