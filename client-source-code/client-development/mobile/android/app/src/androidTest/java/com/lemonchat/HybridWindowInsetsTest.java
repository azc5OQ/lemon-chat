package com.lemonchat;

import android.os.Build;
import android.view.View;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.lang.reflect.Method;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;
import static org.junit.Assume.assumeTrue;

/** checks real view padding without starting the chat service */
@RunWith(AndroidJUnit4.class)
public final class HybridWindowInsetsTest
{
    @Test
    public void keyboardAndCutoutInsetsDoNotAccumulate()
    {
        assumeTrue(Build.VERSION.SDK_INT >= 30);
        InstrumentationRegistry.getInstrumentation().runOnMainSync(() ->
        {
            View root = new View(InstrumentationRegistry.getInstrumentation().getTargetContext());
            root.setPadding(10, 20, 30, 40);
            install(root);
            WindowInsetsCompat keyboard = new WindowInsetsCompat.Builder().setInsets(WindowInsetsCompat.Type.systemBars(), Insets.of(0, 24, 0, 48)).setInsets(WindowInsetsCompat.Type.displayCutout(), Insets.of(35, 0, 0, 0)).setInsets(WindowInsetsCompat.Type.ime(), Insets.of(0, 0, 0, 300)).build();
            assertTrue(ViewCompat.dispatchApplyWindowInsets(root, keyboard).isConsumed());
            padding(root, 45, 44, 30, 340);
            ViewCompat.dispatchApplyWindowInsets(root, keyboard);
            padding(root, 45, 44, 30, 340);
            install(root);
            ViewCompat.dispatchApplyWindowInsets(root, keyboard);
            padding(root, 45, 44, 30, 340);
            WindowInsetsCompat hidden = new WindowInsetsCompat.Builder().setInsets(WindowInsetsCompat.Type.systemBars(), Insets.of(0, 24, 0, 48)).build();
            ViewCompat.dispatchApplyWindowInsets(root, hidden);
            padding(root, 10, 44, 30, 88);
            ViewCompat.dispatchApplyWindowInsets(root, new WindowInsetsCompat.Builder().build());
            padding(root, 10, 20, 30, 40);
        });
    }

    @Test
    public void olderAndroidKeepsExistingPadding()
    {
        assumeTrue(Build.VERSION.SDK_INT < 35);
        InstrumentationRegistry.getInstrumentation().runOnMainSync(() ->
        {
            View root = new View(InstrumentationRegistry.getInstrumentation().getTargetContext());
            root.setPadding(10, 20, 30, 40);
            HybridWindowInsets.apply(null, root);
            padding(root, 10, 20, 30, 40);
        });
    }

    private static void install(View root)
    {
        try
        {
            Method method = HybridWindowInsets.class.getDeclaredMethod("applyToRoot", View.class);
            method.setAccessible(true);
            method.invoke(null, root);
        }
        catch (ReflectiveOperationException failure)
        {
            throw new AssertionError(failure);
        }
    }

    private static void padding(View view, int left, int top, int right, int bottom)
    {
        assertEquals(left, view.getPaddingLeft());
        assertEquals(top, view.getPaddingTop());
        assertEquals(right, view.getPaddingRight());
        assertEquals(bottom, view.getPaddingBottom());
    }
}
