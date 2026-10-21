# Lemon Chat self-hosting guide

This guide covers server settings, network requirements and instructions for hosting directly or through Tailscale Funnel, Cloudflare Tunnel, ngrok or a VPS.

## General setup

There are two connections to share: **HTTPS** loads the chat page; **WSS** carries the chat itself, including WebSocket audio/video. Sharing only the page is not enough.

When a tunnel provides HTTPS and WSS, Lemon Chat can use ordinary HTTP and WS locally. The public ports and local ports do not have to match. For example, Funnel can forward public HTTPS **443** to local HTTP **80**, and public WSS **8443** to local WS **1111**. Enter the public WSS port in the server setup so the served client knows which port to use.

### Use WebSocket only for audio and video

**For the local-server setups in this guide, select WebSocket only for both audio and video.** These connections use the same WS port as the chat, so the WSS forward carries them too.

Lemon Chat's WebRTC connection uses additional UDP ports. Making it reachable from outside a local network requires more port forwarding, firewall rules and correct public-address settings. That extra configuration makes it less practical for a simple local server.

**WebRTC will not work through the Funnel, Cloudflare or ngrok HTTP/WSS setups described below.** It can work with a separately configured UDP connection, such as the WireGuard/VPS setup, but that is an advanced alternative.

### NAT, CGNAT and why port forwarding sometimes fails

**NAT (Network Address Translation)** lets several devices on a local network share one public IPv4 address. A port-forwarding rule tells the router which local computer should receive incoming connections on a chosen port. [Port-forwarding explanation](https://www.tp-link.com/us/support/faq/1379/).

Port forwarding can still fail when:

- **CGNAT (Carrier-Grade NAT):** your internet provider shares one public IPv4 address between multiple customers. Changing your own router's rules does not open a path through the provider's NAT. Ask the provider for a public address or use a tunnel.
- **Two routers perform NAT:** for example, a provider's router sits in front of your own. Forwarding on only one may not be enough.
- **The destination is wrong or blocked:** the rule points to an old local address, uses the wrong port or TCP/UDP choice, the server is not listening, or a firewall blocks it.

Check the router's Internet/WAN address and ask your provider whether it uses CGNAT. Test from another network; reaching the server locally does not prove it is reachable from the internet. [Router troubleshooting](https://www.tp-link.com/ph/support/faq/785/).

A tunnel provides another way in: a program on the server computer connects out to the provider, and the provider passes incoming connections back through it. This avoids needing incoming port-forwarding rules on the home router. [How Cloudflare Tunnel connects](https://developers.cloudflare.com/tunnel/).

## Choose a hosting method

| Option | Suitable for | What you need |
| --- | --- | --- |
| [Tailscale Funnel](#1-tailscale-funnel) | Public HTTPS/WSS for a local server | Tailscale account and app; subject to plan limits |
| [Cloudflare Quick Tunnel](#2-cloudflare-tunnel--another-option-for-short-tests) | Temporary testing | `cloudflared`; no account or domain |
| [Cloudflare named tunnel](#2-cloudflare-tunnel--another-option-for-short-tests) | Keeping a fixed public address | Cloudflare account and a domain |
| [ngrok](#3-ngrok--useful-for-short-tests-with-a-small-free-traffic-allowance) | Temporary testing with a fixed development domain | ngrok account; check the free plan's traffic allowance |
| [Direct hosting](#4-direct-hosting--more-setup-on-your-router) | Hosting without a tunnel provider | Reachable public address and router/firewall configuration |
| [Directly on a VPS](#running-lemon-chat-directly-on-the-vps) | Running the chat on a rented server | Paid VPS rental and server setup |
| [WireGuard with a VPS](wireguard/README.md) | Keeping the chat server local while forwarding TCP and UDP | Paid VPS rental, SSH access and the included setup scripts |

Funnel has been used with Lemon Chat. The Cloudflare and ngrok recipes below follow their documentation but have not yet been tested with real Lemon Chat clients through those providers. Provider limits can change; check the linked documentation before choosing a service for regular use.

## 1. Tailscale Funnel

Funnel is available on Tailscale's free Personal plan. You do not need to buy a domain, rent a server or change your router's port settings. [Personal plan](https://tailscale.com/pricing).

### Install Tailscale

Install [Tailscale](https://tailscale.com/download) on the same computer as Lemon Chat, then sign in. You can use an existing Google/Gmail, Microsoft or Apple account. Users connecting to the public address do not need Tailscale accounts or the Tailscale app. [Sign-in choices](https://tailscale.com/docs/integrations/identity).

If Tailscale asks you to enable Funnel, HTTPS certificates or MagicDNS, follow its instructions. MagicDNS is Tailscale's name for the feature that gives your computer a name ending in `ts.net`. [Funnel setup requirements](https://tailscale.com/docs/features/tailscale-funnel).

### Choose the Lemon Chat settings

In the server's first-run setup:

1. Choose **Tailscale Funnel** when asked how users will connect. Setup then asks only the network questions needed for Funnel.
2. Use local WebSocket port **1111**, public WSS port **8443**, and local HTTP port **80**. These are the defaults for this choice. Funnel provides the public HTTPS page on port **443**; Lemon Chat serves it over local HTTP. There are no domain or certificate questions in Lemon Chat.
3. Setup selects **WebSocket only** for audio/video. **WebRTC will not work through Funnel.** Keep this setting if you change server settings later.
4. Setup allows multiple clients from the same IP address. Keep that enabled: with the commands below, Lemon Chat sees everyone as `127.0.0.1`. Blocking multiple connections from that address prevents multiple users from joining.
5. To connect users automatically when they open the page, allow the server to include connection details in the page. This also includes your connection keys, if any. Otherwise, leave this off and provide the address, port and keys separately.

Choosing Funnel configures Lemon Chat only. Run the commands below to configure Funnel itself. Setup will not ask about WireGuard.

Keep the server running. Open `http://127.0.0.1:80/` on that computer to check that the page loads.

Already have a settings file? See [changing an existing setup](#changing-an-existing-setup) below. You do not need to delete it or lose your channels.

### Run these two commands

Open a terminal on the Lemon Chat computer: PowerShell on Windows, or Terminal on Linux/macOS. Run each line:

```text
tailscale funnel --bg --https=443 http://127.0.0.1:80
tailscale funnel --bg --tls-terminated-tcp=8443 tcp://127.0.0.1:1111
```

The first shares the page. The second lets browsers connect to the chat. If you chose different local ports, replace **80** and **1111** in the commands.

| Public connection | Where Funnel sends it on your computer |
| --- | --- |
| HTTPS, port **443** | HTTP, port **80**, to load `client.html` |
| WSS, port **8443** | WS, port **1111**, for chat, audio and video |

Funnel only allows public ports **443, 8443 and 10000**. The local ports can be different. Funnel handles the secure connection; Lemon Chat does not need a local certificate or stunnel for this setup. Do not add the `--proxy-protocol` option: Lemon Chat does not understand its extra connection header. [Command reference](https://tailscale.com/docs/reference/tailscale-cli/funnel).

### Connect to the server

To see your public address, run:

```text
tailscale funnel status
```

It shows a name like `YOUR-COMPUTER.YOUR-NETWORK.ts.net`. Use the actual name shown there, not this example.

Open `https://YOUR-COMPUTER.YOUR-NETWORK.ts.net/` in a browser and share that URL with users. If you enabled automatic connection details, the page uses the WSS port you entered: **8443**.

Otherwise, enter these values in Lemon Chat's connection window:

- **Address:** the name from Funnel, without `https://` or `wss://`.
- **Port:** `8443`.
- **Connection keys:** the ones you chose, if any.

Always open the **HTTPS page** for this setup. The current client uses WSS when loaded from HTTPS; opening a downloaded HTML file directly uses plain WS instead.

### Keep it running, or stop sharing

Keep the computer, Lemon Chat and Tailscale running. The `--bg` option keeps Funnel working after you close the terminal and after a reboot. To stop these two forwards:

```text
tailscale funnel --bg --https=443 off
tailscale funnel --bg --tls-terminated-tcp=8443 off
```

[Stopping Funnel](https://tailscale.com/docs/reference/tailscale-cli/funnel).

The name belongs to your Tailscale computer; it is not a new random name on every start. The address is public, so anyone who knows it can open the page. Funnel also has a bandwidth limit, which can affect large files or video. [Funnel limits](https://tailscale.com/docs/features/tailscale-funnel).

## 2. Cloudflare Tunnel — another option for short tests

This recipe has not yet been tested with Lemon Chat through Cloudflare. Choose **Cloudflare Tunnel** at the beginning of setup. It selects **WebSocket only** and allows multiple clients from the same IP address. Use local WS port **1111**, public WSS port **443**, and serve the web client on local HTTP port **80**. Answer **no** when asked whether the page and WSS use the same public hostname: this example uses separate names, so automatic connection stays off.

### Temporary addresses, without an account

Install [cloudflared](https://developers.cloudflare.com/tunnel/downloads/). For a quick test, start these in **two separate terminals** and keep both running:

```text
cloudflared tunnel --url http://127.0.0.1:80
```

```text
cloudflared tunnel --url http://127.0.0.1:1111
```

Each prints its own `https://....trycloudflare.com` URL. Open the **first** URL to load the client. In the connection window enter the hostname from the **second** URL, without the scheme, and port **443**. Do not enter the page's hostname as the chat hostname.

Quick Tunnels require no account or domain. Their hostnames change on restart, have no uptime guarantee, and allow only 200 requests being processed at once per tunnel. They are intended for temporary testing. Stop each terminal with Ctrl+C. [Quick Tunnel instructions and limits](https://developers.cloudflare.com/tunnel/get-started/quick-tunnels/).

### A fixed address, using your own domain

For fixed names, create a tunnel in Cloudflare's dashboard, install its connector on the Lemon Chat computer, and add these two public addresses:

| Address you own | Local service |
| --- | --- |
| `chat.example.com` | `http://127.0.0.1:80` |
| `socket.example.com` | `http://127.0.0.1:1111` |

Open `https://chat.example.com/`; connect to `socket.example.com`, port **443**. This route needs an account and a domain configured with Cloudflare. [Named tunnel setup](https://developers.cloudflare.com/tunnel/get-started/).

Cloudflare supports WebSocket connections. Keep WebSockets enabled for the address; provider maintenance can still interrupt an established connection. [WebSocket support](https://developers.cloudflare.com/network/websockets/).

## 3. ngrok — useful for short tests, with a small free traffic allowance

This recipe has not yet been tested with Lemon Chat through ngrok. ngrok can share WebSocket connections as well as pages. It needs an extra step here: another program, Caddy, shares Lemon Chat's page and chat through one local port, so both can use the one address included with ngrok's free plan. [ngrok WebSocket support](https://ngrok.com/docs/using-ngrok-with/websockets).

### Prepare the server

Choose **ngrok** at the beginning of setup. Use local WS port **1111**, public WSS port **443**, and serve the web client on local HTTP port **80**. Setup selects **WebSocket only** and allows multiple clients from the same IP address. For the Caddy setup below, answer **yes** when asked whether the page and WSS use the same public hostname; automatic connection can then be enabled if wanted.

Install ngrok and add your account's authtoken as described in its [setup guide](https://ngrok.com/docs/getting-started/). Do not put the token in this repository.

Install [Caddy](https://caddyserver.com/docs/install). From this `selfhosting-tools` directory, run:

```text
caddy validate --config local-web-and-chat.json
caddy run --config local-web-and-chat.json
```

The included file tells Caddy to listen on local port **8080**, send chat connections to **1111**, and page requests to **80**. Audio and video connections are included. Change those numbers in the JSON file if your local ports differ. [Caddy reverse proxy](https://caddyserver.com/docs/caddyfile/directives/reverse_proxy), [running Caddy](https://caddyserver.com/docs/command-line).

### Share the address

In another terminal:

```text
ngrok http 8080
```

Open the printed HTTPS URL. In Lemon Chat enter the same hostname without the scheme and port **443**. Keep Caddy, ngrok and Lemon Chat running; Ctrl+C stops the first two in their terminals.

The free plan currently includes one assigned development domain and **1 GB of outgoing traffic per month**. Audio/video can use that quickly. HTML visitors may also see ngrok's browser warning page. These limits make the free plan more suitable for short tests than continuous streaming. [Current free plan limits](https://ngrok.com/docs/pricing-limits/free-plan-limits).

If you already host the current client on HTTPS elsewhere, you can skip Caddy and run `ngrok http 1111`, then enter the ngrok hostname and port 443 in that client.

## 4. Direct hosting — more setup on your router

If your connection has a reachable public address, configure your router and firewall to forward the ports you use to the Lemon Chat computer. Check the [NAT and CGNAT explanation](#nat-cgnat-and-why-port-forwarding-sometimes-fails) first if forwarding does not work.

For a public browser page, serve it over HTTPS and provide WSS for the chat. The first-run wizard can start the bundled stunnel:

1. Choose **Host directly**, then enable WSS using the bundled stunnel. If another HTTPS/WSS proxy already handles your secure connections, choose that hosting option instead.
2. Choose a WSS port different from the normal WS port.
3. Choose **Use certificate and private key files you already have**.
4. Enter the PEM certificate chain and private-key paths. No domain entry is required.
5. If serving the page from Lemon Chat, enable its HTTP server and the separate HTTPS listener too.

The certificate must match the address visitors use and be trusted by their browsers. For a self-signed certificate, each visitor must explicitly trust it through their normal certificate setup.

On an already configured server, the corresponding WSS fields are `is_stunnel_enabled` (true), `is_external_wss_enabled` (false), `wss_port`, `stunnel_cert_fullchain` and `stunnel_cert_privkey`; `stunnel_domain` can be empty. Restart after changing these listener settings. Renew manually selected files with the tool that issued them. The alternative Let's Encrypt choice still uses a domain to locate a certificate or, on Linux, request one through certbot.

Forward the selected public WSS/HTTPS TCP ports and keep audio/video set to **WebSocket only** for the simple setup. If you specifically need WebRTC, the [WireGuard guide](wireguard/README.md) explains the extra UDP ports, public-address settings and fixed port range.

## 5. A rented VPS: direct hosting or WireGuard

A VPS is a computer you rent from a hosting company. **VPS rental costs money.** WireGuard itself is free, but using it with a rented VPS is not a free hosting solution.

### Running Lemon Chat directly on the VPS

If you are already paying for a VPS, running Lemon Chat there is usually simpler than using it to forward connections home. Build or install the server on the VPS, configure its firewall, and set up HTTPS/WSS as described under [direct hosting](#4-direct-hosting--more-setup-on-your-router). Your local computer does not need to stay running.

### Keeping Lemon Chat local and forwarding through the VPS

Use this arrangement if you want Lemon Chat to run on your own hardware. The VPS receives users' connections and forwards them through WireGuard to the local server. It runs no chat server. Both machines must stay running.

The Windows/Linux scripts are in [wireguard](wireguard/README.md). Choose **WireGuard through a VPS** at the beginning of Lemon Chat setup. When running from the repository's `buildresult` directory, setup can offer to run these scripts after saving your settings. If the VPS is already configured, decline that offer and enter its public address and forwarded UDP port range. The scripts can also be run directly: `run_setup.bat` on Windows or `setup.sh` on Linux. This setup can forward both TCP and UDP. Saved answers, keys and SSH files remain ignored by Git and must not be committed.

### Privacy considerations

Running Lemon Chat directly on a VPS places its files, running process and server-side keys on infrastructure managed by the hosting provider. If privacy is a concern, that is more trust in the provider than using the VPS only to forward connections. Encrypting network traffic does not by itself protect data while the server processes it. [Cloud-provider access and protection of running applications](https://learn.microsoft.com/en-us/azure/confidential-computing/overview).

Keeping Lemon Chat local leaves its settings, logs and server-side keys on your own machine. For the forwarded web and chat connections, keep HTTPS/WSS encryption all the way to that machine, with its certificates there too. WireGuard only protects the connection between the VPS and the local server; it does not protect the public connection from users to the VPS. [How WireGuard protects a connection](https://www.wireguard.com/).

The VPS can still observe users' and the local server's public IP addresses, connection times and traffic amounts. This reduces what the VPS needs to store or process; it does not make the VPS blind to the connections. End-to-end chat encryption and the keys held by clients are separate from this comparison of server hosting.

## Changing an existing setup

Stop Lemon Chat before editing `server_settings.json`. Change the following fields in your existing file; **do not replace the whole file**. This is the Funnel example:

```json
{
    "websocket_port": 1111,
    "is_client_http_enabled": true,
    "http_port": 80,
    "is_external_wss_enabled": true,
    "wss_port": 8443,
    "is_stunnel_enabled": false,
    "is_https_enabled": false,
    "is_client_config_embedded": true,
    "is_same_ip_address_allowed": true,
    "is_audio_over_websocket_allowed": true,
    "is_audio_over_webrtc_allowed": false
}
```

`is_external_wss_enabled` means another program provides the secure WebSocket connection. `wss_port` is the public port browsers use. `websocket_port` remains the local port that program forwards to. The server sends the configured WSS port to its served page when `is_client_config_embedded` is enabled.

`is_https_enabled` is false because Funnel provides HTTPS on **443** and forwards it to the chosen local `http_port`; the server only needs to serve HTTP locally. Change **80** in the Funnel command if your HTTP port is different.

`is_client_config_embedded` includes the connection keys in the publicly served page. Set it to false if users should enter keys provided separately.

Restart Lemon Chat after saving. For Cloudflare or ngrok, use public WSS port **443**. In the two-address Cloudflare example, also set `is_client_config_embedded` to false so visitors can enter the separate chat address.

## If something does not work

Open the public HTTPS page from another network, enter the public chat hostname and port, and connect two browsers at once. Confirm text and audio in a channel. If only the first browser connects, check the same-IP setting. If the page loads but chat does not connect, check the chat forward separately: a working page forward does not prove the WSS forward works.

If using a proxy already configured elsewhere, forward every WebSocket path, not just `/`; audio and video use their own paths. Keep the local ports behind the host firewall when public access should go only through the tunnel.
