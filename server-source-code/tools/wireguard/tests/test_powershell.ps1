param([string]$Shell = "sh")
. "$PSScriptRoot/../common.ps1"
$ErrorActionPreference = "Stop"
$directory = Join-Path ([IO.Path]::GetTempPath()) ("tunnel-test-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $directory | Out-Null
$path = Join-Path $directory "settings.json"
try {
    foreach ($value in @("spaces ' quote; `$()", 'x"y\z', "")) {
        $encoded = Quote-Sh $value
        $decoded = & $Shell -c "printf '%s' $encoded"
        if ($LASTEXITCODE -ne 0 -or "$decoded" -cne $value) { throw 'Shell quoting failed' }
    }
    foreach ($port in @("0", "65536", "22;id", "08")) {
        if (Test-Port $port) { throw "Invalid port accepted: $port" }
    }
    Write-AtomicText $path '{"old":true}'
    Write-AtomicText $path '{"new":true}'
    if ([IO.File]::ReadAllText($path) -ne '{"new":true}') { throw 'Atomic replacement failed' }
    $lock = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
    $failed = $false
    try { Write-AtomicText $path '{"bad":true}' } catch { $failed = $true } finally { $lock.Dispose() }
    if (-not $failed -or [IO.File]::ReadAllText($path) -ne '{"new":true}') { throw 'Failed replacement damaged original' }
    $script:WgServerSettingsPath = $path
    $settings = Read-ServerSettings
    Write-AtomicText $path '{"changed":true}'
    $failed = $false
    try { Write-ServerSettings $settings } catch { $failed = $true }
    if (-not $failed -or [IO.File]::ReadAllText($path) -ne '{"changed":true}') { throw 'Concurrent modification was overwritten' }
    if (@(Get-ChildItem -LiteralPath $directory).Count -ne 1) { throw 'Temporary file was left behind' }
    Write-Host 'PowerShell argument quoting, validation, atomic writes and conflict checks passed.'
} finally {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    Remove-Item -LiteralPath $directory
}
