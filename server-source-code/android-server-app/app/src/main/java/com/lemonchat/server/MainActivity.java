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

public class MainActivity extends Activity
{
    private final Handler handler = new Handler(Looper.getMainLooper());
    private TextView status, address, portLabel, guidance, memory;
    private Button start, stop, settings, copy;
    private int port = 1111, slots = 16;
    private String shareAddress = "";
    private boolean startPending;
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
        AddLabel(page, "Host a LAN server", 30, true);
        AddLabel(page, "Connect with friends on the same Wi-Fi. Everyone joins using the Lemon Chat client app.", 16,
                 false);
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
        AddLabel(card, "IP ADDRESS FOR FRIENDS", 12, true);
        address = AddLabel(card, "Checking network…", 22, true);
        address.setTextIsSelectable(true);
        portLabel = AddLabel(card, "Websocket port: 1111", 18, true);
        copy = AddButton(card, "Copy IP address", () ->
        {
            getSystemService(ClipboardManager.class)
                .setPrimaryClip(ClipData.newPlainText("Lemon Chat address", shareAddress));
            Toast.makeText(this, "IP address copied", Toast.LENGTH_SHORT).show();
        });
        start = AddButton(page, "Start server", () ->
        {
            if (!new File(getFilesDir(), "server_settings.json").exists())
            {
                ShowSettings(true);
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
        AddGap(page, 14);
        AddLabel(page, "How friends join", 19, true);
        AddLabel(page,
                 "1. Connect to the same Wi-Fi (or your phone’s hotspot).\n2. In the client app’s Settings, enter the " +
                 "IP address and websocket port shown above.\n3. Under Server keys, add the join password if one " +
                 "is set. Otherwise leave Server keys empty. Save, then connect.",
                 16, false);
        AddLabel(page,
                 "On this phone, use IP address 127.0.0.1 and port " + port +
                     " in the client app. Guest Wi-Fi may block connections between devices.",
                 14, false)
            .setTag("localHint");
        AddLabel(page,
                 "Keep this phone powered for longer sessions. Android or battery-saving settings may stop " +
                 "background hosting.",
                 14, false);
        AddButton(page, "View log", this::ShowLog);
        new Thread(() ->
        {
            MemoryInfo.Load(new File(getApplicationInfo().nativeLibraryDir, "libchat-server.so"));
            handler.post(() ->
            {
                if (memory != null && memory.getTag() instanceof Integer)
                {
                    memory.setText(MemoryInfo.Describe((Integer)memory.getTag()));
                }
            });
        }, "lemon-memory-info").start();
    }

    @Override public void onResume()
    {
        super.onResume();
        LoadSummary();
        handler.removeCallbacks(refresh);
        handler.post(refresh);
    }
    @Override public void onPause()
    {
        handler.removeCallbacks(refresh);
        super.onPause();
    }

    private void LoadSummary()
    {
        try
        {
            JSONObject json = ServerSettings.Read(getFilesDir());
            port = json.getInt("websocket_port");
            slots = json.getInt("server_slots");
        }
        catch (Exception ignored)
        {
        }
        TextView hint = findViewById(android.R.id.content).findViewWithTag("localHint");
        if (hint != null)
        {
            hint.setText("On this phone, use IP address 127.0.0.1 and port " + port +
                         " in the client app. Guest Wi-Fi may block connections between devices.");
        }
    }

    private void RenderStatus()
    {
        if (ServerService.active)
        {
            startPending = false;
        }
        status.setText(startPending ? "Starting…" : ServerService.status);
        status.setTextColor(ServerService.hosting ? Color.rgb(50, 105, 28) : Color.rgb(48, 48, 43));
        start.setEnabled(!ServerService.active && !startPending);
        settings.setEnabled(!ServerService.active && !startPending);
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
                                    : "Start the server when you’re ready. Settings are saved on this phone."));
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
        try
        {
            ServerSettings.Read(getFilesDir());
            LoadSummary();
        }
        catch (Exception error)
        {
            ShowMessage("Check your settings", error.getMessage());
            return;
        }
        ActivityManager.MemoryInfo available = new ActivityManager.MemoryInfo();
        getSystemService(ActivityManager.class).getMemoryInfo(available);
        long estimate = MemoryInfo.fixedBytes + MemoryInfo.slotBytes * slots;
        if ((MemoryInfo.slotBytes == 0 && slots > 100) || estimate > available.availMem / 2)
        {
            new AlertDialog.Builder(this)
                .setTitle("Large memory reservation")
                .setMessage(MemoryInfo.Describe(slots) + ("\n\nThis may cause Android to close apps or stop the " +
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
        JSONObject json;
        try
        {
            json = new File(getFilesDir(), "server_settings.json").exists() ? ServerSettings.Read(getFilesDir())
                                                                            : new JSONObject();
        }
        catch (Exception error)
        {
            ShowMessage("Settings could not be read", "Your saved file was kept. Details: " + error.getMessage());
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
        AddLabel(form, "Friends enter this under Server keys in the client app. Leave empty to let anyone on your LAN join.", 14, false);
        JSONArray keys = json.optJSONArray("keys");
        EditText join = AddField(form, "Join password", keys != null ? keys.optString(0, "") : "", true, false);
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
        Button expand = AddButton(form, "Advanced ▾", () -> {});
        form.addView(advanced);
        advanced.setVisibility(View.GONE);
        expand.setOnClickListener(view ->
        {
            boolean open = advanced.getVisibility() == View.GONE;
            advanced.setVisibility(open ? View.VISIBLE : View.GONE);
            expand.setText(open ? "Advanced ▴" : "Advanced ▾");
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
        memory = AddLabel(advanced, MemoryInfo.Describe(json.optInt("server_slots", 16)), 14, false);
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
                    estimateLabel.setText(valueInt >= 1 && valueInt <= 100000 ? MemoryInfo.Describe(valueInt)
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
            "webrtc", "auto", "websocket"
        };
        String[] transportLabels =
        {
            "WebRTC (default)", "WebRTC with WebSocket fallback", "WebSocket only"
        };
        for (int index = 0; index < transportValues.length; index++)
        {
            RadioButton option = new RadioButton(this);
            option.setId(View.generateViewId());
            option.setText(transportLabels[index]);
            option.setTag(transportValues[index]);
            audioTransport.addView(option);
            if (transportValues[index].equals(json.optString("audio_transport", "webrtc")))
            {
                audioTransport.check(option.getId());
            }
        }
        advanced.addView(audioTransport);
        AddLabel(advanced, "WebSocket helps when UDP is blocked. Audio stays encrypted. Poor connections may add delay. " +
                 "Video still uses WebRTC. Each audio connection adds about 38 KiB of buffers and two threads.", 14, false);
        CheckBox video = new CheckBox(this);
        video.setText("Allow video / screen sharing");
        video.setChecked(json.optBoolean("is_video_streaming_active", false));
        advanced.addView(video);
        AddLabel(advanced,
                 "Uses more CPU, battery and network capacity. Voice chat is enabled by default. Other live server " +
                 "options are in the client’s admin settings.",
                 14, false);
        AlertDialog dialog = new AlertDialog.Builder(this)
                                 .setTitle(first ? "Set up your server" : "Server settings")
                                 .setView(scroll)
                                 .setNegativeButton("Cancel", null)
                                 .setPositiveButton(firstStart ? "Save and start" : "Save", null)
                                 .create();
        dialog.setOnShowListener(ignored -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(view ->
        {
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
                ServerSettings.Save(getFilesDir(), Integer.parseInt(portField.getText().toString()),
                                    Integer.parseInt(slotField.getText().toString()), admin.getText().toString(),
                                    join.getText().toString(), video.isChecked(),
                                    (String)audioTransport.findViewById(audioTransport.getCheckedRadioButtonId()).getTag());
                dialog.dismiss();
                LoadSummary();
                RenderStatus();
                if (firstStart)
                {
                    StartServer();
                }
            }
            catch (NumberFormatException error)
            {
                ShowMessage("Check Advanced settings", "Port and slots must be whole numbers.");
            }
            catch (Exception error)
            {
                ShowMessage("Could not save", error.getMessage());
            }
        }));
        dialog.setOnDismissListener(ignored -> memory = null);
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
