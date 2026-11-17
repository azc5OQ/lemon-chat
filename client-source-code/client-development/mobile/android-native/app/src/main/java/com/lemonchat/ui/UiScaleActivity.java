package com.lemonchat.ui;

import android.app.Activity;
import android.content.Context;
import android.content.res.Configuration;
import android.view.ContextThemeWrapper;
import com.lemonchat.settings.LocalSettings;

/** applies app size without changing Android's display or font settings */
public abstract class UiScaleActivity extends Activity
{
    private Configuration platformConfiguration;
    private int uiScale = 100;

    @Override
    protected void attachBaseContext(Context base)
    {
        platformConfiguration = new Configuration(base.getResources().getConfiguration());
        uiScale = LocalSettings.load(base).getInt("native_ui_scale");
        super.attachBaseContext(base.createConfigurationContext(scaledConfiguration(platformConfiguration, uiScale)));
    }

    final int appliedUiScale()
    {
        return uiScale;
    }

    /**
     * creates a temporary settings preview using the original display density
     *
     * @param percent requested app size
     * @param theme Android theme for the dialog
     * @return context with isolated preview resources
     */
    final Context previewContext(int percent, int theme)
    {
        ContextThemeWrapper preview = new ContextThemeWrapper(this, theme);
        preview.applyOverrideConfiguration(scaledConfiguration(platformConfiguration, percent));
        return preview;
    }

    static Configuration scaledConfiguration(Configuration original, int percent)
    {
        Configuration result = new Configuration(original);
        int size = Math.max(70, Math.min(130, percent));
        result.densityDpi = Math.max(1, Math.round(original.densityDpi * size / 100f));
        result.screenWidthDp = Math.round(original.screenWidthDp * 100f / size);
        result.screenHeightDp = Math.round(original.screenHeightDp * 100f / size);
        result.smallestScreenWidthDp = Math.round(original.smallestScreenWidthDp * 100f / size);
        return result;
    }
}
