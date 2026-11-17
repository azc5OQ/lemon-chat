package com.lemonchat.ui;

import android.app.Instrumentation;
import android.graphics.Bitmap;
import android.graphics.drawable.Drawable;
import com.lemonchat.model.ClientCapabilities;
import com.lemonchat.model.ServerEmoji;
import com.lemonchat.model.SessionSnapshot;
import java.io.ByteArrayOutputStream;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.Base64;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** checks real custom emoji decoding and cache ownership */
public final class EmojiImageChecks
{
    private int checks;

    public static int verify(Instrumentation instrumentation) throws Exception
    {
        EmojiImageChecks test = new EmojiImageChecks();
        test.run(instrumentation);
        return test.checks;
    }

    private void run(Instrumentation instrumentation) throws Exception
    {
        byte[] small = Base64.getDecoder().decode("R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==");
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        bytes.write(small, 0, small.length - 1);
        bytes.write(0x21);
        bytes.write(0xfe);
        for (int index = 0; index < 790; index++)
        {
            bytes.write(255);
            bytes.write(new byte[255]);
        }
        bytes.write(0);
        bytes.write(0x3b);
        String image = "data:image/gif;base64," + Base64.getEncoder().encodeToString(bytes.toByteArray());
        check(image.length() > 262144 && bytes.size() < 262144, "valid GIF source exceeds the old encoded-character limit");
        String id = "1".repeat(32);
        ServerEmoji emoji = new ServerEmoji(id, "custom_gif", "", image);
        CountDownLatch changed = new CountDownLatch(1);
        AtomicInteger requested = new AtomicInteger();
        EmojiImages[] cache = new EmojiImages[1];
        try
        {
            main(instrumentation, () ->
            {
                cache[0] = new EmojiImages(instrumentation.getTargetContext().getResources(), changed::countDown, ignored -> requested.incrementAndGet());
                cache[0].update(snapshot(1, Collections.singletonList(emoji)));
                cache[0].drawable(id);
            });
            check(changed.await(10, TimeUnit.SECONDS), "custom GIF is decoded on the image worker");
            main(instrumentation, () ->
            {
                Drawable decoded = cache[0].drawable(id);
                check(decoded != null && decoded.getIntrinsicWidth() > 0, "the large valid GIF produces a drawable");
                cache[0].update(snapshot(1, Collections.singletonList(new ServerEmoji(id, "custom_gif", "", ""))));
                check(cache[0].drawable(id) != null && requested.get() == 0, "encoded source eviction keeps its bounded thumbnail without another request");
                cache[0].update(snapshot(1, Collections.emptyList()));
                check(cache[0].drawable(id) == null, "catalog deletion removes the old thumbnail");
                cache[0].update(snapshot(2, Collections.singletonList(new ServerEmoji(id, "new_server_image", "", ""))));
                check(cache[0].drawable(id) == null && requested.get() == 1, "a replacement connection cannot reuse the previous server's emoji pixels");
                check(cache[0].drawable("unknown") == null && requested.get() == 1, "unknown image identifiers do not cause network requests");
            });
        }
        finally
        {
            main(instrumentation, () ->
            {
                if (cache[0] != null)
                {
                    cache[0].close();
                }
            });
        }
    }

    private SessionSnapshot snapshot(long connection, List<ServerEmoji> emojis)
    {
        return new SessionSnapshot(SessionSnapshot.Status.CONNECTED, "", 1, 0, "You", true, Collections.emptyList(), Collections.emptyList(), Collections.emptyList(), connection, false, true, null, false, Collections.emptyList(), emojis, ClientCapabilities.none());
    }

    private void main(Instrumentation instrumentation, Runnable action) throws Exception
    {
        Throwable[] failure = new Throwable[1];
        instrumentation.runOnMainSync(() ->
        {
            try
            {
                action.run();
            }
            catch (Throwable error)
            {
                failure[0] = error;
            }
        });
        if (failure[0] != null)
        {
            throw new AssertionError("Custom emoji image check failed", failure[0]);
        }
    }

    private void check(boolean condition, String message)
    {
        if (!condition)
        {
            throw new AssertionError(message);
        }
        checks++;
    }
}
