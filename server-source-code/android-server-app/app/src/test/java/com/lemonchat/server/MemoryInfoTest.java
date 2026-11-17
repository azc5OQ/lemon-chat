package com.lemonchat.server;

import org.junit.Test;
import static org.junit.Assert.*;

public class MemoryInfoTest
{
    @Test public void ImportedChannelCapacityChangesTheEstimate()
    {
        long previousFixed = MemoryInfo.fixedBytes;
        long previousSlot = MemoryInfo.slotBytes;
        long previousChannel = MemoryInfo.channelBytes;
        try
        {
            MemoryInfo.fixedBytes = 4096;
            MemoryInfo.slotBytes = 2048;
            MemoryInfo.channelBytes = 8192;
            assertEquals(4096L + 500L * 2048 + 300L * 8192, MemoryInfo.Estimate(500, 300));
            assertEquals(200L * 8192, MemoryInfo.Estimate(500, 300) - MemoryInfo.Estimate(500, 100));
            assertTrue(MemoryInfo.Estimate(500, Integer.MAX_VALUE) > Integer.MAX_VALUE);
        }
        finally
        {
            MemoryInfo.fixedBytes = previousFixed;
            MemoryInfo.slotBytes = previousSlot;
            MemoryInfo.channelBytes = previousChannel;
        }
    }
}
