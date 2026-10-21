# lemon-chat selfhosting-tools/wireguard/status.ps1 - run on the Windows machine that hosts the server.
# shows the tunnel on this machine, then the router side (router/status.sh over ssh) and a tcp check of the
# websocket port through the router. read-only.

. "$PSScriptRoot\common.ps1"

$a = Read-Answers
if ("$(Get-Answer $a 'router_ip' '')" -eq "") {
    Write-Fail "no saved answers (selfhosting-tools\wireguard\answers.json); run setup.ps1 first"
    exit 1
}

# a port changed in server_settings.json after the setup is not forwarded until the setup runs again
$settings = Read-ServerSettings
if ($settings -ne $null) {
    $drift = @()
    if ("$($settings.websocket_port)" -ne "$($a.ws_port)") {
        $drift += "websocket port is $($settings.websocket_port) in the settings, forwarding was set up for $($a.ws_port)"
    }
    if ($settings.is_client_http_enabled -eq $true -and "$($settings.http_port)" -ne "$($a.http_port)") {
        $drift += "http port is $($settings.http_port) in the settings, forwarding was set up for $($a.http_port)"
    }
    if ("$($settings.webrtc_port_range_begin)-$($settings.webrtc_port_range_end)" -ne "$($a.udp_range)") {
        $drift += "udp range is $($settings.webrtc_port_range_begin)-$($settings.webrtc_port_range_end) in the settings, forwarding was set up for $($a.udp_range)"
    }
    if ($settings.is_wireguard_mode_active -ne $true) {
        $drift += "is_wireguard_mode_active is off in the settings; voice and video will not work through the router"
    }
    foreach ($line in $drift) { Write-Warn "$line - run run_setup.bat again" }
}

Write-Host "== tunnel $script:WgTunnelName on this machine ==" -ForegroundColor Cyan
if (-not (Test-IsAdministrator)) {
    Write-Warn "wg.exe can read the tunnel only with administrator rights (run_status.bat asks for them); the local part below may show nothing"
}
if (Test-Path $script:WgExe) {
    & $script:WgExe show $script:WgTunnelName
    $age = Get-WgHandshakeAge
    if ($age -ge 0) {
        Write-Host "last handshake: $age s ago"
    } else {
        Write-Host "no handshake"
    }
} else {
    Write-Warn "WireGuard is not installed here"
}
$firewallRules = @(Get-NetFirewallRule -DisplayName "lemon-chat (wireguard tunnel)*" -ErrorAction SilentlyContinue)
Write-Host "windows firewall rules for the tunnel: $($firewallRules.Count)"

Write-Host ""
Write-Host "== router $($a.router_ip) ==" -ForegroundColor Cyan
$out = Invoke-RemoteScript $a "status.sh"
foreach ($line in @($out)) { Write-Host $line }

Write-Host ""
Write-Host "== reachability ==" -ForegroundColor Cyan
$subnetBase = ("$(Get-Answer $a 'subnet' '10.66.0.0/24')" -split '\.')[0..2] -join '.'
$hostTunnelIp = "$subnetBase.2"
$tunnelCheck = Invoke-RemoteScript $a "check.sh" @($hostTunnelIp, "$($a.ws_port)")
Write-Host ("through the tunnel, router -> tcp {0}:{1} (websocket): {2}" -f $hostTunnelIp, $a.ws_port, $(if ("$tunnelCheck" -match "TUNNEL_TCP_OK") { "answers" } else { "no answer (server not running, or the tunnel is down)" }))
if ((Get-Answer $a "mode" "ports") -eq "ports") {
    $tcpOk = Test-NetConnection -ComputerName $a.router_ip -Port ([int]$a.ws_port) -InformationLevel Quiet -WarningAction SilentlyContinue
    Write-Host ("from outside, tcp {0}:{1} (websocket): {2}" -f $a.router_ip, $a.ws_port, $(if ($tcpOk) { "answers" } else { "no answer (forwarding down, or the provider's firewall)" }))
    if ([int](Get-Answer $a "http_port" 0) -gt 0) {
        $httpOk = Test-NetConnection -ComputerName $a.router_ip -Port ([int]$a.http_port) -InformationLevel Quiet -WarningAction SilentlyContinue
        Write-Host ("from outside, tcp {0}:{1} (http): {2}" -f $a.router_ip, $a.http_port, $(if ($httpOk) { "answers" } else { "no answer" }))
    }
} else {
    Write-Host "full tunnel mode: the public address cannot be checked from this machine (its own packets travel through the tunnel); test from another network"
}
Write-Host "udp ports cannot be probed from here; a voice call between two clients is the test for 3478 and $($a.udp_range)"
