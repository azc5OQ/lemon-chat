# Lemon Chat self-hosting guide

Here is how to make your Lemon Chat server accessible from the internet. You can host it directly, use Tailscale Funnel, Cloudflare Tunnel or ngrok, or run it on a VPS.

## General setup

The website and the chat use separate connections. In the public setups below:

- **HTTPS** loads `client.html` in the browser.
- **WSS** is the secure WebSocket connection to the chat server. It also carries audio and video when they use WebSocket.

Both need to work. Being able to open the page does not mean the chat can connect yet.

When a tunnel handles HTTPS and WSS, Lemon Chat can use normal HTTP and WS on your computer. The ports do not have to match. For example, Funnel can send connections from public HTTPS port **443** to HTTP port **80** on your computer, and public WSS port **8443** to WS port **1111**. Enter the public WSS port during server setup so the client knows which port to connect to.

### Use WebSocket only for audio and video

**If you are hosting from your own computer, use WebSocket only for audio and video.** They use the same WS port as the chat, so you do not need to forward extra ports for them.

WebRTC needs additional UDP ports. Getting it to work over the internet means more router and firewall settings, along with the correct public address. That is more work than you need for a simple setup.

**WebRTC will not work through the Funnel, Cloudflare or ngrok setups shown below.** You can set up UDP separately, for example with WireGuard and a VPS, but that needs extra setup.

### NAT, CGNAT and why port forwarding sometimes fails

**NAT (Network Address Translation)** lets devices on your local network share one public IPv4 address. When someone connects to that address, the router needs to know which computer should receive the connection. That is what a port-forwarding rule tells it. [More about port forwarding](https://www.tp-link.com/us/support/faq/1379/).

Port forwarding can still fail when:

- **Your provider uses CGNAT (Carrier-Grade NAT).** Several customers share one public IPv4 address. Your router cannot change the provider's forwarding rules. Ask the provider for a public address, or use a tunnel.
- **There are two routers doing NAT.** For example, your own router is connected to the provider's router. You may need to forward ports on both.
- **Something in the settings is wrong.** Check the computer's local address, the port number and whether the rule uses TCP or UDP. Also check that the server is running and the firewall allows the connection.

Check the Internet/WAN address in your router's settings. If you are unsure about CGNAT, ask your provider. Try connecting from another network too; a connection from inside your own network does not tell you whether it works from the internet. [Router troubleshooting](https://www.tp-link.com/ph/support/faq/785/).

A tunnel avoids this router setup. A program on your computer connects to the tunnel provider, which then passes users' connections back to your computer. You do not need to forward ports on your router for that. [How Cloudflare Tunnel works](https://developers.cloudflare.com/tunnel/).

## Choose a hosting method

First-time setup checks whether the command-line tools are installed. Missing tools appear as **unavailable, install first** and cannot be selected. Install the tool and make its command available in `PATH` before choosing that option. Windows also checks the usual Tailscale and WireGuard installation folders.

This checks installation only. Account sign-in and tunnel configuration are still required where the tool needs them.

| Option | Use it for | What you need |
| --- | --- | --- |
| [Tailscale Funnel](#1-tailscale-funnel) | Making a server on your computer public | Tailscale account and app; plan limits apply |
| [Cloudflare Quick Tunnel](#2-cloudflare-tunnel--another-option-for-short-tests) | Temporary testing | `cloudflared`; no account or domain |
| [Cloudflare named tunnel](#2-cloudflare-tunnel--another-option-for-short-tests) | Keeping a fixed public address | Cloudflare account and a domain |
| [ngrok](#3-ngrok--useful-for-short-tests-with-a-small-free-traffic-allowance) | Testing with an address that stays the same | ngrok account; check how much traffic the free plan allows |
| [Direct hosting](#4-direct-hosting--more-setup-on-your-router) | Hosting without a tunnel provider | Reachable public address, port forwarding and firewall setup |
| [Directly on a VPS](#running-lemon-chat-directly-on-the-vps) | Running the chat on a rented server | Paid VPS rental and server setup |
| [WireGuard with a VPS](wireguard/README.md) | Running Lemon Chat on your computer through a VPS | Paid VPS rental, SSH access and the included setup scripts |

I have used Funnel with Lemon Chat and it works. The Cloudflare and ngrok instructions below follow their documentation, but I have not tried those with Lemon Chat yet. Check the linked limits if you plan to use any of these services regularly, since they can change.

## 1. Tailscale Funnel

Funnel is included in Tailscale's free Personal plan. You do not need to buy a domain, rent a server or forward ports on your router. [Personal plan](https://tailscale.com/pricing).

### Install Tailscale

Install [Tailscale](https://tailscale.com/download) on the computer running Lemon Chat and sign in. You can use a Google/Gmail, Microsoft or Apple account. People joining your chat do not need to install Tailscale or create an account there. [Sign-in choices](https://tailscale.com/docs/integrations/identity).

If Tailscale asks you to enable Funnel, HTTPS certificates or MagicDNS, follow its instructions. MagicDNS is Tailscale's name for the feature that gives your computer a name ending in `ts.net`. [Funnel setup requirements](https://tailscale.com/docs/features/tailscale-funnel).

### Choose the Lemon Chat settings

When you start Lemon Chat for the first time:

1. Choose **Tailscale Funnel** when asked how users will connect.
2. Use WebSocket port **1111**, public WSS port **8443**, and HTTP port **80**. These are the defaults for Funnel. Users open the page through HTTPS on port **443**, and Funnel passes the connection to Lemon Chat's HTTP server. Lemon Chat will not ask for a domain or certificate.
3. Setup selects **WebSocket only** for audio and video. Leave it that way. **WebRTC will not work through Funnel.**
4. Setup allows multiple clients from the same IP address. Keep that on if users share an internet connection. Funnel's HTTP proxy lets Lemon Chat use each user's public IP address.
5. If you want the page to connect automatically, allow the server to include connection details in it. This includes your connection keys too, if you use them. Otherwise, leave it off and give users the address, port and keys separately.

Lemon Chat configures Funnel when the server starts and prints the public HTTPS page and WSS address. Tailscale must already be installed, running and signed in. Lemon Chat does not install it or change your account.

Existing matching routes are kept. Routes for other ports are left alone. If a selected port belongs to another service, or making it public would expose other private paths, startup reports the conflict instead of replacing it. Direct WS access remains available if Funnel setup fails. Forwarded IP headers are ignored until both Funnel routes have been verified at startup.

Keep the server running. Open `http://127.0.0.1:80/` on that computer to check that the page loads.

If you already have a settings file, follow [changing an existing setup](#changing-an-existing-setup) below. There is no need to delete it and lose your channels.

### Manual commands

These commands are available for troubleshooting. They are no longer needed during normal startup:

```text
tailscale funnel --bg --https=443 http://127.0.0.1:80
tailscale funnel --bg --https=8443 http://127.0.0.1:1111
```

The first command makes the page accessible. The second forwards WebSocket connections through Funnel's HTTP proxy. If you chose different ports in Lemon Chat, replace **80** and **1111** in the commands.

If you used the older TCP forwarding command, stop that route before starting Lemon Chat:

```text
tailscale funnel --bg --tls-terminated-tcp=8443 off
```

### Client IP addresses

Choosing **Tailscale Funnel** saves `is_tailscale_funnel_enabled` as `true` in `server_settings.json`. It takes effect when the server starts. In this mode, Lemon Chat reads the `X-Forwarded-For` header only when the connection comes from `127.0.0.1`. Funnel's HTTP proxy replaces any address supplied by the client with the address it observed. Lemon Chat uses that address for connection limits, bans and client information.

Direct WS connections still work on port **1111**. Headers from addresses other than `127.0.0.1` are ignored. A local connection without the header is accepted as `127.0.0.1`.

This assumes you trust programs running on the server computer. Do not use Funnel's TCP forwarding mode or another local TCP proxy on this WebSocket port while Funnel mode is enabled. These proxies connect from the local computer and preserve headers supplied by clients, so those clients could claim another IP address. Ordinary router port forwarding for direct WS connections can still be used. Use the HTTP proxy command above for WSS.

| Public connection | Where Funnel sends it on your computer |
| --- | --- |
| HTTPS, port **443** | HTTP, port **80**, to load `client.html` |
| WSS, port **8443** | WS, port **1111**, for chat, audio and video |

Funnel only allows public ports **443, 8443 and 10000**. The ports on your computer can be different. Funnel handles HTTPS and WSS, so Lemon Chat does not need a certificate or stunnel here. Do not add `--proxy-protocol`; Lemon Chat does not support it. [Funnel commands](https://tailscale.com/docs/reference/tailscale-cli/funnel).

### Connect to the server

The startup summary prints both public addresses after checking the routes. To inspect them manually, run:

```text
tailscale funnel status
```

You will see an address like `YOUR-COMPUTER.YOUR-NETWORK.ts.net`. Use the address from your output.

Open `https://YOUR-COMPUTER.YOUR-NETWORK.ts.net/` in a browser and share that link with users. If you enabled automatic connection, the page will use the WSS port you entered: **8443**.

Otherwise, enter these values in Lemon Chat's connection window:

- **Address:** your Funnel address, without `https://` or `wss://`.
- **Port:** `8443`.
- **Connection keys:** the ones you chose, if any.

Use the **HTTPS page** for this setup. The client chooses WSS when opened through HTTPS. Opening a downloaded `client.html` file directly makes it use plain WS instead, which will not work with this Funnel setup.

### Keep it running, or stop sharing

Keep the computer, Lemon Chat and Tailscale running. Lemon Chat configures Funnel with `--bg`, so its routes remain configured after Lemon Chat stops and after a reboot. The local page and chat are available only while Lemon Chat is running.

To stop sharing, stop Lemon Chat and set `is_tailscale_funnel_enabled` to `false` in `server_settings.json` so the next startup does not enable the routes again. Then run:

```text
tailscale funnel --bg --https=443 off
tailscale funnel --bg --https=8443 off
```

[Stopping Funnel](https://tailscale.com/docs/reference/tailscale-cli/funnel).

Your computer keeps the same Tailscale address between starts. It is a public address, so anyone who knows it can open the page. Funnel also limits bandwidth, which can affect large files and video. [Funnel limits](https://tailscale.com/docs/features/tailscale-funnel).

## 2. Cloudflare Tunnel — another option for short tests

I have not tested these instructions with Lemon Chat through Cloudflare yet.

In Lemon Chat setup:

1. Choose **Cloudflare Tunnel**. This selects **WebSocket only** and allows multiple clients from the same IP address.
2. Use WS port **1111**, public WSS port **443**, and enable the built-in HTTP server on port **80** to serve `client.html`.
3. Answer **no** when asked whether the page and WSS use the same public hostname. This example uses two separate addresses, so automatic connection stays off.

### Temporary addresses, without an account

Install [cloudflared](https://developers.cloudflare.com/tunnel/downloads/). Run these commands in **two separate terminals** and leave both running:

```text
cloudflared tunnel --url http://127.0.0.1:80
```

```text
cloudflared tunnel --url http://127.0.0.1:1111
```

Each command prints a different `https://....trycloudflare.com` address. Open the **first** one in your browser to load the client. In the connection window, enter the **second** address without `https://`, and use port **443**. The page's address and the chat server's address are different here.

Quick Tunnels do not need an account or your own domain. The addresses change when you restart them, there is no uptime guarantee, and each tunnel can process up to 200 requests at once. They are meant for temporary testing. Press Ctrl+C in each terminal to stop them. [Quick Tunnel instructions and limits](https://developers.cloudflare.com/tunnel/get-started/quick-tunnels/).

### A fixed address, using your own domain

If you want to keep the same addresses, create a tunnel in Cloudflare's dashboard and follow its instructions to install the connector on your Lemon Chat computer. Add two public addresses like these, using a domain you own:

| Address you own | Local service |
| --- | --- |
| `chat.example.com` | `http://127.0.0.1:80` |
| `socket.example.com` | `http://127.0.0.1:1111` |

Open `https://chat.example.com/`, then connect to `socket.example.com` on port **443**. Replace these example names with your own. This setup needs a Cloudflare account and a domain configured with Cloudflare. [Named tunnel setup](https://developers.cloudflare.com/tunnel/get-started/).

Cloudflare supports WebSocket connections, but make sure WebSockets are enabled for your site. Cloudflare maintenance can still disconnect users. [WebSocket support](https://developers.cloudflare.com/network/websockets/).

## 3. ngrok — useful for short tests, with a small free traffic allowance

I have not tested these instructions with Lemon Chat through ngrok yet. ngrok supports both web pages and WebSocket connections. Its free plan gives you one address, so this setup uses another program called Caddy to handle the page and chat through one port. Caddy then sends each connection to the right Lemon Chat port. [ngrok WebSocket support](https://ngrok.com/docs/using-ngrok-with/websockets).

### Prepare the server

Choose **ngrok** in Lemon Chat setup. Use WS port **1111**, public WSS port **443**, and enable the built-in HTTP server on port **80** to serve `client.html`. Setup selects **WebSocket only** and allows multiple clients from the same IP address. With Caddy set up as shown below, answer **yes** when asked whether the page and WSS use the same public hostname. You can then enable automatic connection if you want it.

Install ngrok and add your account's authtoken by following its [setup guide](https://ngrok.com/docs/getting-started/). Keep that token out of Git.

Install [Caddy](https://caddyserver.com/docs/install). From this `selfhosting-tools` directory, run:

```text
caddy validate --config local-web-and-chat.json
caddy run --config local-web-and-chat.json
```

The included `local-web-and-chat.json` file tells Caddy to accept connections on port **8080**. It sends chat, audio and video connections to **1111**, and requests for the page to **80**. Change those numbers in the file if you chose different ports. [Caddy reverse proxy](https://caddyserver.com/docs/caddyfile/directives/reverse_proxy), [running Caddy](https://caddyserver.com/docs/command-line).

### Share the address

Open another terminal and run:

```text
ngrok http 8080
```

Open the HTTPS address ngrok prints. In Lemon Chat, enter that same address without `https://`, and use port **443**. Keep Caddy, ngrok and Lemon Chat running. To stop Caddy or ngrok, press Ctrl+C in its terminal.

The free plan currently gives you one development domain and **1 GB of outgoing traffic per month**. Audio and video can use that up quickly. Users may also see an ngrok warning page before reaching the chat. Because of these limits, the free plan makes more sense for short tests than regular streaming. [Current free plan limits](https://ngrok.com/docs/pricing-limits/free-plan-limits).

If you already serve `client.html` over HTTPS somewhere else, you can skip Caddy. Run `ngrok http 1111`, then enter the ngrok address without `https://` and port **443** in that client.

## 4. Direct hosting — more setup on your router

If your public address can be reached from the internet, forward the ports you use on your router to the Lemon Chat computer, and allow them through its firewall. If it does not work, check the [NAT and CGNAT section](#nat-cgnat-and-why-port-forwarding-sometimes-fails).

Use HTTPS for the public page and WSS for the chat. Lemon Chat includes stunnel to handle these secure connections. During the first setup:

1. Choose **Host directly**, then enable WSS using the bundled stunnel. If you already use another program for HTTPS/WSS, choose that hosting option instead.
2. Choose a WSS port different from the normal WS port.
3. Choose **Use certificate and private key files you already have**.
4. Enter the paths to the certificate chain and private key, in PEM format. You do not need to enter a domain for this choice.
5. If Lemon Chat will serve `client.html`, enable its HTTP server and the separate HTTPS port too.

The certificate must match the address users connect to, and their browsers must trust it. If you use a self-signed certificate, each user has to set up that trust themselves.

If you already have a settings file, the WSS settings are `is_stunnel_enabled` (true), `is_external_wss_enabled` (false), `wss_port`, `stunnel_cert_fullchain` and `stunnel_cert_privkey`. You can leave `stunnel_domain` empty when using your own certificate files. Restart the server after changing these settings. When those certificates expire, renew them with the tool you used to get them.

You can also choose Let's Encrypt during setup. That choice needs a domain so Lemon Chat can find its certificate or, on Linux, request one through certbot.

Forward the WSS and HTTPS TCP ports you chose, and keep audio and video set to **WebSocket only** for a simple setup. If you want to set up WebRTC too, the [WireGuard guide](wireguard/README.md) explains which UDP ports to forward and how to set the public address and port range.

## 5. A rented VPS: direct hosting or WireGuard

A VPS is a computer you rent from a hosting company. You have to pay for it. WireGuard itself is free, but using it with a rented VPS still costs money.

### Running Lemon Chat directly on the VPS

If you already pay for a VPS, you can just run Lemon Chat there. That is usually simpler than forwarding connections to your own computer. Build or install the server on the VPS, allow the ports through its firewall, and set up HTTPS/WSS as described under [direct hosting](#4-direct-hosting--more-setup-on-your-router). Your own computer does not need to stay on.

### Keeping Lemon Chat local and forwarding through the VPS

Use this if you want Lemon Chat to stay on your own computer. The VPS receives users' connections and sends them through WireGuard to your server. Lemon Chat itself does not run on the VPS. Both computers need to stay on.

The Windows and Linux setup scripts are in [wireguard](wireguard/README.md). Choose **WireGuard through a VPS** in Lemon Chat setup. If you run Lemon Chat from the repository's `buildresult` directory, setup can offer to run these scripts after saving your settings.

If you already configured the VPS, answer no to that offer and enter its public address and forwarded UDP port range. You can also run the scripts yourself: `run_setup.bat` on Windows or `setup.sh` on Linux. This setup can forward both TCP and UDP. The saved answers, keys and SSH files are ignored by Git; keep them that way.

### Privacy considerations

If Lemon Chat runs on the VPS, its files, running program and the keys stored by the server are on the hosting provider's computer. If privacy matters to you, keep that in mind. Encrypting the connection does not prevent the provider from accessing the machine running the server. [More about protecting programs running on a provider's servers](https://learn.microsoft.com/en-us/azure/confidential-computing/overview).

If Lemon Chat stays on your own computer, its settings, logs and server-side keys stay there too. Have the VPS forward the encrypted HTTPS/WSS connections all the way to your computer, and keep the certificates there. WireGuard only protects the connection between the VPS and your computer. Users still need HTTPS/WSS for their connection to the VPS. [How WireGuard protects a connection](https://www.wireguard.com/).

The VPS can still see users' public IP addresses, your server's public address, when connections happen and how much data passes through. Keeping the chat server at home means less data is stored or processed on the VPS, but it does not hide the connections from it. The end-to-end encryption between chat clients is separate; this is about where the server runs and what the hosting provider can see.

## Changing an existing setup

Stop Lemon Chat before editing `server_settings.json`. Change these values in your existing file; **do not replace the whole file**. These are the settings for the Funnel example above:

```json
{
    "websocket_port": 1111,
    "is_client_http_enabled": true,
    "http_port": 80,
    "is_external_wss_enabled": true,
    "is_tailscale_funnel_enabled": true,
    "wss_port": 8443,
    "is_stunnel_enabled": false,
    "is_https_enabled": false,
    "is_client_config_embedded": true,
    "is_same_ip_address_allowed": true,
    "is_audio_over_websocket_allowed": true,
    "is_audio_over_webrtc_allowed": false
}
```

`is_external_wss_enabled` tells Lemon Chat that another program handles WSS. Browsers connect to `wss_port`, and that program forwards the connection to `websocket_port` on your computer. When `is_client_config_embedded` is on, Lemon Chat includes the WSS port in the page so the client can use it automatically.

Leave `is_https_enabled` false here. Funnel handles HTTPS on port **443** and forwards it to Lemon Chat's `http_port`, so Lemon Chat only needs to serve HTTP. If you use a different HTTP port, replace **80** in the Funnel command too.

`is_client_config_embedded` also puts your connection keys in the public page. Set it to false if you want to give users the keys separately and have them enter them manually.

Save the file and restart Lemon Chat. Existing installations must add `is_tailscale_funnel_enabled` explicitly because it defaults to `false`. For Cloudflare or ngrok, leave that setting `false` and use public WSS port **443**. For the Cloudflare example with two addresses, also set `is_client_config_embedded` to false so users can enter the separate chat address.

## If something does not work

Open the public HTTPS page from another network and try connecting with the public chat address and port. Connect two browsers at once, then try sending a message and talking in a channel.

- If only one browser can connect, check that the server allows multiple clients from the same IP address.
- If the page opens but the chat does not connect, check that WSS reaches the correct WS port. The page and chat use separate connections.
- If you use your own proxy, make sure it forwards every WebSocket path, not just `/`. Audio and video use their own paths.

If users should only connect through the tunnel, use your firewall to block direct public access to Lemon Chat's local ports.
