package com.lemonchat.server;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** shows hosting instructions separately from the server controls */
public final class InstructionsActivity extends Activity
{
    @Override
    public void onCreate(Bundle saved)
    {
        super.onCreate(saved);
        int selectedPort = getIntent().getIntExtra("port", 1111);
        int port = selectedPort >= 1024 && selectedPort <= 65535 ? selectedPort : 1111;
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(250, 250, 245));
        root.setOnApplyWindowInsetsListener((view, insets) ->
        {
            view.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(), insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            return insets;
        });
        Button back = new Button(this);
        back.setText("‹  Server");
        back.setAllCaps(false);
        back.setMinHeight(dp(48));
        back.setOnClickListener(view -> finish());
        root.addView(back, new LinearLayout.LayoutParams(-2, -2));
        ScrollView scroll = new ScrollView(this);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(24), dp(12), dp(24), dp(28));
        scroll.addView(content);
        root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        heading(content, "Instructions", 24);
        paragraph(content, "To let friends join, start the server and share its address and port. The hosting screen shows both.");
        heading(content, "Host on your local network", 18);
        paragraph(content, "1. Connect your phone and your friends' devices to the same Wi-Fi, or enable a hotspot on the hosting phone.");
        paragraph(content, "2. In each Lemon Chat client, enter the server address shown on the hosting screen and port " + port + ".");
        paragraph(content, "3. If the server requires keys, enter every configured server key in the same order, using the same cipher types and passwords. Otherwise leave the server keys empty. Save and connect.");
        paragraph(content, "Guest Wi-Fi can block connections between devices. Try your own network or a hotspot if the address cannot be reached.");
        heading(content, "Join from this phone", 18);
        paragraph(content, "In the client on this same phone, use address 127.0.0.1 and port " + port + ". The server keys still apply.");
        TextView advanced = heading(content, "Other ways to connect", 18);
        paragraph(content, "A private VPN can connect your friends' devices to your home network. Use the server's reachable VPN or LAN address in Lemon Chat. Each person needs access to that VPN.");
        paragraph(content, "If you manage your router, port forwarding is another option. Forward the server's listening port to this phone and use your public address. Some internet providers block incoming connections or use shared addresses, so forwarding may be unavailable. Use TLS when exposing browser connections publicly.");
        paragraph(content, "For a public link without router forwarding, the optional Tailscale guide is under Or try more advanced options on the server screen. This method uses an account and the helper bundled with this app.");
        paragraph(content, "Server startup settings contain the listening port, capacity and audio transport. Stop the server before changing them. Local Wi-Fi hosting needs none of these remote-access services.");
        heading(content, "Keep the server available", 18);
        paragraph(content, "Keep this phone powered for longer sessions. Android or battery-saving settings may stop background hosting. Stopping the server disconnects everyone.");
        setContentView(root);
        if (getIntent().getBooleanExtra("advanced", false))
        {
            scroll.post(() -> scroll.smoothScrollTo(0, advanced.getTop()));
        }
    }

    private TextView heading(LinearLayout parent, String text, int size)
    {
        TextView label = label(text, size);
        label.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        LinearLayout.LayoutParams position = new LinearLayout.LayoutParams(-1, -2);
        position.topMargin = dp(parent.getChildCount() == 0 ? 0 : 20);
        position.bottomMargin = dp(8);
        parent.addView(label, position);
        return label;
    }

    private void paragraph(LinearLayout parent, String text)
    {
        TextView label = label(text, 14);
        label.setLineSpacing(dp(2), 1);
        LinearLayout.LayoutParams position = new LinearLayout.LayoutParams(-1, -2);
        position.bottomMargin = dp(12);
        parent.addView(label, position);
    }

    private TextView label(String text, int size)
    {
        TextView label = new TextView(this);
        label.setText(text);
        label.setTextSize(size);
        label.setTextColor(Color.rgb(48, 48, 43));
        return label;
    }

    private int dp(int value)
    {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
