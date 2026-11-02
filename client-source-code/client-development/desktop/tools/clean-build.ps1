param
(
    [Parameter(Mandatory = $true)][string]$SourceDirectory,
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [ValidateSet('ask', 'clean', 'keep')][string]$Mode = 'ask'
)

$ErrorActionPreference = 'Stop'

try
{
    if ($Mode -eq 'keep' -or !(Test-Path -LiteralPath $BuildDirectory))
    {
        exit 0
    }

    $sourcePath = (Resolve-Path -LiteralPath $SourceDirectory).Path.TrimEnd('\', '/')
    $buildPath = (Resolve-Path -LiteralPath $BuildDirectory).Path.TrimEnd('\', '/')
    $buildName = Split-Path -Leaf $buildPath
    $buildParent = Split-Path -Parent $buildPath
    if ($buildParent -ne $sourcePath -or $buildName -notmatch '^build($|[-_])')
    {
        throw 'Automatic cleanup is limited to build, build-* or build_* directories directly inside this desktop project. Use --keep-build for another output directory.'
    }
    if ((Get-Item -LiteralPath $buildPath).Attributes -band [IO.FileAttributes]::ReparsePoint)
    {
        throw 'Refusing to clean a build directory that is a link or junction.'
    }
    $cachePath = Join-Path $buildPath 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cachePath)
    {
        $ownerPrefix = 'CMAKE_HOME_DIRECTORY:INTERNAL='
        $owner = @(Select-String -LiteralPath $cachePath -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=')
        if ($owner.Count -ne 1 -or [IO.Path]::GetFullPath($owner[0].Line.Substring($ownerPrefix.Length)).TrimEnd('\', '/') -ne $sourcePath)
        {
            throw 'The CMake cache belongs to another source directory. Cleanup was refused.'
        }
    }
    if ($Mode -eq 'ask')
    {
        if ([Console]::IsInputRedirected)
        {
            Write-Output 'Noninteractive build: keeping existing files. Use --clean or --keep-build to choose explicitly.'
            exit 0
        }
        do
        {
            $answer = (Read-Host 'Clean all build files before starting? [Y/n]').Trim()
        }
        while ($answer -notmatch '^(|y|yes|n|no)$')
        if ($answer -match '^(n|no)$')
        {
            exit 0
        }
    }
    # Never traverse a linked subtree during recursive cleanup.
    $links = Get-ChildItem -LiteralPath $buildPath -Force -Recurse -Attributes ReparsePoint
    if ($links)
    {
        throw 'The build directory contains a link or junction. Cleanup was refused.'
    }
    Write-Output "Cleaning $buildPath"
    Remove-Item -LiteralPath $buildPath -Recurse -Force
    exit 0
}
catch
{
    Write-Error $_
    exit 1
}
