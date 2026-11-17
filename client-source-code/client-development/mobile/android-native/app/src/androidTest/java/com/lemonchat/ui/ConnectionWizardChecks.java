package com.lemonchat.ui;

import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.graphics.Insets;
import android.graphics.Rect;
import android.os.Build;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Predicate;

/** checks guided connection edits without opening a network connection */
public final class ConnectionWizardChecks
{
    private int checks;

    /**
     * checks validation, review, retained drafts and final submission
     *
     * @param instrumentation Android runner
     * @return completed assertion count
     * @throws Exception when a check fails
     */
    public static int verify(Instrumentation instrumentation) throws Exception
    {
        ConnectionWizardChecks suite = new ConnectionWizardChecks();
        Context context = instrumentation.getTargetContext();
        MainActivity activity = (MainActivity)instrumentation.startActivitySync(new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        AtomicReference<Throwable> failure = new AtomicReference<>();
        instrumentation.runOnMainSync(() ->
        {
            try
            {
                suite.run(activity);
            }
            catch (Throwable error)
            {
                failure.set(error);
            }
            finally
            {
                activity.finish();
            }
        });
        if (failure.get() != null)
        {
            throw new AssertionError("Connection wizard checks failed", failure.get());
        }
        return suite.checks;
    }

    private void run(MainActivity activity)
    {
        String originalEndpoint = activity.getSharedPreferences("preferences", Context.MODE_PRIVATE).getString("endpoint", "");
        ConnectionWizard.Draft draft = new ConnectionWizard.Draft("", "", "");
        AtomicReference<String> submission = new AtomicReference<>();
        ConnectionWizard wizard = new ConnectionWizard(activity, Palette.load(activity), draft, (endpoint, name, keys) ->
        {
            submission.set(endpoint + "|" + name + "|" + keys);
            return null;
        });
        wizard.show();
        try
        {
            View root = wizard.getWindow().getDecorView();
            check(hasText(root, "Step 1 of 4"), "the wizard starts with its progress visible");
            checkKeyboardLayout(root);
            next(root);
            check(draft.step == 0 && field(root, "Name").getError() != null, "an empty name stays on its step");
            field(root, "Name").setText("a".repeat(51));
            next(root);
            check(draft.step == 0, "a name longer than the native protocol limit stays on its step");
            field(root, "Name").setText("\ud83c\udf4b".repeat(25));
            next(root);
            check(draft.step == 0, "a name that fills the server byte buffer stays on its step");
            field(root, "Name").setText("Alice");
            next(root);
            check(draft.step == 1 && hasText(root, "Step 2 of 4"), "a name advances to server details");
            field(root, "chat.example.org or 192.168.1.10").setText("192.168.1.23");
            field(root, "1111").setText("0");
            next(root);
            check(draft.step == 1 && field(root, "1111").getError() != null, "an invalid port cannot advance");
            field(root, "1111").setText("1111");
            next(root);
            check(draft.step == 2 && hasText(root, "Step 3 of 4"), "valid server fields advance to optional passwords");
            button(root, "Add server key").performClick();
            field(root, "Server key").setText("wizard-test-secret");
            check(!field(root, "Server key").isSaveEnabled(), "password views stay out of Android saved view state");
            wizard.capture();
            check(draft.keys.contains("wizard-test-secret") && draft.keys.contains("aes256"), "password rows retain their type and value in the draft");
            next(root);
            check(draft.step == 3 && hasText(root, "Step 4 of 4") && hasText(root, "1 password added"), "the review describes the configured password count");
            check(!hasText(root, "wizard-test-secret"), "review never reveals the connection password");
            check(submission.get() == null, "Next never starts a connection");
            button(root, "Back").performClick();
            check(field(root, "Server key").getText().toString().equals("wizard-test-secret"), "Back keeps the password draft");
            wizard.capture();
            wizard.dismiss();
            wizard = new ConnectionWizard(activity, Palette.load(activity), draft, (endpoint, name, keys) ->
            {
                submission.set(endpoint + "|" + name + "|" + keys);
                return null;
            });
            wizard.show();
            root = wizard.getWindow().getDecorView();
            check(hasText(root, "Step 3 of 4") && field(root, "Server key").getText().toString().equals("wizard-test-secret"), "a recreated wizard restores its step and draft");
            next(root);
            button(root, "Connect").performClick();
            check(!wizard.isShowing() && submission.get().startsWith("ws://192.168.1.23:1111/|Alice|"), "only Connect submits the final address and name");
            check(submission.get().contains("wizard-test-secret") && submission.get().contains("aes256"), "Connect preserves the ordered typed passwords");
            check(activity.getSharedPreferences("preferences", Context.MODE_PRIVATE).getString("endpoint", "").equals(originalEndpoint), "the wizard delegates saving to its final action");
        }
        finally
        {
            wizard.dismiss();
        }
        submission.set(null);
        ConnectionWizard cancelled = new ConnectionWizard(activity, Palette.load(activity), new ConnectionWizard.Draft("", "Bob", ""), (endpoint, name, keys) ->
        {
            submission.set(endpoint);
            return null;
        });
        cancelled.show();
        button(cancelled.getWindow().getDecorView(), "Cancel").performClick();
        check(!cancelled.isShowing() && submission.get() == null, "Cancel never saves or connects");
    }

    private static void next(View root)
    {
        button(root, "Next").performClick();
    }

    /** measures the real wizard views with and without keyboard insets */
    private void checkKeyboardLayout(View root)
    {
        ViewGroup viewport = root.findViewWithTag("connection-wizard-viewport");
        View card = root.findViewWithTag("connection-wizard-card");
        ScrollView body = root.findViewWithTag("connection-wizard-body");
        EditText input = field(root, "Name");
        input.setText("Keyboard draft");
        int width = dp(root, 320);
        int height = dp(root, 640);
        int keyboard = dp(root, 300);
        if (Build.VERSION.SDK_INT >= 30)
        {
            WindowInsets shown = new WindowInsets.Builder().setInsets(WindowInsets.Type.systemBars(), Insets.of(0, dp(root, 24), 0, dp(root, 24))).setInsets(WindowInsets.Type.ime(), Insets.of(0, 0, 0, keyboard)).setVisible(WindowInsets.Type.ime(), true).build();
            viewport.dispatchApplyWindowInsets(shown);
            measure(viewport, width, height);
            Rect footer = bounds(viewport, button(root, "Next"));
            check(footer.bottom <= height - keyboard && footer.top >= viewport.getPaddingTop(), "Next stays above the keyboard");
            check(bounds(viewport, button(root, "Cancel")).top >= dp(root, 24), "Cancel stays below the status bar");
            check(body.getChildAt(0).getHeight() > body.getHeight(), "the question body scrolls when the keyboard reduces its space");
            int compactHeight = card.getHeight();
            WindowInsets hidden = new WindowInsets.Builder().setInsets(WindowInsets.Type.systemBars(), Insets.of(0, dp(root, 24), 0, dp(root, 24))).setInsets(WindowInsets.Type.ime(), Insets.NONE).setVisible(WindowInsets.Type.ime(), false).build();
            viewport.dispatchApplyWindowInsets(hidden);
            measure(viewport, width, height);
            check(card.getHeight() > compactHeight, "the card expands again after the keyboard closes");
        }
        else
        {
            measure(viewport, width, height - keyboard);
            check(bounds(viewport, button(root, "Next")).bottom <= height - keyboard, "Next fits the resized window on older Android versions");
        }
        measure(viewport, dp(root, 1000), dp(root, 1000));
        check(card.getWidth() <= dp(root, 520) && card.getHeight() <= dp(root, 620), "the card stays bounded on a large screen");
        check(input.getText().toString().equals("Keyboard draft"), "keyboard layout changes keep the entered name");
        input.setText("");
        viewport.requestApplyInsets();
        viewport.requestLayout();
    }

    private static void measure(View view, int width, int height)
    {
        view.forceLayout();
        view.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY));
        view.layout(0, 0, width, height);
    }

    private static Rect bounds(ViewGroup viewport, View child)
    {
        Rect bounds = new Rect(0, 0, child.getWidth(), child.getHeight());
        viewport.offsetDescendantRectToMyCoords(child, bounds);
        return bounds;
    }

    private static int dp(View view, int value)
    {
        return Math.round(value * view.getResources().getDisplayMetrics().density);
    }

    private static Button button(View root, String label)
    {
        Button button = find(root, Button.class, view -> view.getText().toString().equals(label));
        if (button == null)
        {
            throw new AssertionError("Missing wizard button " + label);
        }
        return button;
    }

    private static EditText field(View root, String hint)
    {
        EditText input = find(root, EditText.class, view -> view.getHint() != null && view.getHint().toString().equals(hint));
        if (input == null)
        {
            throw new AssertionError("Missing wizard input " + hint);
        }
        return input;
    }

    private static boolean hasText(View root, String text)
    {
        return find(root, TextView.class, view -> view.getText().toString().contains(text)) != null;
    }

    private static <T extends View> T find(View root, Class<T> type, Predicate<T> matches)
    {
        if (type.isInstance(root) && matches.test(type.cast(root)))
        {
            return type.cast(root);
        }
        if (root instanceof ViewGroup)
        {
            ViewGroup group = (ViewGroup)root;
            for (int index = 0; index < group.getChildCount(); index++)
            {
                T found = find(group.getChildAt(index), type, matches);
                if (found != null)
                {
                    return found;
                }
            }
        }
        return null;
    }

    private void check(boolean passed, String message)
    {
        if (!passed)
        {
            throw new AssertionError(message);
        }
        checks++;
    }
}
