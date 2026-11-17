package com.lemonchat.model;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** one emoji and the identities that selected it */
public final class Reaction
{
    public final String emojiId;
    public final String emojiName;
    public final List<String> authorKeys;
    public final boolean mine;

    public Reaction(String emojiId, String emojiName, List<String> authorKeys, String localKey)
    {
        this(emojiId, emojiName, authorKeys, authorKeys.contains(localKey));
    }

    public Reaction(String emojiId, String emojiName, List<String> authorKeys, boolean mine)
    {
        this.emojiId = emojiId;
        this.emojiName = emojiName;
        this.authorKeys = Collections.unmodifiableList(new ArrayList<>(authorKeys));
        this.mine = mine;
    }
}
