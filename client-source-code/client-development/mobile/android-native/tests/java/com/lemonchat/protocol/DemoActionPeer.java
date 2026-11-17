package com.lemonchat.protocol;

import com.lemonchat.crypto.Identity;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.model.Member;
import com.lemonchat.model.SessionSnapshot;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.TimeUnit;

/** disposable peer for call and notification checks against the demo server */
public final class DemoActionPeer
{
    public static void main(String[] arguments) throws Exception
    {
        if (arguments.length < 3)
        {
            throw new IllegalArgumentException("Expected endpoint, recipient name and call, poke or message");
        }
        String endpoint = arguments[0];
        if (!endpoint.startsWith("ws://127.0.0.1:"))
        {
            throw new IllegalArgumentException("Use only the disposable local demo server");
        }
        String recipient = arguments[1];
        String operation = arguments[2];
        String text = arguments.length > 3 ? arguments[3] : "Hello from the native test peer";
        try (ChatSession session = new ChatSession(Runnable::run))
        {
            session.setListener(new ChatSession.Listener()
            {
                @Override
                public void onState(SessionSnapshot state)
                {
                }

                @Override
                public void onMessage(ChatMessage message)
                {
                    System.out.println("MESSAGE " + message.senderName + " " + message.delivery);
                }

                @Override
                public void onError(String message)
                {
                    System.out.println("ERROR " + message);
                }
            });
            session.connect(endpoint, "Call tester", Identity.create(2048), "");
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(60);
            Member target = null;
            while (System.nanoTime() < deadline)
            {
                SessionSnapshot state = session.state();
                if (state.connected && state.channelKeysReady)
                {
                    for (Member member : state.members)
                    {
                        if (member.name.equals(recipient) && (!operation.equals("call") || member.idle))
                        {
                            target = member;
                            break;
                        }
                    }
                }
                if (target != null)
                {
                    break;
                }
                Thread.sleep(100);
            }
            if (target == null)
            {
                throw new IllegalStateException("Recipient was not available for " + operation);
            }
            Thread.sleep(200);
            switch (operation)
            {
                case "call":
                    session.callMember(target.id);
                    break;
                case "poke":
                    session.sendPoke(target.id, text);
                    break;
                case "message":
                    session.sendDirectMessage(target.id, text);
                    break;
                default:
                    throw new IllegalArgumentException("Choose call, poke or message");
            }
            System.out.println("SENT " + operation + " to " + recipient);
            long hold = operation.equals("call") ? 90 : 5;
            long until = System.nanoTime() + TimeUnit.SECONDS.toNanos(hold);
            while (System.nanoTime() < until)
            {
                if (arguments.length > 4 && Files.exists(Path.of(arguments[4])))
                {
                    break;
                }
                Thread.sleep(100);
            }
        }
    }
}
