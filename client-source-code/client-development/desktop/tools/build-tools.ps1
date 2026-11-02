param
(
    [string]$EnvironmentFile,
    [ValidateSet('ask', 'download', 'off')][string]$Mode = 'ask',
    [switch]$WithTests,
    [switch]$ProbeOnly,
    [switch]$FunctionsOnly
)

$ErrorActionPreference = 'Stop'
# A launcher such as CTest can inherit PowerShell 7's module path when starting
# Windows PowerShell. Resolve this host's bundled modules first in this process.
$hostModules = Join-Path $PSHOME 'Modules'
if (($env:PSModulePath -split ';') -notcontains $hostModules)
{
    $env:PSModulePath = $hostModules + ';' + $env:PSModulePath
}

function Get-DesktopTool
{
    param([string]$Name, [string[]]$Candidates = @())
    foreach ($candidate in $Candidates)
    {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf))
        {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command)
    {
        return $command.Source
    }
    return $null
}

function Test-DesktopDigest
{
    param([string]$Path, [string]$Digest)
    return (Test-Path -LiteralPath $Path -PathType Leaf) -and ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -eq $Digest)
}

function Get-DesktopDownload
{
    param([string]$Name, [string]$Url, [string]$Archive, [string]$Digest)
    if (Test-DesktopDigest $Archive $Digest)
    {
        return
    }
    $partial = $Archive + '.part'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Write-Host "Downloading $Name from $Url"
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $partial
    if (!(Test-DesktopDigest $partial $Digest))
    {
        throw "The $Name download failed its SHA-256 check. It was not executed or unpacked."
    }
    Move-Item -LiteralPath $partial -Destination $Archive -Force
}

function Expand-DesktopTool
{
    param([string]$Archive, [string]$Destination, [switch]$SelfExtracting)
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    if ($SelfExtracting)
    {
        # Only a verified, pinned upstream archive reaches this point.
        $process = Start-Process -FilePath $Archive -ArgumentList @('-y', ('-o"' + $Destination + '"')) -WindowStyle Hidden -Wait -PassThru
        if ($process.ExitCode -ne 0)
        {
            throw "Tool extraction failed with exit code $($process.ExitCode)."
        }
    }
    else
    {
        Expand-Archive -LiteralPath $Archive -DestinationPath $Destination -Force
    }
}

if ($FunctionsOnly)
{
    return
}

try
{
    $toolRoot = Join-Path $env:LOCALAPPDATA 'LemonChat\BuildTools'
    $downloads = Join-Path $toolRoot 'downloads'
    $compilerRoot = Join-Path $toolRoot 'w64devkit-2.8.0'
    $cmakeRoot = Join-Path $toolRoot 'cmake-4.3.1'
    $ninjaRoot = Join-Path $toolRoot 'ninja-1.12.1'
    $nodeRoot = Join-Path $toolRoot 'node-24.22.2'
    $compilerCandidates = @()
    if ($env:MINGWPATH)
    {
        $compilerCandidates += (Join-Path $env:MINGWPATH 'gcc.exe')
    }
    $compilerCandidates += (Join-Path $env:USERPROFILE 'Downloads\w64devkit\bin\gcc.exe')
    $compilerCandidates += (Join-Path $compilerRoot 'w64devkit\bin\gcc.exe')
    $compiler = Get-DesktopTool 'gcc.exe' $compilerCandidates
    if (!$compiler)
    {
        $compiler = Get-DesktopTool 'cl.exe'
    }
    $cmake = Get-DesktopTool 'cmake.exe' @((Join-Path $cmakeRoot 'cmake-4.3.1-windows-x86_64\bin\cmake.exe'), (Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'))
    if ($cmake)
    {
        $version = & $cmake --version
        if ($LASTEXITCODE -ne 0 -or ($version -join ' ') -notmatch 'cmake version (\d+\.\d+\.\d+)' -or [version]$Matches[1] -lt [version]'3.22.0')
        {
            $cmake = $null
        }
    }
    $ninjaCandidates = @((Join-Path $ninjaRoot 'ninja.exe'))
    if ($compiler)
    {
        $ninjaCandidates += (Join-Path (Split-Path -Parent $compiler) 'ninja.exe')
    }
    $ninja = Get-DesktopTool 'ninja.exe' $ninjaCandidates
    $node = Get-DesktopTool 'node.exe' @((Join-Path $env:ProgramFiles 'nodejs\node.exe'), (Join-Path $nodeRoot 'node-v24.22.2-win-x64\node.exe'))
    $missing = @()
    if (!$compiler)
    {
        $missing += 'w64devkit 2.8.0 (GCC, linker and Windows resource compiler)'
    }
    if (!$cmake)
    {
        $missing += 'CMake 4.3.1'
    }
    if (!$ninja)
    {
        $missing += 'Ninja 1.12.1'
    }
    if ($WithTests -and !$node)
    {
        $missing += 'Node.js 24.22.2 LTS (local protocol checks only)'
    }
    if ($ProbeOnly)
    {
        [pscustomobject]@{ compiler = $compiler; cmake = $cmake; ninja = $ninja; node = $node; missing = $missing } | ConvertTo-Json
        exit 0
    }
    if (!$EnvironmentFile)
    {
        throw 'The build script must supply an environment output path.'
    }
    if ($missing.Count)
    {
        Write-Host 'Missing build tools:'
        foreach ($tool in $missing)
        {
            Write-Host "  $tool"
        }
        Write-Host "Portable tools will be kept in $toolRoot. No administrator installation or permanent PATH change is needed."
        $approved = $Mode -eq 'download'
        if ($Mode -eq 'ask' -and ![Console]::IsInputRedirected)
        {
            $approved = (Read-Host 'Download the missing tools now? [y/N]') -match '^(y|yes)$'
        }
        if (!$approved)
        {
            throw 'Install the missing tools yourself, or run build.bat --download-tools. No download was started.'
        }
        if ($env:PROCESSOR_ARCHITECTURE -ne 'AMD64' -and $env:PROCESSOR_ARCHITEW6432 -ne 'AMD64')
        {
            throw 'Automatic downloads target Windows x64. On another architecture, supply a compatible compiler, CMake and Ninja on PATH.'
        }
        New-Item -ItemType Directory -Path $downloads -Force | Out-Null
        if (!$compiler)
        {
            $archive = Join-Path $downloads 'w64devkit-x64-2.8.0.7z.exe'
            Get-DesktopDownload 'w64devkit' 'https://github.com/skeeto/w64devkit/releases/download/v2.8.0/w64devkit-x64-2.8.0.7z.exe' $archive '6252bf34fe2231a55ac7f03d482b36d2c7c58697990551bba508102cfb3f342e'
            Expand-DesktopTool $archive $compilerRoot -SelfExtracting
            $compiler = Join-Path $compilerRoot 'w64devkit\bin\gcc.exe'
        }
        if (!$cmake)
        {
            $archive = Join-Path $downloads 'cmake-4.3.1-windows-x86_64.zip'
            Get-DesktopDownload 'CMake' 'https://github.com/Kitware/CMake/releases/download/v4.3.1/cmake-4.3.1-windows-x86_64.zip' $archive '03a610be931546474e46a442d171ef10532f4fc847de9cf76b7a8065bbef6a23'
            Expand-DesktopTool $archive $cmakeRoot
            $cmake = Join-Path $cmakeRoot 'cmake-4.3.1-windows-x86_64\bin\cmake.exe'
        }
        if (!$ninja)
        {
            $archive = Join-Path $downloads 'ninja-1.12.1-win.zip'
            Get-DesktopDownload 'Ninja' 'https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip' $archive 'f550fec705b6d6ff58f2db3c374c2277a37691678d6aba463adcbb129108467a'
            Expand-DesktopTool $archive $ninjaRoot
            $ninja = Join-Path $ninjaRoot 'ninja.exe'
        }
        if ($WithTests -and !$node)
        {
            $archive = Join-Path $downloads 'node-v24.22.2-win-x64.zip'
            Get-DesktopDownload 'Node.js' 'https://nodejs.org/dist/v24.22.2/node-v24.22.2-win-x64.zip' $archive 'ca1bff5d8ead13c9f2ea404a176db46bd430704d5b5113734049fa365e7799e2'
            Expand-DesktopTool $archive $nodeRoot
            $node = Join-Path $nodeRoot 'node-v24.22.2-win-x64\node.exe'
        }
    }
    $tools = @($compiler, $cmake, $ninja)
    if ($WithTests)
    {
        $tools += $node
    }
    $paths = @()
    foreach ($tool in $tools)
    {
        if (!(Test-Path -LiteralPath $tool -PathType Leaf))
        {
            throw "Tool was not found after preparation: $tool"
        }
        $directory = Split-Path -Parent $tool
        if ($directory -match '["\r\n]')
        {
            throw 'Tool paths cannot contain quotes or newlines.'
        }
        if ($paths -notcontains $directory)
        {
            $paths += $directory
        }
    }
    [IO.File]::WriteAllText($EnvironmentFile, ($paths -join ';'), [Console]::OutputEncoding)
    Write-Host 'Build tools are ready.'
    exit 0
}
catch
{
    Write-Error $_ -ErrorAction Continue
    exit 1
}
