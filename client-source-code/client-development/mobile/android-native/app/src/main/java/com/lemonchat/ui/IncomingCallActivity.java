package com.lemonchat.ui;

import android.app.Activity;
import android.app.KeyguardManager;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import com.lemonchat.model.CallInvitation;
import com.lemonchat.model.Channel;
import com.lemonchat.model.SessionSnapshot;
import com.lemonchat.service.ChatService;

/** shows one channel invitation without starting microphone capture */
public final class IncomingCallActivity extends UiScaleActivity implements ChatService.Observer
{
    private final Handler main = new Handler(Looper.getMainLooper());
    private ChatService service;
    private Palette palette;
    private boolean bound;
    private boolean started;
    private boolean answerRequested;
    private boolean actionTaken;
    private String callId;
    private long connectionId;
    private TextView caller;
    private TextView detail;
    private TextView remaining;
    private Button answer;
    private Button decline;
    private Button close;

    private final Runnable expiry = new Runnable()
    {
        @Override
        public void run()
        {
            if (started)
            {
                render();
                main.postDelayed(this, 1000);
            }
        }
    };

    private final ServiceConnection connection = new ServiceConnection()
    {
        @Override
        public void onServiceConnected(ComponentName name, IBinder binder)
        {
            if (!started)
            {
                return;
            }
            service = ((ChatService.LocalBinder) binder).service();
            service.addObserver(IncomingCallActivity.this, false);
        }

        @Override
        public void onServiceDisconnected(ComponentName name)
        {
            service = null;
            showUnavailable();
        }
    };

    @Override
    public void onCreate(Bundle saved)
    {
        super.onCreate(saved);
        readIntent(getIntent());
        if (saved != null)
        {
            answerRequested = saved.getBoolean("answer-requested", false);
            actionTaken = saved.getBoolean("action-taken", false);
        }
        if (Build.VERSION.SDK_INT >= 27)
        {
            setShowWhenLocked(true);
            setTurnScreenOn(true);
        }
        else
        {
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED | WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON);
        }
        palette = Palette.load(this);
        setTheme(palette.theme.windowStyle(palette.dark));
        buildContent();
        render();
    }

    @Override
    public void onNewIntent(Intent intent)
    {
        super.onNewIntent(intent);
        setIntent(intent);
        readIntent(intent);
        onChanged();
    }

    @Override
    public void onStart()
    {
        super.onStart();
        started = true;
        bound = bindService(new Intent(this, ChatService.class), connection, Context.BIND_AUTO_CREATE);
        main.post(expiry);
    }

    @Override
    public void onStop()
    {
        started = false;
        main.removeCallbacks(expiry);
        if (service != null)
        {
            service.removeObserver(this);
        }
        if (bound)
        {
            unbindService(connection);
            bound = false;
        }
        service = null;
        super.onStop();
    }

    @Override
    public void onSaveInstanceState(Bundle saved)
    {
        saved.putBoolean("answer-requested", answerRequested);
        saved.putBoolean("action-taken", actionTaken);
        super.onSaveInstanceState(saved);
    }

    @Override
    public void onChanged()
    {
        if (!started)
        {
            return;
        }
        render();
        if (answerRequested && currentCall() != null)
        {
            answerRequested = false;
            answerCall();
        }
    }

    @Override
    public void onProblem(String message)
    {
        if (started)
        {
            Toast.makeText(this, message, Toast.LENGTH_LONG).show();
        }
    }

    @Override
    public void onBackPressed()
    {
        declineCall();
    }

    private void readIntent(Intent intent)
    {
        callId = intent.getStringExtra(ChatService.EXTRA_CALL_ID);
        connectionId = intent.getLongExtra(ChatService.EXTRA_CONNECTION_ID, -1);
        answerRequested = intent.getBooleanExtra(ChatService.EXTRA_ANSWER_CALL, false);
        actionTaken = false;
    }

    private CallInvitation currentCall()
    {
        SessionSnapshot snapshot = service != null ? service.snapshot() : null;
        CallInvitation call = snapshot != null ? snapshot.call : null;
        if (actionTaken || call == null || !snapshot.connected || snapshot.connectionId != connectionId || !call.callId.equals(callId) || call.connectionId != connectionId || call.expired())
        {
            return null;
        }
        return call;
    }

    private void buildContent()
    {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setPadding(dp(28), dp(36), dp(28), dp(36));
        root.setBackgroundColor(palette.background);
        TextView icon = text("", 24, palette.accentText, false);
        icon.setBackground(palette.rounded(palette.accent, dp(48)));
        GlyphDrawable phone = new GlyphDrawable("call", palette.accentText);
        phone.setBounds(0, 0, dp(38), dp(38));
        icon.setCompoundDrawables(null, phone, null, null);
        icon.setPadding(0, dp(29), 0, 0);
        root.addView(icon, new LinearLayout.LayoutParams(dp(96), dp(96)));
        TextView label = text("INCOMING CHANNEL CALL", 12, palette.secondary, true);
        label.setLetterSpacing(0.08f);
        label.setPadding(0, dp(28), 0, dp(12));
        root.addView(label);
        caller = text("Incoming call", 28, palette.text, true);
        root.addView(caller, new LinearLayout.LayoutParams(-1, -2));
        detail = text("Checking this invitation…", 16, palette.secondary, false);
        detail.setLineSpacing(dp(5), 1);
        detail.setPadding(0, dp(20), 0, dp(16));
        root.addView(detail, new LinearLayout.LayoutParams(-1, -2));
        remaining = text("", 13, palette.secondary, false);
        root.addView(remaining);
        LinearLayout actions = new LinearLayout(this);
        actions.setGravity(Gravity.CENTER);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams actionArea = new LinearLayout.LayoutParams(-1, dp(56));
        actionArea.topMargin = dp(36);
        root.addView(actions, actionArea);
        decline = button("Decline", false);
        decline.setTextColor(palette.danger);
        decline.setOnClickListener(view -> declineCall());
        LinearLayout.LayoutParams first = new LinearLayout.LayoutParams(0, -1, 1);
        first.rightMargin = dp(12);
        actions.addView(decline, first);
        answer = button("Answer", true);
        answer.setOnClickListener(view -> answerCall());
        actions.addView(answer, new LinearLayout.LayoutParams(0, -1, 1));
        close = button("Close", false);
        close.setOnClickListener(view -> finish());
        LinearLayout.LayoutParams closePosition = new LinearLayout.LayoutParams(dp(180), dp(52));
        closePosition.topMargin = dp(14);
        root.addView(close, closePosition);
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setFitsSystemWindows(true);
        scroll.setBackgroundColor(palette.background);
        scroll.addView(root, new ScrollView.LayoutParams(-1, -2));
        setContentView(scroll);
    }

    private void render()
    {
        if (answer == null)
        {
            return;
        }
        CallInvitation call = currentCall();
        if (call == null)
        {
            if (service == null && !actionTaken)
            {
                answer.setEnabled(false);
                decline.setEnabled(false);
                close.setVisibility(View.VISIBLE);
                return;
            }
            showUnavailable();
            return;
        }
        String channel = "their channel";
        for (Channel candidate : service.snapshot().channels)
        {
            if (candidate.id == call.channelId)
            {
                channel = candidate.name;
                break;
            }
        }
        caller.setText(call.callerName);
        detail.setText("Invites you to " + channel + ". Answer to join the channel and listen. Hold the microphone button when you want to speak.");
        long seconds = Math.max(1, (call.expiresAtNanos - System.nanoTime() + 999999999L) / 1000000000L);
        remaining.setText("Invitation ends in " + seconds + " seconds");
        answer.setEnabled(true);
        decline.setEnabled(true);
        close.setVisibility(View.GONE);
    }

    private void showUnavailable()
    {
        answerRequested = false;
        detail.setText("This call has ended or belongs to an earlier connection.");
        remaining.setText("");
        answer.setEnabled(false);
        decline.setEnabled(false);
        close.setVisibility(View.VISIBLE);
    }

    private void answerCall()
    {
        if (currentCall() == null || !service.answerCall(callId, connectionId))
        {
            showUnavailable();
            return;
        }
        actionTaken = true;
        KeyguardManager keyguard = getSystemService(KeyguardManager.class);
        if (keyguard != null && keyguard.isKeyguardLocked())
        {
            keyguard.requestDismissKeyguard(this, null);
        }
        Intent intent = new Intent(this, MainActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        intent.putExtra(ChatService.EXTRA_CALL_ID, callId);
        intent.putExtra(ChatService.EXTRA_CONNECTION_ID, connectionId);
        intent.putExtra(ChatService.EXTRA_SHOW_CHANNEL, true);
        startActivity(intent);
        finish();
    }

    private void declineCall()
    {
        if (currentCall() != null)
        {
            service.declineCall(callId, connectionId);
        }
        actionTaken = true;
        finish();
    }

    private TextView text(String value, float size, int color, boolean bold)
    {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        view.setGravity(Gravity.CENTER);
        view.setTypeface(Typeface.create(bold ? "sans-serif-medium" : "sans-serif", Typeface.NORMAL));
        return view;
    }

    private Button button(String title, boolean primary)
    {
        Button view = new Button(this);
        view.setText(title);
        view.setAllCaps(false);
        view.setTextSize(16);
        view.setTextColor(primary ? palette.accentText : palette.text);
        view.setBackground(palette.rounded(primary ? palette.accent : palette.surface, dp(18)));
        return view;
    }

    private int dp(float value)
    {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }
}
