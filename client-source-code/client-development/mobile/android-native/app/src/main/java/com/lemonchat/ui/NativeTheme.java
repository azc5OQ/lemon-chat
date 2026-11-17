package com.lemonchat.ui;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import com.lemonchat.R;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/** keeps each native theme's colors and layout together */
final class NativeTheme
{
    static final int NO_STRIP = 0;
    static final int TOP_STRIP = 1;
    static final int SIDE_STRIP = 2;
    static final int MOVABLE_STRIP = 3;

    // background, surface, text, secondary, border, accent, accent text, incoming, outgoing, input
    private static final NativeTheme MOBILE = new NativeTheme("default-mobile", "Mobile 1", "Compact channel tree and teal controls", true, SIDE_STRIP, true, false, 56, 56, 5,
        "#17262c #24373e #e6edef #9bafb6 #344b54 #05a3ff #ffffff #25434d #254b64 #1b3038",
        "#f1f6f8 #ffffff #172c36 #607985 #d5e2e8 #087ebd #ffffff #ffffff #d8ecf8 #e6eef2");
    private static final NativeTheme MOBILE_2 = new NativeTheme("mobile-2", "Mobile 2", "Blue channel tree with people beside chat", true, SIDE_STRIP, true, false, 56, 56, 5,
        "#1c2b43 #17243a #dce8f7 #91a8c2 #293d57 #8475ff #ffffff #263953 #304963 #263953",
        "#edf2fa #ffffff #1c2b43 #667d9b #d4deee #6956dc #ffffff #ffffff #e1ddfc #e2eaf5");
    private static final NativeTheme POCKET = new NativeTheme("pocket", "Pocket", "Quilted panels and chrome controls", false, NO_STRIP, false, true, 62, 64, 22,
        "#252b30 #343c44 #f2f4f5 #b7c1ca #59636d #dbe2e8 #252c33 #343c44 #46515c #30383f",
        "#f7f8f9 #f1f3f4 #1c2024 #626a72 #c8cfd5 #3d444c #ffffff #ffffff #343a40 #ffffff");
    private static final NativeTheme MONO_LIGHT = new NativeTheme("mono", "Mono Light", "Simple white conversations", false, NO_STRIP, false, false, 64, 64, 16,
        "#ffffff #f4f4f5 #19191c #696970 #e5e5e8 #19191c #ffffff #f4f4f5 #e8e8eb #f4f4f5",
        "#ffffff #f4f4f5 #19191c #696970 #e5e5e8 #19191c #ffffff #f4f4f5 #e8e8eb #f4f4f5");
    private static final NativeTheme MONO_DARK = new NativeTheme("mono-dark", "Mono Dark", "Simple dark conversations", false, NO_STRIP, true, false, 64, 64, 16,
        "#09090b #19191c #f2f2f3 #a0a0a9 #303036 #f2f2f3 #101013 #19191c #303036 #19191c",
        "#09090b #19191c #f2f2f3 #a0a0a9 #303036 #f2f2f3 #101013 #19191c #303036 #19191c");
    private static final NativeTheme SIMPLE = new NativeTheme("simple", "Simple", "Warm surfaces and lemon controls", false, NO_STRIP, false, false, 64, 64, 14,
        "#131311 #201f1b #f4f1e8 #aaa698 #36332b #ffd83d #332a00 #201f1b #3b3310 #201f1b",
        "#ffffff #f5f3ec #1d1c18 #67645a #e7e3d6 #ffd83d #332a00 #f5f3ec #fff3ba #f5f3ec");
    private static final NativeTheme BLUEBELL = new NativeTheme("bluebell", "Bluebell", "Blue and violet with a movable people strip", false, MOVABLE_STRIP, true, false, 62, 64, 18,
        "#0c1022 #141a32 #eef0ff #9ca6c4 #2a3557 #7b6cf6 #ffffff #1d2440 #39336b #1b2242",
        "#f1f0fa #ffffff #27223e #77708f #ddd8ee #735bd5 #ffffff #ffffff #e4dcfc #e8e4f4");

    static final List<NativeTheme> ALL = Collections.unmodifiableList(Arrays.asList(MOBILE, MOBILE_2, POCKET, MONO_LIGHT, MONO_DARK, SIMPLE, BLUEBELL));
    final String id;
    final String name;
    final String description;
    final boolean tree;
    final int strip;
    final boolean defaultDark;
    final boolean quilt;
    final int toolbarHeight;
    final int navigationHeight;
    final int radius;
    private final int[] darkColors;
    private final int[] lightColors;

    private NativeTheme(String id, String name, String description, boolean tree, int strip, boolean defaultDark, boolean quilt, int toolbarHeight, int navigationHeight, int radius, String darkColors, String lightColors)
    {
        this.id = id;
        this.name = name;
        this.description = description;
        this.tree = tree;
        this.strip = strip;
        this.defaultDark = defaultDark;
        this.quilt = quilt;
        this.toolbarHeight = toolbarHeight;
        this.navigationHeight = navigationHeight;
        this.radius = radius;
        this.darkColors = colors(darkColors);
        this.lightColors = colors(lightColors);
    }

    static NativeTheme find(String id)
    {
        for (NativeTheme theme : ALL)
        {
            if (theme.id.equals(id))
            {
                return theme;
            }
        }
        return MOBILE;
    }

    static NativeTheme selected(Context context)
    {
        return find(context.getSharedPreferences("preferences", Context.MODE_PRIVATE).getString("theme", "default-mobile"));
    }

    boolean allowsAppearance()
    {
        return !id.equals("mono") && !id.equals("mono-dark");
    }

    boolean dark(SharedPreferences preferences)
    {
        return allowsAppearance() ? preferences.getBoolean("theme-dark-" + id, defaultDark) : defaultDark;
    }

    int color(boolean dark, int index)
    {
        return (dark ? darkColors : lightColors)[index];
    }

    boolean sideStrip(boolean requested)
    {
        return strip == SIDE_STRIP || strip == MOVABLE_STRIP && requested;
    }

    int sideWidth(int screenWidth)
    {
        return tree ? Math.max(76, Math.min(110, screenWidth / 4)) : 76;
    }

    boolean hasNavigation()
    {
        return !tree;
    }

    int messageTextSize()
    {
        return tree ? 14 : 15;
    }

    int windowStyle(boolean dark)
    {
        switch (id)
        {
            case "default-mobile":
                return dark ? R.style.NativeMobileDark : R.style.NativeMobileLight;
            case "mobile-2":
                return dark ? R.style.NativeMobile2Dark : R.style.NativeMobile2Light;
            case "pocket":
                return dark ? R.style.NativePocketDark : R.style.NativePocketLight;
            case "simple":
                return dark ? R.style.NativeSimpleDark : R.style.NativeSimpleLight;
            case "bluebell":
                return dark ? R.style.NativeBluebellDark : R.style.NativeBluebellLight;
            default:
                return dark ? R.style.NativeDark : R.style.NativeLight;
        }
    }

    private static int[] colors(String values)
    {
        String[] parts = values.split(" ");
        int[] colors = new int[parts.length];
        for (int index = 0; index < parts.length; index++)
        {
            colors[index] = Color.parseColor(parts[index]);
        }
        return colors;
    }
}
