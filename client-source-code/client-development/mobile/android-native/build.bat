@echo off
setlocal
set "LEMON_BUILD_SCRIPT=%~f0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "$source = [IO.File]::ReadAllText($env:LEMON_BUILD_SCRIPT); $marker = '# POWERSHELL' + '_START'; & ([scriptblock]::Create($source.Substring($source.IndexOf($marker) + $marker.Length)))"
exit /b %errorlevel%
# POWERSHELL_START

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$project = Split-Path -Parent $env:LEMON_BUILD_SCRIPT
$interactive = $true
$checkOnly = $false
$abi = ''
$variant = 'debug'
$toolHome = $env:LEMON_BUILD_HOME
if ([string]::IsNullOrWhiteSpace($toolHome))
{
    $toolHome = Join-Path $env:LOCALAPPDATA 'LemonChat\build-tools'
}
$toolHome = [IO.Path]::GetFullPath($toolHome)
$temporaryDirectories = [Collections.Generic.List[string]]::new()
$requiredPackages = @('platforms;android-36', 'build-tools;35.0.0', 'ndk;27.2.12479018', 'cmake;3.22.1')

function Read-Menu([string] $title, [string[]] $options, [int] $defaultChoice)
{
    Write-Host "`n$title"
    for ($index = 0; $index -lt $options.Count; $index++)
    {
        Write-Host ('  {0}. {1}' -f ($index + 1), $options[$index])
    }
    while ($true)
    {
        $answer = Read-Host "Choose a number [$defaultChoice]"
        if (!$answer)
        {
            return $defaultChoice
        }
        $choice = 0
        if ([int]::TryParse($answer, [ref]$choice) -and $choice -ge 1 -and $choice -le $options.Count)
        {
            return $choice
        }
        Write-Host "Enter a number from 1 to $($options.Count)."
    }
}

function Confirm-Action([string] $message)
{
    return (Read-Menu $message @('Yes', 'No') 2) -eq 1
}

function Read-Property([string] $file, [string] $name)
{
    if (!(Test-Path -LiteralPath $file -PathType Leaf))
    {
        return ''
    }
    $logical = ''
    $result = ''
    foreach ($line in [IO.File]::ReadAllLines($file, [Text.Encoding]::GetEncoding('iso-8859-1')))
    {
        if ($logical)
        {
            $logical += $line.TrimStart()
        }
        else
        {
            $logical = $line
        }
        $slashes = $logical.Length - $logical.TrimEnd('\').Length
        if ($slashes % 2 -eq 1)
        {
            $logical = $logical.Substring(0, $logical.Length - 1)
            continue
        }
        if ($logical -cmatch ('^\s*' + [regex]::Escape($name) + '\s*(?:[=:]\s*|\s+)(.*)$'))
        {
            $result = [regex]::Replace($Matches[1], '\\(u[0-9a-fA-F]{4}|.)',
            {
                param($match)
                $value = $match.Groups[1].Value
                if ($value.StartsWith('u') -and $value.Length -eq 5)
                {
                    return [string][char][Convert]::ToInt32($value.Substring(1), 16)
                }
                return $value
            })
        }
        $logical = ''
    }
    return $result
}

function Test-Jdk([string] $directory)
{
    if ([string]::IsNullOrWhiteSpace($directory))
    {
        return $false
    }
    $java = Join-Path $directory 'bin\java.exe'
    $javac = Join-Path $directory 'bin\javac.exe'
    if (!(Test-Path -LiteralPath $java) -or !(Test-Path -LiteralPath $javac))
    {
        return $false
    }
    $version = Read-Property (Join-Path $directory 'release') 'JAVA_VERSION'
    if ($version -notmatch '^"?(17|21)[.\-"]')
    {
        return $false
    }
    $architecture = Read-Property (Join-Path $directory 'release') 'OS_ARCH'
    if ($architecture.Trim('"') -notin @('amd64', 'x86_64'))
    {
        return $false
    }
    try
    {
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $null = & $javac -version 2>&1
        return $LASTEXITCODE -eq 0
    }
    catch
    {
        return $false
    }
    finally
    {
        $ErrorActionPreference = $previous
    }
}

function Find-Jdk
{
    $candidates = [Collections.Generic.List[string]]::new()
    if ($env:JAVA_HOME)
    {
        $candidates.Add($env:JAVA_HOME.Trim('"'))
    }
    $candidates.Add((Join-Path $toolHome 'jdk-21'))
    $compiler = Get-Command javac.exe -ErrorAction SilentlyContinue
    if ($compiler)
    {
        $candidates.Add((Split-Path -Parent (Split-Path -Parent $compiler.Source)))
    }
    foreach ($directory in @('Eclipse Adoptium', 'Java', 'Microsoft'))
    {
        $parent = Join-Path $env:ProgramFiles $directory
        if (Test-Path -LiteralPath $parent)
        {
            foreach ($item in Get-ChildItem -LiteralPath $parent -Directory)
            {
                $candidates.Add($item.FullName)
            }
        }
    }
    $candidates.Add((Join-Path $env:ProgramFiles 'Android\Android Studio\jbr'))
    foreach ($candidate in $candidates)
    {
        if (Test-Jdk $candidate)
        {
            return [IO.Path]::GetFullPath($candidate)
        }
    }
    return ''
}

function New-DownloadDirectory
{
    $directory = Join-Path $toolHome ('downloads\' + [Guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $directory -Force
    $temporaryDirectories.Add([IO.Path]::GetFullPath($directory))
    return $directory
}

function Get-VerifiedDownload([string] $url, [string] $destination, [string] $algorithm, [string] $checksum)
{
    if (![Uri]::IsWellFormedUriString($url, [UriKind]::Absolute) -or !($url.StartsWith('https://')))
    {
        throw 'The tool download URL is not HTTPS.'
    }
    if (($algorithm -eq 'SHA256' -and $checksum -notmatch '^[a-fA-F0-9]{64}$') -or ($algorithm -eq 'SHA1' -and $checksum -notmatch '^[a-fA-F0-9]{40}$'))
    {
        throw 'The download metadata has no valid checksum.'
    }
    Write-Host "Downloading $url"
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $destination -TimeoutSec 600
    if ((Get-FileHash -LiteralPath $destination -Algorithm $algorithm).Hash -ne $checksum)
    {
        throw 'Download checksum mismatch. Nothing from this archive was installed.'
    }
}

function Move-DownloadedDirectory([string] $source, [string] $destination, [string] $unpacked, [string] $installRoot)
{
    $source = [IO.Path]::GetFullPath($source)
    $destination = [IO.Path]::GetFullPath($destination)
    $sourceRoot = [IO.Path]::GetFullPath($unpacked) + [IO.Path]::DirectorySeparatorChar
    $targetRoot = [IO.Path]::GetFullPath($installRoot).TrimEnd('\') + [IO.Path]::DirectorySeparatorChar
    if (!$source.StartsWith($sourceRoot, [StringComparison]::OrdinalIgnoreCase) -or !$destination.StartsWith($targetRoot, [StringComparison]::OrdinalIgnoreCase))
    {
        throw 'The tool installation path is outside its selected directory.'
    }
    Move-Item -LiteralPath $source -Destination $destination
}

function Install-Jdk
{
    Write-Host 'Building requires JDK 17 or 21. A JRE alone cannot compile the app.'
    if (!(Confirm-Action "Download and install Eclipse Temurin JDK 21 in ${toolHome}?"))
    {
        throw 'JDK setup declined. Set JAVA_HOME to JDK 17 or 21 and run this script again.'
    }
    $assets = @(Invoke-RestMethod -Uri 'https://api.adoptium.net/v3/assets/latest/21/hotspot?architecture=x64&image_type=jdk&os=windows&vendor=eclipse' -TimeoutSec 60)
    if ($assets.Count -eq 0)
    {
        throw 'Adoptium returned no Windows x64 JDK 21 package.'
    }
    $package = $assets[0].binary.package
    if ($package.link -notmatch '^https://github\.com/adoptium/temurin21-binaries/releases/download/')
    {
        throw 'Unexpected JDK download host. Check the Adoptium release metadata.'
    }
    $download = New-DownloadDirectory
    $archive = Join-Path $download 'jdk.zip'
    Get-VerifiedDownload $package.link $archive 'SHA256' $package.checksum
    $unpacked = Join-Path $download 'unpacked'
    Expand-Archive -LiteralPath $archive -DestinationPath $unpacked
    $jdks = @(foreach ($item in Get-ChildItem -LiteralPath $unpacked -Directory)
    {
        if (Test-Jdk $item.FullName)
        {
            $item
        }
    })
    if ($jdks.Count -ne 1)
    {
        throw 'The downloaded JDK did not contain one working Java compiler.'
    }
    $destination = Join-Path $toolHome 'jdk-21'
    if (Test-Path -LiteralPath $destination)
    {
        throw "An unusable JDK already exists at $destination. Move it aside and rerun setup."
    }
    Move-DownloadedDirectory $jdks[0].FullName $destination $unpacked $toolHome
    return $destination
}

function Find-Sdk
{
    $local = Read-Property (Join-Path $project 'local.properties') 'sdk.dir'
    if ($local)
    {
        if (![IO.Path]::IsPathRooted($local))
        {
            $local = Join-Path $project $local
        }
        return [IO.Path]::GetFullPath($local)
    }
    foreach ($candidate in @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT, (Join-Path $env:LOCALAPPDATA 'Android\Sdk'), (Join-Path $toolHome 'android-sdk')))
    {
        if ($candidate)
        {
            if ((Test-Path -LiteralPath $candidate) -or $candidate -eq $env:ANDROID_HOME -or $candidate -eq $env:ANDROID_SDK_ROOT)
            {
                return [IO.Path]::GetFullPath($candidate)
            }
        }
    }
    return Join-Path $toolHome 'android-sdk'
}

function Find-SdkManager([string] $sdk)
{
    foreach ($version in @('19.0', 'latest'))
    {
        $manager = Join-Path $sdk "cmdline-tools\$version\bin\sdkmanager.bat"
        if (Test-Path -LiteralPath $manager -PathType Leaf)
        {
            return $manager
        }
    }
    $tools = Join-Path $sdk 'cmdline-tools'
    if (Test-Path -LiteralPath $tools)
    {
        foreach ($item in Get-ChildItem -LiteralPath $tools -Directory | Sort-Object Name -Descending)
        {
            $manager = Join-Path $item.FullName 'bin\sdkmanager.bat'
            if (Test-Path -LiteralPath $manager -PathType Leaf)
            {
                return $manager
            }
        }
    }
    return ''
}

function Install-SdkManager([string] $sdk)
{
    Write-Host 'Android SDK license: https://developer.android.com/studio#terms-and-conditions'
    if (!(Confirm-Action "Have you read and accepted the SDK license and want to install command-line tools 19.0 in ${sdk}?"))
    {
        throw 'Android SDK setup declined.'
    }
    $download = New-DownloadDirectory
    $zip = Join-Path $download 'android-tools.zip'
    Get-VerifiedDownload 'https://dl.google.com/android/repository/commandlinetools-win-13114758_latest.zip' $zip 'SHA256' '98b565cb657b012dae6794cefc0f66ae1efb4690c699b78a614b4a6a3505b003'
    $unpacked = Join-Path $download 'unpacked'
    Expand-Archive -LiteralPath $zip -DestinationPath $unpacked
    $source = Join-Path $unpacked 'cmdline-tools'
    if (!(Test-Path -LiteralPath (Join-Path $source 'bin\sdkmanager.bat')))
    {
        throw 'The Android tools archive has no sdkmanager.'
    }
    $destination = Join-Path $sdk 'cmdline-tools\19.0'
    if (Test-Path -LiteralPath $destination)
    {
        throw "An incomplete command-line tool directory exists at $destination. Move it aside and rerun setup."
    }
    $null = New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force
    Move-DownloadedDirectory $source $destination $unpacked $sdk
    return Join-Path $destination 'bin\sdkmanager.bat'
}

function Get-MissingPackages([string] $sdk)
{
    $files = @(
        @('android.jar'),
        @('aapt2.exe', 'zipalign.exe', 'lib\apksigner.jar'),
        @('build\cmake\android.toolchain.cmake', 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe'),
        @('bin\cmake.exe', 'bin\ninja.exe')
    )
    for ($index = 0; $index -lt $requiredPackages.Count; $index++)
    {
        $directory = Join-Path $sdk $requiredPackages[$index].Replace(';', '\')
        $missing = !(Test-Path -LiteralPath (Join-Path $directory 'package.xml'))
        foreach ($file in $files[$index])
        {
            if (!(Test-Path -LiteralPath (Join-Path $directory $file)))
            {
                $missing = $true
            }
        }
        if ($missing)
        {
            $requiredPackages[$index]
        }
    }
}

function Invoke-Tool([string] $executable, [string[]] $arguments)
{
    & $executable @arguments
    if ($LASTEXITCODE -ne 0)
    {
        throw "$([IO.Path]::GetFileName($executable)) failed with exit code $LASTEXITCODE. See its output above."
    }
}

function Save-UserEnvironment([string] $jdk, [string] $sdk, [string] $manager)
{
    if (!(Confirm-Action 'Also save JAVA_HOME, ANDROID_HOME, ANDROID_SDK_ROOT and tool PATH entries for future terminals?'))
    {
        return
    }
    [Environment]::SetEnvironmentVariable('JAVA_HOME', $jdk, 'User')
    [Environment]::SetEnvironmentVariable('ANDROID_HOME', $sdk, 'User')
    [Environment]::SetEnvironmentVariable('ANDROID_SDK_ROOT', $sdk, 'User')
    $entries = [Collections.Generic.List[string]]::new()
    foreach ($entry in @((Join-Path $jdk 'bin'), (Split-Path -Parent $manager)))
    {
        $entries.Add($entry)
    }
    $savedPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    foreach ($entry in ($savedPath -split ';'))
    {
        if ($entry -and !$entries.Contains($entry))
        {
            $entries.Add($entry)
        }
    }
    [Environment]::SetEnvironmentVariable('Path', ($entries -join ';'), 'User')
    Write-Host 'Saved user environment. Open a new terminal to use it outside this build.'
}

try
{
    Write-Host "`nLemon Native APK builder"
    Write-Host 'Choose a numbered option. Android Studio is not needed.'
    $action = Read-Menu 'What would you like to build?' @('Testing APK (ready to install)', 'Release APK (unsigned, requires your signing key)', 'Check installed build tools only', 'Exit') 1
    if ($action -eq 4)
    {
        exit 0
    }
    if ($action -eq 3)
    {
        $checkOnly = $true
    }
    if ($action -eq 2)
    {
        $variant = 'release'
    }
    if (!$checkOnly)
    {
        Write-Host 'ARMv6 and 32-bit Intel are not supported by this project.'
        $choice = Read-Menu 'Which phone or emulator architecture?' @('All supported architectures (one universal APK)', 'ARMv8 / ARM64 (arm64-v8a)', 'ARMv7 (armeabi-v7a)', 'Intel / AMD 64-bit (x86_64)') 1
        $abi = @('all', 'arm64-v8a', 'armeabi-v7a', 'x86_64')[$choice - 1]
    }
    if (![Environment]::Is64BitOperatingSystem -or $env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'ARM64')
    {
        throw 'This toolchain requires 64-bit Intel/AMD Windows. This is the build computer, not the target phone.'
    }
    $repositoryRoot = [IO.Path]::GetFullPath((Join-Path $project '..\..\..\..'))
    foreach ($file in @('client-source-code\client-development\desktop\main\crypto\crypto_session.c', 'server-source-code\third-party\libopus-1.6.1\CMakeLists.txt', 'server-source-code\third-party\libdatachannel-0.24.2\CMakeLists.txt', 'client-source-code\client-development\browser\main\scripts\app\audio\sounds.js'))
    {
        if (!(Test-Path -LiteralPath (Join-Path $repositoryRoot $file)))
        {
            throw "Missing shared source: $file. Clone the complete lemon-chat repository."
        }
    }
    if (!(Test-Path -LiteralPath (Join-Path $project 'gradle\wrapper\gradle-wrapper.jar')))
    {
        throw 'The Gradle wrapper is missing. Restore the complete project from Git.'
    }
    Write-Host "Project: $project"
    Write-Host "Tool cache: $toolHome"
    $jdk = Find-Jdk
    $sdk = Find-Sdk
    $manager = Find-SdkManager $sdk
    $missing = @(Get-MissingPackages $sdk)
    $jdkLabel = $jdk
    if (!$jdkLabel)
    {
        $jdkLabel = 'JDK 17/21 not found'
    }
    $managerLabel = $manager
    if (!$managerLabel)
    {
        $managerLabel = 'not found'
    }
    Write-Host "JDK: $jdkLabel"
    Write-Host "Android SDK: $sdk"
    Write-Host "SDK manager: $managerLabel"
    foreach ($package in $missing)
    {
        Write-Host "Missing: $package"
    }
    if ($checkOnly)
    {
        if (!$jdk -or !$manager -or $missing.Count -gt 0)
        {
            throw 'Prerequisites are incomplete. Run this builder again and choose a build to set them up.'
        }
        if (!(Test-Path -LiteralPath (Join-Path $sdk 'licenses\android-sdk-license')))
        {
            throw 'No saved Android SDK license acceptance was found. Choose a build to review the licenses.'
        }
        Write-Host 'Prerequisite files are present. SDK license acceptance is checked by the build.'
        exit 0
    }
    if (!$jdk)
    {
        $jdk = Install-Jdk
    }
    $env:JAVA_HOME = $jdk
    $env:ANDROID_HOME = $sdk
    $env:ANDROID_SDK_ROOT = $sdk
    $env:Path = (Join-Path $jdk 'bin') + ';' + $env:Path
    if (!$manager)
    {
        $manager = Install-SdkManager $sdk
    }
    $env:Path = (Split-Path -Parent $manager) + ';' + $env:Path
    if ($missing.Count -gt 0)
    {
        Write-Host "`nThe Android SDK, NDK and Gradle caches can require several GB. The first build can take several minutes."
        if (!(Confirm-Action "Install missing SDK packages in ${sdk}?"))
        {
            throw 'SDK package installation declined.'
        }
    }
    if ($missing.Count -gt 0 -or !(Test-Path -LiteralPath (Join-Path $sdk 'licenses\android-sdk-license')))
    {
        Write-Host 'Review the SDK licenses below. Answer the license prompts yourself.'
        Invoke-Tool $manager @("--sdk_root=$sdk", '--licenses')
    }
    if ($missing.Count -gt 0)
    {
        Invoke-Tool $manager (@("--sdk_root=$sdk") + $missing)
        if (@(Get-MissingPackages $sdk).Count -gt 0)
        {
            throw 'Required SDK packages are still missing. Check license acceptance and available disk space.'
        }
    }
    if ($interactive)
    {
        Save-UserEnvironment $jdk $sdk $manager
    }
    if (!$env:GRADLE_USER_HOME)
    {
        $env:GRADLE_USER_HOME = Join-Path $toolHome 'gradle'
    }
    $env:CMAKE_BUILD_PARALLEL_LEVEL = '2'
    $sha = [Security.Cryptography.SHA256]::Create()
    try
    {
        $projectHash = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($project.ToLowerInvariant())))).Replace('-', '').Substring(0, 12)
    }
    finally
    {
        $sha.Dispose()
    }
    $buildRoot = Join-Path $toolHome "build\$projectHash\$abi"
    $abis = $abi
    if ($abi -eq 'all')
    {
        $abis = 'arm64-v8a,armeabi-v7a,x86_64'
    }
    $task = ':app:assembleDebug'
    $apkName = 'app-debug.apk'
    if ($variant -eq 'release')
    {
        $task = ':app:assembleRelease'
        $apkName = 'app-release-unsigned.apk'
        Write-Host 'Release output is unsigned. Sign it before installation or distribution.'
    }
    Write-Host "`nBuilding $variant APK for $abi"
    Write-Host "Build cache: $buildRoot"
    Push-Location -LiteralPath $project
    try
    {
        Invoke-Tool (Join-Path $project 'gradlew.bat') @($task, "-PnativeAbis=$abis", "-PnativeBuildRoot=$($buildRoot.Replace('\', '/'))", '-Pandroid.builder.sdkDownload=false', '--no-daemon', '--max-workers=2', '--console=plain')
    }
    finally
    {
        Pop-Location
    }
    $apk = Join-Path $buildRoot "app\outputs\apk\$variant\$apkName"
    if (!(Test-Path -LiteralPath $apk -PathType Leaf))
    {
        throw "Gradle completed but the expected APK is missing: $apk"
    }
    $output = Join-Path $project "build-output\$abi\$variant"
    $null = New-Item -ItemType Directory -Path $output -Force
    $destination = Join-Path $output "lemon-native-$abi-$variant.apk"
    Copy-Item -LiteralPath $apk -Destination $destination -Force
    Write-Host "`nAPK ready: $destination" -ForegroundColor Green
    Write-Host ('Size: {0:N1} MiB' -f ((Get-Item -LiteralPath $destination).Length / 1MB))
    if ($variant -eq 'debug')
    {
        Write-Host 'This APK is signed with your local debug key and can be installed for testing.'
    }
}
catch
{
    Write-Host "`nBuild stopped: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally
{
    $null = Read-Host 'Press Enter to close the builder'
    foreach ($directory in $temporaryDirectories)
    {
        $downloadRoot = [IO.Path]::GetFullPath((Join-Path $toolHome 'downloads')) + [IO.Path]::DirectorySeparatorChar
        $resolved = [IO.Path]::GetFullPath($directory)
        if ($resolved.StartsWith($downloadRoot, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $resolved))
        {
            Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
