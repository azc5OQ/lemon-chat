package com.lemonchat.ui;

import android.content.res.Resources;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import com.lemonchat.model.Member;
import com.lemonchat.model.ServerIcon;
import com.lemonchat.model.SessionSnapshot;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;

/** decodes avatar snapshots without owning network state */
final class AvatarImages implements AutoCloseable
{
    private final ServerIconCache images;
    private Map<String, String> catalog = new HashMap<>();
    private long connection = -1;
    private long revision;

    AvatarImages(Resources resources, Runnable changed)
    {
        images = new ServerIconCache(resources, changed, 123000, 128, 8 * 1024 * 1024);
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
        ArrayList<ServerIcon> values = new ArrayList<>();
        Map<String, String> replacement = new HashMap<>();
        for (Member member : snapshot.members)
        {
            if (member.avatarDataUri != null && !member.avatarDataUri.isEmpty())
            {
                values.add(new ServerIcon(member.id, member.avatarDataUri));
                replacement.put(member.publicKey, member.avatarDataUri);
            }
        }
        if (!catalog.equals(replacement))
        {
            catalog = replacement;
            revision++;
        }
        images.update(values);
    }

    /** @return revision of the avatar data used by message rows */
    long revision()
    {
        return revision;
    }

    Drawable drawable(Member member)
    {
        if (member == null || member.musicBot)
        {
            return null;
        }
        Drawable picture = images.drawable(member.id);
        return picture == null ? null : new Circle(picture);
    }

    @Override
    public void close()
    {
        images.close();
        catalog.clear();
        revision++;
    }

    private static final class Circle extends Drawable
    {
        private final Drawable image;
        private final Path outline = new Path();

        Circle(Drawable image)
        {
            this.image = image;
        }

        @Override
        public void draw(Canvas canvas)
        {
            Rect bounds = getBounds();
            outline.reset();
            outline.addCircle(bounds.exactCenterX(), bounds.exactCenterY(), Math.min(bounds.width(), bounds.height()) / 2f, Path.Direction.CW);
            int saved = canvas.save();
            canvas.clipPath(outline);
            int width = Math.max(1, image.getIntrinsicWidth());
            int height = Math.max(1, image.getIntrinsicHeight());
            float scale = Math.max(bounds.width() / (float) width, bounds.height() / (float) height);
            int drawnWidth = Math.round(width * scale);
            int drawnHeight = Math.round(height * scale);
            int left = bounds.left + (bounds.width() - drawnWidth) / 2;
            int top = bounds.top + (bounds.height() - drawnHeight) / 2;
            image.setBounds(left, top, left + drawnWidth, top + drawnHeight);
            image.draw(canvas);
            canvas.restoreToCount(saved);
        }

        @Override
        public void setAlpha(int alpha)
        {
            image.setAlpha(alpha);
        }

        @Override
        public void setColorFilter(ColorFilter filter)
        {
            image.setColorFilter(filter);
        }

        @Override
        public int getOpacity()
        {
            return PixelFormat.TRANSLUCENT;
        }
    }
}
