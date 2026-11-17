package com.lemonchat.server;

import org.json.JSONObject;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import java.util.concurrent.TimeUnit;

/** reads array sizes from the executable built for this phone */
final class MemoryInfo
{
    static volatile long slotBytes;
    static volatile long fixedBytes;
    static volatile long channelBytes;
    static volatile int defaultChannels = 100;

    static void Load(File executable)
    {
        Process query = null;
        try
        {
            query = new ProcessBuilder(executable.getAbsolutePath(), "--memory-info").redirectErrorStream(true).start();
            query.getOutputStream().close();
            if (!query.waitFor(5, TimeUnit.SECONDS) || query.exitValue() != 0)
            {
                return;
            }
            byte[] buffer = new byte[4096];
            int count = query.getInputStream().read(buffer);
            if (count <= 0)
            {
                return;
            }
            JSONObject json = new JSONObject(new String(buffer, 0, count, StandardCharsets.UTF_8));
            fixedBytes = json.getLong("fixed_bytes");
            channelBytes = json.getLong("channel_bytes");
            defaultChannels = json.getInt("default_channels");
            slotBytes = json.getLong("slot_bytes");
        }
        catch (Exception ignored)
        {
        }
        finally
        {
            if (query != null)
            {
                query.destroyForcibly();
            }
        }
    }

    /**
     * calculates reserved array memory for the saved capacity
     *
     * @param slots reserved client slots
     * @param channels saved channel capacity
     * @return estimated bytes before active connections and media
     */
    static long Estimate(int slots, int channels)
    {
        return fixedBytes + slotBytes * slots + channelBytes * channels;
    }

    /**
     * explains the startup memory estimate
     *
     * @param slots reserved client slots
     * @param channels saved channel capacity
     * @return readable estimate or an unavailable notice
     */
    static String Describe(int slots, int channels)
    {
        if (slotBytes == 0)
        {
            return "Memory estimate unavailable. More slots reserve more RAM at startup; use a small number for a " +
                   "phone.";
        }
        return String.format(Locale.getDefault(),
                             "Each reserved slot uses about %.1f KiB. %,d slots, %,d channel entries and fixed tables reserve at least %.1f " +
                             "MiB. Active clients, audio/video and Android need extra RAM.",
                             slotBytes / 1024.0, slots, channels, Estimate(slots, channels) / 1048576.0);
    }
}
