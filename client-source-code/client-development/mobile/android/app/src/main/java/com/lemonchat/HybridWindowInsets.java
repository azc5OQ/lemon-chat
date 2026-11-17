package com.lemonchat;

import android.app.Activity;
import android.os.Build;
import android.view.View;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import java.util.WeakHashMap;

/** keeps controls clear of system bars and the keyboard */
public final class HybridWindowInsets
{
    private static final WeakHashMap<View, Insets> originalPadding = new WeakHashMap<>();

    private HybridWindowInsets()
    {
    }

    /**
     * applies window insets where Android enforces edge to edge
     *
     * @param activity visible window owner
     * @param root content root receiving safe padding
     */
    public static void apply(Activity activity, View root)
    {
        if (Build.VERSION.SDK_INT < 35 || root == null)
        {
            return;
        }
        WindowCompat.setDecorFitsSystemWindows(activity.getWindow(), false);
        applyToRoot(root);
    }

    private static void applyToRoot(View root)
    {
        if (!originalPadding.containsKey(root))
        {
            originalPadding.put(root, Insets.of(root.getPaddingLeft(), root.getPaddingTop(), root.getPaddingRight(), root.getPaddingBottom()));
        }
        Insets padding = originalPadding.get(root);
        ViewCompat.setOnApplyWindowInsetsListener(root, (view, windowInsets) ->
        {
            Insets safe = windowInsets.getInsets(WindowInsetsCompat.Type.systemBars() | WindowInsetsCompat.Type.displayCutout() | WindowInsetsCompat.Type.ime());
            view.setPadding(padding.left + safe.left, padding.top + safe.top, padding.right + safe.right, padding.bottom + safe.bottom);
            return WindowInsetsCompat.CONSUMED;
        });
        ViewCompat.requestApplyInsets(root);
    }
}
