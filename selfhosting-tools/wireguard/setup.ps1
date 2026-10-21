# lemon-chat selfhosting-tools/wireguard/setup.ps1 - run on the WINDOWS machine that hosts the lemon-chat server.
#
# turns a VPS into a router for this machine: asks a few questions, sets WireGuard up on the VPS over ssh
# (router/setup.sh), installs the WireGuard tunnel here as a service, forwards the chat ports from the router's
# public address through the tunnel, and switches the server's wireguard mode on in server_settings.json.
# see README.md in this directory. run again to change answers; the saved answers are the defaults.
#
# needs: an elevated PowerShell (the tunnel service), the Windows OpenSSH client, internet access.

. "$PSScriptRoot\common.ps1"

if (-not (Test-IsAdministrator)) {
    Write-Fail "administrator rights are needed to install the tunnel service: double-click run_setup.bat, or start PowerShell with Run as administrator"
    exit 1
}

$ssh = Find-Ssh
if ($ssh -eq $null) {
    Write-Fail "ssh.exe was not found. install it: Settings > Apps > Optional features > OpenSSH Client"
    exit 1
}

$saved = Read-Answers
$settings = Read-ServerSettings
if ($settings -eq $null) {
    throw "buildresult\server_settings.json is missing or invalid; complete server setup first"
}

function Default-FromSettings([string]$name, $fallback) {
    if ($settings -ne $null -and $settings.PSObject.Properties[$name] -ne $null -and "$($settings.$name)" -ne "") {
        return $settings.$name
    }
    return $fallback
}

Write-Host ""
Write-Host "lemon-chat: expose this machine's server through a VPS (the router) over WireGuard" -ForegroundColor Green
Write-Host "every answer shows its default in brackets; Enter keeps it. answers are saved to selfhosting-tools\wireguard\answers.json (never a password)."
Write-Host ""

# Collect connection details and ports before changing either machine.
$a = New-Object PSObject
$a | Add-Member NoteProperty router_ip (Ask "router (VPS) public IPv4 address" (Get-Answer $saved "router_ip" ""))
$a | Add-Member NoteProperty router_ssh_port (Ask "router SSH port" (Get-Answer $saved "router_ssh_port" "22"))
$a | Add-Member NoteProperty router_ssh_user (Ask "router SSH user (root, or a user that may sudo)" (Get-Answer $saved "router_ssh_user" "root"))

$defaultAuth = if ("$(Get-Answer $saved 'ssh_key' '')" -ne "") { "1" } else { "2" }
$auth = Ask "SSH authentication: 1 = private key file, 2 = password typed at each ssh prompt" $defaultAuth
$sshKey = ""
if ($auth -eq "1") {
    $keyDefault = Get-Answer $saved "ssh_key" ""
    if ($keyDefault -eq "" -and (Test-Path (Join-Path $env:USERPROFILE ".ssh\id_ed25519"))) { $keyDefault = Join-Path $env:USERPROFILE ".ssh\id_ed25519" }
    $sshKey = Ask "Path to the private key file" $keyDefault
    if (-not (Test-Path $sshKey)) {
        Write-Fail "key file not found: $sshKey"
        exit 1
    }
}
$a | Add-Member NoteProperty ssh_key $sshKey

$modeDefault = if ((Get-Answer $saved "mode" "ports") -eq "full") { "2" } else { "1" }
Write-Host ""
Write-Host "tunnel mode:"
Write-Host "  1 = ports only: only the chat ports go through the tunnel. the server sees every visitor as the router's"
Write-Host "      tunnel address, so country flags, IP bans and the same-IP rule do not work."
Write-Host "  2 = full tunnel: ALL of this computer's IPv4 internet traffic leaves through the router. visitors keep their"
Write-Host "      real addresses. the router carries this computer's traffic too (its bandwidth plan matters)."
$modeChoice = Ask "Tunnel mode" $modeDefault
$mode = if ($modeChoice -eq "2") { "full" } else { "ports" }
$a | Add-Member NoteProperty mode $mode

# the ports come from the server's current settings first: a port changed there after an earlier
# setup must show up here, the saved answer only fills in when the settings file is missing
$a | Add-Member NoteProperty ws_port (Ask "lemon-chat websocket port to forward" (Default-FromSettings "websocket_port" (Get-Answer $saved "ws_port" 1111)))
$httpDefault = Get-Answer $saved "http_port" 0
if ($settings -ne $null) {
    $serve = Default-FromSettings "is_client_http_enabled" $false
    $httpDefault = if ($serve -eq $true) { Default-FromSettings "http_port" 80 } else { 0 }
}
$a | Add-Member NoteProperty http_port (Ask "http port of the served client page to forward (0 = the page is not served)" $httpDefault)

$rangeDefault = Get-Answer $saved "udp_range" "20000-20099"
if ($settings -ne $null) { $rangeDefault = "$(Default-FromSettings 'webrtc_port_range_begin' 20000)-$(Default-FromSettings 'webrtc_port_range_end' 20099)" }
$udpRange = ""
while ($true) {
    $udpRange = Ask "UDP port range for voice and video, forwarded to this machine (BEGIN-END)" $rangeDefault
    if ($udpRange -match '^(\d+)-(\d+)$' -and [int]$Matches[1] -ge 1024 -and [int]$Matches[2] -le 65535 -and [int]$Matches[2] -ge [int]$Matches[1]) { break }
    Write-Warn "enter two ports from 1024 to 65535 as BEGIN-END"
}
$rangeBegin = [int]($udpRange -split '-')[0]
$rangeEnd = [int]($udpRange -split '-')[1]
$a | Add-Member NoteProperty udp_range $udpRange

# tls ends on this machine, in the server's own stunnel; the router forwards the encrypted connections
# unchanged. the question is asked while stunnel is still off too, because the certificate can only be
# obtained once tcp 80 is forwarded
$hostStunnel = ((Default-FromSettings "is_stunnel_enabled" $false) -eq $true)
$tlsDefault = ($hostStunnel -or ("$(Get-Answer $saved 'is_tls_forwarded' 'no')" -eq "yes"))
$hostWss = 0
$hostHttps = 0
Write-Host ""
Write-Host "tls (wss and https) is handled on this machine by the server's own stunnel. the router forwards the"
Write-Host "encrypted connections unchanged and holds no certificate. answer yes before the certificate exists:"
Write-Host "its validation arrives on tcp 80, which is forwarded with the tls ports."
$isTlsForwarded = Ask-YesNo "Forward the tls ports too (wss, https and tcp 80)?" $tlsDefault
if ($isTlsForwarded) {
    $wssDefault = [int](Default-FromSettings "wss_port" (Get-Answer $saved "wss_port" 0))
    if ($wssDefault -le 0) { $wssDefault = 1112 }
    $hostWss = [int](Ask "wss port of the server's stunnel to forward" $wssDefault)
    $httpsDefault = [int](Get-Answer $saved "https_port" 0)
    if ($settings -ne $null -and (Default-FromSettings "is_https_enabled" $false) -eq $true) { $httpsDefault = [int](Default-FromSettings "https_port" 443) }
    $hostHttps = [int](Ask "https port of the served client page to forward (0 = the page is not served over https)" $httpsDefault)
}
$a | Add-Member NoteProperty is_tls_forwarded $(if ($isTlsForwarded) { "yes" } else { "no" })
$a | Add-Member NoteProperty wss_port $hostWss
$a | Add-Member NoteProperty https_port $hostHttps

$a | Add-Member NoteProperty wg_port (Ask "WireGuard port on the router (UDP)" (Get-Answer $saved "wg_port" "51820"))
$subnet = Ask "Tunnel subnet (the router is .1, this machine .2)" (Get-Answer $saved "subnet" "10.66.0.0/24")
if ($subnet -notmatch '^(\d+\.\d+\.\d+)\.\d+/(\d+)$') {
    Write-Fail "the subnet must look like 10.66.0.0/24"
    exit 1
}
$subnetBase = $Matches[1]
$subnetPrefix = $Matches[2]
$routerTunnelIp = "$subnetBase.1"
$hostTunnelIp = "$subnetBase.2"
$a | Add-Member NoteProperty subnet $subnet

$tcpPorts = "$($a.ws_port)"
if ([int]$a.http_port -gt 0) { $tcpPorts += ",$($a.http_port)" }

if ($isTlsForwarded) {
    $tcpPorts += ",$hostWss"
    if ($hostHttps -gt 0) { $tcpPorts += ",$hostHttps" }
    # the certificate is validated over tcp 80 (certbot or another acme client)
    $tcpPorts += ",80"
}
# a port named twice (an http port of 80, for example) is forwarded once
$tcpPorts = (($tcpPorts -split ",") | Select-Object -Unique) -join ","
$udpPorts = "3478,$rangeBegin`:$rangeEnd"
$a | Add-Member NoteProperty tcp_ports $tcpPorts
$a | Add-Member NoteProperty udp_ports $udpPorts

Write-Host ""
Assert-SshAnswers $a
foreach ($port in @("$($a.ws_port)", "$($a.wg_port)")) {
    if (-not (Test-Port $port)) {
        throw "Invalid port $port"
    }
}
foreach ($port in @("$($a.http_port)", "$hostWss", "$hostHttps")) {
    if (-not (Test-Port $port $true)) {
        throw "Invalid optional port $port"
    }
}
if (($tcpPorts -split ',') -contains "$($a.router_ssh_port)") {
    throw 'A forwarded TCP port overlaps the VPS SSH port'
}
if ([int]$a.wg_port -eq 3478 -or ([int]$a.wg_port -ge $rangeBegin -and [int]$a.wg_port -le $rangeEnd)) {
    throw 'Forwarded UDP ports overlap the WireGuard listener'
}

# Canonical RFC1918 network; .1 and .2 must lie inside it. Router validation repeats this.
$network = ($subnet -split '/')[0]
if (-not (Test-IPv4 $network) -or $subnetPrefix -notmatch '^[0-9]{1,2}$') {
    throw 'Invalid tunnel subnet'
}
$octets = $network.Split('.')
$prefix = [int]$subnetPrefix
$private = ($octets[0] -eq '10' -and $prefix -ge 8) -or
    ($octets[0] -eq '172' -and [int]$octets[1] -ge 16 -and [int]$octets[1] -le 31 -and $prefix -ge 12) -or
    ($octets[0] -eq '192' -and $octets[1] -eq '168' -and $prefix -ge 16)
$addressNumber = (([long]$octets[0]*256 + [long]$octets[1])*256 + [long]$octets[2])*256 + [long]$octets[3]
if (-not $private -or $prefix -gt 30 -or $octets[3] -ne '0' -or $addressNumber % [Math]::Pow(2,32-$prefix) -ne 0) {
    throw 'Use a canonical private subnet containing .1 and .2, e.g. 10.66.0.0/24'
}

Write-Host "summary:"
Write-Host "  router ............. $($a.router_ssh_user)@$($a.router_ip) port $($a.router_ssh_port) $(if ($sshKey -ne '') { '(key)' } else { '(password)' })"
Write-Host "  mode ............ $mode"
Write-Host "  tcp forwarded ... $tcpPorts"
Write-Host "  udp forwarded ... $udpPorts"
Write-Host "  tls ............. $(if ($isTlsForwarded) { "ends on this machine (stunnel); wss $hostWss, https $hostHttps and tcp 80 are forwarded" } else { 'not forwarded' })"
Write-Host "  tunnel .......... $routerTunnelIp <-> $hostTunnelIp, udp $($a.wg_port)"
if (-not (Ask-YesNo "Continue?" $true)) { exit 0 }

# ---- 1. WireGuard on this machine ----

if (-not (Test-Path $script:WgExe)) {
    Write-Step "installing WireGuard for Windows with winget"
    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
    if ($winget -eq $null) {
        Write-Fail "winget is not available. install WireGuard from https://www.wireguard.com/install/ and run this script again"
        exit 1
    }
    & winget.exe install --id WireGuard.WireGuard -e --accept-source-agreements --accept-package-agreements
    if (-not (Test-Path $script:WgExe)) { Write-Fail "WireGuard is still missing after the install"; exit 1 }
}

# ---- 2. this machine's key pair ----

New-Item -ItemType Directory -Force -Path $script:WgKeysDir | Out-Null
$hostKeyPath = Join-Path $script:WgKeysDir "host.key"
$hostPubPath = Join-Path $script:WgKeysDir "host.pub"
if (-not (Test-Path $hostKeyPath)) {
    Write-Step "generating this machine's WireGuard key pair"
    $priv = (& $script:WgExe genkey).Trim()
    $pub = ($priv | & $script:WgExe pubkey).Trim()
    [System.IO.File]::WriteAllText($hostKeyPath, $priv)
    [System.IO.File]::WriteAllText($hostPubPath, $pub)
}
$hostPrivateKey = (Get-Content -Raw $hostKeyPath).Trim()
$hostPublicKey = (Get-Content -Raw $hostPubPath).Trim()

# ---- 3. ssh access ----

if ($sshKey -eq "") {
    Write-Host ""
    if (Ask-YesNo "Create a key for this setup and install it on the router, so the password is asked only once?" $true) {
        New-Item -ItemType Directory -Force -Path $script:WgSshDir | Out-Null
        $generated = Join-Path $script:WgSshDir "lemon_router_key"
        if (-not (Test-Path $generated)) {
            # --% hands the rest to the process verbatim, so the empty passphrase (-N "") arrives as an
            # empty argument; powershell would otherwise drop it or pass literal quotes
            $env:TUNNEL_HOST_KEY = $generated
            & ssh-keygen.exe --% -q -t ed25519 -N "" -C tunnel-host -f "%TUNNEL_HOST_KEY%"
        }
        if (Test-Path "$generated.pub") {
            Write-Step "installing the key on the router (the router password is asked now)"
            $pubLine = (Get-Content -Raw "$generated.pub").Trim()
            $pubLine | & $ssh -p $a.router_ssh_port -o StrictHostKeyChecking=accept-new "$($a.router_ssh_user)@$($a.router_ip)" "umask 077; mkdir -p ~/.ssh; cat >> ~/.ssh/authorized_keys"
            if ($LASTEXITCODE -eq 0) {
                $a.ssh_key = $generated
                $sshKey = $generated
                
            } else {
                Write-Warn "the key could not be installed; continuing with password prompts"
            }
        }
    }
}

Write-Step "checking ssh access to the router"
$probe = Invoke-Ssh $a "echo SSH_OK"
if ("$probe" -notmatch "SSH_OK") {
    Write-Fail "ssh to $($a.router_ssh_user)@$($a.router_ip) port $($a.router_ssh_port) failed"
    exit 1
}

# ---- 4. the router side ----

Write-Step "setting the router up (packages, keys, tunnel, port forwarding)"
$remoteArgs = @(
    "--mode", $mode,
    "--host-pubkey", $hostPublicKey,
    "--wg-port", "$($a.wg_port)",
    "--ssh-port", "$($a.router_ssh_port)",
    "--subnet", $subnet,
    "--router-ip", $routerTunnelIp,
    "--host-ip", $hostTunnelIp,
    "--tcp-ports", $tcpPorts,
    "--udp-ports", $udpPorts
)
$out = Invoke-RemoteScript $a "setup.sh" $remoteArgs
foreach ($line in @($out)) { Write-Host "  $line" }
$routerPublicKey = ""
$setupOk = $false
foreach ($line in @($out)) {
    if ("$line" -match '^WG_PUBLIC_KEY=(.+)$') { $routerPublicKey = $Matches[1].Trim() }
    if ("$line" -match '^SETUP_OK=1') { $setupOk = $true }
}
if ($global:LemonSshExit -ne 0 -or -not $setupOk -or $routerPublicKey -eq "") {
    Write-Fail "the router setup did not finish; read the output above"
    exit 1
}

Save-Answers $a

# ---- 5. the tunnel on this machine ----

# full tunnel: everything except private ranges goes through the router (the list WireGuard's own "exclude
# private IPs" option produces), plus the tunnel subnet itself. ports only: just the tunnel subnet
$allowedIps = $subnet
if ($mode -eq "full") {
    $allowedIps = "0.0.0.0/5, 8.0.0.0/7, 11.0.0.0/8, 12.0.0.0/6, 16.0.0.0/4, 32.0.0.0/3, 64.0.0.0/2, 128.0.0.0/3, 160.0.0.0/5, 168.0.0.0/6, 172.0.0.0/12, 172.32.0.0/11, 172.64.0.0/10, 172.128.0.0/9, 173.0.0.0/8, 174.0.0.0/7, 176.0.0.0/4, 192.0.0.0/9, 192.128.0.0/11, 192.160.0.0/13, 192.169.0.0/16, 192.170.0.0/15, 192.172.0.0/14, 192.176.0.0/12, 192.192.0.0/10, 193.0.0.0/8, 194.0.0.0/7, 196.0.0.0/6, 200.0.0.0/5, 208.0.0.0/4, $subnet"
}

$conf = @"
# written by lemon-chat selfhosting-tools/wireguard/setup.ps1 - the tunnel to the VPS that acts as the router
[Interface]
PrivateKey = $hostPrivateKey
Address = $hostTunnelIp/$subnetPrefix

[Peer]
PublicKey = $routerPublicKey
Endpoint = $($a.router_ip):$($a.wg_port)
AllowedIPs = $allowedIps
PersistentKeepalive = 25
"@
[System.IO.File]::WriteAllText($script:WgConfPath, $conf.Replace("`r`n", "`n"))

Write-Step "installing the tunnel service $script:WgTunnelName"
& $script:WireGuardExe /uninstalltunnelservice $script:WgTunnelName 2>$null | Out-Null
Start-Sleep -Seconds 2
& $script:WireGuardExe /installtunnelservice $script:WgConfPath
if ($LASTEXITCODE -ne 0) {
    Write-Fail "wireguard.exe /installtunnelservice failed"
    exit 1
}

Write-Step "waiting for the handshake"
$age = -1
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Seconds 1
    $age = Get-WgHandshakeAge
    if ($age -ge 0) { break }
}
if ($age -lt 0) {
    Write-Warn "no handshake within 30 s. check that udp $($a.wg_port) is open in the VPS provider's firewall, then run status.ps1"
} else {
    Write-Step "tunnel is up (handshake $age s ago)"
}

# ---- 6. the server's settings ----

if ($settings -ne $null) {
    Set-SettingsField $settings "is_wireguard_mode_active" $true
    Set-SettingsField $settings "webrtc_public_address" "$($a.router_ip)"
    Set-SettingsField $settings "webrtc_port_range_begin" $rangeBegin
    Set-SettingsField $settings "webrtc_port_range_end" $rangeEnd
    if ($mode -eq "ports") {
        # every visitor arrives from the router's tunnel address; the same-IP rule would admit only one
        Set-SettingsField $settings "is_same_ip_address_allowed" $true
    }
    Write-ServerSettings $settings
    Write-Step "server_settings.json updated: wireguard mode on, public address $($a.router_ip), udp $rangeBegin-$rangeEnd. restart the chat server"
} else {
    Write-Warn "set these in server_settings.json by hand: is_wireguard_mode_active true, webrtc_public_address $($a.router_ip), webrtc_port_range_begin $rangeBegin, webrtc_port_range_end $rangeEnd$(if ($mode -eq 'ports') { ', is_same_ip_address_allowed true' })"
}

# ---- 7. windows firewall: the forwarded connections arrive on the tunnel adapter ----

# the adapter counts as a public network, where windows blocks unsolicited inbound traffic; one rule per
# protocol, limited to the tunnel adapter, lets the forwarded ports through. re-created on every run
$firewallRuleName = "lemon-chat (wireguard tunnel)"
try {
    Get-NetFirewallRule -DisplayName "$firewallRuleName*" -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue
    New-NetFirewallRule -DisplayName "$firewallRuleName tcp" -Direction Inbound -Action Allow -Protocol TCP -LocalPort ($tcpPorts -split ",") -InterfaceAlias $script:WgTunnelName -Profile Any -ErrorAction Stop | Out-Null
    New-NetFirewallRule -DisplayName "$firewallRuleName udp" -Direction Inbound -Action Allow -Protocol UDP -LocalPort @("3478", "$rangeBegin-$rangeEnd") -InterfaceAlias $script:WgTunnelName -Profile Any -ErrorAction Stop | Out-Null
    Write-Step "windows firewall: inbound tcp $tcpPorts and udp 3478, $rangeBegin-$rangeEnd allowed on the tunnel adapter"
} catch {
    Write-Warn "windows firewall rules could not be created ($($_.Exception.Message)); allow chat-server.exe for public networks by hand"
}

# ---- 8. checks ----

# through the tunnel first: proves the tunnel passes traffic and the server listens (the server must run)
$tunnelCheck = Invoke-RemoteScript $a "check.sh" @($hostTunnelIp, "$($a.ws_port)")
if ("$tunnelCheck" -match "TUNNEL_TCP_OK") {
    Write-Step "the router reaches the chat server through the tunnel (tcp $hostTunnelIp`:$($a.ws_port))"
} else {
    Write-Warn "the router does not reach tcp $hostTunnelIp`:$($a.ws_port) through the tunnel yet. start (or restart) the chat server, then run status.ps1"
}

# from outside: only possible from here in ports-only mode. in full tunnel mode this machine's own
# packets to the router address travel through the tunnel and never meet the forwarding rules
if ($mode -eq "ports") {
    $tcpOk = Test-NetConnection -ComputerName $a.router_ip -Port ([int]$a.ws_port) -InformationLevel Quiet -WarningAction SilentlyContinue
    if ($tcpOk) {
        Write-Step "tcp $($a.router_ip):$($a.ws_port) answers from outside (the forwarding works)"
    } else {
        Write-Warn "tcp $($a.router_ip):$($a.ws_port) does not answer from outside; check the VPS provider's firewall, then run status.ps1"
    }
} else {
    Write-Host "  in full tunnel mode the public address cannot be checked from this machine; a connection from another network is the test"
}

Write-Host ""
Write-Host "done." -ForegroundColor Green
Write-Host "  users connect to:  $($a.router_ip)  port $($a.ws_port)"
if ([int]$a.http_port -gt 0) { Write-Host "  browser:             http://$($a.router_ip):$($a.http_port)/client.html" }
if ($isTlsForwarded) {
    Write-Host "  tls:                 ends on this machine; the certificate and its key are not on the router"
    $stunnelDomain = "$(Default-FromSettings 'stunnel_domain' '')"
    if ($stunnelDomain -ne "") {
        Write-Host "  domain:              the A record of $stunnelDomain must point at $($a.router_ip)"
        if ($hostHttps -gt 0) { Write-Host "  browser, https:      https://$stunnelDomain`:$hostHttps/client.html" }
    } else {
        Write-Host "  domain:              point its A record at $($a.router_ip), then obtain the certificate on this machine (README.md)"
    }
}
Write-Host "  voice and video:     udp 3478 and $rangeBegin-$rangeEnd are forwarded; the server announces $($a.router_ip)"
Write-Host "  status:              .\status.ps1      remove everything: .\teardown.ps1"
