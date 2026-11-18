# Bootstrap only Node here. Build.cjs handles Zig, optional Terser, and the build.
$ErrorActionPreference = 'Stop'
$buildArguments = $args
if ($buildArguments -contains '--download-tools' -and $buildArguments -contains '--no-download-tools')
{
    throw 'Choose only one tool-download policy.'
}
$browserDirectory = Split-Path -Parent $PSScriptRoot
$toolsDirectory = Join-Path $browserDirectory '.build-tools'
$architecture = 'x64'
if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'ARM64')
{
    $architecture = 'arm64'
}
foreach ($line in Get-Content (Join-Path $PSScriptRoot 'node-downloads.txt'))
{
    if ($line.StartsWith("win32-$architecture "))
    {
        $package = $line -split ' '
        break
    }
}
$cached = Join-Path (Join-Path $toolsDirectory $package[3]) 'node.exe'
$node = $null
foreach ($candidate in @('node', $cached))
{
    if (Get-Command $candidate -ErrorAction SilentlyContinue)
    {
        $version = & $candidate -p 'parseInt(process.versions.node)' 2>$null
        if ($LASTEXITCODE -eq 0 -and [int]$version -ge 20)
        {
            $node = $candidate
            break
        }
    }
}
if (-not $node)
{
    $download = $buildArguments -contains '--download-tools'
    if (-not $download -and $buildArguments -notcontains '--no-download-tools' -and -not [Console]::IsInputRedirected)
    {
        $answer = Read-Host 'Node.js is needed to run the client builder. Download portable Node 24.14.0 into browser/.build-tools? [y/N]'
        $download = $answer -match '^(y|yes)$'
    }
    if (-not $download)
    {
        Write-Host 'Install Node.js 20 or newer from https://nodejs.org/ or run build-windows.bat --download-tools.'
        exit 1
    }
    New-Item -ItemType Directory -Force -Path $toolsDirectory | Out-Null
    $archive = Join-Path $toolsDirectory ($package[2].Split('/')[-1])
    & curl.exe --fail --location --retry 2 --output $archive $package[2]
    if ($LASTEXITCODE -ne 0)
    {
        throw 'Node.js download failed.'
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $package[1])
    {
        Remove-Item -LiteralPath $archive
        throw 'Node.js checksum mismatch; nothing was installed.'
    }
    & tar.exe -xf $archive -C $toolsDirectory
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $cached))
    {
        throw 'Could not unpack Node.js.'
    }
    Remove-Item -LiteralPath $archive
    $node = $cached
}
if ($node -eq $cached)
{
    $env:PATH = (Split-Path -Parent $cached) + ';' + $env:PATH
}

# client.html only, or also the Electron app, chosen by --electron, --no-electron, LEMON_CLIENT_BUILD_TARGET=html
# or electron, or else by the question below
$electronChoice = $null
if ($buildArguments -contains '--electron')
{
    $electronChoice = 'electron'
}
elseif ($buildArguments -contains '--no-electron')
{
    $electronChoice = 'html'
}
elseif ($env:LEMON_CLIENT_BUILD_TARGET)
{
    $electronChoice = $env:LEMON_CLIENT_BUILD_TARGET.Trim().ToLowerInvariant()
}
elseif (-not [Console]::IsInputRedirected)
{
    Write-Host ''
    Write-Host '1. client.html only'
    Write-Host '2. client.html and the Electron app, client-electron-amd64.exe, client-electron-arm64.exe and client-electron-i386.exe in buildresult'
    do
    {
        $answer = Read-Host 'Choose [1/2]'
    }
    while ($answer -ne '1' -and $answer -ne '2')
    $electronChoice = if ($answer -eq '2') { 'electron' } else { 'html' }
}
$buildArguments = @($buildArguments | Where-Object { $_ -ne '--electron' -and $_ -ne '--no-electron' })
if ($electronChoice -eq 'electron')
{
    $buildArguments += '--electron'
}
& $node (Join-Path $browserDirectory 'build.cjs') @buildArguments
exit $LASTEXITCODE
