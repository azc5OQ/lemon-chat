package com.lemonchat.audio;

import com.lemonchat.crypto.CryptoException;
import com.lemonchat.crypto.CryptoSession;
import com.lemonchat.crypto.MediaCrypto;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

/** checks independent media keys using a real authenticated connection */
final class MediaSnapshotChecks
{
    private MediaSnapshotChecks()
    {
    }

    static void verify(CryptoSession chat, MediaCrypto media, byte[] received, int sender) throws Exception
    {
        byte[] content = new byte[]{1, 2, 3, 4};
        byte[] expected = chat.sealMedia(sender, 777, content);
        require(Arrays.equals(expected, media.sealMedia(sender, 777, content)), "snapshot changed the media wire format");
        synchronized (chat)
        {
            CompletableFuture<byte[]> independent = CompletableFuture.supplyAsync(() -> media.sealMedia(sender, 777, content));
            require(Arrays.equals(expected, independent.get(2, TimeUnit.SECONDS)), "media waits for the chat monitor");
        }
        Method seal = MediaCrypto.class.getDeclaredMethod("nativeSeal", long.class, int.class, long.class, byte[].class);
        Method open = MediaCrypto.class.getDeclaredMethod("nativeOpen", long.class, byte[].class);
        Method destroy = MediaCrypto.class.getDeclaredMethod("nativeDestroy", long.class);
        seal.setAccessible(true);
        open.setAccessible(true);
        destroy.setAccessible(true);
        for (long handle : new long[]{0, -1, Long.MAX_VALUE})
        {
            require(seal.invoke(null, handle, sender, 1L, content) == null, "invalid native handle accepted for sealing");
            require(open.invoke(null, handle, received) == null, "invalid native handle accepted for opening");
            destroy.invoke(null, handle);
        }
        rejects(() -> media.sealMedia(-1, 1, content), "negative sender");
        rejects(() -> media.sealMedia(sender, 1, null), "null content");
        rejects(() -> media.sealMedia(sender, 1, new byte[2]), "short content");
        rejects(() -> media.sealMedia(sender, 1, new byte[3841]), "oversized content");
        rejects(() -> media.openMedia(null), "null frame");
        rejects(() -> media.openMedia(new byte[31]), "short frame");
        rejects(() -> media.openMedia(new byte[3870]), "oversized frame");
        byte[] altered = received.clone();
        altered[altered.length - 1] ^= 1;
        rejects(() -> media.openMedia(altered), "tampered media");
        List<MediaCrypto> copies = new ArrayList<>();
        try
        {
            for (int index = 0; index < 65; index++)
            {
                try
                {
                    copies.add(chat.createMediaCrypto());
                }
                catch (CryptoException full)
                {
                    break;
                }
            }
            require(!copies.isEmpty() && copies.size() < 64, "media registry is not bounded");
        }
        finally
        {
            for (MediaCrypto copy : copies)
            {
                copy.close();
                copy.close();
            }
        }
        MediaCrypto retired = chat.createMediaCrypto();
        Field handleField = MediaCrypto.class.getDeclaredField("handle");
        handleField.setAccessible(true);
        long retiredHandle = handleField.getLong(retired);
        retired.close();
        require(seal.invoke(null, retiredHandle, sender, 1L, content) == null, "retired native handle remained usable");
        destroy.invoke(null, retiredHandle);
        rejects(() -> retired.sealMedia(sender, 1, content), "closed Java media wrapper");
        try (MediaCrypto fresh = chat.createMediaCrypto())
        {
            require(handleField.getLong(fresh) != retiredHandle, "retired handle was reused");
        }
        CryptoSession.MediaPacket before = media.openMedia(received);
        chat.close();
        rejects(chat::createMediaCrypto, "snapshot after chat close");
        require(Arrays.equals(expected, media.sealMedia(sender, 777, content)), "chat close invalidated copied send keys");
        CryptoSession.MediaPacket after = media.openMedia(received);
        require(before.sender == after.sender && before.senderCounter == after.senderCounter && before.frameCounter == after.frameCounter && Arrays.equals(before.content, after.content), "chat close invalidated copied receive keys");
        System.out.println("PASS: media snapshot wire equivalence, independent chat lock, bounded handles, invalid input, retirement, tamper rejection and media after chat close");
    }

    private static void rejects(Runnable operation, String name)
    {
        try
        {
            operation.run();
        }
        catch (CryptoException expected)
        {
            return;
        }
        throw new AssertionError("Accepted " + name);
    }

    private static void require(boolean value, String name)
    {
        if (!value)
        {
            throw new AssertionError(name);
        }
    }
}
