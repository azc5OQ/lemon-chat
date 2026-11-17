package com.lemonchat.protocol;

import com.lemonchat.crypto.Identity;
import com.lemonchat.model.ChatMessage;
import com.lemonchat.model.Member;
import com.lemonchat.model.SessionSnapshot;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashSet;
import java.util.Set;
import org.json.JSONObject;

/** supplies two real peers for the emulator's isolated demo server */
public final class DemoPeers
{
    private DemoPeers()
    {
    }

    /**
     * keeps the demo peers connected until the stop file exists
     *
     * @param arguments server URL and stop file path
     * @throws Exception when the demo cannot connect
     */
    public static void main(String[] arguments) throws Exception
    {
        try (ChatSession maya = peer("Maya"); ChatSession sam = peer("Sam"))
        {
            maya.connect(arguments[0], "Maya", Identity.create(2048), "");
            sam.connect(arguments[0], "Sam", Identity.create(2048), "");
            Set<Integer> greeted = new HashSet<>();
            boolean botRequested = false;
            while (!Files.exists(Path.of(arguments[1])))
            {
                if (maya.state().channelKeysReady && sam.state().channelKeysReady)
                {
                    if (!botRequested)
                    {
                        botRequested = maya.sendControl(new JSONObject().put("type", "create_music_bot").put("channel_id", 2).put("music_bot_username", "Lemon Radio"));
                    }
                    for (Member member : maya.state().members)
                    {
                        if (!member.musicBot && !member.name.equals("Maya") && !member.name.equals("Sam") && greeted.add(member.id))
                        {
                            Thread.sleep(1000);
                            maya.sendChannelMessage("Anyone up for a walk this afternoon?");
                            Thread.sleep(300);
                            sam.sendChannelMessage("Count me in. Let's meet by the river.");
                            Thread.sleep(300);
                            maya.sendDirectMessage(member.id, "Hey! Good to see you here. How does the new app feel?");
                        }
                    }
                }
                Thread.sleep(500);
            }
        }
    }

    private static ChatSession peer(String name)
    {
        ChatSession session = new ChatSession(Runnable::run);
        session.setListener(new ChatSession.Listener()
        {
            @Override
            public void onState(SessionSnapshot state)
            {
                if (state.status == SessionSnapshot.Status.FAILED)
                {
                    System.out.println(name + ": " + state.detail);
                }
            }

            @Override
            public void onMessage(ChatMessage message)
            {
            }

            @Override
            public void onError(String message)
            {
                System.out.println(name + ": " + message);
            }
        });
        return session;
    }
}
