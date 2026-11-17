package com.lemonchat.ui;

import android.app.Instrumentation;
import android.content.Context;
import android.graphics.Bitmap;
import android.net.Uri;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import com.lemonchat.model.Attachment;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.model.ReplyQuote;
import com.lemonchat.model.Member;
import com.lemonchat.model.SessionSnapshot;
import com.lemonchat.settings.LocalSettings;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicReference;

/** checks attachment bounds and actual native message rendering */
public final class ChatUiChecks
{
    private int checks;

    public static int verify(Instrumentation instrumentation) throws Exception
    {
        ChatUiChecks suite = new ChatUiChecks();
        Context context = instrumentation.getTargetContext();
        suite.images();
        suite.provider(context);
        suite.erasure(instrumentation);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        instrumentation.runOnMainSync(() ->
        {
            try
            {
                suite.views(context);
            }
            catch (Throwable error)
            {
                failure.set(error);
            }
        });
        if (failure.get() != null)
        {
            throw new AssertionError("Native chat view check failed", failure.get());
        }
        return suite.checks;
    }

    private void images()
    {
        check(AttachmentImages.decode(new byte[]{1, 2, 3}) == null, "invalid image bytes rejected");
        check(AttachmentImages.decode(new byte[Attachment.MAX_BYTES + 1]) == null, "encoded picture limit");
        Bitmap source = Bitmap.createBitmap(2400, 1200, Bitmap.Config.ARGB_8888);
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        source.compress(Bitmap.CompressFormat.PNG, 100, output);
        source.recycle();
        Bitmap decoded = AttachmentImages.decode(output.toByteArray());
        check(decoded != null && decoded.getWidth() <= 1024 && decoded.getWidth() == decoded.getHeight() * 2, "image preview preserves proportions and is sampled");
        if (decoded != null)
        {
            decoded.recycle();
        }
        source = Bitmap.createBitmap(8193, 1, Bitmap.Config.ARGB_8888);
        output.reset();
        source.compress(Bitmap.CompressFormat.PNG, 100, output);
        source.recycle();
        check(AttachmentImages.decode(output.toByteArray()) == null, "oversized picture dimensions rejected");
    }

    private void provider(Context context) throws Exception
    {
        Attachment selected = new Attachment("ui-check", "../../outside.txt", "text/plain", new byte[]{7, 8, 9}, false);
        Uri uri = AttachmentProvider.publish(context, selected);
        File file = new File(new File(context.getCacheDir(), "opened-attachments"), uri.getLastPathSegment() + ".bin");
        try
        {
            check(uri.getPathSegments().size() == 1 && !uri.toString().contains("outside"), "filename cannot become an attachment path");
            try (java.io.InputStream input = context.getContentResolver().openInputStream(uri))
            {
                check(input != null && input.read() == 7 && input.read() == 8 && input.read() == 9 && input.read() == -1, "explicit open serves exact bytes");
            }
            boolean rejected = false;
            try (android.os.ParcelFileDescriptor ignored = context.getContentResolver().openFileDescriptor(uri, "w"))
            {
            }
            catch (java.io.FileNotFoundException expected)
            {
                rejected = true;
            }
            check(rejected, "attachment sharing is read only");
            rejected = false;
            try (java.io.InputStream ignored = context.getContentResolver().openInputStream(Uri.withAppendedPath(uri, "extra")))
            {
            }
            catch (java.io.FileNotFoundException expected)
            {
                rejected = true;
            }
            check(rejected, "extra path segments cannot access another file");
        }
        finally
        {
            file.delete();
        }
    }

    private void erasure(Instrumentation instrumentation) throws Exception
    {
        File cache = new File(instrumentation.getTargetContext().getCacheDir(), "attachment-clear-test-" + java.util.UUID.randomUUID());
        Context context = new android.content.ContextWrapper(instrumentation.getTargetContext())
        {
            @Override
            public File getCacheDir()
            {
                return cache;
            }
        };
        byte[] bytes = new byte[4096];
        java.util.Arrays.fill(bytes, (byte)47);
        Attachment attachment = new Attachment("erase-test", "test.bin", "application/octet-stream", bytes, false);
        AttachmentFiles files = new AttachmentFiles();
        java.util.concurrent.CountDownLatch release = new java.util.concurrent.CountDownLatch(1);
        try
        {
            Uri uri = AttachmentProvider.publish(context, attachment);
            File opened = new File(new File(cache, "opened-attachments"), uri.getLastPathSegment() + ".bin");
            long previous = AttachmentProvider.generation();
            files.worker.execute(() ->
            {
                try
                {
                    release.await();
                }
                catch (InterruptedException interrupted)
                {
                    Thread.currentThread().interrupt();
                }
            });
            instrumentation.runOnMainSync(() -> check(files.open(context, attachment), "queued attachment open accepted before clear"));
            try (java.io.FileInputStream retained = new java.io.FileInputStream(opened))
            {
                AttachmentProvider.clearOpenedCopies(context);
                check(!opened.exists(), "clear removes the temporary opened copy");
                byte[] remaining = new byte[bytes.length];
                new java.io.DataInputStream(retained).readFully(remaining);
                check(!java.util.Arrays.equals(bytes, remaining), "the opened file is overwritten before unlinking");
            }
            check(!AttachmentProvider.current(previous), "clear invalidates old open admission");
            boolean rejected = false;
            try
            {
                AttachmentProvider.publish(context, attachment, previous);
            }
            catch (java.io.IOException expected)
            {
                rejected = true;
            }
            check(rejected, "old admission cannot recreate an attachment copy");
            release.countDown();
            files.worker.submit(() ->
            {
            }).get(10, java.util.concurrent.TimeUnit.SECONDS);
            instrumentation.waitForIdleSync();
            instrumentation.runOnMainSync(() -> check(!files.busy && files.openUri == null && files.takeNotice() != null, "a queued open finishes without publishing a stale link"));
            File directory = new File(cache, "opened-attachments");
            check(directory.listFiles() != null && directory.listFiles().length == 0, "the old worker leaves no plaintext copy");
            Uri fresh = AttachmentProvider.publish(context, attachment);
            check(new File(directory, fresh.getLastPathSegment() + ".bin").isFile(), "a new explicit open works after clear");
            AttachmentProvider.clearOpenedCopies(context);
            AttachmentProvider.clearOpenedCopies(context);
            check(directory.listFiles() != null && directory.listFiles().length == 0, "repeating clear is safe");
            instrumentation.runOnMainSync(() -> check(files.open(context, attachment), "a detached screen can finish an explicit file open"));
            files.worker.submit(() ->
            {
            }).get(10, java.util.concurrent.TimeUnit.SECONDS);
            instrumentation.waitForIdleSync();
            instrumentation.runOnMainSync(() -> check(files.openUri != null, "the pending link waits for the detached screen"));
            AttachmentProvider.clearOpenedCopies(context);
            instrumentation.runOnMainSync(() -> files.attach(() -> check(files.openUri == null, "reattaching cannot launch a link cleared while the screen was absent")));
            AtomicReference<Throwable> callbackFailure = new AtomicReference<>();
            instrumentation.runOnMainSync(() ->
            {
                try
                {
                    files.attach(null);
                    check(files.open(context, attachment), "the callback race starts a new explicit open");
                    files.worker.submit(() ->
                    {
                    }).get(10, java.util.concurrent.TimeUnit.SECONDS);
                    AttachmentProvider.clearOpenedCopies(context);
                }
                catch (Throwable error)
                {
                    callbackFailure.set(error);
                }
            });
            if (callbackFailure.get() != null)
            {
                throw new AssertionError("Attachment callback race failed", callbackFailure.get());
            }
            instrumentation.waitForIdleSync();
            instrumentation.runOnMainSync(() -> check(files.openUri == null && !files.busy && files.takeNotice() != null, "a queued main callback cannot publish a cleared link"));
            File external = new File(cache, "keep.txt");
            File link = new File(directory, java.util.UUID.randomUUID() + ".bin");
            try
            {
                try (java.io.FileOutputStream output = new java.io.FileOutputStream(external))
                {
                    output.write(bytes);
                }
                android.system.Os.symlink(external.getAbsolutePath(), link.getAbsolutePath());
                rejected = false;
                try
                {
                    AttachmentProvider.clearOpenedCopies(context);
                }
                catch (java.io.IOException expected)
                {
                    rejected = true;
                }
                check(rejected, "clear rejects a file redirected outside its cache");
                try (java.io.FileInputStream input = new java.io.FileInputStream(external))
                {
                    check(input.read() == 47 && external.length() == bytes.length, "a rejected path leaves the unrelated target unchanged");
                }
            }
            finally
            {
                link.delete();
                external.delete();
            }
        }
        finally
        {
            release.countDown();
            instrumentation.runOnMainSync(files::close);
            AttachmentProvider.clearOpenedCopies(context);
            new File(cache, "opened-attachments").delete();
            cache.delete();
        }
    }

    private void views(Context context) throws Exception
    {
        check(!MainActivity.countryFlag("sk").isEmpty(), "valid country flag");
        check(MainActivity.countryFlag("../../").isEmpty() && MainActivity.countryFlag(null).isEmpty(), "invalid country codes stay hidden");
        AvatarImages avatars = new AvatarImages(context.getResources(), () ->
        {
        });
        AttachmentImages images = new AttachmentImages(context.getResources(), () ->
        {
        });
        EmojiImages emojis = new EmojiImages(context.getResources(), () ->
        {
        }, ignored ->
        {
        });
        AttachmentFiles draft = new AttachmentFiles();
        try
        {
            Member member = new Member(7, 0, "Maya", "", "avatar-author", false, false, 2, 0, null).withProfile("", "data:image/png;base64,AQ==");
            SessionSnapshot avatarState = new SessionSnapshot(SessionSnapshot.Status.CONNECTED, "", 1, 0, "Alex", true, Collections.emptyList(), Collections.singletonList(member), 100);
            avatars.update(avatarState);
            long revision = avatars.revision();
            avatars.update(avatarState);
            check(avatars.revision() == revision, "unchanged snapshots do not invalidate message avatars");
            avatars.update(new SessionSnapshot(SessionSnapshot.Status.CONNECTED, "", 1, 0, "Alex", true, Collections.emptyList(), Collections.singletonList(member.withProfile("", "")), 100));
            check(avatars.revision() != revision && avatars.drawable(member) == null, "avatar deletion invalidates message rows without a decoder callback");
            revision = avatars.revision();
            avatars.update(avatarState);
            check(avatars.revision() != revision, "a restored avatar invalidates its message row");
            revision = avatars.revision();
            avatars.update(new SessionSnapshot(SessionSnapshot.Status.CONNECTED, "", 1, 0, "Alex", true, Collections.emptyList(), Collections.singletonList(member), 101));
            check(avatars.revision() != revision, "a replacement connection retires the old avatar catalog");
            Attachment preview = new Attachment("cache-check", "picture.png", "image/png", new byte[]{1}, true);
            java.lang.reflect.Field cacheField = AttachmentImages.class.getDeclaredField("images");
            cacheField.setAccessible(true);
            @SuppressWarnings("unchecked")
            android.util.LruCache<String, Bitmap> cache = (android.util.LruCache<String, Bitmap>) cacheField.get(images);
            Bitmap cached = Bitmap.createBitmap(4, 4, Bitmap.Config.ARGB_8888);
            cached.eraseColor(android.graphics.Color.RED);
            cache.put(preview.id, cached);
            android.graphics.drawable.Drawable drawable = images.drawable(preview);
            drawable.setBounds(0, 0, 4, 4);
            Bitmap output = Bitmap.createBitmap(4, 4, Bitmap.Config.ARGB_8888);
            drawable.draw(new android.graphics.Canvas(output));
            check(output.getPixel(1, 1) == android.graphics.Color.RED, "preview draws its cached image");
            cache.evictAll();
            output.eraseColor(android.graphics.Color.TRANSPARENT);
            drawable.draw(new android.graphics.Canvas(output));
            check(output.getPixel(1, 1) == android.graphics.Color.TRANSPARENT, "preview releases evicted bitmap instead of retaining it in the view");
            output.recycle();
            cached.recycle();
            draft.owner = new AttachmentFiles.Ticket("server-one", "channel:1", 100, false, false);
            draft.draft = new Attachment("draft", "file.txt", "text/plain", new byte[]{1}, false);
            check(draft.matches("server-one", "channel:1"), "attachment stays in its selected conversation");
            check(!draft.matches("server-two", "channel:1") && !draft.matches("server-one", "channel:2"), "other server or channel cannot send retained attachment");
            draft.clearDraft();
            check(draft.draft == null && draft.owner == null && !draft.sending, "sent attachment clears the preview state");
            ChatMessage message = new ChatMessage(1, "user:key", 2, "Maya", "Visible text", true, System.currentTimeMillis(), "Sent", "key", 2, 1, "reaction", new ReplyQuote("1", "key", "Maya", "Quoted text"), Collections.emptyList(), null, true);
            Map<String, Object> values = new HashMap<>();
            LocalSettings settings = LocalSettings.validate(values);
            ChatMessageViews.Actions actions = new ChatMessageViews.Actions()
            {
                @Override
                public void onMessageActions(ChatMessage selected)
                {
                }

                @Override
                public void onReaction(ChatMessage selected, String id)
                {
                }

                @Override
                public void onOpenAttachment(Attachment attachment)
                {
                }
            };
            View view = new ChatMessageViews(context, new Palette(true), settings, avatars, images, emojis, actions).message(message, true, null, Collections.emptyList(), "text");
            TextView body = find(view, "Visible text");
            check(body != null && body.getCurrentTextColor() != android.graphics.Color.BLACK, "default black message remains readable in dark appearance");
            check(find(view, "Quoted text") != null, "enabled quoted reply is rendered");
            check(findContaining(view, "Read") != null, "enabled read receipt is rendered");
            values.put("lemon_chat_replies", false);
            values.put("lemon_seen_indicator", false);
            view = new ChatMessageViews(context, new Palette(false), LocalSettings.validate(values), avatars, images, emojis, actions).message(message, true, null, Collections.emptyList(), "");
            check(find(view, "Quoted text") == null, "disabled quoted reply is hidden");
            check(findContaining(view, "Read") == null, "disabled read receipt is hidden");
            values.put("lemon_use_theme_chat_formatting", true);
            ChatMessage shortMessage = new ChatMessage(2, "channel:0", 2, "Maya", "A short message", false, System.currentTimeMillis(), "received", "key", 3);
            for (String theme : new String[]{"default-mobile", "mobile-2", "mono"})
            {
                Palette palette = new Palette(NativeTheme.find(theme), true);
                view = new ChatMessageViews(context, palette, LocalSettings.validate(values), avatars, images, emojis, actions).message(shortMessage, false, null, Collections.emptyList(), "");
                body = find(view, "A short message");
                float expected = android.util.TypedValue.applyDimension(android.util.TypedValue.COMPLEX_UNIT_SP, palette.theme.messageTextSize(), context.getResources().getDisplayMetrics());
                check(Math.abs(body.getTextSize() - expected) < 1, theme + " applies theme size and Android font scale once");
                int width = Math.round(280 * context.getResources().getDisplayMetrics().density);
                view.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
                int allowance = textHeight(view) + Math.round(22 * context.getResources().getDisplayMetrics().density);
                check(view.getMeasuredHeight() <= allowance, theme + " keeps single line message metadata compact");
                check(((ViewGroup)view).getChildAt(0).isLongClickable(), theme + " retains message actions without a fixed footer row");
            }
        }
        finally
        {
            avatars.close();
            images.close();
            emojis.close();
            draft.close();
        }
    }

    private TextView find(View view, String text)
    {
        TextView result = findContaining(view, text);
        return result != null && result.getText().toString().equals(text) ? result : null;
    }

    private int textHeight(View view)
    {
        if (view instanceof TextView)
        {
            return ((TextView)view).getLineHeight();
        }
        int height = 0;
        if (view instanceof ViewGroup)
        {
            ViewGroup group = (ViewGroup)view;
            for (int index = 0; index < group.getChildCount(); index++)
            {
                height += textHeight(group.getChildAt(index));
            }
        }
        return height;
    }

    private TextView findContaining(View view, String text)
    {
        if (view instanceof TextView && ((TextView) view).getText().toString().contains(text))
        {
            return (TextView) view;
        }
        if (view instanceof ViewGroup)
        {
            ViewGroup group = (ViewGroup) view;
            for (int index = 0; index < group.getChildCount(); index++)
            {
                TextView found = findContaining(group.getChildAt(index), text);
                if (found != null)
                {
                    return found;
                }
            }
        }
        return null;
    }

    private void check(boolean value, String description)
    {
        if (!value)
        {
            throw new AssertionError(description);
        }
        checks++;
    }
}
