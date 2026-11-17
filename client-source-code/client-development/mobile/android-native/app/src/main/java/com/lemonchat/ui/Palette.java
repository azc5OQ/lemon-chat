package com.lemonchat.ui;

import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.LayerDrawable;
import android.graphics.drawable.RippleDrawable;
import android.view.Gravity;
import android.content.res.ColorStateList;

/** shared colors and surfaces for the native screens */
final class Palette
{
    final boolean dark;
    final NativeTheme theme;
    final int background;
    final int surface;
    final int text;
    final int secondary;
    final int border;
    final int accent;
    final int accentText;
    final int incoming;
    final int outgoing;
    final int outgoingText;
    final int outgoingSecondary;
    final int selection;
    final int danger;
    final int input;
    final int bar;
    final int barText;
    final int barSecondary;

    Palette(boolean dark)
    {
        this(NativeTheme.find(dark ? "mono-dark" : "mono"), dark);
    }

    Palette(NativeTheme theme, boolean dark)
    {
        this.theme = theme;
        this.dark = dark;
        background = theme.color(dark, 0);
        surface = theme.color(dark, 1);
        text = theme.color(dark, 2);
        secondary = theme.color(dark, 3);
        border = theme.color(dark, 4);
        accent = theme.color(dark, 5);
        accentText = theme.color(dark, 6);
        incoming = theme.color(dark, 7);
        outgoing = theme.color(dark, 8);
        outgoingText = theme.quilt ? Color.WHITE : text;
        outgoingSecondary = theme.quilt ? 0xffd2d9df : theme.id.equals("bluebell") && dark ? 0xffe2defa : secondary;
        selection = theme.quilt && !dark ? 0xffdfe5e9 : outgoing;
        input = theme.color(dark, 9);
        danger = Color.parseColor(dark ? "#F0ADA6" : "#AD4640");
        bar = theme.quilt ? Color.parseColor("#24282c") : background;
        barText = theme.quilt ? Color.WHITE : text;
        barSecondary = theme.quilt ? Color.parseColor("#c0c8ce") : secondary;
    }

    static Palette load(Context context)
    {
        NativeTheme theme = NativeTheme.selected(context);
        return new Palette(theme, theme.dark(context.getSharedPreferences("preferences", Context.MODE_PRIVATE)));
    }

    GradientDrawable barSurface()
    {
        if (theme.quilt)
        {
            return new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[]{Color.parseColor("#333a40"), Color.parseColor("#15181b")});
        }
        return rounded(bar, 0);
    }

    android.graphics.drawable.Drawable panelSurface(float density)
    {
        if (theme.quilt)
        {
            return new ThemePanelDrawable(this, density);
        }
        if (dark && theme.id.equals("default-mobile"))
        {
            return new GradientDrawable(GradientDrawable.Orientation.TL_BR, new int[]{0xff10151b, 0xff17333c, 0xff177b8e});
        }
        if (dark && theme.id.equals("bluebell"))
        {
            return new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[]{0xff121933, 0xff0c1022});
        }
        return rounded(background, 0);
    }

    /**
     * @brief background of the people column at the left of the chat. Mobile 1 gets a vertical gradient that
     *        ends in the teal of its chat background and a line at the right edge; the other themes keep their
     *        flat surface colour.
     *
     * @param density display density, used for the width of the edge line
     *
     * @return the drawable for the column
     */
    Drawable memberColumnSurface(float density)
    {
        // other themes
        if (!theme.id.equals("default-mobile"))
        {
            return rounded(surface, 0);
        }

        // gradient and edge line
        GradientDrawable fill = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, dark ? new int[]{0xff1d2731, 0xff172f39, 0xff135261} : new int[]{0xfff7fafb, 0xffe7f1f3, 0xffd2e8eb});
        GradientDrawable edge = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, dark ? new int[]{0x2617a2b8, 0x7317a2b8} : new int[]{0x1f177b8e, 0x4d177b8e});
        LayerDrawable layers = new LayerDrawable(new Drawable[]{fill, edge});
        layers.setLayerGravity(1, Gravity.END | Gravity.FILL_VERTICAL);
        layers.setLayerWidth(1, Math.max(1, Math.round(density)));
        return layers;
    }

    GradientDrawable microphone(boolean active, float radius)
    {
        GradientDrawable drawable;
        if (theme.quilt && !active)
        {
            drawable = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[]{Color.WHITE, Color.parseColor("#c9cfd5"), Color.parseColor("#f3f5f6")});
            drawable.setCornerRadius(radius);
        }
        else
        {
            drawable = rounded(active ? accent : surface, radius);
        }
        drawable.setStroke(Math.max(1, Math.round(radius / 28)), border);
        return drawable;
    }

    GradientDrawable messageSurface(boolean sent, float radius)
    {
        if (theme.quilt && !dark)
        {
            GradientDrawable drawable = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, sent ? new int[]{0xff525960, 0xff2b3035} : new int[]{Color.WHITE, 0xffe9edf0});
            drawable.setCornerRadius(radius);
            drawable.setStroke(Math.max(1, Math.round(radius / 22)), border);
            return drawable;
        }
        if (theme.id.equals("bluebell") && dark && sent)
        {
            GradientDrawable drawable = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, new int[]{0xff5b49d8, 0xff3a63c8});
            drawable.setCornerRadius(radius);
            return drawable;
        }
        return rounded(theme.tree ? Color.TRANSPARENT : sent ? outgoing : incoming, radius);
    }

    GradientDrawable rounded(int color, float radius)
    {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(color);
        drawable.setCornerRadius(radius);
        return drawable;
    }

    /** gives clickable surfaces visible touch and keyboard feedback */
    Drawable interactive(int color, float radius)
    {
        int highlight = (dark ? 0x50000000 : 0x30000000) | (text & 0x00ffffff);
        ColorStateList states = new ColorStateList(new int[][]{new int[]{android.R.attr.state_focused}, new int[]{android.R.attr.state_pressed}, new int[]{}}, new int[]{highlight, highlight, highlight});
        return new RippleDrawable(states, rounded(color, radius), rounded(Color.WHITE, radius));
    }
}
