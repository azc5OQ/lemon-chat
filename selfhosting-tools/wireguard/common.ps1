# lemon-chat selfhosting-tools/wireguard/common.ps1 - shared by setup.ps1, status.ps1 and teardown.ps1 (dot-sourced, not run directly).
# paths, the saved answers, the ssh helpers and the server_settings.json access. Windows PowerShell 5.1 and PowerShell 7.

$script:WgHere = Split-Path -Parent $MyInvocation.MyCommand.Path
# The project root is two directories above selfhosting-tools/wireguard.
$script:WgRepo = Split-Path -Parent (Split-Path -Parent $script:WgHere)
$script:WgAnswersPath = Join-Path $script:WgHere "answers.json"
$script:WgKeysDir = Join-Path $script:WgHere "keys"
$script:WgSshDir = Join-Path $script:WgHere "ssh"
$script:WgTunnelName = "lemon-wg0"
$script:WgConfPath = Join-Path $script:WgHere "$script:WgTunnelName.conf"
$script:WgServerSettingsPath = Join-Path $script:WgRepo "buildresult\server_settings.json"
$script:WgInstallDir = "C:\Program Files\WireGuard"
$script:WgExe = Join-Path $script:WgInstallDir "wg.exe"
$script:WireGuardExe = Join-Path $script:WgInstallDir "wireguard.exe"

# native tools write progress to stderr; that must not become a terminating error
$ErrorActionPreference = "Continue"
$OutputEncoding = New-Object System.Text.UTF8Encoding($false)

# Console output and interactive questions.
function Write-Step([string]$text) { Write-Host "[lemon-wg] $text" -ForegroundColor Cyan }
function Write-Warn([string]$text) { Write-Host "[lemon-wg] $text" -ForegroundColor Yellow }
function Write-Fail([string]$text) { Write-Host "[lemon-wg] $text" -ForegroundColor Red }

function Ask([string]$prompt, [string]$default) {
    # one question with its default in brackets; Enter takes the default
    if ($default -ne $null -and $default -ne "") {
        $answer = Read-Host "$prompt [$default]"
        if ($answer -eq "") { return $default }
        return $answer.Trim()
    }
    while ($true) {
        $answer = Read-Host $prompt
        if ($answer.Trim() -ne "") { return $answer.Trim() }
    }
}

function Ask-YesNo([string]$prompt, [bool]$defaultYes) {
    $hint = if ($defaultYes) { "(Y/n)" } else { "(y/N)" }
    while ($true) {
        $answer = (Read-Host "$prompt $hint").Trim().ToLower()
        if ($answer -eq "") { return $defaultYes }
        if ($answer -eq "y" -or $answer -eq "yes") { return $true }
        if ($answer -eq "n" -or $answer -eq "no") { return $false }
    }
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Saved answers and settings are JSON data, never executable PowerShell.
function Read-Answers {
    # the saved answers of the last setup, or an empty object. never holds a password
    if (Test-Path $script:WgAnswersPath) {
        try {
            return (Get-Content -Raw -Path $script:WgAnswersPath | ConvertFrom-Json)
        } catch {
            # Missing/invalid saved answers fall back to the setup defaults below.
        }
    }
    return New-Object PSObject
}

function Write-AtomicText([string]$path, [string]$text) {
    # Flush a sibling temporary file before replacing the destination.
    # A failed write or replacement leaves the previous destination intact.
    $ErrorActionPreference = "Stop"
    $temp = Join-Path (Split-Path -Parent $path) (".tunnel-" + [Guid]::NewGuid().ToString("N"))
    try {
        $bytes = (New-Object System.Text.UTF8Encoding($false)).GetBytes($text)
        $stream = [IO.File]::Open($temp, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try {
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush($true)
        } finally {
            $stream.Dispose()
        }
        if (Test-Path -LiteralPath $path) {
            # NullString supplies a real null backup path to .NET on Windows PowerShell.
            [IO.File]::Replace($temp, $path, [System.Management.Automation.Language.NullString]::Value)
        } else {
            [IO.File]::Move($temp, $path)
        }
    } finally {
        if (Test-Path -LiteralPath $temp) {
            Remove-Item -LiteralPath $temp -Force
        }
    }
}

# Validate values before using them as SSH arguments or tunnel configuration.
function Test-Port([string]$value, [bool]$allowZero = $false) {
    if ($allowZero -and $value -eq "0") { return $true }
    return ($value -match '^[1-9][0-9]{0,4}$' -and [int]$value -le 65535)
}

function Test-IPv4([string]$value) {
    $parts = $value.Split('.')
    if ($parts.Count -ne 4) { return $false }
    foreach ($part in $parts) {
        if ($part -notmatch '^(0|[1-9][0-9]{0,2})$' -or [int]$part -gt 255) {
            return $false
        }
    }
    return $true
}

function Assert-SshAnswers($answers) {
    if (-not (Test-IPv4 "$($answers.router_ip)") -or
        -not (Test-Port "$($answers.router_ssh_port)") -or
        "$($answers.router_ssh_user)" -notmatch '^[a-zA-Z0-9_][a-zA-Z0-9_.-]*$') {
        throw 'Invalid SSH IPv4 address, port or username'
    }
}

function Quote-Sh([string]$value) {
    # Escape each apostrophe for the remote POSIX shell, keeping one argument intact.
    return "'" + $value.Replace("'", "'" + [char]92 + "''") + "'"
}

function Save-Answers($answers) {
    $json = $answers | ConvertTo-Json -Depth 5
    Write-AtomicText $script:WgAnswersPath $json
}

function Get-Answer($answers, [string]$name, $default) {
    if ($answers -ne $null -and $answers.PSObject.Properties[$name] -ne $null -and "$($answers.$name)" -ne "") {
        return $answers.$name
    }
    return $default
}

function Read-ServerSettings {
    if (-not (Test-Path $script:WgServerSettingsPath)) { return $null }
    try {
        $script:SettingsOriginalText = [IO.File]::ReadAllText($script:WgServerSettingsPath)
        return ($script:SettingsOriginalText | ConvertFrom-Json)
    } catch {
        return $null
    }
}

function Write-ServerSettings($settings) {
    # written without a byte order mark, the way the server writes it
    $json = $settings | ConvertTo-Json -Depth 30
    if ([IO.File]::ReadAllText($script:WgServerSettingsPath) -cne $script:SettingsOriginalText) {
        throw 'Settings changed during setup; stop the server and retry'
    }
    Write-AtomicText $script:WgServerSettingsPath $json
    $script:SettingsOriginalText = $json
}

function Set-SettingsField($settings, [string]$name, $value) {
    if ($settings.PSObject.Properties[$name] -ne $null) {
        $settings.$name = $value
    } else {
        $settings | Add-Member -NotePropertyName $name -NotePropertyValue $value
    }
}

# SSH execution and private temporary uploads to the VPS.
function Find-Ssh {
    $ssh = Get-Command ssh.exe -ErrorAction SilentlyContinue
    if ($ssh -ne $null) { return $ssh.Source }
    $builtin = Join-Path $env:SystemRoot "System32\OpenSSH\ssh.exe"
    if (Test-Path $builtin) { return $builtin }
    return $null
}

function Get-SshArgs($answers) {
    # the argument list every ssh call shares: port, key (when one is used) and the host key policy
    Assert-SshAnswers $answers
    $list = @(
        "-o", "ConnectTimeout=10",
        "-o", "ServerAliveCountMax=3",
        "-p", "$($answers.router_ssh_port)",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", "ServerAliveInterval=15"
    )
    if ("$($answers.ssh_key)" -ne "") {
        $list += @("-i", "$($answers.ssh_key)", "-o", "IdentitiesOnly=yes")
    }
    $list += "$($answers.router_ssh_user)@$($answers.router_ip)"
    return $list
}

function Invoke-Ssh($answers, [string]$remoteCommand, [bool]$terminal = $false) {
    # runs one command on the router; returns its stdout lines, sets $global:LemonSshExit
    $ssh = Find-Ssh
    $sshArgs = Get-SshArgs $answers
    if ($terminal) { $sshArgs = @("-tt") + $sshArgs }
    $output = & $ssh @sshArgs $remoteCommand | ForEach-Object {
        # Display interactive sudo prompts even while the caller captures stdout.
        if ($terminal) { Write-Host $_ }
        $_
    }
    $global:LemonSshExit = $LASTEXITCODE
    return $output
}

function Find-Scp {
    $scp = Get-Command scp.exe -ErrorAction SilentlyContinue
    if ($scp -ne $null) { return $scp.Source }
    $builtin = Join-Path $env:SystemRoot "System32\OpenSSH\scp.exe"
    if (Test-Path $builtin) { return $builtin }
    return $null
}

function Send-RemoteScript($answers, [string]$localPath, [string]$remotePath) {
    # uploads a script with scp from a temporary copy that has unix line endings (a checkout on windows
    # may carry cr lf, and a powershell pipe would add its own line ending)
    $scp = Find-Scp
    if ($scp -eq $null) {
        Write-Fail "scp.exe was not found next to ssh.exe"
        return $false
    }
    $content = (Get-Content -Raw -Path $localPath) -replace "`r`n", "`n"
    $temp = Join-Path $env:TEMP ("tunnel-" + [Guid]::NewGuid().ToString("N"))
    [System.IO.File]::WriteAllText($temp, $content, (New-Object System.Text.UTF8Encoding($false)))
    $scpArgs = @("-P", "$($answers.router_ssh_port)", "-o", "StrictHostKeyChecking=accept-new")
    if ("$($answers.ssh_key)" -ne "") { $scpArgs += @("-i", "$($answers.ssh_key)", "-o", "IdentitiesOnly=yes") }
    $scpArgs += @($temp, "$($answers.router_ssh_user)@$($answers.router_ip):$remotePath")
    & $scp @scpArgs | Out-Null
    $ok = ($LASTEXITCODE -eq 0)
    Remove-Item -Path $temp -ErrorAction SilentlyContinue
    return $ok
}

function Invoke-RemoteScript($answers, [string]$scriptName, [string[]]$arguments = @()) {
    Assert-SshAnswers $answers
    if ($scriptName -notin @("setup.sh", "status.sh", "teardown.sh", "check.sh")) {
        throw 'Invalid remote script'
    }
    $stage = ((Invoke-Ssh $answers 'umask 077; mktemp -d /tmp/tunnel.XXXXXXXXXX') -join "").Trim()
    if ($global:LemonSshExit -ne 0 -or $stage -notmatch '^/tmp/tunnel\.[a-zA-Z0-9]+$') {
        throw 'Could not create private upload directory'
    }
    $resultCode = 1
    try {
        foreach ($name in @($scriptName, "validate.sh", "firewalld.sh")) {
            $localPath = Join-Path (Join-Path $script:WgHere "router") $name
            if (-not (Send-RemoteScript $answers $localPath "$stage/$name")) {
                throw "Could not upload $name"
            }
        }
        $command = "sh " + (Quote-Sh "$stage/$scriptName")
        foreach ($argument in $arguments) {
            $command += " " + (Quote-Sh $argument)
        }
        $output = Invoke-Ssh $answers $command ($answers.router_ssh_user -ne "root" -and $scriptName -ne "check.sh")
        $resultCode = $global:LemonSshExit
        return $output
    } finally {
        # Delete only this call's uploads and preserve the script's exit code,
        # rather than reporting the result of the cleanup SSH command.
        Invoke-Ssh $answers "rm -f '$stage/$scriptName' '$stage/validate.sh' '$stage/firewalld.sh'; rmdir '$stage'" | Out-Null
        $global:LemonSshExit = $resultCode
    }
}

# Local tunnel diagnostics.
function Get-WgHandshakeAge {
    # seconds since the last handshake of the tunnel, or -1 when there was none
    if (-not (Test-Path $script:WgExe)) { return -1 }
    $lines = & $script:WgExe show $script:WgTunnelName latest-handshakes 2>$null
    if ($LASTEXITCODE -ne 0 -or $lines -eq $null) { return -1 }
    foreach ($line in @($lines)) {
        $parts = "$line" -split "\s+"
        if ($parts.Length -ge 2 -and $parts[1] -match '^\d+$' -and [int64]$parts[1] -gt 0) {
            $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
            return ($now - [int64]$parts[1])
        }
    }
    return -1
}
