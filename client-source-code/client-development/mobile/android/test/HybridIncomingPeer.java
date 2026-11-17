package com.lemonchat.protocol;

import com.lemonchat.crypto.Identity;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.model.Member;
import com.lemonchat.model.SessionSnapshot;
import java.io.InputStream;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** supplies real encrypted traffic for the isolated hybrid phone check */
public final class HybridIncomingPeer
{
    private static final Pattern READY = Pattern.compile("HYBRID_BACKGROUND_READY username=(HybridPhone[a-z0-9]+)");

    /**
     * waits for the phone test before sending its message and call
     *
     * @param arguments local server endpoint and a new instrumentation log path
     * @throws Exception when connection or either test phase fails
     */
    public static void main(String[] arguments) throws Exception
    {
        if (arguments.length != 2)
        {
            throw new IllegalArgumentException("Expected a local server endpoint and a new instrumentation log path");
        }
        URI endpoint = URI.create(arguments[0]);
        if (!"ws".equals(endpoint.getScheme()) || !"127.0.0.1".equals(endpoint.getHost()) || endpoint.getPort() < 1024 || endpoint.getPort() > 65535 || endpoint.getUserInfo() != null)
        {
            throw new IllegalArgumentException("Use only the disposable local demo server");
        }
        Path log = Path.of(arguments[1]);
        if (Files.exists(log) && Files.size(log) != 0)
        {
            throw new IllegalArgumentException("Choose an absent or empty instrumentation log");
        }
        AtomicReference<String> failure = new AtomicReference<>();
        AtomicBoolean delivered = new AtomicBoolean();
        String message = "A real encrypted message for the hybrid background check";
        try (ChatSession session = new ChatSession(Runnable::run))
        {
            session.setListener(new ChatSession.Listener()
            {
                @Override
                public void onState(SessionSnapshot state)
                {
                    if (state.status == SessionSnapshot.Status.FAILED)
                    {
                        failure.compareAndSet(null, state.detail);
                    }
                }

                @Override
                public void onMessage(ChatMessage value)
                {
                    if (value.outgoing && value.text.equals(message) && value.delivery.equals("Sent"))
                    {
                        delivered.set(true);
                        System.out.println("PASS encrypted private message acknowledged by server");
                    }
                }

                @Override
                public void onError(String value)
                {
                    failure.compareAndSet(null, value);
                }
            });
            session.connect(endpoint.toString(), "Hybrid check peer", Identity.create(2048), "");
            System.out.println("Waiting for the isolated phone test");
            String recipient = null;
            String recipientKey = null;
            boolean sent = false;
            boolean called = false;
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(300);
            while (System.nanoTime() < deadline)
            {
                if (failure.get() != null)
                {
                    throw new IllegalStateException("Peer failed: " + failure.get());
                }
                String output = ReadLog(log);
                Matcher ready = READY.matcher(output);
                if (recipient == null && ready.find())
                {
                    recipient = ready.group(1);
                    System.out.println("Phone background phase reached for " + recipient);
                }
                Member target = recipient == null ? null : Find(session.state(), recipient);
                if (target != null && recipientKey != null && !recipientKey.equals(target.publicKey))
                {
                    throw new IllegalStateException("The test recipient identity changed");
                }
                if (!sent && target != null && session.state().connected)
                {
                    recipientKey = target.publicKey;
                    session.sendDirectMessage(target.id, message);
                    sent = true;
                }
                if (sent && !called && delivered.get() && target != null && target.idle && session.state().channelKeysReady && output.contains("HYBRID_CALL_READY"))
                {
                    session.callMember(target.id);
                    called = true;
                    System.out.println("Requested the real server call for the idle test recipient");
                }
                if (output.contains("FAILURES!!!") || output.contains("INSTRUMENTATION_FAILED") || output.contains("Process crashed"))
                {
                    throw new IllegalStateException("Phone instrumentation failed");
                }
                if (output.contains("hybrid live checks; screenshots="))
                {
                    if (!called || !delivered.get())
                    {
                        throw new IllegalStateException("Phone finished without both incoming phases");
                    }
                    System.out.println("PASS phone confirmed message and call checks");
                    return;
                }
                Thread.sleep(100);
            }
            throw new IllegalStateException("Phone verification did not finish within five minutes");
        }
    }

    private static Member Find(SessionSnapshot state, String name)
    {
        for (Member member : state.members)
        {
            if (!member.musicBot && member.name.equals(name))
            {
                return member;
            }
        }
        return null;
    }

    private static String ReadLog(Path path) throws Exception
    {
        if (!Files.exists(path))
        {
            return "";
        }
        byte[] bytes;
        try (InputStream input = Files.newInputStream(path))
        {
            bytes = input.readNBytes(1024 * 1024 + 1);
        }
        if (bytes.length > 1024 * 1024)
        {
            throw new IllegalStateException("Instrumentation log exceeded the test limit");
        }
        if (bytes.length >= 2 && bytes[0] == (byte)0xff && bytes[1] == (byte)0xfe)
        {
            return new String(bytes, StandardCharsets.UTF_16LE);
        }
        return new String(bytes, StandardCharsets.UTF_8);
    }
}
