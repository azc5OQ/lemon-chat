package com.lemonchat;

import android.content.Context;
import android.content.res.Configuration;
import android.view.ContextThemeWrapper;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.EditText;
import android.widget.LinearLayout;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.google.android.material.textfield.TextInputLayout;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

/** checks the actual outlined resources without opening a connection */
@RunWith(AndroidJUnit4.class)
public final class ConnectionFieldTest
{
    @Test
    public void outlinedFieldsHaveBalancedPadding()
    {
        verify(1.0f);
    }

    @Test
    public void largerTextFitsWithoutChangingDeviceSettings()
    {
        verify(1.6f);
    }

    private void verify(float fontScale)
    {
        InstrumentationRegistry.getInstrumentation().runOnMainSync(() ->
        {
            Context target = InstrumentationRegistry.getInstrumentation().getTargetContext();
            float original = target.getResources().getConfiguration().fontScale;
            Configuration configuration = new Configuration(target.getResources().getConfiguration());
            configuration.fontScale = fontScale;
            Context context = new ContextThemeWrapper(target.createConfigurationContext(configuration), R.style.Theme_WebViewTest);
            LayoutInflater inflater = LayoutInflater.from(context);
            LinearLayout parent = new LinearLayout(context);
            TextInputLayout wizard = (TextInputLayout)inflater.inflate(R.layout.connection_wizard_field, parent, false);
            wizard.setHint("IP address or hostname");
            EditText input = wizard.findViewById(R.id.connection_wizard_input);
            input.setText("127.0.0.1");
            assertEquals(TextInputLayout.BOX_BACKGROUND_OUTLINE, wizard.getBoxBackgroundMode());
            measure(wizard, context);
            balanced(input);
            View keyRow = inflater.inflate(R.layout.key_field_row, parent, false);
            EditText password = keyRow.findViewById(R.id.key_input);
            password.setText("synthetic check");
            measure(keyRow, context);
            balanced(password);
            View advanced = inflater.inflate(R.layout.main_activity, parent, false);
            measure(advanced, context);
            android.widget.Button setup = advanced.findViewById(R.id.run_connection_setup);
            assertEquals("Run setup again", setup.getText().toString());
            assertEquals(View.VISIBLE, setup.getVisibility());
            assertTrue(setup.getMeasuredHeight() >= Math.round(48 * context.getResources().getDisplayMetrics().density));
            for (int id : new int[]{R.id.ip_address, R.id.websocket_port, R.id.default_username})
            {
                EditText field = advanced.findViewById(id);
                assertEquals(field.getPaddingTop(), field.getPaddingBottom());
            }
            assertEquals(original, target.getResources().getConfiguration().fontScale, 0.0f);
        });
    }

    private void measure(View view, Context context)
    {
        int width = Math.round(300 * context.getResources().getDisplayMetrics().density);
        view.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
        view.layout(0, 0, view.getMeasuredWidth(), view.getMeasuredHeight());
    }

    private void balanced(EditText input)
    {
        assertEquals(input.getPaddingTop(), input.getPaddingBottom());
        assertEquals(1, input.getLineCount());
        assertTrue(input.getMeasuredHeight() - input.getCompoundPaddingTop() - input.getCompoundPaddingBottom() >= input.getLayout().getLineBottom(0));
    }
}
