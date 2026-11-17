package com.lemonchat.service;

import android.app.Instrumentation;
import android.content.Context;
import android.content.ContextWrapper;
import android.os.Handler;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.settings.LocalSettings;
import com.lemonchat.storage.ArchiveVault;
import com.lemonchat.storage.ConversationArchive;
import com.lemonchat.storage.HistoryChecks;
import com.lemonchat.storage.HistoryPolicy;
import com.lemonchat.storage.HistoryStore;
import com.lemonchat.storage.IdentityVault;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

/** controls storage completion order without starting a connection or notifications */
public final class HistoryServiceChecks
{
    private static final String FIRST_SCOPE = "a".repeat(64);
    private static final String SECOND_SCOPE = "b".repeat(64);
    private static final String CONVERSATION = "user:" + "c".repeat(64);
    private int checks;

    public static int verify(Instrumentation instrumentation) throws Exception
    {
        HistoryServiceChecks suite = new HistoryServiceChecks();
        suite.startupConsent(instrumentation);
        suite.restoreAndMerge(instrumentation);
        suite.obsoleteCompletions(instrumentation);
        suite.clearQueuedWrites(instrumentation);
        return suite.checks;
    }

    private void startupConsent(Instrumentation instrumentation) throws Exception
    {
        try (Fixture fixture = new Fixture(instrumentation))
        {
            fixture.save(SECOND_SCOPE, "saved", message(1, "saved before startup"));
            fixture.context.getSharedPreferences(HistoryPolicy.PREFERENCES, Context.MODE_PRIVATE).edit().clear().commit();
            fixture.main(() -> set(fixture.service, "historyPolicy", HistoryPolicy.load(fixture.context)));
            long oldEpoch = fixture.epoch.get();
            CountDownLatch entered = new CountDownLatch(1);
            CountDownLatch release = new CountDownLatch(1);
            fixture.storage.execute(() ->
            {
                entered.countDown();
                await(release);
            });
            await(entered);
            CountDownLatch completed = new CountDownLatch(1);
            AtomicReference<String> error = new AtomicReference<>();
            try
            {
                fixture.main(() ->
                {
                    fixture.service.setHistoryPolicy(0, result ->
                    {
                        error.set(result);
                        completed.countDown();
                    });
                    fixture.selectScope(SECOND_SCOPE);
                    fixture.restore(staleHistory(), oldEpoch);
                    check(fixture.service.messages(CONVERSATION).isEmpty(), "startup result stays hidden while consent is pending");
                });
            }
            finally
            {
                release.countDown();
            }
            await(completed);
            fixture.drain();
            fixture.main(() ->
            {
                check(error.get() == null, "startup consent commits successfully");
                check(hasText(fixture.service.messages(CONVERSATION), "saved before startup"), "consent reloads the selected scope after an empty startup scope");
                check(!hasText(fixture.service.messages(CONVERSATION), "stale result"), "startup never merges the earlier policy result");
                check(fixture.service.messages(CONVERSATION).size() == 1, "startup reload does not duplicate saved messages");
            });
        }
    }

    private void restoreAndMerge(Instrumentation instrumentation) throws Exception
    {
        try (Fixture fixture = new Fixture(instrumentation))
        {
            fixture.save(SECOND_SCOPE, "live", message(1, "older disk version"));
            fixture.save(SECOND_SCOPE, "disk", message(2, "saved second message"));
            long oldEpoch = fixture.epoch.get();
            CountDownLatch completed = new CountDownLatch(1);
            AtomicReference<String> error = new AtomicReference<>();
            fixture.main(() ->
            {
                fixture.selectScope(FIRST_SCOPE);
                fixture.service.setHistoryPolicy(7, result ->
                {
                    error.set(result);
                    completed.countDown();
                });
            });
            await(completed);
            fixture.drain();
            fixture.main(() ->
            {
                check(error.get() == null, "duration change finishes before connection restoration");
                fixture.selectScope(SECOND_SCOPE);
                call(fixture.service, "remember", new Class<?>[]{ChatMessage.class, String.class}, message(101, "new live version"), "live");
                fixture.restore(staleHistory(), oldEpoch);
            });
            fixture.drain();
            fixture.main(() ->
            {
                List<ChatMessage> messages = fixture.service.messages(CONVERSATION);
                check(hasText(messages, "saved second message"), "obsolete connection restoration reloads the current archive");
                check(hasText(messages, "new live version"), "archive merge preserves a newer live message");
                check(!hasText(messages, "older disk version") && !hasText(messages, "stale result"), "archive merge rejects obsolete disk and callback versions");
                check(messages.size() == 2, "stable record identifiers prevent duplicate live and disk messages");
                fixture.restore(staleHistory(), oldEpoch);
                call(fixture.service, "reloadCurrentHistory", new Class<?>[0]);
            });
            fixture.drain();
            fixture.main(() ->
            {
                check(fixture.service.messages(CONVERSATION).size() == 2, "repeated identity restoration and reload remain idempotent");
                fixture.context.getSharedPreferences(HistoryPolicy.PREFERENCES, Context.MODE_PRIVATE).edit().putInt("retention_days", -1).commit();
                set(fixture.service, "historyPolicy", HistoryPolicy.load(fixture.context));
                fixture.restore(new ConversationArchive.Loaded(), fixture.epoch.get());
                check(hasText(fixture.service.messages(CONVERSATION), "new live version"), "same scope reconnect keeps live messages when saving is disabled");
            });
        }
    }

    private void obsoleteCompletions(Instrumentation instrumentation) throws Exception
    {
        try (Fixture fixture = new Fixture(instrumentation))
        {
            fixture.save(FIRST_SCOPE, "first", message(1, "first scope only"));
            fixture.save(SECOND_SCOPE, "second", message(2, "second scope only"));
            fixture.main(() ->
            {
                fixture.selectScope(FIRST_SCOPE);
                fixture.queueLoadedResult();
                fixture.selectScope(SECOND_SCOPE);
            });
            fixture.drain();
            fixture.main(() ->
            {
                check(fixture.service.messages(CONVERSATION).isEmpty(), "a queued callback cannot merge into a replacement scope");
                fixture.queueLoadedResult();
                fixture.epoch.incrementAndGet();
            });
            fixture.drain();
            fixture.main(() ->
            {
                check(fixture.service.messages(CONVERSATION).isEmpty(), "a queued callback cannot cross a history policy generation");
                fixture.queueLoadedResult();
                set(fixture.service, "historyPolicy", HistoryPolicy.load(fixture.context).suspended());
            });
            fixture.drain();
            fixture.main(() ->
            {
                check(fixture.service.messages(CONVERSATION).isEmpty(), "a queued callback cannot restore history while access is suspended");
                set(fixture.service, "historyPolicy", HistoryPolicy.load(fixture.context));
                fixture.queueLoadedResult();
                set(fixture.service, "destroyed", true);
            });
            fixture.drain();
            fixture.main(() -> check(fixture.service.messages(CONVERSATION).isEmpty(), "a stopped service ignores completed archive reads"));
        }
    }

    private static ConversationArchive.Loaded staleHistory()
    {
        ConversationArchive.Loaded loaded = new ConversationArchive.Loaded();
        loaded.messages.add(message(-999, "stale result"));
        loaded.recordIds.put(-999L, "obsolete");
        return loaded;
    }

    private void clearQueuedWrites(Instrumentation instrumentation) throws Exception
    {
        try (Fixture fixture = new Fixture(instrumentation))
        {
            fixture.save(FIRST_SCOPE, "earlier", message(1, "earlier saved message"));
            ChatMessage old = new ChatMessage(300, CONVERSATION, 1, "You", "before clear", true, System.currentTimeMillis(), "Sending", "local key", -1);
            CountDownLatch entered = new CountDownLatch(1);
            CountDownLatch release = new CountDownLatch(1);
            CountDownLatch completed = new CountDownLatch(1);
            AtomicReference<String> error = new AtomicReference<>();
            fixture.storage.execute(() ->
            {
                entered.countDown();
                await(release);
            });
            await(entered);
            try
            {
                fixture.main(() ->
                {
                    fixture.selectScope(FIRST_SCOPE);
                    fixture.suppressNotifications();
                    fixture.service.onMessage(old);
                    fixture.service.clearSavedHistory(result ->
                    {
                        error.set(result);
                        completed.countDown();
                    });
                });
            }
            finally
            {
                release.countDown();
            }
            await(completed);
            fixture.drain();
            check(error.get() == null, "deletion completes with an older message write queued");
            check(fixture.savedMessages().isEmpty(), "deletion removes saved rows and rejects the older queued write");
            fixture.main(() ->
            {
                check(hasText(fixture.service.messages(CONVERSATION), "before clear"), "deletion leaves the current visible conversation available");
                fixture.service.onMessage(old.withDelivery("Delivered", 123));
            });
            fixture.drain();
            check(fixture.savedMessages().isEmpty(), "a delivery update cannot restore a message from before deletion");
            fixture.main(() -> fixture.service.onMessage(new ChatMessage(301, CONVERSATION, 1, "You", "after clear", true, System.currentTimeMillis(), "Delivered", "local key", 124)));
            fixture.drain();
            List<ChatMessage> saved = fixture.savedMessages();
            check(saved.size() == 1 && hasText(saved, "after clear"), "new messages save after successful deletion when consent stays enabled");
        }
    }

    private static ChatMessage message(long id, String text)
    {
        return new ChatMessage(id, CONVERSATION, 5, "Maya", text, false, System.currentTimeMillis(), "Received", "public key", -1);
    }

    private static boolean hasText(List<ChatMessage> messages, String text)
    {
        for (ChatMessage message : messages)
        {
            if (message.text.equals(text))
            {
                return true;
            }
        }
        return false;
    }

    private void check(boolean value, String description)
    {
        if (!value)
        {
            throw new AssertionError(description);
        }
        checks++;
    }

    private static void await(CountDownLatch latch)
    {
        try
        {
            if (!latch.await(30, TimeUnit.SECONDS))
            {
                throw new AssertionError("History fixture timed out");
            }
        }
        catch (InterruptedException error)
        {
            Thread.currentThread().interrupt();
            throw new AssertionError(error);
        }
    }

    private static Object get(Object target, String name) throws Exception
    {
        Field field = ChatService.class.getDeclaredField(name);
        field.setAccessible(true);
        return field.get(target);
    }

    private static void set(Object target, String name, Object value) throws Exception
    {
        Field field = ChatService.class.getDeclaredField(name);
        field.setAccessible(true);
        field.set(target, value);
    }

    private static void call(Object target, String name, Class<?>[] types, Object... arguments) throws Exception
    {
        Method method = ChatService.class.getDeclaredMethod(name, types);
        method.setAccessible(true);
        method.invoke(target, arguments);
    }

    private interface Action
    {
        void run() throws Exception;
    }

    private static final class Fixture implements AutoCloseable
    {
        private final Instrumentation instrumentation;
        private final HistoryChecks.FixtureContext context;
        private final ArchiveVault cipher;
        private final HistoryStore store;
        private final ChatService service = new ChatService();
        private final ExecutorService storage;
        private final AtomicLong epoch;

        private Fixture(Instrumentation instrumentation) throws Exception
        {
            this.instrumentation = instrumentation;
            context = new HistoryChecks.FixtureContext(instrumentation.getTargetContext());
            Constructor<ArchiveVault> cipherConstructor = ArchiveVault.class.getDeclaredConstructor(String.class);
            cipherConstructor.setAccessible(true);
            cipher = cipherConstructor.newInstance("lemon-native-history-test-" + UUID.randomUUID());
            Constructor<HistoryStore> storeConstructor = HistoryStore.class.getDeclaredConstructor(Context.class, IdentityVault.class, ArchiveVault.class);
            storeConstructor.setAccessible(true);
            store = storeConstructor.newInstance(context, new IdentityVault(context), cipher);
            storage = (ExecutorService)get(null, "STORAGE");
            epoch = (AtomicLong)get(null, "HISTORY_EPOCH");
            Method attach = ContextWrapper.class.getDeclaredMethod("attachBaseContext", Context.class);
            attach.setAccessible(true);
            attach.invoke(service, context);
            HistoryPolicy policy = HistoryPolicy.save(context, 0, false);
            main(() ->
            {
                set(service, "archive", store);
                set(service, "historyPolicy", policy);
                set(service, "localSettings", LocalSettings.load(context));
                set(service, "connectionLog", new ConnectionLog(context, storage));
            });
        }

        private void save(String scope, String recordId, ChatMessage message) throws Exception
        {
            storage.submit(() ->
            {
                store.save(scope, recordId, message, "Maya", System.currentTimeMillis(), true, HistoryPolicy.load(context));
                return null;
            }).get(30, TimeUnit.SECONDS);
        }

        private void main(Action action) throws Exception
        {
            AtomicReference<Throwable> failure = new AtomicReference<>();
            instrumentation.runOnMainSync(() ->
            {
                try
                {
                    action.run();
                }
                catch (Throwable error)
                {
                    failure.set(error);
                }
            });
            if (failure.get() != null)
            {
                throw new AssertionError("History main thread check failed", failure.get());
            }
        }

        private void selectScope(String scope) throws Exception
        {
            // select the test scope without touching notification state
            for (String name : new String[]{"conversations", "conversationTitles", "visibleArchiveRecords"})
            {
                ((Map<?, ?>)get(service, name)).clear();
            }
            set(service, "activeScope", scope);
        }

        private void restore(ConversationArchive.Loaded loaded, long version) throws Exception
        {
            call(service, "restoreHistory", new Class<?>[]{String.class, ConversationArchive.Loaded.class, long.class}, service.conversationScope(), loaded, version);
        }

        private void queueLoadedResult() throws Exception
        {
            call(service, "reloadCurrentHistory", new Class<?>[0]);
            // finish the read while its main thread completion is still queued
            storage.submit(() -> null).get(30, TimeUnit.SECONDS);
        }

        @SuppressWarnings("unchecked")
        private void suppressNotifications() throws Exception
        {
            ((Set<ChatService.Observer>)get(service, "chatObservers")).add(new ChatService.Observer()
            {
                @Override
                public void onChanged()
                {
                }

                @Override
                public void onProblem(String message)
                {
                }
            });
        }

        private List<ChatMessage> savedMessages() throws Exception
        {
            return storage.submit(() -> store.load(FIRST_SCOPE, HistoryPolicy.load(context)).messages).get(30, TimeUnit.SECONDS);
        }

        private void drain() throws Exception
        {
            for (int pass = 0; pass < 3; pass++)
            {
                storage.submit(() -> null).get(30, TimeUnit.SECONDS);
                instrumentation.waitForIdleSync();
            }
        }

        @Override
        public void close() throws Exception
        {
            main(() ->
            {
                set(service, "destroyed", true);
                ((Handler)get(service, "main")).removeCallbacksAndMessages(null);
            });
            drain();
            storage.submit(() ->
            {
                store.close();
                return null;
            }).get(30, TimeUnit.SECONDS);
            cipher.revoke();
            context.close();
        }
    }
}
