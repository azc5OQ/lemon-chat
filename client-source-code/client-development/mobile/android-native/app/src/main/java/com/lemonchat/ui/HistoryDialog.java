package com.lemonchat.ui;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.res.ColorStateList;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;
import com.lemonchat.service.ChatService;
import com.lemonchat.storage.HistoryPolicy;

/** asks before saving private history and offers early deletion */
final class HistoryDialog
{
    private static final int[] DAYS = {-1, 1, 7, 30, 90, 0};
    private static final String[] LABELS = {"Don't save", "1 day", "7 days", "30 days", "90 days", "No time limit"};

    static AlertDialog show(Activity activity, ChatService service, boolean firstRun, Runnable changed)
    {
        Palette palette = Palette.load(activity);
        int padding = dp(activity, 20);
        LinearLayout form = new LinearLayout(activity);
        form.setOrientation(LinearLayout.VERTICAL);
        form.setPadding(padding, dp(activity, 8), padding, dp(activity, 12));
        TextView explanation = new TextView(activity);
        explanation.setText("Save private chat history on this device?\n\nKeep an encrypted copy of new private messages and attachments for the time you choose. Older messages may be removed sooner when storage is full.\n\nDon't save still lets you receive messages while connected, including messages waiting on the server. Choosing Don't save also deletes any previously saved copy.");
        explanation.setTextSize(14);
        explanation.setTextColor(palette.secondary);
        explanation.setPadding(0, 0, 0, dp(activity, 12));
        form.addView(explanation);
        RadioGroup choices = new RadioGroup(activity);
        choices.setOrientation(RadioGroup.VERTICAL);
        choices.setTag("history-retention-choices");
        HistoryPolicy policy = service.historyPolicy();
        int selected = firstRun ? -1 : policy.retentionDays;
        for (int index = 0; index < DAYS.length; index++)
        {
            RadioButton choice = new RadioButton(activity);
            choice.setId(View.generateViewId());
            choice.setText(LABELS[index]);
            choice.setTextSize(15);
            choice.setTextColor(palette.text);
            choice.setButtonTintList(ColorStateList.valueOf(palette.text));
            choice.setMinHeight(dp(activity, 42));
            choices.addView(choice, new RadioGroup.LayoutParams(-1, -2));
            if (DAYS[index] == selected)
            {
                choices.check(choice.getId());
            }
        }
        form.addView(choices);
        TextView problem = new TextView(activity);
        problem.setTextSize(13);
        problem.setTextColor(palette.danger);
        problem.setPadding(0, dp(activity, 10), 0, 0);
        problem.setVisibility(View.GONE);
        form.addView(problem);
        ScrollView scroll = new ScrollView(activity);
        scroll.addView(form);
        AlertDialog.Builder builder = new AlertDialog.Builder(activity).setTitle(firstRun ? "Private chat history" : "Offline history").setView(scroll).setPositiveButton(firstRun ? "Continue" : "Apply", null);
        if (!firstRun)
        {
            builder.setNegativeButton("Cancel", null).setNeutralButton("Clear saved history", null);
        }
        AlertDialog dialog = builder.create();
        dialog.setCancelable(!firstRun);
        dialog.setCanceledOnTouchOutside(false);
        AlertDialog[] confirmation = new AlertDialog[1];
        dialog.setOnDismissListener(ignored ->
        {
            if (confirmation[0] != null)
            {
                confirmation[0].dismiss();
            }
        });
        dialog.setOnShowListener(ignored ->
        {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(view ->
            {
                int checked = choices.indexOfChild(choices.findViewById(choices.getCheckedRadioButtonId()));
                if (checked < 0 || checked >= DAYS.length || service.historyBusy())
                {
                    return;
                }
                busy(dialog, choices, true);
                service.setHistoryPolicy(DAYS[checked], error ->
                {
                    if (!usable(activity, dialog))
                    {
                        return;
                    }
                    busy(dialog, choices, false);
                    if (error != null)
                    {
                        problem.setTextColor(palette.danger);
                        problem.setText(error);
                        problem.setVisibility(View.VISIBLE);
                        return;
                    }
                    dialog.dismiss();
                    changed.run();
                });
            });
            if (!firstRun)
            {
                dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(view ->
                {
                    if (service.historyBusy())
                    {
                        return;
                    }
                    confirmation[0] = new AlertDialog.Builder(activity).setTitle("Clear saved history?").setMessage("Delete private messages and attachments saved by this app. Current conversations stay visible in this session. Files you saved outside the app are kept. This does not delete messages from anyone else's device.").setNegativeButton("Cancel", null).setPositiveButton("Clear", (question, button) ->
                    {
                        busy(dialog, choices, true);
                        service.clearSavedHistory(error ->
                        {
                            if (!usable(activity, dialog))
                            {
                                return;
                            }
                            busy(dialog, choices, false);
                            problem.setTextColor(error == null ? palette.secondary : palette.danger);
                            problem.setText(error == null ? "Saved history cleared" : error);
                            problem.setVisibility(View.VISIBLE);
                            if (error == null)
                            {
                                changed.run();
                            }
                        });
                    }).create();
                    confirmation[0].show();
                });
            }
        });
        dialog.show();
        if (dialog.getWindow() != null)
        {
            dialog.getWindow().setGravity(Gravity.CENTER);
            dialog.getWindow().setLayout(Math.min(activity.getResources().getDisplayMetrics().widthPixels - dp(activity, 24), dp(activity, 460)), -2);
        }
        return dialog;
    }

    static String summary(HistoryPolicy policy)
    {
        if (!policy.configured)
        {
            return "Choose whether to save private conversations";
        }
        if (!policy.enabled())
        {
            return "Not saved on this device";
        }
        return policy.retentionDays == 0 ? "No time limit" : "Keep for " + policy.retentionDays + (policy.retentionDays == 1 ? " day" : " days");
    }

    private static boolean usable(Activity activity, AlertDialog dialog)
    {
        return !activity.isFinishing() && !activity.isDestroyed() && dialog.isShowing();
    }

    private static void busy(AlertDialog dialog, RadioGroup choices, boolean busy)
    {
        for (int index = 0; index < choices.getChildCount(); index++)
        {
            choices.getChildAt(index).setEnabled(!busy);
        }
        for (int button : new int[]{AlertDialog.BUTTON_POSITIVE, AlertDialog.BUTTON_NEGATIVE, AlertDialog.BUTTON_NEUTRAL})
        {
            if (dialog.getButton(button) != null)
            {
                dialog.getButton(button).setEnabled(!busy);
            }
        }
    }

    private static int dp(Activity activity, int value)
    {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }
}
