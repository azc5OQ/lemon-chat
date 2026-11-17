package com.lemonchat.server;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.Gravity;
import android.view.View;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.lang.ref.WeakReference;
import org.json.JSONObject;

/** guides optional public hosting while the service owns its processes */
public final class FunnelActivity extends Activity
{
    static final String DOCUMENTATION = "https://tailscale.com/docs/features/tailscale-funnel";
    private final Handler handler = new Handler(Looper.getMainLooper());
    private State state;
    private LinearLayout body;
    private TextView progress;
    private Button back;
    private Button next;
    private AlertDialog confirmation;
    private OnBackInvokedCallback backCallback;
    private boolean resumed;
    private String rendered = "";
    private final Runnable refresh = new Runnable()
    {
        @Override
        public void run()
        {
            Complete();
            Render();
            handler.postDelayed(this, 1000);
        }
    };

    @Override
    public void onCreate(Bundle saved)
    {
        super.onCreate(saved);
        state = getLastNonConfigurationInstance() instanceof State ? (State)getLastNonConfigurationInstance() : new State();
        if (saved != null && state.step == 0)
        {
            state.step = Math.max(0, Math.min(3, saved.getInt("step", 0)));
        }
        state.owner = new WeakReference<>(this);
        FrameLayout root = new FrameLayout(this);
        root.setFitsSystemWindows(true);
        root.setBackgroundColor(Color.rgb(250, 250, 245));
        LinearLayout page = Column();
        root.addView(page, new FrameLayout.LayoutParams(-1, -1, Gravity.CENTER_HORIZONTAL));
        root.addOnLayoutChangeListener((view, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) ->
        {
            int width = Math.min(right - left, Dp(560));
            if (width > 0 && page.getLayoutParams().width != width)
            {
                page.getLayoutParams().width = width;
                page.requestLayout();
            }
        });
        LinearLayout header = Column();
        header.setPadding(Dp(24), Dp(20), Dp(24), Dp(8));
        Label(header, "PUBLIC ACCESS · OPTIONAL", 12, true);
        progress = Label(header, "", 14, false);
        page.addView(header);
        ScrollView scroll = new ScrollView(this);
        scroll.setPadding(Dp(24), Dp(8), Dp(24), Dp(16));
        body = Column();
        scroll.addView(body);
        page.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        LinearLayout footer = new LinearLayout(this);
        footer.setPadding(Dp(24), Dp(8), Dp(24), Dp(20));
        back = Button("Back", this::GoBack);
        next = Button("Next", this::GoNext);
        LinearLayout.LayoutParams previous = new LinearLayout.LayoutParams(0, -2, 1);
        previous.rightMargin = Dp(12);
        footer.addView(back, previous);
        footer.addView(next, new LinearLayout.LayoutParams(0, -2, 2));
        page.addView(footer);
        setContentView(root);
        if (Build.VERSION.SDK_INT >= 33)
        {
            backCallback = this::GoBack;
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, backCallback);
        }
        Render();
    }

    @Override
    public Object onRetainNonConfigurationInstance()
    {
        return state;
    }

    @Override
    protected void onSaveInstanceState(Bundle saved)
    {
        saved.putInt("step", state.step);
        super.onSaveInstanceState(saved);
    }

    @Override
    protected void onResume()
    {
        super.onResume();
        resumed = true;
        handler.removeCallbacks(refresh);
        handler.post(refresh);
    }

    @Override
    protected void onPause()
    {
        resumed = false;
        handler.removeCallbacks(refresh);
        super.onPause();
    }

    @Override
    protected void onDestroy()
    {
        handler.removeCallbacks(refresh);
        if (confirmation != null)
        {
            confirmation.dismiss();
        }
        if (Build.VERSION.SDK_INT >= 33 && backCallback != null)
        {
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(backCallback);
        }
        if (state.owner.get() == this)
        {
            state.owner.clear();
            state.disposed = !isChangingConfigurations();
        }
        super.onDestroy();
    }

    @Override
    @SuppressLint("GestureBackNavigation")
    public void onBackPressed()
    {
        GoBack();
    }

    private FunnelGuideState.Phase Phase()
    {
        return FunnelGuideState.PhaseFor(ServerSettings.Exists(getFilesDir()), HostingSettings.IsFunnelEnabled(this), ServerService.active, ServerService.hosting, ServerService.funnelLogin, ServerService.funnelAddress);
    }

    private void Render()
    {
        FunnelGuideState.Phase phase = Phase();
        if (state.step == 3 && phase != FunnelGuideState.Phase.READY)
        {
            state.step = 2;
        }
        boolean starting = state.startPendingUntil > SystemClock.elapsedRealtime();
        String fingerprint = state.step + ":" + phase + ":" + ServerService.status + ":" + ServerService.funnelAddress + ":" + state.busy + ":" + starting + ":" + state.failure;
        if (fingerprint.equals(rendered))
        {
            return;
        }
        rendered = fingerprint;
        body.removeAllViews();
        progress.setText("Step " + (state.step + 1) + " of 4");
        back.setText(state.step == 0 ? "Close" : "Back");
        next.setVisibility(state.step == 2 ? View.GONE : View.VISIBLE);
        next.setText(state.step == 3 ? "Done" : "Next");
        if (state.step == 0)
        {
            Label(body, "Let friends join from anywhere", 24, true);
            Label(body, "On the same Wi-Fi, your local address is enough. If friends are elsewhere, Tailscale Funnel can give this phone a public HTTPS address.", 16, false);
            Label(body, "Anyone on the internet can reach that address. Your server keys and permissions still apply. Share server keys privately, never your admin password.", 16, false);
            Label(body, "Lemon Chat includes the connection helper. You do not need the separate Tailscale VPN app, and visitors do not need a Tailscale account.", 16, false);
            Label(body, "Opening this guide changes nothing. A later step asks you to enable public access and start the server.", 14, false);
        }
        else if (state.step == 1)
        {
            Label(body, "Prepare your Tailscale account", 24, true);
            Label(body, "Use your own Tailscale account, or ask its administrator to allow this device. Sign-in happens in the browser after the next step starts the helper.", 16, false);
            Label(body, "1. In the account's DNS settings, enable MagicDNS and HTTPS certificates.", 16, false);
            AddButton("Open DNS settings", () -> Open("https://login.tailscale.com/admin/dns"));
            Label(body, "2. In Access controls, open the Funnel section and add Funnel to the policy for the users who may publish services.", 16, false);
            AddButton("Open Access controls", () -> Open("https://login.tailscale.com/admin/acls"));
            Label(body, "The account administrator controls these permissions. Lemon Chat cannot approve them for you. Tailscale documents Funnel as a beta feature.", 14, false);
            AddButton("Read the Funnel requirements", () -> Open(DOCUMENTATION));
        }
        else if (state.step == 2)
        {
            RenderConnect(phase);
        }
        else
        {
            Label(body, "Your public address is ready", 24, true);
            Label(body, ServerService.funnelAddress, 20, true).setTextIsSelectable(true);
            Label(body, "Friends can open this address in a browser. In a Lemon Chat app, use the same hostname and port 8443.", 16, false);
            Label(body, "Keep this phone online and powered. Public DNS can take a few minutes to become available. Closing this guide leaves the server running.", 14, false);
            AddButton("Copy address", this::CopyAddress);
            AddButton("Share address", this::ShareAddress);
            AddButton("Stop public hosting", () -> Stop(true));
        }
        if (!state.failure.isEmpty())
        {
            Label(body, state.failure, 14, false).setTextColor(Color.rgb(159, 47, 38));
        }
    }

    private void RenderConnect(FunnelGuideState.Phase phase)
    {
        Label(body, "Start and sign in", 24, true);
        Label(body, ServerService.status, 17, true).setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        if (phase == FunnelGuideState.Phase.NEEDS_SETUP)
        {
            Label(body, "Create the server's admin password and other basic settings first. Return here when setup is complete.", 16, false);
            AddButton("Set up this server", () ->
            {
                startActivity(new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP));
                finish();
            });
        }
        else if (phase == FunnelGuideState.Phase.LOCAL_RUNNING)
        {
            Label(body, "Your local server is running. Stop it before changing how people connect. Everyone connected will be disconnected.", 16, false);
            AddButton("Stop local server", () -> Stop(false)).setEnabled(!"Stopping…".equals(ServerService.status));
        }
        else if (phase == FunnelGuideState.Phase.STOPPED)
        {
            Label(body, "This starts the server with public HTTPS access. Audio and video will use WebSocket because Funnel does not forward UDP.", 16, false);
            Label(body, "Your existing channels, groups, keys and music remain in the saved server configuration. Public hosting stays selected for later starts until you turn it off.", 14, false);
            boolean starting = state.startPendingUntil > SystemClock.elapsedRealtime();
            AddButton(starting ? "Starting…" : state.busy ? "Checking settings…" : "Enable public access and start", this::PrepareStart).setEnabled(!state.busy && !starting);
            if (HostingSettings.IsFunnelEnabled(this))
            {
                AddButton("Turn off public access", () ->
                {
                    HostingSettings.SetFunnelEnabled(this, false);
                    state.failure = "Public access is off. Start local hosting from the server screen.";
                    Render();
                }).setEnabled(!state.busy && !starting);
            }
        }
        else if (phase == FunnelGuideState.Phase.READY)
        {
            Label(body, "The helper has opened both public listeners. Continue to copy or share the address.", 16, false);
            AddButton("Continue to your address", () ->
            {
                state.step = 3;
                Render();
            });
            AddButton("Stop public hosting", () -> Stop(true));
        }
        else
        {
            Label(body, "The server keeps running while you use the browser. Return here after signing in to check the connection.", 16, false);
            if (phase == FunnelGuideState.Phase.SIGN_IN)
            {
                AddButton("Sign in to Tailscale", () ->
                {
                    if (FunnelGuideState.LoginAddress(ServerService.funnelLogin))
                    {
                        Open(ServerService.funnelLogin);
                    }
                });
            }
            else
            {
                Label(body, "Waiting for Tailscale. If this account is already signed in, no browser step is needed.", 14, false);
            }
            AddButton("Open account settings", () -> Open("https://login.tailscale.com/admin"));
            AddButton("Funnel troubleshooting", () -> Open(DOCUMENTATION + "#troubleshoot-funnel"));
            AddButton("Stop public hosting", () -> Stop(true)).setEnabled(!"Stopping…".equals(ServerService.status));
        }
    }

    private void PrepareStart()
    {
        if (state.busy || ServerService.active || state.startPendingUntil > SystemClock.elapsedRealtime())
        {
            return;
        }
        state.busy = true;
        state.failure = "";
        State pending = state;
        int request = ++state.request;
        File directory = getFilesDir();
        new Thread(() ->
        {
            int slots = 0;
            int channels = 0;
            String failure = "";
            try
            {
                JSONObject settings = ServerSettings.Read(directory);
                slots = settings.getInt("server_slots");
                channels = settings.optInt("max_channel_count", MemoryInfo.defaultChannels);
            }
            catch (Exception error)
            {
                failure = "Settings could not be read. Your saved file was kept. " + error.getMessage();
            }
            final int selectedSlots = slots;
            final int selectedChannels = channels;
            final String problem = failure;
            new Handler(Looper.getMainLooper()).post(() ->
            {
                if (!pending.disposed && pending.request == request)
                {
                    pending.busy = false;
                    pending.slots = selectedSlots;
                    pending.channels = selectedChannels;
                    pending.failure = problem;
                    pending.confirm = problem.isEmpty();
                    FunnelActivity owner = pending.owner.get();
                    if (owner != null)
                    {
                        owner.Complete();
                        owner.Render();
                    }
                }
            });
        }, "lemon-funnel-settings").start();
        Render();
    }

    private void Complete()
    {
        if (!resumed || isFinishing() || state.disposed)
        {
            return;
        }
        if (state.launch)
        {
            state.launch = false;
            Launch();
        }
        if (state.confirm && !ServerService.active && confirmation == null)
        {
            ActivityManager.MemoryInfo available = new ActivityManager.MemoryInfo();
            getSystemService(ActivityManager.class).getMemoryInfo(available);
            boolean large = (MemoryInfo.slotBytes == 0 && state.slots > 100) || MemoryInfo.Estimate(state.slots, state.channels) > available.availMem / 2;
            String detail = "This makes the server reachable from the internet. Your server keys and permissions still apply.";
            if (large)
            {
                detail += "\n\n" + MemoryInfo.Describe(state.slots, state.channels) + "\nA smaller slot count is recommended for a phone.";
            }
            confirmation = new AlertDialog.Builder(this).setTitle("Enable public access?").setMessage(detail)
                .setNegativeButton("Cancel", (dialog, which) -> state.confirm = false)
                .setPositiveButton("Enable and start", (dialog, which) ->
                {
                    state.confirm = false;
                    RequestStart();
                }).create();
            confirmation.setOnCancelListener(dialog -> state.confirm = false);
            confirmation.setOnDismissListener(dialog -> confirmation = null);
            confirmation.show();
        }
    }

    private void RequestStart()
    {
        if (ServerService.active)
        {
            return;
        }
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
        {
            requestPermissions(new String[] {Manifest.permission.POST_NOTIFICATIONS}, 1);
        }
        else
        {
            Launch();
        }
    }

    @Override
    public void onRequestPermissionsResult(int request, String[] permissions, int[] results)
    {
        super.onRequestPermissionsResult(request, permissions, results);
        if (request == 1)
        {
            state.launch = true;
            Complete();
        }
    }

    private void Launch()
    {
        if (ServerService.active || !resumed)
        {
            return;
        }
        try
        {
            HostingSettings.SetFunnelEnabled(this, true);
            startForegroundService(new Intent(this, ServerService.class));
            state.startPendingUntil = SystemClock.elapsedRealtime() + 3000;
            state.failure = "";
        }
        catch (Exception error)
        {
            state.failure = "Could not start public hosting. " + error.getMessage();
        }
        rendered = "";
        Render();
    }

    private void Stop(boolean disablePublic)
    {
        confirmation = new AlertDialog.Builder(this).setTitle("Stop the server?")
            .setMessage(disablePublic ? "Everyone connected will be disconnected. Public access will be turned off for future starts." : "Everyone connected will be disconnected. The server will stay stopped until you start it again.")
            .setNegativeButton("Keep hosting", null).setPositiveButton("Stop server", (dialog, which) ->
            {
                if (disablePublic)
                {
                    HostingSettings.SetFunnelEnabled(this, false);
                }
                startService(new Intent(this, ServerService.class).setAction(ServerService.ACTION_STOP));
                state.step = 2;
                Render();
            }).create();
        confirmation.setOnDismissListener(dialog -> confirmation = null);
        confirmation.show();
    }

    private void CopyAddress()
    {
        if (Phase() == FunnelGuideState.Phase.READY)
        {
            getSystemService(ClipboardManager.class).setPrimaryClip(ClipData.newPlainText("Lemon Chat server", ServerService.funnelAddress));
            Toast.makeText(this, "Address copied", Toast.LENGTH_SHORT).show();
        }
    }

    private void ShareAddress()
    {
        if (Phase() == FunnelGuideState.Phase.READY)
        {
            Intent share = new Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, ServerService.funnelAddress);
            startActivity(Intent.createChooser(share, "Share server address"));
        }
    }

    private void Open(String address)
    {
        try
        {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(address)));
        }
        catch (android.content.ActivityNotFoundException error)
        {
            state.failure = "No browser is available to open this page.";
            Render();
        }
    }

    private void GoNext()
    {
        if (state.step == 3)
        {
            finish();
        }
        else if (state.step < 2)
        {
            state.step++;
            Render();
        }
    }

    private void GoBack()
    {
        state.request++;
        state.busy = false;
        state.confirm = false;
        state.launch = false;
        if (state.step > 0)
        {
            state.step--;
            Render();
        }
        else
        {
            finish();
        }
    }

    private LinearLayout Column()
    {
        LinearLayout result = new LinearLayout(this);
        result.setOrientation(LinearLayout.VERTICAL);
        return result;
    }

    private TextView Label(LinearLayout parent, String value, int size, boolean bold)
    {
        TextView label = new TextView(this);
        label.setText(value);
        label.setTextSize(size);
        label.setTextColor(Color.rgb(35, 42, 30));
        if (bold)
        {
            label.setTypeface(null, android.graphics.Typeface.BOLD);
        }
        label.setLineSpacing(Dp(2), 1);
        LinearLayout.LayoutParams position = new LinearLayout.LayoutParams(-1, -2);
        position.bottomMargin = Dp(14);
        parent.addView(label, position);
        return label;
    }

    private Button Button(String title, Runnable action)
    {
        Button button = new Button(this);
        button.setText(title);
        button.setAllCaps(false);
        button.setMinHeight(Dp(48));
        button.setOnClickListener(view -> action.run());
        return button;
    }

    private Button AddButton(String title, Runnable action)
    {
        Button button = Button(title, action);
        LinearLayout.LayoutParams position = new LinearLayout.LayoutParams(-1, -2);
        position.bottomMargin = Dp(12);
        body.addView(button, position);
        return button;
    }

    private int Dp(int value)
    {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private static final class State
    {
        private WeakReference<FunnelActivity> owner = new WeakReference<>(null);
        private int step;
        private int slots;
        private int channels;
        private int request;
        private long startPendingUntil;
        private boolean disposed;
        private boolean busy;
        private boolean confirm;
        private boolean launch;
        private String failure = "";
    }
}
