# lemon-chat selfhosting-tools/wireguard/teardown.ps1 - run on the Windows machine that hosts the server, elevated.
# removes the tunnel service here, undoes the router side (router/teardown.sh over ssh) and switches the
# server's wireguard mode off in server_settings.json. the saved answers and the key pairs stay.

. "$PSScriptRoot\common.ps1"

if (-not (Test-IsAdministrator)) {
    Write-Fail "administrator rights are needed to remove the tunnel service: double-click run_teardown.bat, or start PowerShell with Run as administrator"
    exit 1
}

$a = Read-Answers
if ("$(Get-Answer $a 'router_ip' '')" -eq "") {
    Write-Fail "no saved answers (selfhosting-tools\wireguard\answers.json); nothing to remove"
    exit 1
}

Write-Host "this removes the WireGuard tunnel on this machine and on $($a.router_ip), and the port forwarding."
if (-not (Ask-YesNo "Continue?" $false)) { exit 0 }

if (Test-Path $script:WireGuardExe) {
    Write-Step "removing the tunnel service $script:WgTunnelName"
    & $script:WireGuardExe /uninstalltunnelservice $script:WgTunnelName 2>$null | Out-Null
}

Write-Step "removing the windows firewall rules of the tunnel"
Get-NetFirewallRule -DisplayName "lemon-chat (wireguard tunnel)*" -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue

Write-Step "undoing the router side"
$out = Invoke-RemoteScript $a "teardown.sh"
foreach ($line in @($out)) { Write-Host "  $line" }
if ($global:LemonSshExit -ne 0 -or "$out" -notmatch "TEARDOWN_OK=1") {
    throw 'Router teardown failed; saved answers were retained for retry'
}

$settings = Read-ServerSettings
if ($settings -ne $null) {
    Set-SettingsField $settings "is_wireguard_mode_active" $false
    Write-ServerSettings $settings
    Write-Step "server_settings.json: wireguard mode off. restart the chat server"
}

Write-Host "done. selfhosting-tools\wireguard\answers.json, selfhosting-tools\wireguard\keys and selfhosting-tools\wireguard\ssh were kept; delete them by hand if wanted." -ForegroundColor Green
