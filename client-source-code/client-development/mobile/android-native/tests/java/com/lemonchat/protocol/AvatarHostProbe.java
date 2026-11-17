package com.lemonchat.protocol;

import com.lemonchat.model.Member;
import java.util.ArrayList;
import java.util.Base64;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import org.json.JSONObject;

/** verifies avatar ownership without any Android screen */
public final class AvatarHostProbe
{
    private static int checks;

    public static void main(String[] arguments)
    {
        run();
    }

    public static void run()
    {
        Fixture fixture = new Fixture();
        for (int id = 1; id <= 25; id++)
        {
            fixture.members.put(id, member(id, "key-" + id));
        }
        fixture.loader.replaceMembers(fixture.members.values());
        fixture.loader.tick(0);
        require(fixture.requests.isEmpty(), "avatars wait for the authenticated server policy");
        fixture.loader.setEnabled(true);
        fixture.loader.tick(0);
        require(fixture.requests.size() == 4, "initial avatars load without a UI and use a bounded request batch");
        fixture.loader.tick(100);
        require(fixture.requests.size() == 4, "initial avatar requests are paced");
        fixture.loader.tick(250);
        fixture.loader.tick(500);
        fixture.loader.tick(750);
        fixture.loader.tick(1000);
        require(fixture.requests.size() == 16 && fixture.loader.nextDelay(1000) > 1000, "only sixteen unanswered requests remain in flight without polling");
        String image = "data:image/png;base64,AQID";
        fixture.loader.received(1, image);
        fixture.loader.received(2, "");
        require(fixture.members.get(1).avatarDataUri.equals(image) && fixture.members.get(2).avatarDataUri.isEmpty(), "loaded and explicitly empty avatars are authoritative state");
        fixture.members.put(1, member(1, "key-1"));
        fixture.members.put(2, member(2, "key-2"));
        fixture.loader.replaceMembers(fixture.members.values());
        require(fixture.members.get(1).avatarDataUri.equals(image), "same-identity membership refresh keeps loaded avatar bytes");
        int before = fixture.requests.size();
        fixture.loader.hint(new int[]{2});
        fixture.loader.tick(1250);
        require(fixture.requests.subList(before, fixture.requests.size()).stream().noneMatch(id -> id == 2), "visible empty avatars are not fetched repeatedly");

        Fixture replacement = new Fixture();
        replacement.members.put(1, member(1, "old-key"));
        replacement.loader.replaceMembers(replacement.members.values());
        replacement.loader.setEnabled(true);
        replacement.loader.tick(0);
        replacement.members.put(1, member(1, "new-key"));
        replacement.loader.memberChanged(replacement.members.get(1));
        replacement.loader.received(1, image);
        require(replacement.members.get(1).avatarDataUri.isEmpty(), "an old occupant's pending reply cannot populate a reused client ID");
        replacement.loader.tick(250);
        require(replacement.requests.size() == 2, "a replacement identity receives its own avatar request");
        replacement.loader.received(1, image);
        require(replacement.members.get(1).avatarDataUri.equals(image), "the replacement identity's response is accepted");
        replacement.loader.changed(1);
        require(replacement.members.get(1).avatarDataUri.isEmpty(), "avatar changes clear the old visible image immediately");
        replacement.loader.tick(500);
        replacement.loader.received(1, "");
        require(replacement.loader.nextDelay(500) < 0, "avatar deletion completes without a repeating timer");

        Fixture missing = new Fixture();
        missing.members.put(1, member(1, "missing-key"));
        missing.loader.replaceMembers(missing.members.values());
        missing.loader.setEnabled(true);
        missing.loader.tick(0);
        missing.loader.tick(10000);
        missing.loader.tick(20000);
        require(missing.requests.size() == 2 && missing.loader.nextDelay(20000) < 0, "an unanswered avatar retries once then stops waking the worker");

        Fixture bounded = new Fixture();
        for (int id = 1; id <= 30; id++)
        {
            bounded.members.put(id, member(id, "bounded-" + id));
        }
        bounded.loader.replaceMembers(bounded.members.values());
        bounded.loader.setEnabled(true);
        String large = "data:image/png;base64," + Base64.getEncoder().encodeToString(new byte[90000]);
        int answered = 0;
        for (int pass = 0; pass < 20; pass++)
        {
            bounded.loader.tick(pass * 250L);
            while (answered < bounded.requests.size())
            {
                bounded.loader.received(bounded.requests.get(answered++), large);
            }
        }
        long total = bounded.members.values().stream().mapToLong(member -> member.avatarDataUri.length()).sum();
        require(total <= 2 * 1024 * 1024 && bounded.requests.size() < 30 && bounded.loader.nextDelay(5000) < 0, "a full avatar cache stops background fetching within its global budget");
        bounded.loader.hint(new int[]{30});
        bounded.loader.tick(5000);
        bounded.loader.received(30, large);
        total = bounded.members.values().stream().mapToLong(member -> member.avatarDataUri.length()).sum();
        require(bounded.members.get(30).avatarDataUri.equals(large) && total <= 2 * 1024 * 1024, "a visible cache miss replaces older cached bytes without exceeding the budget");
        bounded.loader.reset();
        require(bounded.loader.nextDelay(6000) < 0, "connection reset retires all avatar requests and deadlines");
        System.out.println("PASS " + checks + " detached avatar ownership checks");
    }

    private static Member member(int id, String key)
    {
        return new Member(id, 0, "Member " + id, "", key, false, false, 3, 0, "");
    }

    private static void require(boolean value, String message)
    {
        if (!value)
        {
            throw new AssertionError(message);
        }
        checks++;
    }

    private static final class Fixture implements AvatarLoader.Host
    {
        final Map<Integer, Member> members = new LinkedHashMap<>();
        final List<Integer> requests = new ArrayList<>();
        final AvatarLoader loader = new AvatarLoader(this);

        @Override
        public Member member(int id)
        {
            return members.get(id);
        }

        @Override
        public void update(Member member)
        {
            members.put(member.id, member);
        }

        @Override
        public void send(JSONObject message)
        {
            requests.add(message.optInt("client_id"));
        }
    }
}
