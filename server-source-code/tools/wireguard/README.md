# Running the lemon-chat server at home behind a router

These scripts let a computer without a public address host the chat server. A low-cost VPS acts as
the router: it holds the public address, a WireGuard tunnel connects it to the host, and it forwards
the chat ports through the tunnel. Friends connect to the router's address or to a domain that points
at it. The router runs no chat software and does not terminate the application's TLS connections.

## What runs where

| machine | what it does |
|---|---|
| host (Windows or Linux) | runs the chat server; the WireGuard tunnel to the router is installed as a service; `setup.ps1` or `setup.sh` runs here |
| router (the VPS: Linux, root or sudo over ssh) | WireGuard and SSH endpoint; iptables forwards application connections to the host |

## File guide

Start with `setup.sh` on Linux or `run_setup.bat` on Windows. The matching status and teardown
scripts inspect or remove the setup. Shared helpers are called by these entry points.

| file | purpose |
|---|---|
| `README.md` | This guide: requirements, files, system changes and troubleshooting. |
| `run_setup.bat` | Requests administrator rights, starts `setup.ps1` and keeps the window open. |
| `run_status.bat` | Requests administrator rights to read WireGuard state and starts `status.ps1`. |
| `run_teardown.bat` | Requests administrator rights and starts `teardown.ps1`. |
| `setup.ps1` | Windows host setup: prompts, installs WireGuard if needed, configures both ends, updates server settings and firewall rules. |
| `status.ps1` | Reports Windows/VPS tunnel state, settings mismatches and connection checks. |
| `teardown.ps1` | Removes the Windows tunnel service/firewall rules and VPS setup; disables WireGuard mode in server settings. |
| `common.ps1` | Shared Windows prompts, paths, JSON writes, SSH uploads and command quoting. |
| `setup.sh` | Linux host setup: prompts, installs dependencies if needed, configures both ends, updates server settings and firewall rules. |
| `status.sh` | Reports Linux/VPS tunnel state, settings mismatches and connection checks. |
| `teardown.sh` | Removes Linux tunnel configuration/hooks and VPS setup; disables WireGuard mode in server settings. |
| `common.sh` | Shared Linux prompts, paths, JSON access, service management, SSH uploads and command quoting. |
| `host-firewall.sh` | Adds/removes Linux host rules for forwarded ports on `lemon-wg0`; installed as a tunnel startup/shutdown hook. |
| `json_file.py` | Linux helper that writes answers/settings through a temporary file and atomic replacement. |
| `router/setup.sh` | Runs on the VPS: installs dependencies, creates `wg0`, forwarding rules and startup service; attempts rollback on failure. |
| `router/status.sh` | Reads VPS tunnel, routing, firewall and listening-socket state. |
| `router/teardown.sh` | Removes this setup's VPS tunnel/rules and restores the saved IP-forwarding setting; keeps keys. |
| `router/check.sh` | Makes a bounded TCP connection check from the VPS to the host; reports whether a connection completed. |
| `router/validate.sh` | Shared input checks for addresses, subnets, names, ports and SSH/WireGuard port conflicts; sourcing it changes no system configuration. |
| `router/firewalld.sh` | Included in the generated VPS forwarding hook; manages dedicated firewalld zone/policy files when firewalld is active. |
| `tests/test_scripts.py` | Shell/Python regression tests using temporary files and mocked privileged commands; no live VPS needed. |
| `tests/mock_system.py` | Fake Linux commands and state used by the shell tests; never uploaded or installed by setup. |
| `tests/test_powershell.ps1` | Local tests for quoting, validation and JSON writes; never uploaded or installed by setup. |
| `.gitattributes` | Keeps shell scripts in Unix LF line-ending format. |
| `.gitignore` | Excludes Python bytecode/cache files; generated credentials are excluded by the repository's root `.gitignore`. |

Setup generates these local files; they are not additional programs:

| file | purpose |
|---|---|
| `answers.json` | Reusable setup answers, including the VPS address and key-file path; no SSH/sudo password. |
| `keys/host.key`, `keys/host.pub` | Host WireGuard private/public key pair. The private key stays on the host. |
| `ssh/lemon_router_key`, `ssh/lemon_router_key.pub` | Optional SSH key pair; only the public key is added to the VPS account's `authorized_keys`. |
| `lemon-wg0.conf` | Windows tunnel configuration, containing the host's private key. Linux uses `/etc/wireguard/lemon-wg0.conf`. |

The Windows launchers use `-ExecutionPolicy Bypass` for that PowerShell process; they do not change
the machine's saved execution policy. Setup can install packages, enable a tunnel service, change
firewall/routing rules and edit `buildresult/server_settings.json`. It connects to the VPS you enter;
package installation also contacts the configured package repositories (or winget on Windows).
Status uploads temporary diagnostic scripts and makes connection checks, but does not reconfigure
the tunnel. The installed background component is WireGuard and its startup/shutdown firewall hooks.
Teardown retains saved answers and keys; Windows also retains its local tunnel configuration.

## Every port the server uses and how it is handled

| port | protocol | who listens | handled how |
|---|---|---|---|
| websocket port (`websocket_port`, default 1111) | TCP | server on the host | forwarded through the tunnel |
| http port of the client page (`http_port`, default 8090) | TCP | server on the host | forwarded when the page is served (`serve_client_http`) |
| STUN 3478 | UDP | the server's violet listener on the host | forwarded for compatibility; updated tunnel-mode clients skip this STUN endpoint |
| voice and video datachannel (`webrtc_port_range_begin`-`_end`, default 20000-20099) | UDP | libdatachannel on the host | forwarded one to one; the server binds inside the range only in WireGuard mode |
| TURN 3478 | UDP and TCP | nobody | the client lists TURN urls, but violet runs without credentials, so no TURN exists; nothing to forward |
| wss port (`wss_port`, default 1112) | TCP | stunnel on the host, started by the server (`use_stunnel`) | forwarded when the setup question about the TLS ports is answered with yes |
| https port (`https_port`) | TCP | stunnel on the host (`serve_https`) | forwarded with the TLS ports when it is not 0 |
| 80 | TCP | the certificate client on the host (certbot, or another ACME client) | forwarded with the TLS ports, for the domain validation of the certificate |
| WireGuard (default 51820) | UDP | WireGuard on the router | opened on the router; the tunnel itself |
| ssh port of the router | TCP | sshd on the router | used by the scripts, not changed |
| loopback ports of the Android client | TCP, local | the phone | local to the device, not involved |

The router installs tagged iptables rules and, when firewalld is active, dedicated policies
for the actual public zone and the tunnel. Existing UFW/firewalld port permissions are preserved. The provider's
firewall outside the router is not touched. IPv6 is not handled; visitors reach the router over IPv4.

## Requirements

- Router (VPS): Linux with apt, dnf, yum, apk or pacman, reachable over ssh as root or as a user
  that may sudo. A kernel with WireGuard support (Linux 5.6 or newer, or the wireguard-dkms package).
- VPS provider firewall: some providers filter ports outside the VPS (Hetzner Cloud firewalls,
  Oracle Cloud security lists, AWS security groups). Open UDP 51820 and the forwarded ports. The
  scripts open the router's own firewall, not the provider's.
- Host on Windows: Windows 10 or 11, the OpenSSH client (Settings > Apps > Optional features), and an
  elevated PowerShell. WireGuard for Windows is installed with winget when missing.
- Host on Linux: POSIX `sh`, OpenSSH, Python 3, and root or sudo access. The scripts install
  wireguard-tools and iptables when required. macOS/BSD are not supported by these Linux scripts.
- All project `.sh` files here use POSIX syntax, including the server's `sh` launcher. The upstream
  Linux `wg-quick` utility itself depends on Bash; its distribution package supplies that dependency.
- VPS: systemd or OpenRC for startup persistence. Without either, setup explicitly reports that
  the tunnel must be started again after reboot. Firewalld installations need policy support.
- SSH users requiring a sudo password get a remote terminal and an interactive sudo prompt.
  SSH and sudo passwords are never saved.
- Bandwidth: every chat, voice and video byte crosses the router twice, in and out. Pick the VPS plan by
  its traffic allowance.

## Setup

The setup can be started in two ways.

From the server's first-time setup: the last question is "Make this lemon-chat server reachable from
the internet using WireGuard?". After a yes the server asks "Run the tunnel setup now?". It saves its
settings first and then starts the setup. On Windows `run_setup.bat`
opens in its own window; on Linux `setup.sh` runs in the same terminal. When the setup has ended, the
server reads the values it wrote and continues its start, so no restart is needed. The question
appears when the server is started from the repository's `buildresult` directory, because the scripts
update `buildresult/server_settings.json`. The port check of step 6 reports no answer in this case,
because the server does not listen yet; `status` shows the result once the server runs.

By hand: complete the server's first-time setup so that `buildresult/server_settings.json` exists.
The tunnel setup refuses to continue with missing or invalid settings.

Windows: double-click `run_setup.bat` in the `server-source-code\tools\wireguard` folder. It asks for administrator rights,
runs `setup.ps1` with the execution policy bypassed and keeps the window open. `run_status.bat` and
`run_teardown.bat` do the same for the other two scripts. Running the `.ps1` files directly from an
elevated PowerShell works as well.

Linux:

```sh
cd server-source-code/tools/wireguard
sh setup.sh
```

The script asks for the router address, ssh port, ssh user, key file or password, the tunnel mode, the
ports, whether the TLS ports are forwarded, the WireGuard port and the tunnel subnet. It saves the answers in
`answers.json` (never a password) and uses them as defaults on the next run. With password
authentication it offers to create an ssh key for this setup and install it on the router, so the
password is asked once.

Steps the script performs:

1. Installs WireGuard on the host when missing and generates the host key pair in `keys/`.
2. Uploads `router/setup.sh` and runs it as root on the router: installs wireguard-tools and iptables,
   generates the router key pair, writes `/etc/wireguard/wg0.conf` and `/etc/wireguard/wg0-forward.sh`,
   enables IP forwarding, installs owned firewall rules and starts `wg-quick@wg0`.
   An unrelated existing `wg0` is refused; it is never backed up and replaced.
3. Writes the host tunnel config (`lemon-wg0.conf` on Windows, `/etc/wireguard/lemon-wg0.conf` on
   Linux), installs it as a service and waits for the handshake.
4. Sets in `buildresult/server_settings.json`: `is_wireguard_mode_active` true,
   `webrtc_public_address` to the router address, `webrtc_port_range_begin` and `webrtc_port_range_end`
   to the UDP range, and `is_same_ip_address_allowed` true in ports-only mode. A running chat server
   has to be restarted afterwards. A server that started this setup from its first-time setup reads
   the values itself.
5. Opens the host's own firewall for the forwarded ports on the tunnel interface: Windows Firewall
   rules limited to the tunnel adapter on Windows; tagged iptables rules or a dedicated firewalld
   zone on Linux. Linux PostUp/PostDown hooks reapply/remove these rules with the tunnel.
6. Checks that the router reaches the chat server through the tunnel, and in ports-only mode also that
   the websocket port answers from outside. In full tunnel mode the host's own packets to the router
   address travel through the tunnel, so the outside check has to come from another network.

## Verification status

The shell regression suite runs without root or a VPS:

```sh
python3 tests/test_scripts.py --shell /bin/dash -v
# Also run against BusyBox sh when available.
```

It exercises validation, shell argument escaping, connection-check failures, atomic JSON writes,
repeated setup, port changes, rollback after a failed firewall insertion, teardown and ownership
checks using a temporary filesystem and mocked Linux commands. PowerShell scripts also require
parser validation. These checks do not replace live systemd/OpenRC, UFW/firewalld and ICE testing
on a Linux VPS.

On Windows, `tests/test_powershell.ps1 -Shell <path-to-sh.exe>` tests remote argument quoting,
port validation, atomic replacement failures and settings modification conflicts without SSH.

Setup stops the old tunnel before replacing its PostDown rules. A failed router installation
returns failure and attempts to restore the previous configuration. Keep the server stopped while
changing its JSON settings; writes use a temporary file and atomic replacement. Old releases did
not record firewall ownership: ambiguous pre-existing files are refused rather than overwritten,
and legacy UFW/firewalld openings may need manual review. After an administrator reloads UFW or
firewalld, restart the tunnel to reapply its tagged iptables rules.

`status.ps1` / `status.sh` show the tunnel on both sides, the forwarding rules and the port checks.
`teardown.ps1` / `teardown.sh` remove the tunnel on both sides and the forwarding, and switch
wireguard mode off in the server settings.

## The two tunnel modes

**Ports only.** Only the chat ports use the tunnel. On the router, traffic to the host is masqueraded, so
the host sees every visitor as the router's tunnel address (10.66.0.1 by default). Consequences on the
server: country flags show the router's country, IP bans cannot separate visitors, and the same-IP rule
would admit one visitor, so the script switches `is_same_ip_address_allowed` on.

**Full tunnel.** All of the host's IPv4 internet traffic leaves through the router. Visitors keep their real
addresses on the server, and flags, bans and the same-IP rule work. On Windows the tunnel routes every
public range and leaves private ranges (LAN) alone; on Linux wg-quick routes everything and keeps the
LAN reachable through its own policy routing. The router carries the host's own traffic in this mode.

Voice and video work in both modes.

## Wireguard mode in the server

The server's WebRTC path (voice and video) needs two things when it runs behind a router:

- Its datachannel sockets use a fixed UDP port range (`webrtc_port_range_begin` to
  `webrtc_port_range_end`) instead of random ports, so the router can forward the range one to one.
- Every host candidate the server gathers is also sent to the client once more with the router's
  public address (`webrtc_public_address`), typed as a server-reflexive candidate with the same port.
  A client sends its connectivity checks to that address, the router forwards them through the tunnel,
  and the replies return along the same path. A client on the host's own LAN still prefers the direct
  host candidate.

Both apply only while `is_wireguard_mode_active` is true. With the setting off, the server behaves as
before. The last question of the first-time setup wizard is "Make this lemon-chat server reachable
from the internet using WireGuard?". With a yes it offers to start the tunnel setup; with a no to that
offer it asks for the public address and the UDP range itself. The scripts here set the same values in
an existing `server_settings.json`.

In WireGuard mode, the server advertises `stun_port: 0`. Updated clients skip STUN/TURN discovery
and initiate ICE checks toward the server's explicit public candidates; peer-reflexive discovery
handles their NAT mappings. This avoids returning the router's private address as a visitor's
reflexive address in ports-only mode. Deploy the updated client with the updated server. UDP 3478
remains forwarded for compatibility, but is not used by updated clients in this mode. No TURN relay
is involved; restrictive networks that require TURN remain unsupported.

## Domain and HTTPS

TLS ends on the host, in the server's own stunnel. The router forwards the encrypted connections
unchanged. It holds no application TLS certificate or TLS private key, so it cannot decrypt that TLS
traffic. It does hold its own WireGuard private key and can observe addresses, ports and traffic volume.

Steps for a domain with HTTPS:

1. Point the domain's A record at the router's public address.
2. Run the tunnel setup and answer yes to "Forward the tls ports too?". The router then forwards the
   wss port, the https port when it is not 0, and TCP 80.
3. Obtain the certificate on the host, as described below. The validation request arrives on TCP 80
   of the router and is forwarded to the host.
4. Switch stunnel on: answer yes to the stunnel question of the server's first-time setup, or set
   `use_stunnel` to true and the certificate paths in `server_settings.json`. Restart the server.

The setup asks its question while stunnel is still off, because step 3 needs TCP 80 forwarded before
a certificate exists. The plain websocket port stays forwarded as well, for a client.html file opened
from disk.

### stunnel on the host

The server can run its own stunnel for wss and https. On every platform it writes `stunnel.conf`
next to its executable at start and launches the bundled stunnel with it. On Windows it prefers the
console build `tstunnel.exe` and falls back to `stunnel.exe`; the child is tied to the server's
process, so it ends with the server, and a leftover from a hard kill is terminated at the next start.
The Windows build script builds both binaries in Git for Windows' bash and copies them into
`buildresult`; no extra Perl is needed, the modules OpenSSL's build wants are vendored under
`third-party/stunnel/perl-extra-lib`.

The certificate is obtained once by the operator and renewed like this:

- **certbot on Windows.** `pip install certbot` (Python 3), then in an administrator prompt
  `certbot certonly --standalone -d chat.example.com`. certbot answers the validation on TCP 80 itself
  and stores the files under `C:\Certbot\live\chat.example.com\`. The wizard finds them there by
  domain name. At every start the server runs `certbot renew` for a certificate stored under a `live`
  directory, the same as on Linux. A server that runs for months without a restart needs a scheduled
  task running `certbot renew`, because the pip install creates none.
- **win-acme.** `wacs.exe` with the PEM files store; it installs its own renewal task. Enter the two
  file paths in the wizard. The server does not renew these itself.
- **Linux.** certbot from the distribution; the wizard runs `certbot certonly` and the server renews
  at start, as before.

## What is uploaded and kept on the VPS

The host uploads the selected `router/setup.sh`, `router/status.sh`, `router/teardown.sh` or
`router/check.sh`, together with `router/validate.sh` and `router/firewalld.sh`. You can read their
complete source in this folder.

- Uploaded scripts and their helpers live in a randomly named private `/tmp/tunnel.*` directory.
  The caller cleans that directory after execution; scripts do not delete their own source files.
- The files they leave are `/etc/wireguard/wg0.conf`, `/etc/wireguard/wg0.key`,
  `/etc/wireguard/wg0-forward.sh`, the saved IP-forwarding state and `/etc/sysctl.d/99-wg0-forward.conf`.
  Dedicated firewalld zone/policy files are also installed when applicable. Managed config is marked
  "managed by the tunnel setup".
- With TLS switched on, the VPS forwards encrypted application bytes.
  Without TLS, http and websocket traffic passes in clear text, and the chat's own per-client
  encryption is what protects the message content.
- The console output the host scripts read carries the markers `WG_PUBLIC_KEY`, `SETUP_OK`,
  `TUNNEL_TCP_OK` and `TEARDOWN_OK`.
- The ssh key the setup can install in `authorized_keys` has the comment `tunnel-host`.
- The SSH session exposes the host's public address and OpenSSH client version to the VPS.
  VPS auditing and logging are controlled by its administrator; these scripts do not disable them.

## Checks when something does not work

- No handshake: UDP 51820 (or the chosen WireGuard port) is not reachable on the router. Check the
  provider's firewall. `status` on the router shows `no handshake yet`.
- Handshake, but the websocket port does not answer: the chat server is not running on the host, or
  it listens on another port than the one forwarded. `status` shows the DNAT rules.
- Text works, voice does not: the UDP range in `server_settings.json` and the forwarded range differ,
  or the server was not restarted after the setup. The server's startup summary prints the range.
- Certificate errors: the domain does not resolve to the router yet, TCP 80 is not forwarded (answer
  yes to the TLS question of the setup), or port 80 is closed at the provider.
