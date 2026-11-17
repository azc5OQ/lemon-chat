package com.lemonchat.ui;

import android.content.res.Resources;
import android.graphics.drawable.Drawable;
import com.lemonchat.model.ServerEmoji;
import com.lemonchat.model.ServerIcon;
import com.lemonchat.model.SessionSnapshot;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import java.util.function.Consumer;

/** decodes only images from the connected server's emoji catalog */
final class EmojiImages implements AutoCloseable
{
    private final ServerIconCache images;
    private final Consumer<String> request;
    private final Map<String, ServerEmoji> catalog = new HashMap<>();
    private final Map<String, Long> ids = new HashMap<>();
    private long connection = -1;
    private long nextId;

    EmojiImages(Resources resources, Runnable changed, Consumer<String> request)
    {
        images = new ServerIconCache(resources, changed, 262144 * 4 / 3 + 64, 64, 4 * 1024 * 1024);
        this.request = request;
    }

    void update(SessionSnapshot snapshot)
    {
        if (snapshot == null)
        {
            return;
        }
        if (connection != snapshot.connectionId)
        {
            close();
            connection = snapshot.connectionId;
        }
        catalog.clear();
        ArrayList<ServerIcon> values = new ArrayList<>();
        for (ServerEmoji emoji : snapshot.emojis)
        {
            catalog.put(emoji.id, emoji);
            long id = ids.computeIfAbsent(emoji.id, ignored -> ++nextId);
            if (emoji.unicode.isEmpty())
            {
                values.add(new ServerIcon(id, emoji.dataUri));
            }
        }
        ids.keySet().retainAll(catalog.keySet());
        images.update(values, true);
    }

    Drawable drawable(String id)
    {
        ServerEmoji emoji = catalog.get(id);
        if (emoji == null || !emoji.unicode.isEmpty())
        {
            return null;
        }
        Drawable cached = images.drawable(ids.get(id));
        if (cached == null && emoji.dataUri.isEmpty())
        {
            request.accept(id);
            return null;
        }
        return cached;
    }

    @Override
    public void close()
    {
        images.close();
        catalog.clear();
        ids.clear();
        nextId = 0;
    }
}
