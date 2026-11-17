package com.lemonchat.model;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** flattens channel membership into bounded rows for a recycled list */
public final class ChannelTree
{
    public static final class Row
    {
        public final Channel channel;
        public final Member member;
        public final int depth;
        public final boolean expandable;
        public final boolean expanded;
        public final int people;
        public final int musicBots;

        private Row(Channel channel, Member member, int depth, boolean expandable, boolean expanded, int people, int musicBots)
        {
            this.channel = channel;
            this.member = member;
            this.depth = depth;
            this.expandable = expandable;
            this.expanded = expanded;
            this.people = people;
            this.musicBots = musicBots;
        }

        public long stableId()
        {
            return channel != null ? channel.id : member != null ? (1L << 32) + member.id : Long.MAX_VALUE;
        }
    }

    private static final class Branch
    {
        final Channel channel;
        final int depth;
        final boolean visible;

        Branch(Channel channel, int depth, boolean visible)
        {
            this.channel = channel;
            this.depth = depth;
            this.visible = visible;
        }
    }

    private ChannelTree()
    {
    }

    /**
     * lists nested channels and their members without recursive traversal
     *
     * @param snapshot current server state
     * @param collapsed channels hidden by the user
     * @return visible rows in server channel order
     */
    public static List<Row> rows(SessionSnapshot snapshot, Set<Integer> collapsed)
    {
        Map<Integer, Channel> channels = new LinkedHashMap<>();
        Map<Integer, List<Channel>> children = new LinkedHashMap<>();
        Map<Integer, List<Member>> members = new LinkedHashMap<>();
        ArrayList<Member> outside = new ArrayList<>();
        for (Channel channel : snapshot.channels)
        {
            channels.put(channel.id, channel);
        }
        for (Channel channel : snapshot.channels)
        {
            children.computeIfAbsent(channel.parentId, ignored -> new ArrayList<>()).add(channel);
        }
        for (Member member : snapshot.members)
        {
            if (channels.containsKey(member.channelId))
            {
                members.computeIfAbsent(member.channelId, ignored -> new ArrayList<>()).add(member);
            }
            else
            {
                outside.add(member);
            }
        }
        ArrayList<Row> result = new ArrayList<>();
        HashSet<Integer> visited = new HashSet<>();
        for (Channel channel : snapshot.channels)
        {
            if (!channels.containsKey(channel.parentId) || channel.id == channel.parentId)
            {
                append(channel, collapsed, children, members, visited, result);
            }
        }
        for (Channel channel : snapshot.channels)
        {
            if (!visited.contains(channel.id))
            {
                append(channel, collapsed, children, members, visited, result);
            }
        }
        if (!outside.isEmpty())
        {
            result.add(new Row(null, null, 0, false, false, 0, 0));
            for (Member member : outside)
            {
                result.add(new Row(null, member, 0, false, false, 0, 0));
            }
        }
        return result;
    }

    private static void append(Channel root, Set<Integer> collapsed, Map<Integer, List<Channel>> children, Map<Integer, List<Member>> members, Set<Integer> visited, List<Row> result)
    {
        ArrayDeque<Branch> pending = new ArrayDeque<>();
        pending.push(new Branch(root, 0, true));
        while (!pending.isEmpty())
        {
            Branch branch = pending.pop();
            if (!visited.add(branch.channel.id))
            {
                continue;
            }
            List<Channel> nested = children.getOrDefault(branch.channel.id, Collections.emptyList());
            List<Member> occupants = members.getOrDefault(branch.channel.id, Collections.emptyList());
            boolean expanded = !collapsed.contains(branch.channel.id);
            int bots = 0;
            for (Member member : occupants)
            {
                if (member.musicBot)
                {
                    bots++;
                }
            }
            if (branch.visible)
            {
                result.add(new Row(branch.channel, null, branch.depth, !nested.isEmpty() || !occupants.isEmpty(), expanded, occupants.size() - bots, bots));
                if (expanded)
                {
                    for (Member member : occupants)
                    {
                        result.add(new Row(null, member, branch.depth + 1, false, false, 0, 0));
                    }
                }
            }
            for (int index = nested.size() - 1; index >= 0; index--)
            {
                pending.push(new Branch(nested.get(index), branch.depth + 1, branch.visible && expanded));
            }
        }
    }

    /**
     * names the audio state sent by the server
     *
     * @param member member whose indicator is displayed
     * @return accessible audio description
     */
    public static String audioDescription(Member member)
    {
        if (member.musicBot)
        {
            return "Music bot";
        }
        if (member.idle)
        {
            return "Idle";
        }
        switch (member.audioState)
        {
            case 1:
                return "Speaking";
            case 2:
                return "Push to talk ready";
            case 3:
                return "Microphone off";
            case 4:
                return "Audio off";
            default:
                return "Audio state unavailable";
        }
    }
}
