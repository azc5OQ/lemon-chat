package com.lemonchat.storage;

import android.content.Context;
import com.lemonchat.model.ChatMessage;

/** owns consent gated archive access on the serial storage worker */
public final class HistoryStore implements AutoCloseable
{
    private final Context context;
    private final IdentityVault identity;
    private final ArchiveVault cipher;
    private final HistoryFiles files;
    private ConversationArchive archive;

    public HistoryStore(Context context, IdentityVault identity)
    {
        this(context, identity, new ArchiveVault());
    }

    HistoryStore(Context context, IdentityVault identity, ArchiveVault cipher)
    {
        this.context = context.getApplicationContext();
        this.identity = identity;
        this.cipher = cipher;
        files = new HistoryFiles(this.context);
    }

    private ConversationArchive prepare(HistoryPolicy policy) throws Exception
    {
        if (!policy.enabled())
        {
            throw new IllegalStateException("Saving history is disabled");
        }
        if (archive == null)
        {
            archive = new ConversationArchive(context, cipher);
        }
        if (files.legacyExists())
        {
            if (!archive.legacyImported())
            {
                try (ConversationArchive legacy = ConversationArchive.legacy(context, identity))
                {
                    archive.migrate(legacy, System.currentTimeMillis());
                }
            }
            files.erase(false);
        }
        return archive;
    }

    public ConversationArchive.Loaded load(String scope, HistoryPolicy policy) throws Exception
    {
        if (!policy.enabled() || (archive == null && !files.currentExists() && !files.legacyExists()))
        {
            return new ConversationArchive.Loaded();
        }
        ConversationArchive selected = prepare(policy);
        selected.expire(policy, System.currentTimeMillis());
        return scope.isEmpty() ? new ConversationArchive.Loaded() : selected.load(scope);
    }

    public void save(String scope, String recordId, ChatMessage message, String title, long receivedAt, boolean allowInsert, HistoryPolicy policy) throws Exception
    {
        if (!policy.enabled())
        {
            return;
        }
        ConversationArchive selected = prepare(policy);
        selected.expire(policy, System.currentTimeMillis());
        if (policy.retentionDays == 0 || receivedAt > policy.cutoff(System.currentTimeMillis()))
        {
            selected.save(scope, recordId, message, title, receivedAt, allowInsert);
        }
    }

    /**
     * @brief deletes one saved message when saved history exists; does nothing when no history files exist
     *
     * @param scope server and identity scope
     * @param recordId identifier the message was saved with
     * @param policy current history policy
     *
     * @throws Exception when the history files cannot be opened
     */
    public void delete(String scope, String recordId, HistoryPolicy policy) throws Exception
    {
        // no saved history
        if (!policy.enabled() || (archive == null && !files.currentExists() && !files.legacyExists()))
        {
            return;
        }

        // delete the record
        prepare(policy).delete(scope, recordId);
    }

    public long maintain(HistoryPolicy policy) throws Exception
    {
        if (!policy.enabled() || (archive == null && !files.currentExists() && !files.legacyExists()))
        {
            return 3600000;
        }
        return prepare(policy).expire(policy, System.currentTimeMillis());
    }

    /**
     * closes storage, revokes its key and removes all accessible history files
     *
     * @throws Exception when any required erasure step fails
     */
    public void erase() throws Exception
    {
        close();
        cipher.revoke();
        files.erase(true);
    }

    @Override
    public void close()
    {
        if (archive != null)
        {
            archive.close();
            archive = null;
        }
    }
}
