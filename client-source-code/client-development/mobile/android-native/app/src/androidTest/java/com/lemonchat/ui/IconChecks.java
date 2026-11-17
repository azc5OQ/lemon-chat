package com.lemonchat.ui;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Rect;
import java.io.ByteArrayOutputStream;
import java.lang.reflect.Method;
import java.util.Base64;

/** checks server image bounds using Android's actual bitmap decoder */
public final class IconChecks
{
    private int checks;

    /** @return number of passing image checks */
    public static int verify() throws Exception
    {
        IconChecks suite = new IconChecks();
        suite.run();
        return suite.checks;
    }

    private void run() throws Exception
    {
        check(!ServerIconCache.supported("https://example.org/icon.png"), "remote images are rejected");
        check(!ServerIconCache.supported("data:image/svg+xml;base64,PHN2Zy8+"), "custom SVG is unsupported");
        check(!ServerIconCache.supported("data:image/png;base64," + "A".repeat(40960)), "encoded payload limit");
        check(decode("data:image/png;base64,@@@@") == null, "invalid base64");
        check(decode("data:image/png;base64,AAAA") == null, "invalid image bytes");
        String png = image(20, 20, Bitmap.CompressFormat.PNG, "png");
        Bitmap decoded = decode(png);
        check(decoded != null && decoded.getWidth() == 20 && decoded.getHeight() == 20, "original PNG dimensions");
        release(decoded);
        decoded = decode(png.replace("image/png", "IMAGE/PNG"));
        check(decoded != null, "case insensitive image header");
        release(decoded);
        decoded = decode(image(1024, 1024, Bitmap.CompressFormat.PNG, "png"));
        check(decoded != null && decoded.getWidth() <= 64 && decoded.getHeight() <= 64 && decoded.getAllocationByteCount() <= 65536, "large image sampled before display");
        release(decoded);
        check(decode(image(1025, 1, Bitmap.CompressFormat.PNG, "png")) == null, "oversized width");
        check(decode(image(1, 1025, Bitmap.CompressFormat.PNG, "png")) == null, "oversized height");
        decoded = decode(image(32, 16, Bitmap.CompressFormat.JPEG, "jpeg"));
        check(decoded != null && decoded.getWidth() == 32, "JPEG image");
        release(decoded);
        decoded = decode(image(32, 16, Bitmap.CompressFormat.WEBP, "webp"));
        check(decoded != null && decoded.getWidth() == 32, "WebP image");
        release(decoded);
        decoded = decode("data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==");
        check(decoded != null && decoded.getWidth() == 1, "GIF first frame");
        release(decoded);
        glyphBounds(64, 96);
        glyphBounds(96, 64);
    }

    private void glyphBounds(int width, int height)
    {
        Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        GlyphDrawable drawable = new GlyphDrawable("circle", 0xffffffff);
        drawable.setBounds(0, 0, width, height);
        drawable.draw(new Canvas(bitmap));
        Rect pixels = new Rect(width, height, 0, 0);
        for (int y = 0; y < height; y++)
        {
            for (int x = 0; x < width; x++)
            {
                if ((bitmap.getPixel(x, y) >>> 24) != 0)
                {
                    pixels.left = Math.min(pixels.left, x);
                    pixels.top = Math.min(pixels.top, y);
                    pixels.right = Math.max(pixels.right, x + 1);
                    pixels.bottom = Math.max(pixels.bottom, y + 1);
                }
            }
        }
        check(Math.abs(pixels.width() - pixels.height()) <= 1, "glyph keeps square proportions in rectangular bounds");
        check(Math.abs(pixels.centerX() - width / 2) <= 1 && Math.abs(pixels.centerY() - height / 2) <= 1, "glyph stays centered in rectangular bounds");
        bitmap.recycle();
    }

    private Bitmap decode(String data) throws Exception
    {
        Method method = ServerIconCache.class.getDeclaredMethod("decode", String.class);
        method.setAccessible(true);
        return (Bitmap) method.invoke(null, data);
    }

    private String image(int width, int height, Bitmap.CompressFormat format, String type)
    {
        Bitmap image = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        image.eraseColor(0xff558866);
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        image.compress(format, 90, bytes);
        image.recycle();
        return "data:image/" + type + ";base64," + Base64.getEncoder().encodeToString(bytes.toByteArray());
    }

    private void release(Bitmap bitmap)
    {
        if (bitmap != null)
        {
            bitmap.recycle();
        }
    }

    private void check(boolean value, String description)
    {
        if (!value)
        {
            throw new AssertionError(description);
        }
        checks++;
    }
}
