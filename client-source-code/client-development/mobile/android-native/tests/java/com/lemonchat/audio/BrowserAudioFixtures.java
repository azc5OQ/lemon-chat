package com.lemonchat.audio;

import com.lemonchat.crypto.CryptoSession;
import com.lemonchat.crypto.Identity;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Base64;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * produces native encrypted Opus packets for the browser's real decoder
 */
public final class BrowserAudioFixtures
{
    /**
     * writes counter boundaries that must not pass through a JavaScript number
     *
     * @param arguments fixture output path
     * @throws Exception when fixture generation fails
     */
    public static void main(String[] arguments) throws Exception
    {
        JSONArray fixtures = new JSONArray();
        long[] counters = {1, (1L << 48) + 7, (1L << 53) + 1, Long.MAX_VALUE, Long.MIN_VALUE + 9, -1L};
        try (CryptoSession crypto = new CryptoSession(Identity.create(2048), ""); OpusCodec encoder = new OpusCodec(true, 48000, 1))
        {
            String keys = crypto.newContentKeys();
            for (int sequence = 0; sequence < counters.length; sequence++)
            {
                short[] pcm = new short[960];
                for (int index = 0; index < pcm.length; index++)
                {
                    pcm[index] = (short)(Math.sin(2 * Math.PI * 440 * (sequence * 960 + index) / 48000) * 10000);
                }
                byte[] opus = encoder.encode(pcm, pcm.length);
                byte[] content = new byte[opus.length + 2];
                content[0] = (byte)sequence;
                content[1] = (byte)(sequence >>> 8);
                System.arraycopy(opus, 0, content, 2, opus.length);
                byte[] encrypted = CryptoSession.contentMediaLayer(keys, 41, counters[sequence], content, true);
                fixtures.put(new JSONObject().put("sender", 41).put("counter", Long.toUnsignedString(counters[sequence])).put("keys", new JSONArray(keys)).put("plain", Base64.getEncoder().encodeToString(content)).put("encrypted", Base64.getEncoder().encodeToString(encrypted)));
            }
        }
        Files.writeString(Path.of(arguments[0]), fixtures.toString());
        System.out.println("PASS: native encrypted Opus fixtures at six unsigned counter boundaries");
    }
}
