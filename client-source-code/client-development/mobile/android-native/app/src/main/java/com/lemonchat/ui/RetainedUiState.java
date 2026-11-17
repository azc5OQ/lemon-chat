package com.lemonchat.ui;

import android.content.ContentResolver;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import com.lemonchat.crypto.IdentityFile;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.service.ChatService;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.ref.WeakReference;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** keeps bounded drafts and file operations across activity recreation */
final class RetainedUiState implements AutoCloseable
{
    interface Listener
    {
        void onFilesChanged();
    }

    static final int MESSAGE_LIMIT = 65536;
    private static final int DRAFT_BYTES_LIMIT = 2 * 1024 * 1024;
    final Map<String, String> drafts = new LinkedHashMap<>();
    final AttachmentFiles attachments = new AttachmentFiles();
    LayoutPosition layoutPosition;
    ConnectionWizard.Draft connectionDraft;
    final Map<Integer, LayoutPosition> mobilePositions = new LinkedHashMap<>();
    ChatMessage reply;
    ChatMessage pendingReply;
    String replyConversation;
    String searchQuery = "";
    String searchConversation;
    String pendingConversation;
    String pendingText;
    long pendingAfterId;
    byte[] exportData;
    byte[] importData;
    boolean importInFlight;
    int legacyBits = 2048;
    private final ExecutorService files = Executors.newSingleThreadExecutor();
    private final Handler main = new Handler(Looper.getMainLooper());
    private WeakReference<Listener> listener = new WeakReference<>(null);
    private boolean fileBusy;
    private boolean closed;
    private String notice;

    static final class LayoutPosition
    {
        int screen;
        int first;
        float top;
        float scroll;
        int selectionStart;
        int selectionEnd;
        long connection;
        String scope;
        String conversation;
        java.util.Set<Integer> collapsed = new java.util.HashSet<>();
    }

    /**
     * attaches the visible activity to pending file results
     *
     * @param value current observer
     */
    void attach(Listener value)
    {
        listener = new WeakReference<>(value);
        changed();
    }

    /**
     * detaches only the observer that is leaving
     *
     * @param value previous observer
     */
    void detach(Listener value)
    {
        if (listener.get() == value)
        {
            listener.clear();
        }
    }

    /**
     * retains a draft within the total text budget
     *
     * @param conversation conversation identifier
     * @param text current composer text
     */
    void storeDraft(String conversation, String text)
    {
        drafts.remove(conversation);
        if (text.isEmpty())
        {
            return;
        }
        String bounded = text.length() <= MESSAGE_LIMIT ? text : text.substring(0, MESSAGE_LIMIT);
        drafts.put(conversation, bounded);
        int bytes = 0;
        for (String value : drafts.values())
        {
            bytes += value.length() * 2;
        }
        while (drafts.size() > 24 || bytes > DRAFT_BYTES_LIMIT)
        {
            String oldest = drafts.keySet().iterator().next();
            bytes -= drafts.remove(oldest).length() * 2;
        }
    }

    /** @return whether another file operation is in progress */
    boolean busy()
    {
        return fileBusy || importInFlight;
    }

    /** @return the next file result notice, or null */
    String takeNotice()
    {
        String result = notice;
        notice = null;
        return result;
    }

    /**
     * reads an identity without retaining the activity
     *
     * @param resolver application content resolver
     * @param uri selected document
     * @param defaultBits key size used when an older file omits it
     * @return whether the read started
     */
    boolean readIdentity(ContentResolver resolver, Uri uri, int defaultBits)
    {
        if (closed || busy())
        {
            return false;
        }
        clearImport();
        legacyBits = defaultBits;
        fileBusy = true;
        files.execute(() ->
        {
            byte[] result = null;
            String failure = null;
            try (InputStream stream = resolver.openInputStream(uri))
            {
                if (stream == null)
                {
                    throw new java.io.IOException("Cannot open this file");
                }
                ByteArrayOutputStream output = new ByteArrayOutputStream();
                byte[] buffer = new byte[4096];
                int count;
                while ((count = stream.read(buffer)) != -1)
                {
                    if (output.size() + count > IdentityFile.MAX_BYTES)
                    {
                        throw new java.io.IOException("This file is too large to be a Lemon identity");
                    }
                    output.write(buffer, 0, count);
                }
                result = output.toByteArray();
            }
            catch (Exception error)
            {
                failure = error.getMessage() != null ? error.getMessage() : "The file could not be opened";
            }
            byte[] contents = result;
            String error = failure;
            main.post(() ->
            {
                fileBusy = false;
                if (closed)
                {
                    wipe(contents);
                    return;
                }
                importData = contents;
                notice = error;
                changed();
            });
        });
        return true;
    }

    /**
     * writes the selected export and reports success after the stream closes
     *
     * @param resolver application content resolver
     * @param uri selected destination
     * @return whether the write started
     */
    boolean writeIdentity(ContentResolver resolver, Uri uri)
    {
        if (closed || busy() || exportData == null)
        {
            return false;
        }
        byte[] contents = exportData;
        exportData = null;
        fileBusy = true;
        files.execute(() ->
        {
            String result;
            try
            {
                try (OutputStream stream = resolver.openOutputStream(uri, "wt"))
                {
                    if (stream == null)
                    {
                        throw new java.io.IOException("Cannot open the destination");
                    }
                    stream.write(contents);
                    stream.flush();
                }
                result = "Identity saved. Keep this file private.";
            }
            catch (Exception error)
            {
                result = "The identity file could not be saved";
            }
            finally
            {
                wipe(contents);
            }
            String completion = result;
            main.post(() ->
            {
                fileBusy = false;
                if (!closed)
                {
                    notice = completion;
                    changed();
                }
            });
        });
        return true;
    }

    /**
     * applies the pending import through the connection service
     *
     * @param service service that owns the identity
     * @param legacyBits selected size for an older file
     */
    void applyImport(ChatService service, int legacyBits)
    {
        if (closed || busy() || importData == null)
        {
            return;
        }
        importInFlight = true;
        service.importIdentity(importData, legacyBits, error ->
        {
            importInFlight = false;
            if (error == null)
            {
                clearImport();
            }
            if (!closed)
            {
                notice = error == null ? "Identity imported" : error;
                changed();
            }
        });
        changed();
    }

    /** clears a cancelled or completed import */
    void clearImport()
    {
        wipe(importData);
        importData = null;
    }

    /** clears an unused export */
    void clearExport()
    {
        wipe(exportData);
        exportData = null;
    }

    private void changed()
    {
        Listener current = listener.get();
        if (!closed && current != null)
        {
            current.onFilesChanged();
        }
    }

    private static void wipe(byte[] bytes)
    {
        if (bytes != null)
        {
            Arrays.fill(bytes, (byte) 0);
        }
    }

    /** releases retained data after the activity finishes permanently */
    @Override
    public void close()
    {
        closed = true;
        listener.clear();
        drafts.clear();
        pendingConversation = null;
        pendingText = null;
        clearImport();
        connectionDraft = null;
        clearExport();
        files.shutdown();
        attachments.close();
        reply = null;
        pendingReply = null;
    }
}
