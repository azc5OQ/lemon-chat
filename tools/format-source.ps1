param
(
    [Parameter(Mandatory = $true)]
    [string]$SourceDirectory,
    [switch]$Check
)

$ErrorActionPreference = 'Stop'

try
{
    $formatter = Join-Path $PSScriptRoot 'clang-format.exe'
    if (-not (Test-Path -LiteralPath $formatter -PathType Leaf))
    {
        throw "Missing formatter: $formatter"
    }

    $sourceRoot = (Resolve-Path -LiteralPath $SourceDirectory).Path
    if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot '.clang-format') -PathType Leaf))
    {
        throw "Missing .clang-format in $sourceRoot"
    }

    $directories = New-Object 'System.Collections.Generic.Stack[string]'
    $files = New-Object 'System.Collections.Generic.List[string]'
    $directories.Push($sourceRoot)
    while ($directories.Count -gt 0)
    {
        $directory = $directories.Pop()
        foreach ($entry in Get-ChildItem -LiteralPath $directory -Force)
        {
            if (($entry.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
            {
                continue
            }
            if ($entry.PSIsContainer)
            {
                if ($entry.Name -notin @('build', 'CMakeFiles', 'linkage-files', 'third-party', 'node_modules') -and
                    $entry.Name -notlike 'cmake-build-*' -and -not $entry.Name.StartsWith('.'))
                {
                    $directories.Push($entry.FullName)
                }
            }
            elseif ($entry.Extension -in @('.c', '.h', '.cpp', '.hpp'))
            {
                $files.Add($entry.FullName)
            }
        }
    }

    $files.Sort()
    $arguments = @('--style=file', '--fallback-style=none', '--Werror')
    if ($Check)
    {
        $arguments += '--dry-run'
    }
    else
    {
        $arguments += '-i'
    }

    $processed = 0
    foreach ($file in $files)
    {
        $processed++
        Write-Host "$processed / $($files.Count): $file"
        & $formatter @arguments -- $file
        if ($LASTEXITCODE -ne 0)
        {
            throw "clang-format failed for $file (exit $LASTEXITCODE)."
        }
    }
    Write-Host "Completed $processed files."
    exit 0
}
catch
{
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
