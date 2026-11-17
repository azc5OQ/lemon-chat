package com.lemonchat.ui;

import android.app.Dialog;
import android.content.Context;
import android.os.Build;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.FrameLayout;

/** keeps administration controls within the keyboard and system insets */
class AdminWindow extends Dialog
{
    private final FrameLayout viewport;

    AdminWindow(Context context, View content, int maxWidth, int maxHeight)
    {
        super(context, windowTheme(context));
        viewport = new FrameLayout(context)
        {
            @Override
            protected void onMeasure(int widthSpec, int heightSpec)
            {
                FrameLayout.LayoutParams position = (FrameLayout.LayoutParams)content.getLayoutParams();
                position.width = Math.min(dp(maxWidth), Math.max(0, MeasureSpec.getSize(widthSpec) - getPaddingLeft() - getPaddingRight()));
                position.height = Math.min(dp(maxHeight), Math.max(0, MeasureSpec.getSize(heightSpec) - getPaddingTop() - getPaddingBottom()));
                super.onMeasure(widthSpec, heightSpec);
            }
        };
        viewport.setTag("admin-window-viewport");
        viewport.setPadding(dp(8), dp(8), dp(8), dp(8));
        viewport.addView(content, new FrameLayout.LayoutParams(0, 0, Gravity.CENTER));
        setCanceledOnTouchOutside(false);
        setContentView(viewport);
    }

    @Override
    protected void onCreate(Bundle saved)
    {
        super.onCreate(saved);
        Window window = getWindow();
        if (window == null)
        {
            return;
        }
        window.setBackgroundDrawableResource(android.R.color.transparent);
        window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        window.setDimAmount(0.4f);
        window.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
        if (Build.VERSION.SDK_INT >= 30)
        {
            window.setDecorFitsSystemWindows(false);
            viewport.setOnApplyWindowInsetsListener((view, insets) ->
            {
                android.graphics.Insets edges = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout() | WindowInsets.Type.ime());
                view.setPadding(edges.left + dp(8), edges.top + dp(8), edges.right + dp(8), edges.bottom + dp(8));
                return insets;
            });
        }
        window.setLayout(WindowManager.LayoutParams.MATCH_PARENT, WindowManager.LayoutParams.MATCH_PARENT);
    }

    private int dp(int value)
    {
        return Math.round(value * getContext().getResources().getDisplayMetrics().density);
    }

    private static int windowTheme(Context context)
    {
        Palette palette = Palette.load(context);
        return palette.theme.windowStyle(palette.dark);
    }
}
