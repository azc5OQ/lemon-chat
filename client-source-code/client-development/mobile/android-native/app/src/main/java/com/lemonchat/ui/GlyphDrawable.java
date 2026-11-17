package com.lemonchat.ui;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;

/** draws the same outline icon set throughout the app */
final class GlyphDrawable extends Drawable
{
    private final String kind;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

    GlyphDrawable(String kind, int color)
    {
        this.kind = kind;
        paint.setColor(color);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(1.7f);
        paint.setStrokeCap(Paint.Cap.ROUND);
        paint.setStrokeJoin(Paint.Join.ROUND);
    }

    @Override
    public void draw(Canvas canvas)
    {
        Rect bounds = getBounds();
        canvas.save();
        float size = Math.min(bounds.width(), bounds.height());
        canvas.translate(bounds.left + (bounds.width() - size) / 2f, bounds.top + (bounds.height() - size) / 2f);
        canvas.scale(size / 24f, size / 24f);
        switch (kind)
        {
            case "eye":
            case "eye-off":
                android.graphics.Path eye = new android.graphics.Path();
                eye.moveTo(2, 12);
                eye.cubicTo(7, 3, 17, 3, 22, 12);
                eye.cubicTo(17, 21, 7, 21, 2, 12);
                canvas.drawPath(eye, paint);
                canvas.drawCircle(12, 12, 3, paint);
                if (kind.equals("eye-off"))
                {
                    canvas.drawLine(3, 3, 21, 21, paint);
                }
                break;
            case "theme":
                canvas.drawCircle(12, 12, 8, paint);
                paint.setStyle(Paint.Style.FILL);
                canvas.drawArc(4, 4, 20, 20, 90, 180, true, paint);
                paint.setStyle(Paint.Style.STROKE);
                break;
            case "people-side":
                canvas.drawRoundRect(3, 3, 21, 21, 2, 2, paint);
                canvas.drawLine(9, 3, 9, 21, paint);
                canvas.drawCircle(6, 8, 1, paint);
                canvas.drawCircle(6, 13, 1, paint);
                canvas.drawCircle(6, 18, 1, paint);
                break;
            case "people-top":
                canvas.drawRoundRect(3, 3, 21, 21, 2, 2, paint);
                canvas.drawLine(3, 9, 21, 9, paint);
                canvas.drawCircle(7, 6, 1, paint);
                canvas.drawCircle(12, 6, 1, paint);
                canvas.drawCircle(17, 6, 1, paint);
                break;
            case "people":
                canvas.drawCircle(9, 8, 3, paint);
                path(canvas, 3, 20, 3, 18, 5, 15, 9, 14, 13, 15, 15, 18, 15, 20);
                path(canvas, 17, 5, 20, 7, 20, 10, 18, 12);
                path(canvas, 18, 15, 21, 17, 21, 20);
                break;
            case "chat":
                path(canvas, 4, 4, 20, 4, 20, 17, 10, 17, 5, 21, 5, 17, 4, 17, 4, 4);
                canvas.drawLine(8, 9, 16, 9, paint);
                canvas.drawLine(8, 13, 13, 13, paint);
                break;
            case "call":
                path(canvas, 4, 3, 8, 3, 10, 8, 7, 10, 10, 14, 14, 17, 16, 14, 21, 16, 21, 20, 18, 22, 13, 20, 7, 16, 3, 10, 2, 6, 4, 3);
                break;
            case "channels":
                canvas.drawLine(9, 3, 6, 21, paint);
                canvas.drawLine(17, 3, 14, 21, paint);
                canvas.drawLine(4, 9, 21, 9, paint);
                canvas.drawLine(3, 15, 20, 15, paint);
                break;
            case "me":
                canvas.drawCircle(12, 8, 4, paint);
                path(canvas, 4, 21, 4, 19, 7, 15, 12, 14, 17, 15, 20, 19, 20, 21);
                break;
            case "mic":
            case "mic-off":
            case "mic-active":
                canvas.drawRoundRect(9, 2, 15, 14, 3, 3, paint);
                path(canvas, 5, 10, 5, 13, 7, 17, 12, 19, 17, 17, 19, 13, 19, 10);
                canvas.drawLine(12, 19, 12, 23, paint);
                if (kind.equals("mic-active"))
                {
                    canvas.drawLine(2, 7, 2, 14, paint);
                    canvas.drawLine(22, 7, 22, 14, paint);
                }
                if (kind.equals("mic-off"))
                {
                    canvas.drawLine(3, 3, 21, 21, paint);
                }
                break;
            case "speaker-off":
                path(canvas, 3, 9, 7, 9, 12, 5, 12, 19, 7, 15, 3, 15, 3, 9);
                canvas.drawLine(16, 9, 22, 15, paint);
                canvas.drawLine(22, 9, 16, 15, paint);
                break;
            case "speaking":
                canvas.drawLine(4, 10, 4, 14, paint);
                canvas.drawLine(8, 6, 8, 18, paint);
                canvas.drawLine(12, 3, 12, 21, paint);
                canvas.drawLine(16, 7, 16, 17, paint);
                canvas.drawLine(20, 10, 20, 14, paint);
                break;
            case "down":
                path(canvas, 6, 9, 12, 15, 18, 9);
                break;
            case "send":
                path(canvas, 3, 3, 22, 12, 3, 21, 7, 12, 3, 3);
                canvas.drawLine(7, 12, 22, 12, paint);
                break;
            case "back":
                path(canvas, 15, 4, 7, 12, 15, 20);
                break;
            case "settings":
                canvas.drawLine(3, 6, 21, 6, paint);
                canvas.drawLine(3, 12, 21, 12, paint);
                canvas.drawLine(3, 18, 21, 18, paint);
                canvas.drawCircle(8, 6, 2, paint);
                canvas.drawCircle(16, 12, 2, paint);
                canvas.drawCircle(10, 18, 2, paint);
                break;
            case "lock":
                canvas.drawRoundRect(5, 10, 19, 22, 3, 3, paint);
                path(canvas, 8, 10, 8, 6, 10, 3, 14, 3, 16, 6, 16, 10);
                break;
            case "close":
                canvas.drawLine(6, 6, 18, 18, paint);
                canvas.drawLine(18, 6, 6, 18, paint);
                break;
            case "search":
                canvas.drawCircle(10, 10, 6, paint);
                canvas.drawLine(15, 15, 21, 21, paint);
                break;
            case "attach":
                path(canvas, 8, 15, 15, 8, 18, 8, 19, 10, 18, 12, 9, 21, 5, 21, 2, 18, 2, 14, 14, 2, 18, 2, 22, 6, 22, 10, 10, 22);
                break;
            case "arrow":
                path(canvas, 9, 6, 15, 12, 9, 18);
                break;
            case "music":
                path(canvas, 9, 17, 9, 4, 20, 2, 20, 15);
                canvas.drawOval(3, 16, 9, 21, paint);
                canvas.drawOval(14, 14, 20, 19, paint);
                break;
            default:
                canvas.drawCircle(12, 12, 9, paint);
                break;
        }
        canvas.restore();
    }

    private void path(Canvas canvas, float... values)
    {
        Path path = new Path();
        path.moveTo(values[0], values[1]);
        for (int i = 2; i < values.length; i += 2)
        {
            path.lineTo(values[i], values[i + 1]);
        }
        canvas.drawPath(path, paint);
    }

    @Override
    public void setAlpha(int alpha)
    {
        paint.setAlpha(alpha);
        invalidateSelf();
    }

    @Override
    public void setColorFilter(ColorFilter filter)
    {
        paint.setColorFilter(filter);
        invalidateSelf();
    }

    @Override
    public int getIntrinsicWidth()
    {
        return 24;
    }

    @Override
    public int getIntrinsicHeight()
    {
        return 24;
    }

    @Override
    public int getOpacity()
    {
        return PixelFormat.TRANSLUCENT;
    }
}
