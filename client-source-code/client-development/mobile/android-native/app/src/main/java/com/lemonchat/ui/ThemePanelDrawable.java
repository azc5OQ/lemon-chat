package com.lemonchat.ui;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;

/** draws pocket's stitched panel without a bitmap or web view */
final class ThemePanelDrawable extends Drawable
{
    private final Palette palette;
    private final float density;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path clip = new Path();

    ThemePanelDrawable(Palette palette, float density)
    {
        this.palette = palette;
        this.density = density;
    }

    @Override
    public void draw(Canvas canvas)
    {
        Rect bounds = getBounds();
        RectF panel = new RectF(bounds.left + density * 3, bounds.top + density * 2, bounds.right - density * 3, bounds.bottom - density * 2);
        float radius = 22 * density;
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(palette.background);
        canvas.drawRoundRect(panel, radius, radius, paint);
        int saved = canvas.save();
        clip.reset();
        clip.addRoundRect(panel, radius, radius, Path.Direction.CW);
        canvas.clipPath(clip);
        paint.setStrokeWidth(Math.max(1, density * 0.45f));
        paint.setColor(palette.dark ? 0xff3b444d : 0xffd4d8dc);
        float width = 36 * density;
        float height = 54 * density;
        float run = panel.height() * width / height;
        for (float x = panel.left - run; x < panel.right + run; x += width)
        {
            canvas.drawLine(x, panel.top, x + run, panel.bottom, paint);
            canvas.drawLine(x, panel.top, x - run, panel.bottom, paint);
        }
        canvas.restoreToCount(saved);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(density * 1.5f);
        paint.setColor(palette.dark ? 0xff6a7580 : Color.WHITE);
        canvas.drawRoundRect(panel, radius, radius, paint);
        panel.inset(3 * density, 3 * density);
        paint.setStrokeWidth(Math.max(1, density * 0.6f));
        paint.setColor(palette.border);
        paint.setPathEffect(new android.graphics.DashPathEffect(new float[]{density, 3 * density}, 0));
        canvas.drawRoundRect(panel, radius - 3 * density, radius - 3 * density, paint);
        paint.setPathEffect(null);
    }

    @Override
    public void setAlpha(int alpha)
    {
        paint.setAlpha(alpha);
    }

    @Override
    public void setColorFilter(ColorFilter filter)
    {
        paint.setColorFilter(filter);
    }

    @Override
    public int getOpacity()
    {
        return PixelFormat.TRANSLUCENT;
    }
}
