package com.lemonchat.server;

import android.Manifest;
import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import org.json.JSONArray;
import org.json.JSONObject;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import java.io.File;
import java.io.RandomAccessFile;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.concurrent.Callable;
import java.util.function.Consumer;

public class MainActivity extends Activity
{
    private static final int SETUP_REQUEST = 2;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private TextView status, address, portLabel, guidance, memory;
    private Button start, stop, settings, copy, tailscaleSignIn;
    private int port = 1111, slots = 16;
    private int channels = MemoryInfo.defaultChannels;
    private String shareAddress = "";
    private boolean startPending;
    private boolean settingsBusy;
    private boolean setupOpened;
    private String settingsProgress = "";
    private AlertDialog settingsDialog;
    private final Runnable refresh = new Runnable()
    {
        @Override public void run()
        {
            RenderStatus();
            handler.postDelayed(this, 1000);
        }
    };

    @Override public void onCreate(Bundle saved)
    {
        super.onCreate(saved);
        setupOpened = saved != null && saved.getBoolean("setup_opened", false);
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        LinearLayout page = CreateColumn();
        page.setPadding(Dp(24), Dp(24), Dp(24), Dp(24));
        scroll.addView(page);
        scroll.setOnApplyWindowInsetsListener((view, insets) ->
        {
            view.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(),
                            insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            return insets;
        });
        setContentView(scroll);
        AddLabel(page, "LEMON CHAT", 13, true).setTextColor(Color.rgb(66, 101, 27));
        AddLabel(page, "Host a chat server", 30, true);
        AddLabel(page, "Host on your local network.", 16, false);
        AddButton(page, "Or try more advanced options", this::ShowAdvancedOptions);
        AddGap(page, 20);
        LinearLayout card = CreateColumn();
        card.setPadding(Dp(18), Dp(14), Dp(18), Dp(18));
        GradientDrawable background = new GradientDrawable();
        background.setColor(Color.WHITE);
        background.setCornerRadius(Dp(16));
        background.setStroke(Dp(1), Color.rgb(222, 226, 212));
        card.setBackground(background);
        page.addView(card);
        status = AddLabel(card, "Stopped", 22, true);
        guidance = AddLabel(card, "", 15, false);
        AddLabel(card, "SERVER ADDRESS", 12, true);
        address = AddLabel(card, "Checking network…", 22, true);
        address.setTextIsSelectable(true);
        portLabel = AddLabel(card, "Websocket port: 1111", 18, true);
        copy = AddButton(card, "Copy address", () ->
        {
            getSystemService(ClipboardManager.class)
                .setPrimaryClip(ClipData.newPlainText("Lemon Chat address", shareAddress));
            Toast.makeText(this, "Address copied", Toast.LENGTH_SHORT).show();
        });
        tailscaleSignIn = AddButton(card, "Sign in to Tailscale", () ->
        {
            android.net.Uri uri = android.net.Uri.parse(ServerService.funnelLogin);
            if ("https".equals(uri.getScheme()) && "login.tailscale.com".equals(uri.getHost()))
            {
                startActivity(new Intent(Intent.ACTION_VIEW, uri));
            }
        });
        tailscaleSignIn.setVisibility(View.GONE);
        start = AddButton(page, "Start server", () ->
        {
            if (!ServerSettings.Exists(getFilesDir()))
            {
                OpenSetup();
            }
            else
            {
                StartServer();
            }
        });
        stop = AddButton(
            page, "Stop server",
            ()
                -> new AlertDialog.Builder(this)
                       .setTitle("Stop the server?")
                       .setMessage("Everyone connected will be disconnected.")
                       .setNegativeButton("Keep hosting", null)
                       .setPositiveButton(
                           "Stop server",
                           (dialog, which) ->
                           {
                               stop.setEnabled(false);
                               startService(new Intent(this, ServerService.class).setAction(ServerService.ACTION_STOP));
                           })
                       .show());
        settings = AddButton(page, "Settings", () -> ShowSettings(false));
        AddLabel(page, "Stop the server before changing startup settings. Advanced options are inside Settings.", 14,
                 false);
        AddButton(page, "Instructions", () -> startActivity(new Intent(this, InstructionsActivity.class).putExtra("port", port)));
        AddButton(page, "View log", this::ShowLog);
        new Thread(() ->
        {
            MemoryInfo.Load(new File(getApplicationInfo().nativeLibraryDir, "libchat-server.so"));
            handler.post(() ->
            {
                if (memory != null && memory.getTag() instanceof Integer)
                {
                    memory.setText(MemoryInfo.Describe((Integer)memory.getTag(), channels));
                }
            });
        }, "lemon-memory-info").start();
    }

    @Override public void onResume()
    {
        super.onResume();
        boolean configured = ServerSettings.Exists(getFilesDir());
        if (!configured && !setupOpened && !ServerService.active)
        {
            OpenSetup();
        }
        else if (configured && (settingsDialog == null || !settingsDialog.isShowing()))
        {
            LoadSummary();
        }
        handler.removeCallbacks(refresh);
        handler.post(refresh);
    }
    @Override public void onPause()
    {
        handler.removeCallbacks(refresh);
        super.onPause();
    }

    @Override
    public void onSaveInstanceState(Bundle saved)
    {
        saved.putBoolean("setup_opened", setupOpened);
        super.onSaveInstanceState(saved);
    }

    @Override public void onDestroy()
    {
        if (settingsDialog != null)
        {
            settingsDialog.dismiss();
        }
        handler.removeCallbacks(refresh);
        super.onDestroy();
    }

    /**
     * performs one settings operation away from the main thread
     *
     * @param progress temporary status label
     * @param task file operation
     * @param completed callback receiving the settings on the main thread
     * @param failed callback receiving an error on the main thread
     */
    private void RunSettingsTask(String progress, Callable<JSONObject> task, Consumer<JSONObject> completed, Consumer<Exception> failed)
    {
        if (settingsBusy)
        {
            return;
        }
        settingsBusy = true;
        settingsProgress = progress;
        RenderStatus();
        new Thread(() ->
        {
            JSONObject result = null;
            Exception failure = null;
            try
            {
                result = task.call();
            }
            catch (Exception error)
            {
                failure = error;
            }
            final JSONObject loaded = result;
            final Exception error = failure;
            handler.post(() ->
            {
                settingsBusy = false;
                if (isFinishing() || isDestroyed())
                {
                    return;
                }
                RenderStatus();
                if (error == null)
                {
                    completed.accept(loaded);
                }
                else
                {
                    failed.accept(error);
                }
            });
        }, "lemon-server-settings").start();
    }

    private void LoadSummary()
    {
        RunSettingsTask("Reading settings…", () -> ServerSettings.Read(getFilesDir()), this::ApplySummary, error ->
        {
        });
    }

    /**
     * refreshes labels from settings already read by the worker
     *
     * @param json validated server settings
     */
    private void ApplySummary(JSONObject json)
    {
        port = json.optInt("websocket_port", 1111);
        slots = json.optInt("server_slots", 16);
        channels = json.optInt("max_channel_count", MemoryInfo.defaultChannels);
    }

    private void RenderStatus()
    {
        if (ServerService.active)
        {
            startPending = false;
        }
        status.setText(startPending ? "Starting…" : settingsBusy ? settingsProgress : ServerService.status);
        status.setTextColor(ServerService.hosting ? Color.rgb(50, 105, 28) : Color.rgb(48, 48, 43));
        start.setEnabled(!ServerService.active && !startPending && !settingsBusy);
        settings.setEnabled(!ServerService.active && !startPending && !settingsBusy);
        stop.setVisibility(ServerService.active ? View.VISIBLE : View.GONE);
        stop.setEnabled(!"Stopping…".equals(ServerService.status));
        LinkedHashSet<String> addresses = GetLocalAddresses();
        shareAddress = addresses.isEmpty() ? "" : addresses.iterator().next();
        StringBuilder display = new StringBuilder();
        for (String host : addresses)
        {
            if (display.length() > 0)
            {
                display.append('\n');
            }
            display.append(host);
        }
        address.setText(display.length() == 0 ? "No LAN address yet" : display.toString());
        portLabel.setText("Websocket port: " + port);
        copy.setEnabled(ServerService.hosting && !shareAddress.isEmpty());
        guidance.setText(ServerService.hosting
                             ? (addresses.isEmpty() ? "Join Wi-Fi or enable a hotspot so friends can connect."
                                                    : "Ready to join. Share the IP address and port below with your friends.")
                             : (ServerService.active
                                    ? "Please wait while the server starts or stops."
                                    : "Start the server when you're ready. Settings are saved on this phone."));
        tailscaleSignIn.setVisibility(ServerService.funnelLogin.isEmpty() ? View.GONE : View.VISIBLE);

        // web page address
        if (HostingSettings.WebPage(this).equals(HostingSettings.PAGE_HTTP))
        {
            int pagePort = HostingSettings.WebPagePort(this);
            portLabel.setText("Websocket port: " + port + "\nWeb page: " + (shareAddress.isEmpty() ? "port " + pagePort : "http://" + shareAddress + ":" + pagePort + "/"));
        }
        if (HostingSettings.IsFunnelEnabled(this))
        {
            shareAddress = ServerService.funnelAddress;
            address.setText(shareAddress.isEmpty() ? "Public address not ready" : shareAddress);
            portLabel.setText("HTTPS: 443   WSS: 8443");
            copy.setEnabled(!shareAddress.isEmpty());
            guidance.setText("Visitors can open the HTTPS address without installing Tailscale. " +
                "In the client, use the same hostname and WSS port 8443. Audio and video use WebSocket.");
        }
    }

    private LinkedHashSet<String> GetLocalAddresses()
    {
        LinkedHashSet<String> result = new LinkedHashSet<>();
        try
        {
            ConnectivityManager manager = getSystemService(ConnectivityManager.class);
            for (Network network : manager.getAllNetworks())
            {
                NetworkCapabilities capabilities = manager.getNetworkCapabilities(network);
                LinkProperties properties = manager.getLinkProperties(network);
                if (capabilities == null || properties == null ||
                    (!capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) &&
                     !capabilities.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)))
                {
                    continue;
                }
                for (LinkAddress link : properties.getLinkAddresses())
                {
                    AddAddress(result, link.getAddress());
                }
            }
            // A hotspot interface need not appear as an Android Network.
            if (result.isEmpty())
            {
                for (NetworkInterface network : Collections.list(NetworkInterface.getNetworkInterfaces()))
                {
                    if (!network.isUp() || network.isLoopback() || network.isPointToPoint())
                    {
                        continue;
                    }
                    String name = network.getName();
                    if (!(name.startsWith("wlan") || name.startsWith("ap") || name.startsWith("swlan") ||
                          name.startsWith("eth")))
                    {
                        continue;
                    }
                    for (InetAddress host : Collections.list(network.getInetAddresses()))
                    {
                        AddAddress(result, host);
                    }
                }
            }
        }
        catch (Exception ignored)
        {
        }
        return result;
    }

    private static void AddAddress(LinkedHashSet<String> result, InetAddress host)
    {
        if (host instanceof Inet4Address && !host.isLoopbackAddress() && !host.isLinkLocalAddress())
        {
            result.add(host.getHostAddress());
        }
    }

    private void StartServer()
    {
        if (ServerService.active || startPending)
        {
            return;
        }
        RunSettingsTask("Checking settings…", () -> ServerSettings.Read(getFilesDir()), this::ConfirmStart, error -> ShowMessage("Check your settings", error.getMessage()));
    }

    /** shows the available hosting approaches without changing settings */
    private void ShowAdvancedOptions()
    {
        new AlertDialog.Builder(this).setTitle("Advanced options")
            .setItems(new String[] {"Public address with Tailscale", "Other ways to connect", "Server startup settings"}, (dialog, which) ->
            {
                if (which == 0)
                {
                    startActivity(new Intent(this, FunnelActivity.class));
                }
                else if (which == 1)
                {
                    startActivity(new Intent(this, InstructionsActivity.class).putExtra("port", port).putExtra("advanced", true));
                }
                else if (ServerService.active || startPending)
                {
                    ShowMessage("Server is running", "Stop the server before changing its startup settings.");
                }
                else
                {
                    ShowSettings(false);
                }
            }).setNegativeButton("Close", null).show();
    }

    private void OpenSetup()
    {
        if (!ServerService.active && !startPending && !settingsBusy)
        {
            setupOpened = true;
            startActivityForResult(new Intent(this, SetupActivity.class), SETUP_REQUEST);
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data)
    {
        super.onActivityResult(request, result, data);
        if (request == SETUP_REQUEST && result == RESULT_OK)
        {
            StartServer();
        }
    }

    /**
     * confirms the memory reservation before starting the service
     *
     * @param json validated server settings
     */
    private void ConfirmStart(JSONObject json)
    {
        if (ServerService.active || startPending)
        {
            return;
        }
        ApplySummary(json);
        ActivityManager.MemoryInfo available = new ActivityManager.MemoryInfo();
        getSystemService(ActivityManager.class).getMemoryInfo(available);
        long estimate = MemoryInfo.Estimate(slots, channels);
        if ((MemoryInfo.slotBytes == 0 && slots > 100) || estimate > available.availMem / 2)
        {
            new AlertDialog.Builder(this)
                .setTitle("Large memory reservation")
                .setMessage(MemoryInfo.Describe(slots, channels) + ("\n\nThis may cause Android to close apps or stop the " +
                                                          "server. A smaller slot count is recommended for a phone."))
                .setNegativeButton("Back to settings", (dialog, which) -> ShowSettings(false))
                .setPositiveButton("Start anyway", (dialog, which) -> RequestStart())
                .show();
        }
        else
        {
            RequestStart();
        }
    }

    private void RequestStart()
    {
        if (ServerService.active || startPending)
        {
            return;
        }
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
        {
            requestPermissions(new String[] {Manifest.permission.POST_NOTIFICATIONS}, 1);
        }
        else
        {
            LaunchService();
        }
    }

    @Override public void onRequestPermissionsResult(int request, String[] permissions, int[] results)
    {
        super.onRequestPermissionsResult(request, permissions, results);
        if (request == 1)
        {
            LaunchService();
        } // Hosting still works when notification permission is declined.
    }

    private void LaunchService()
    {
        if (ServerService.active || startPending)
        {
            return;
        }
        try
        {
            startPending = true;
            startForegroundService(new Intent(this, ServerService.class));
            handler.postDelayed(() ->
            {
                startPending = false;
                RenderStatus();
            }, 3000);
            RenderStatus();
        }
        catch (Exception error)
        {
            startPending = false;
            ShowMessage("Could not start", error.getMessage());
        }
    }

    private void ShowSettings(boolean firstStart)
    {
        if (ServerService.active || startPending)
        {
            return;
        }
        if (!ServerSettings.Exists(getFilesDir()))
        {
            OpenSetup();
            return;
        }
        RunSettingsTask("Reading settings…", () -> ServerSettings.Read(getFilesDir()),
            json -> ShowSettingsForm(firstStart, json), error -> ShowMessage("Settings could not be read", "Your saved file was kept. Details: " + error.getMessage()));
    }

    /**
     * opens the form after the worker has loaded its settings
     *
     * @param firstStart whether saving should start the server
     * @param json current server settings or an empty first setup
     */
    private void ShowSettingsForm(boolean firstStart, JSONObject json)
    {
        if (ServerService.active || startPending)
        {
            return;
        }
        final boolean first = !json.has("admin_password");
        ScrollView scroll = new ScrollView(this);
        LinearLayout form = CreateColumn();
        form.setPadding(Dp(24), Dp(8), Dp(24), Dp(12));
        scroll.addView(form);
        AddLabel(form, "Admin password — only for you", 16, true);
        AddLabel(form,
                 first ? "Use this to become the admin in the client app. Keep it private."
                       : "Leave blank to keep your existing admin password.",
                 14, false);
        EditText admin = AddField(form, "Admin password", "", true, false);
        EditText confirm = AddField(form, "Repeat admin password", "", true, false);
        AddLabel(form, "Join password — for friends (optional)", 16, true);
        JSONArray keys = json.optJSONArray("keys");
        AddLabel(form, keys != null && keys.length() > 1 ? "This changes the first server key. Its cipher and the other keys are kept. Clearing a nonempty password removes all server keys. Manage individual layers in the client's server settings." : "Enter this under Server keys in the client. Leave empty to allow anyone who can reach the server to join.", 14, false);
        EditText join = AddField(form, "Join password", ServerSettings.JoinPassword(json), true, false);
        CheckBox showPasswords = new CheckBox(this);
        showPasswords.setText("Show passwords");
        form.addView(showPasswords);
        showPasswords.setOnCheckedChangeListener((button, checked) ->
        {
            for (EditText field : new EditText[] {admin, confirm, join})
            {
                field.setTransformationMethod(checked ? null
                                                      : android.text.method.PasswordTransformationMethod.getInstance());
                field.setSelection(field.length());
            }
        });
        LinearLayout advanced = CreateColumn();
        Button expand = AddButton(form, "Advanced ▾", () ->
        {
        });
        form.addView(advanced);
        advanced.setVisibility(View.GONE);
        expand.setOnClickListener(view ->
        {
            boolean open = advanced.getVisibility() == View.GONE;
            advanced.setVisibility(open ? View.VISIBLE : View.GONE);
            expand.setText(open ? "Advanced ▴" : "Advanced ▾");
        });
        AddLabel(advanced, "Web page for other devices", 16, true);
        RadioGroup webPage = new RadioGroup(this);
        String[] pageValues = {HostingSettings.PAGE_NONE, HostingSettings.PAGE_HTTP, HostingSettings.PAGE_FUNNEL};
        String[] pageLabels = {"Do not offer the web page", "Offer it in the local network (HTTP)", "Offer it on the internet through Tailscale Funnel (HTTPS)"};
        for (int index = 0; index < pageValues.length; index++)
        {
            RadioButton option = new RadioButton(this);
            option.setId(View.generateViewId());
            option.setText(pageLabels[index]);
            option.setTag(pageValues[index]);
            webPage.addView(option);
            if (pageValues[index].equals(HostingSettings.WebPage(this)))
            {
                webPage.check(option.getId());
            }
        }
        advanced.addView(webPage);
        LinearLayout localPage = CreateColumn();
        advanced.addView(localPage);
        AddLabel(localPage, "Web page port", 15, true);
        EditText pagePortField = AddField(localPage, String.valueOf(HostingSettings.DEFAULT_PAGE_PORT), String.valueOf(HostingSettings.WebPagePort(this)), false, true);
        CheckBox pageConfig = new CheckBox(this);
        pageConfig.setText("Visitors connect without typing the address");
        pageConfig.setChecked(HostingSettings.IsPageConfigEmbedded(this));
        localPage.addView(pageConfig);
        AddLabel(localPage, "The page then contains the chat port and the join password, so anyone who can open the page can connect.", 14, false);
        TextView funnelNote = AddLabel(advanced, "Sign in to your Tailscale account after starting. Enable HTTPS and Funnel in that account. " +
            "The public address can be reached by anyone. Your join password still applies.", 14, false);
        AddLabel(advanced, "Web browsers allow the microphone only on HTTPS pages. On the HTTP page, visitors can read and write but not talk.", 14, false);
        AddButton(advanced, "Open the public-access guide", () ->
        {
            new AlertDialog.Builder(this).setTitle("Open the guide?")
                .setMessage("Unsaved changes in this settings window will be discarded. The guide uses your current saved server settings.")
                .setNegativeButton("Keep editing", null).setPositiveButton("Open guide", (dialog, which) ->
                {
                    if (settingsDialog != null)
                    {
                        settingsDialog.dismiss();
                    }
                    startActivity(new Intent(this, FunnelActivity.class));
                }).show();
        });
        AddLabel(advanced, "Port", 16, true);
        EditText portField =
            AddField(advanced, "1111", String.valueOf(json.optInt("websocket_port", 1111)), false, true);
        AddLabel(advanced, "Usually leave this at 1111. If it is already in use, choose another port (1024–65535).", 14,
                 false);
        AddLabel(advanced, "Server slots", 16, true);
        EditText slotField = AddField(advanced, "16", String.valueOf(json.optInt("server_slots", 16)), false, true);
        AddLabel(advanced,
                 "Maximum simultaneous clients (1–100,000). Start with 16 for a few friends. Memory is reserved at " +
                 "startup; this cannot change while hosting.",
                 14, false);
        channels = json.optInt("max_channel_count", MemoryInfo.defaultChannels);
        memory = AddLabel(advanced, MemoryInfo.Describe(json.optInt("server_slots", 16), channels), 14, false);
        memory.setTag(json.optInt("server_slots", 16));
        final TextView estimateLabel = memory;
        slotField.addTextChangedListener(new TextWatcher()
        {
            public void beforeTextChanged(CharSequence value, int start, int count, int after)
            {
            }
            public void onTextChanged(CharSequence value, int start, int before, int count)
            {
                try
                {
                    int valueInt = Integer.parseInt(value.toString());
                    estimateLabel.setTag(valueInt >= 1 && valueInt <= 100000 ? valueInt : null);
                    estimateLabel.setText(valueInt >= 1 && valueInt <= 100000 ? MemoryInfo.Describe(valueInt, channels)
                                                                              : "Enter 1–100,000 slots.");
                }
                catch (NumberFormatException ignored)
                {
                    estimateLabel.setTag(null);
                    estimateLabel.setText("Enter 1–100,000 slots.");
                }
            }
            public void afterTextChanged(Editable value)
            {
            }
        });
        AddLabel(advanced, "Audio connection", 16, true);
        RadioGroup audioTransport = new RadioGroup(this);
        String[] transportValues =
        {
            "webrtc", "auto", "websocket", "disabled"
        };
        String[] transportLabels =
        {
            "WebRTC (default)", "WebRTC with WebSocket fallback", "WebSocket only", "Audio connections disabled"
        };
        for (int index = 0; index < transportValues.length; index++)
        {
            RadioButton option = new RadioButton(this);
            option.setId(View.generateViewId());
            option.setText(transportLabels[index]);
            option.setTag(transportValues[index]);
            audioTransport.addView(option);
            if (transportValues[index].equals(first ? "webrtc" : ServerSettings.AudioTransport(json)))
            {
                audioTransport.check(option.getId());
            }
        }
        advanced.addView(audioTransport);
        Runnable updateTransport = () ->
        {
            String page = (String)webPage.findViewById(webPage.getCheckedRadioButtonId()).getTag();
            boolean isFunnel = page.equals(HostingSettings.PAGE_FUNNEL);
            localPage.setVisibility(page.equals(HostingSettings.PAGE_HTTP) ? View.VISIBLE : View.GONE);
            funnelNote.setVisibility(isFunnel ? View.VISIBLE : View.GONE);
            for (int index = 0; index < audioTransport.getChildCount(); index++)
            {
                RadioButton option = (RadioButton)audioTransport.getChildAt(index);
                option.setEnabled(!isFunnel);
                if (isFunnel && "websocket".equals(option.getTag()))
                {
                    audioTransport.check(option.getId());
                }
            }
        };
        webPage.setOnCheckedChangeListener((group, checked) -> updateTransport.run());
        updateTransport.run();
        AddLabel(advanced, "Funnel uses WebSocket for audio and video because it does not forward UDP. " +
                 "Audio stays encrypted. Poor connections may add delay.", 14, false);
        AddLabel(advanced, "What people can do", 16, true);
        ServerFeatures features = first ? ServerFeatures.Proposed() : ServerFeatures.FromSettings(json);
        for (int index = 0; index < ServerFeatures.KEYS.length; index++)
        {
            int feature = index;
            CheckBox option = new CheckBox(this);
            option.setText(ServerFeatures.LABELS[index]);
            option.setChecked(features.values[index]);
            option.setOnCheckedChangeListener((button, checked) -> features.values[feature] = checked);
            advanced.addView(option);
            AddLabel(advanced, ServerFeatures.NOTES[index], 13, false);
        }
        AddLabel(advanced, "Other live server options are in the client’s admin settings.", 14, false);
        AlertDialog dialog = new AlertDialog.Builder(this)
                                 .setTitle(first ? "Set up your server" : "Server settings")
                                 .setView(scroll)
                                 .setNegativeButton("Cancel", null)
                                 .setPositiveButton(firstStart ? "Save and start" : "Save", null)
                                 .create();
        dialog.setOnShowListener(ignored -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(view ->
        {
            if (settingsBusy)
            {
                return;
            }
            if (ServerService.active || startPending)
            {
                dialog.dismiss();
                return;
            }
            if (!admin.getText().toString().equals(confirm.getText().toString()))
            {
                confirm.setError("Passwords do not match");
                return;
            }
            try
            {
                int savedPort = Integer.parseInt(portField.getText().toString());
                int savedSlots = Integer.parseInt(slotField.getText().toString());
                String savedAdmin = admin.getText().toString();
                String savedJoin = join.getText().toString();
                ServerFeatures savedFeatures = features.Copy();
                String savedPage = (String)webPage.findViewById(webPage.getCheckedRadioButtonId()).getTag();
                int savedPagePort = Integer.parseInt(pagePortField.getText().toString());
                boolean isSavedPageConfigEmbedded = pageConfig.isChecked();
                if (savedPage.equals(HostingSettings.PAGE_HTTP) && (savedPagePort < 1024 || savedPagePort > 65535 || savedPagePort == savedPort))
                {
                    ShowMessage("Check Advanced settings", "Choose a web page port from 1024 to 65535 that differs from the chat port.");
                    return;
                }
                String savedTransport = (String)audioTransport.findViewById(audioTransport.getCheckedRadioButtonId()).getTag();
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false);
                dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setEnabled(false);
                dialog.setCancelable(false);
                RunSettingsTask("Saving settings…", () ->
                {
                    ServerSettings.Save(getFilesDir(), savedPort, savedSlots, savedAdmin, savedJoin, savedFeatures, savedTransport);
                    HostingSettings.SetWebPage(getApplicationContext(), savedPage, savedPagePort, isSavedPageConfigEmbedded);
                    return ServerSettings.Read(getFilesDir());
                }, updated ->
                {
                    dialog.dismiss();
                    ApplySummary(updated);
                    RenderStatus();
                    if (firstStart)
                    {
                        ConfirmStart(updated);
                    }
                }, error ->
                {
                    dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true);
                    dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setEnabled(true);
                    dialog.setCancelable(true);
                    ShowMessage("Could not save", error.getMessage());
                });
            }
            catch (NumberFormatException error)
            {
                ShowMessage("Check Advanced settings", "Ports and slots must be whole numbers.");
            }
            catch (Exception error)
            {
                ShowMessage("Could not save", error.getMessage());
            }
        }));
        dialog.setOnDismissListener(ignored ->
        {
            memory = null;
            settingsDialog = null;
        });
        settingsDialog = dialog;
        dialog.show();
        dialog.getWindow().addFlags(WindowManager.LayoutParams.FLAG_SECURE);
    }

    private void ShowLog()
    {
        ScrollView scroll = new ScrollView(this);
        TextView log = AddLabel(CreateColumn(), "", 12, false);
        log.setPadding(Dp(20), Dp(12), Dp(20), Dp(12));
        log.setTypeface(Typeface.MONOSPACE);
        log.setTextIsSelectable(true);
        ((LinearLayout)log.getParent()).removeView(log);
        scroll.addView(log);
        Runnable read = () ->
        {
            try (RandomAccessFile file = new RandomAccessFile(new File(getFilesDir(), "server.log"), "r"))
            {
                long length = file.length();
                long offset = Math.max(0, length - 64 * 1024);
                file.seek(offset);
                byte[] bytes = new byte[(int)(length - offset)];
                file.readFully(bytes);
                log.setText(new String(bytes, StandardCharsets.UTF_8).replaceAll("\u001B\\[[;\\d]*m", ""));
            }
            catch (Exception error)
            {
                log.setText("No log yet. Start the server first.");
            }
        };
        read.run();
        AlertDialog dialog = new AlertDialog.Builder(this)
                                 .setTitle("Server log")
                                 .setView(scroll)
                                 .setPositiveButton("Close", null)
                                 .setNeutralButton("Refresh", null)
                                 .create();
        dialog.setOnShowListener(
            ignored -> dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(view -> read.run()));
        dialog.show();
        dialog.getWindow().addFlags(WindowManager.LayoutParams.FLAG_SECURE);
    }

    private int Dp(int value)
    {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
    private LinearLayout CreateColumn()
    {
        LinearLayout view = new LinearLayout(this);
        view.setOrientation(LinearLayout.VERTICAL);
        return view;
    }
    private TextView AddLabel(LinearLayout parent, String text, int size, boolean bold)
    {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextSize(size);
        view.setTextColor(Color.rgb(48, 48, 43));
        view.setPadding(0, Dp(6), 0, Dp(6));
        if (bold)
        {
            view.setTypeface(null, Typeface.BOLD);
        }
        parent.addView(view);
        return view;
    }
    private Button AddButton(LinearLayout parent, String text, Runnable action)
    {
        Button view = new Button(this);
        view.setText(text);
        view.setAllCaps(false);
        view.setMinHeight(Dp(52));
        parent.addView(view, new LinearLayout.LayoutParams(-1, -2));
        view.setOnClickListener(ignored -> action.run());
        return view;
    }
    private EditText AddField(LinearLayout parent, String hint, String value, boolean password, boolean number)
    {
        EditText view = new EditText(this);
        view.setSingleLine(true);
        view.setHint(hint);
        view.setInputType(number ? InputType.TYPE_CLASS_NUMBER
                                 : InputType.TYPE_CLASS_TEXT | (password ? InputType.TYPE_TEXT_VARIATION_PASSWORD : 0));
        view.setText(value);
        view.setMinHeight(Dp(52));
        parent.addView(view, new LinearLayout.LayoutParams(-1, -2));
        return view;
    }
    private void AddGap(LinearLayout parent, int height)
    {
        View view = new View(this);
        parent.addView(view, new LinearLayout.LayoutParams(1, Dp(height)));
    }
    private void ShowMessage(String title, String detail)
    {
        new AlertDialog.Builder(this).setTitle(title).setMessage(detail).setPositiveButton("OK", null).show();
    }
}
