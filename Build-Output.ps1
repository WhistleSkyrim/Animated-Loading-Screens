#Requires -Version 5.1

[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string] $Configuration = "Release",

    [string] $BuildDir = "",

    [string] $OutputDir = "",

    [ValidateSet("Mod", "Game")]
    [string] $Layout = "Mod",

    [string] $FFmpegRoot = $env:FFMPEG_ROOT,

    [ValidateSet("PluginDirectory", "ReadmeDirectory", "Both")]
    [string] $FFmpegPlacement = "ReadmeDirectory",

    [string] $ToolchainFile = $env:CMAKE_TOOLCHAIN_FILE,

    [string] $VcpkgRoot = $env:VCPKG_ROOT,

    [string] $VcpkgTriplet = "x64-windows-v143",

    [string] $VcpkgHostTriplet = "",

    [string] $VcpkgInstalledDir = "",

    [string] $Generator = "",

    [string] $Architecture = "x64",

    [string[]] $CMakeArgs = @(),

    [switch] $Clean,

    [switch] $DisableFFmpeg,

    [switch] $IncludeSymbols,

    [switch] $NoPause
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Wait-BeforeExit {
    if ($NoPause) {
        return
    }

    Write-Host ""
    Read-Host "Press Enter to close this window"
}

trap {
    Write-Host ""
    Write-Host "Build-Output.ps1 failed:" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    if ($_.ScriptStackTrace) {
        Write-Host ""
        Write-Host $_.ScriptStackTrace
    }

    Wait-BeforeExit
    exit 1
}

$ScriptRoot = $PSScriptRoot
if (-not $ScriptRoot -and $PSCommandPath) {
    $ScriptRoot = Split-Path -Parent $PSCommandPath
}
if (-not $ScriptRoot -and $MyInvocation.MyCommand.Path) {
    $ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
}
if (-not $ScriptRoot) {
    $ScriptRoot = (Get-Location).Path
}

$ProjectRoot = (Resolve-Path -LiteralPath $ScriptRoot).Path
if (-not $BuildDir) {
    $BuildDir = Join-Path $ProjectRoot "build-plugin"
}
if (-not $OutputDir) {
    $OutputDir = Join-Path $ProjectRoot "Output"
}

$StageDir = Join-Path $BuildDir "_install"

function ConvertTo-FullPath {
    param([Parameter(Mandatory = $true)][string] $Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot $Path))
}

function Test-IsPathInside {
    param(
        [Parameter(Mandatory = $true)][string] $Path,
        [Parameter(Mandatory = $true)][string] $Parent
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $fullParent = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
    return $fullPath.Equals($fullParent, [System.StringComparison]::OrdinalIgnoreCase) -or
        $fullPath.StartsWith($fullParent + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)
}

function Remove-DirectoryInProject {
    param([Parameter(Mandatory = $true)][string] $Path)

    $fullPath = ConvertTo-FullPath $Path
    if ($fullPath.TrimEnd('\', '/').Equals($ProjectRoot.TrimEnd('\', '/'), [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove the project root '$ProjectRoot'."
    }

    if (-not (Test-IsPathInside -Path $fullPath -Parent $ProjectRoot)) {
        throw "Refusing to remove '$fullPath' because it is outside the project root '$ProjectRoot'."
    }

    if (Test-Path -LiteralPath $fullPath) {
        Remove-Item -LiteralPath $fullPath -Recurse -Force
    }
}

function New-EmptyDirectory {
    param([Parameter(Mandatory = $true)][string] $Path)

    Remove-DirectoryInProject $Path
    New-Item -ItemType Directory -Path (ConvertTo-FullPath $Path) -Force | Out-Null
}

function Invoke-CheckedCommand {
    param(
        [Parameter(Mandatory = $true)][string] $FilePath,
        [Parameter(Mandatory = $true)][string[]] $Arguments
    )

    Write-Host "> $FilePath $($Arguments -join ' ')"
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "'$FilePath' failed with exit code $LASTEXITCODE."
    }
}

function ConvertTo-CMakeBool {
    param([Parameter(Mandatory = $true)][bool] $Value)

    if ($Value) {
        return "ON"
    }

    return "OFF"
}

function Get-CMakeCacheValue {
    param(
        [Parameter(Mandatory = $true)][string] $CachePath,
        [Parameter(Mandatory = $true)][string] $Name
    )

    if (-not (Test-Path -LiteralPath $CachePath)) {
        return $null
    }

    $pattern = "^$([regex]::Escape($Name))(?::[^=]*)?=(.*)$"
    $match = Select-String -Path $CachePath -Pattern $pattern | Select-Object -First 1
    if (-not $match) {
        return $null
    }

    return $match.Matches[0].Groups[1].Value
}

function Test-CMakeDefinitionSpecified {
    param(
        [string[]] $Arguments = @(),
        [Parameter(Mandatory = $true)][string] $Name
    )

    if (-not $Arguments) {
        return $false
    }

    $escapedName = [regex]::Escape($Name)
    foreach ($argument in $Arguments) {
        if ($argument -match "^-D$escapedName(:[^=]+)?(=.*)?$") {
            return $true
        }
    }

    return $false
}

function ConvertTo-ComparablePath {
    param([string] $Path)

    if (-not $Path) {
        return ""
    }

    try {
        return [System.IO.Path]::GetFullPath($Path).TrimEnd([char[]]@('\', '/')).ToLowerInvariant()
    } catch {
        return $Path.TrimEnd([char[]]@('\', '/')).ToLowerInvariant()
    }
}

function Test-IsVcpkgToolchain {
    param([string] $Path)

    if (-not $Path) {
        return $false
    }

    $fullPath = ConvertTo-FullPath $Path
    if ((Split-Path -Leaf $fullPath) -ine "vcpkg.cmake") {
        return $false
    }

    $buildsystemsDir = Split-Path -Parent $fullPath
    return (Split-Path -Leaf $buildsystemsDir) -ieq "buildsystems"
}

function Get-VcpkgRootFromToolchain {
    param([Parameter(Mandatory = $true)][string] $Path)

    $fullPath = ConvertTo-FullPath $Path
    $buildsystemsDir = Split-Path -Parent $fullPath
    $scriptsDir = Split-Path -Parent $buildsystemsDir
    return Split-Path -Parent $scriptsDir
}

function Find-VisualStudio2022Generator {
    $programFilesX86 = [Environment]::GetFolderPath("ProgramFilesX86")
    if ($programFilesX86) {
        $vswhere = Join-Path $programFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path -LiteralPath $vswhere) {
            $installPath = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -version "[17.0,18.0)" -property installationPath 2>$null
            if ($LASTEXITCODE -eq 0 -and $installPath) {
                return "Visual Studio 17 2022"
            }
        }
    }

    $programFiles = [Environment]::GetFolderPath("ProgramFiles")
    if ($programFiles) {
        $editionRoots = @("Community", "Professional", "Enterprise", "BuildTools") | ForEach-Object {
            Join-Path $programFiles "Microsoft Visual Studio\2022\$_"
        }
        foreach ($editionRoot in $editionRoots) {
            if (Test-Path -LiteralPath $editionRoot) {
                return "Visual Studio 17 2022"
            }
        }
    }

    return ""
}

function Copy-DirectoryContents {
    param(
        [Parameter(Mandatory = $true)][string] $Source,
        [Parameter(Mandatory = $true)][string] $Destination
    )

    if (-not (Test-Path -LiteralPath $Source)) {
        throw "Expected directory '$Source' was not created."
    }

    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Get-ChildItem -LiteralPath $Source -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $Destination -Recurse -Force
    }
}

function Save-ExistingLoadingScreens {
    param(
        [Parameter(Mandatory = $true)][string] $OutputDir,
        [Parameter(Mandatory = $true)][string] $BuildDir,
        [Parameter(Mandatory = $true)][string] $Layout
    )

    if ($Layout -eq "Mod") {
        $source = Join-Path $OutputDir "SKSE\Plugins\AnimatedLoadingScreens\LoadingScreens"
    } else {
        $source = Join-Path $OutputDir "Data\SKSE\Plugins\AnimatedLoadingScreens\LoadingScreens"
    }

    if (-not (Test-Path -LiteralPath $source)) {
        return ""
    }

    $preserveRoot = Join-Path $BuildDir "_preserved-loading-screens"
    Remove-DirectoryInProject $preserveRoot

    $sourceFull = [System.IO.Path]::GetFullPath($source).TrimEnd([char[]]@('\', '/'))
    $files = @(Get-ChildItem -LiteralPath $sourceFull -Recurse -File -Force |
        Where-Object { $_.Name -ine "put_your_videos_here.txt" })
    if ($files.Count -eq 0) {
        return ""
    }

    New-Item -ItemType Directory -Path $preserveRoot -Force | Out-Null
    foreach ($file in $files) {
        $relativePath = $file.FullName.Substring($sourceFull.Length).TrimStart([char[]]@('\', '/'))
        $destination = Join-Path $preserveRoot $relativePath
        $destinationParent = Split-Path -Parent $destination
        if ($destinationParent) {
            New-Item -ItemType Directory -Path $destinationParent -Force | Out-Null
        }
        Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
    }

    Write-Host "Preserved $($files.Count) existing loading screen file(s) from Output."
    return $preserveRoot
}

function Restore-ExistingLoadingScreens {
    param(
        [string] $PreservedRoot,
        [Parameter(Mandatory = $true)][string] $PluginsDir
    )

    if (-not $PreservedRoot -or -not (Test-Path -LiteralPath $PreservedRoot)) {
        return
    }

    $destination = Join-Path $PluginsDir "AnimatedLoadingScreens\LoadingScreens"
    Copy-DirectoryContents -Source $PreservedRoot -Destination $destination
}

function Find-FFmpegDlls {
    param([Parameter(Mandatory = $true)][string] $Root)

    $fullRoot = ConvertTo-FullPath $Root
    $candidateDirs = @(
        $fullRoot,
        (Join-Path $fullRoot "bin"),
        (Join-Path $fullRoot "bin\x64")
    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -Unique

    if (-not $candidateDirs) {
        throw "FFmpeg root '$fullRoot' does not exist or has no known runtime DLL directory."
    }

    $runtimeNamePattern = '^(avcodec|avformat|avutil|swscale|swresample|avdevice|avfilter|postproc)-.*\.dll$'
    $dlls = foreach ($dir in $candidateDirs) {
        Get-ChildItem -LiteralPath $dir -File -Filter "*.dll" |
            Where-Object { $_.Name -match $runtimeNamePattern }
    }

    $requiredPrefixes = @("avcodec", "avformat", "avutil", "swscale")
    foreach ($prefix in $requiredPrefixes) {
        if (-not ($dlls | Where-Object { $_.Name -like "$prefix-*.dll" })) {
            Write-Warning "No '$prefix-*.dll' was found under '$fullRoot'."
        }
    }

    return $dlls | Sort-Object FullName -Unique
}

function Find-VcpkgFFmpegRoot {
    param(
        [string] $InstalledDir,
        [string] $Triplet
    )

    if (-not $InstalledDir) {
        return ""
    }

    $fullInstalledDir = ConvertTo-FullPath $InstalledDir
    $candidateDirs = @()
    if ($Triplet) {
        $candidateDirs += Join-Path $fullInstalledDir "$Triplet\bin"
    }
    $candidateDirs += Join-Path $fullInstalledDir "bin"

    foreach ($candidateDir in ($candidateDirs | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $candidateDir)) {
            continue
        }

        $hasRequiredDlls = $true
        foreach ($prefix in @("avcodec", "avformat", "avutil", "swscale")) {
            if (-not (Get-ChildItem -LiteralPath $candidateDir -File -Filter "$prefix-*.dll" | Select-Object -First 1)) {
                $hasRequiredDlls = $false
                break
            }
        }

        if ($hasRequiredDlls) {
            return $candidateDir
        }
    }

    return ""
}

$BuildDir = ConvertTo-FullPath $BuildDir
$OutputDir = ConvertTo-FullPath $OutputDir
$StageDir = ConvertTo-FullPath $StageDir
$ffmpegEnabled = -not $DisableFFmpeg.IsPresent

if ($ToolchainFile) {
    $ToolchainFile = ConvertTo-FullPath $ToolchainFile
}

if (-not $VcpkgRoot -and $ToolchainFile -and (Test-IsVcpkgToolchain $ToolchainFile)) {
    $VcpkgRoot = Get-VcpkgRootFromToolchain $ToolchainFile
}

if (-not $ToolchainFile -and $VcpkgRoot) {
    $vcpkgToolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    if (Test-Path -LiteralPath $vcpkgToolchain) {
        $ToolchainFile = ConvertTo-FullPath $vcpkgToolchain
    }
}

$usesVcpkg = Test-IsVcpkgToolchain $ToolchainFile
$vcpkgOverlayTriplets = Join-Path $ProjectRoot "vcpkg-triplets"

if ($usesVcpkg) {
    if (-not $VcpkgHostTriplet) {
        $VcpkgHostTriplet = $VcpkgTriplet
    }

    if (-not $VcpkgInstalledDir -and $VcpkgRoot) {
        $featureName = "novideo"
        if ($ffmpegEnabled) {
            $featureName = "ffmpeg"
        }

        $tripletName = "default"
        if ($VcpkgTriplet) {
            $tripletName = $VcpkgTriplet
        }

        $installedTreeName = "build-output-$tripletName-$featureName"
        if ($VcpkgTriplet -eq "x64-windows-v143") {
            $installedTreeName = "$installedTreeName-static-md"
        }

        if ($VcpkgTriplet -eq "x64-windows-v143" -and $ffmpegEnabled) {
            $installedTreeName = "release-v143-x64-static-md"
        }

        $VcpkgInstalledDir = Join-Path $VcpkgRoot "installed-animated-loading-screens\$installedTreeName"
    }
}

if (-not $Generator) {
    $detectedGenerator = Find-VisualStudio2022Generator
    if ($detectedGenerator) {
        $Generator = $detectedGenerator
    }
}

$cachePath = Join-Path $BuildDir "CMakeCache.txt"

if ($Clean) {
    Remove-DirectoryInProject $BuildDir
} elseif (Test-Path -LiteralPath $cachePath) {
    $reconfigureReasons = @()

    if ($ToolchainFile) {
        $cachedToolchainFile = Get-CMakeCacheValue -CachePath $cachePath -Name "CMAKE_TOOLCHAIN_FILE"
        if ((ConvertTo-ComparablePath $cachedToolchainFile) -ne (ConvertTo-ComparablePath $ToolchainFile)) {
            $reconfigureReasons += "CMAKE_TOOLCHAIN_FILE changed or was not applied to the existing cache."
        }
    }

    $cachedGenerator = Get-CMakeCacheValue -CachePath $cachePath -Name "CMAKE_GENERATOR"
    if ($Generator -and $cachedGenerator -and $cachedGenerator -ne $Generator) {
        $reconfigureReasons += "CMake generator changed from '$cachedGenerator' to '$Generator'."
    }

    $usesPlatformGenerator = $false
    if ($Generator) {
        $usesPlatformGenerator = $Generator -like "Visual Studio*"
    } elseif ($cachedGenerator) {
        $usesPlatformGenerator = $cachedGenerator -like "Visual Studio*"
    }

    if ($Architecture -and $usesPlatformGenerator) {
        $cachedArchitecture = Get-CMakeCacheValue -CachePath $cachePath -Name "CMAKE_GENERATOR_PLATFORM"
        if ($cachedArchitecture -ne $Architecture) {
            $reconfigureReasons += "CMake generator platform changed from '$cachedArchitecture' to '$Architecture'."
        }
    }

    if ($usesVcpkg) {
        if ($VcpkgTriplet -and -not (Test-CMakeDefinitionSpecified -Arguments $CMakeArgs -Name "VCPKG_TARGET_TRIPLET")) {
            $cachedTriplet = Get-CMakeCacheValue -CachePath $cachePath -Name "VCPKG_TARGET_TRIPLET"
            if ($cachedTriplet -ne $VcpkgTriplet) {
                $reconfigureReasons += "VCPKG_TARGET_TRIPLET changed from '$cachedTriplet' to '$VcpkgTriplet'."
            }
        }

        if ($VcpkgHostTriplet -and -not (Test-CMakeDefinitionSpecified -Arguments $CMakeArgs -Name "VCPKG_HOST_TRIPLET")) {
            $cachedHostTriplet = Get-CMakeCacheValue -CachePath $cachePath -Name "VCPKG_HOST_TRIPLET"
            if ($cachedHostTriplet -ne $VcpkgHostTriplet) {
                $reconfigureReasons += "VCPKG_HOST_TRIPLET changed from '$cachedHostTriplet' to '$VcpkgHostTriplet'."
            }
        }

        if ($VcpkgInstalledDir -and -not (Test-CMakeDefinitionSpecified -Arguments $CMakeArgs -Name "VCPKG_INSTALLED_DIR")) {
            $cachedInstalledDir = Get-CMakeCacheValue -CachePath $cachePath -Name "VCPKG_INSTALLED_DIR"
            if ((ConvertTo-ComparablePath $cachedInstalledDir) -ne (ConvertTo-ComparablePath $VcpkgInstalledDir)) {
                $reconfigureReasons += "VCPKG_INSTALLED_DIR changed or was not applied to the existing cache."
            }
        }

        if ((Test-Path -LiteralPath $vcpkgOverlayTriplets) -and -not (Test-CMakeDefinitionSpecified -Arguments $CMakeArgs -Name "VCPKG_OVERLAY_TRIPLETS")) {
            $cachedOverlayTriplets = Get-CMakeCacheValue -CachePath $cachePath -Name "VCPKG_OVERLAY_TRIPLETS"
            if ((ConvertTo-ComparablePath $cachedOverlayTriplets) -ne (ConvertTo-ComparablePath $vcpkgOverlayTriplets)) {
                $reconfigureReasons += "VCPKG_OVERLAY_TRIPLETS changed or was not applied to the existing cache."
            }
        }

        if ($ffmpegEnabled -and -not (Test-CMakeDefinitionSpecified -Arguments $CMakeArgs -Name "VCPKG_MANIFEST_FEATURES")) {
            $cachedManifestFeatures = Get-CMakeCacheValue -CachePath $cachePath -Name "VCPKG_MANIFEST_FEATURES"
            if (-not $cachedManifestFeatures -or $cachedManifestFeatures -notmatch "(^|;)ffmpeg($|;)") {
                $reconfigureReasons += "VCPKG_MANIFEST_FEATURES does not include 'ffmpeg'."
            }
        }
    }

    if ($reconfigureReasons.Count -gt 0) {
        Write-Warning "Existing CMake cache in '$BuildDir' is incompatible with this build. Recreating it."
        foreach ($reason in $reconfigureReasons) {
            Write-Warning "  $reason"
        }
        Remove-DirectoryInProject $BuildDir
    }
}

New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
$preservedLoadingScreensDir = Save-ExistingLoadingScreens -OutputDir $OutputDir -BuildDir $BuildDir -Layout $Layout
New-EmptyDirectory $OutputDir
New-EmptyDirectory $StageDir

$configureArgs = @("-S", $ProjectRoot, "-B", $BuildDir)
$isFreshConfigure = -not (Test-Path -LiteralPath $cachePath)

if ($isFreshConfigure) {
    if ($Generator) {
        $configureArgs += @("-G", $Generator)
    }

    $usesPlatformGenerator = -not $Generator -or $Generator -like "Visual Studio*"
    if ($Architecture -and $usesPlatformGenerator) {
        $configureArgs += @("-A", $Architecture)
    }
}

$configureArgs += @(
    "-DALS_BUILD_PLUGIN=ON",
    "-DALS_BUILD_TESTS=OFF",
    "-DALS_ENABLE_FFMPEG=$(ConvertTo-CMakeBool $ffmpegEnabled)"
)

if ($ffmpegEnabled -and $FFmpegRoot) {
    $configureArgs += "-DFFMPEG_ROOT=$(ConvertTo-FullPath $FFmpegRoot)"
}

if ($ToolchainFile -and $isFreshConfigure) {
    $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$(ConvertTo-FullPath $ToolchainFile)"
}

if ($usesVcpkg) {
    if ((Test-Path -LiteralPath $vcpkgOverlayTriplets) -and -not (Test-CMakeDefinitionSpecified -Arguments $CMakeArgs -Name "VCPKG_OVERLAY_TRIPLETS")) {
        $configureArgs += "-DVCPKG_OVERLAY_TRIPLETS=$vcpkgOverlayTriplets"
    }

    if ($VcpkgTriplet -and -not (Test-CMakeDefinitionSpecified -Arguments $CMakeArgs -Name "VCPKG_TARGET_TRIPLET")) {
        $configureArgs += "-DVCPKG_TARGET_TRIPLET=$VcpkgTriplet"
    }

    if ($VcpkgHostTriplet -and -not (Test-CMakeDefinitionSpecified -Arguments $CMakeArgs -Name "VCPKG_HOST_TRIPLET")) {
        $configureArgs += "-DVCPKG_HOST_TRIPLET=$VcpkgHostTriplet"
    }

    if ($VcpkgInstalledDir -and -not (Test-CMakeDefinitionSpecified -Arguments $CMakeArgs -Name "VCPKG_INSTALLED_DIR")) {
        $configureArgs += "-DVCPKG_INSTALLED_DIR=$(ConvertTo-FullPath $VcpkgInstalledDir)"
    }

    if ($ffmpegEnabled -and -not (Test-CMakeDefinitionSpecified -Arguments $CMakeArgs -Name "VCPKG_MANIFEST_FEATURES")) {
        $configureArgs += "-DVCPKG_MANIFEST_FEATURES=ffmpeg"
    }
}

$configureArgs += $CMakeArgs

Invoke-CheckedCommand "cmake" $configureArgs
Invoke-CheckedCommand "cmake" @("--build", $BuildDir, "--config", $Configuration, "--target", "AnimatedLoadingScreens")
Invoke-CheckedCommand "cmake" @("--install", $BuildDir, "--config", $Configuration, "--prefix", $StageDir)

if ($Layout -eq "Mod") {
    Copy-DirectoryContents -Source (Join-Path $StageDir "Data") -Destination $OutputDir
    $pluginsDir = Join-Path $OutputDir "SKSE\Plugins"
} else {
    Copy-DirectoryContents -Source $StageDir -Destination $OutputDir
    $pluginsDir = Join-Path $OutputDir "Data\SKSE\Plugins"
}

Restore-ExistingLoadingScreens -PreservedRoot $preservedLoadingScreensDir -PluginsDir $pluginsDir

$pluginDll = Join-Path $pluginsDir "AnimatedLoadingScreens.dll"
if (-not (Test-Path -LiteralPath $pluginDll)) {
    throw "The packaged plugin DLL was not found at '$pluginDll'."
}

$readmeFFmpegDir = Join-Path $pluginsDir "AnimatedLoadingScreens\FFmpeg"
New-Item -ItemType Directory -Path $readmeFFmpegDir -Force | Out-Null

if ($ffmpegEnabled) {
    $effectiveFFmpegRoot = $FFmpegRoot
    if (-not $effectiveFFmpegRoot -and $usesVcpkg) {
        $effectiveFFmpegRoot = Find-VcpkgFFmpegRoot -InstalledDir $VcpkgInstalledDir -Triplet $VcpkgTriplet
    }

    if ($effectiveFFmpegRoot) {
        $ffmpegDlls = @(Find-FFmpegDlls $effectiveFFmpegRoot)
        if ($ffmpegDlls.Count -gt 0) {
            if ($FFmpegPlacement -eq "PluginDirectory" -or $FFmpegPlacement -eq "Both") {
                foreach ($dll in $ffmpegDlls) {
                    Copy-Item -LiteralPath $dll.FullName -Destination $pluginsDir -Force
                }
            }

            if ($FFmpegPlacement -eq "ReadmeDirectory" -or $FFmpegPlacement -eq "Both") {
                foreach ($dll in $ffmpegDlls) {
                    Copy-Item -LiteralPath $dll.FullName -Destination $readmeFFmpegDir -Force
                }
            }
        }
    } else {
        Write-Warning "FFmpeg is enabled, but no runtime FFmpeg DLL directory was found. Set FFMPEG_ROOT or install the vcpkg ffmpeg feature."
    }
}

if ($IncludeSymbols) {
    $symbol = Get-ChildItem -LiteralPath $BuildDir -Recurse -File -Filter "AnimatedLoadingScreens.pdb" |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1

    if ($symbol) {
        Copy-Item -LiteralPath $symbol.FullName -Destination $pluginsDir -Force
    } else {
        Write-Warning "IncludeSymbols was requested, but AnimatedLoadingScreens.pdb was not found."
    }
}

Write-Host ""
Write-Host "Packaged Animated Loading Screens:"
Write-Host "  Output:  $OutputDir"
Write-Host "  Layout:  $Layout"
Write-Host "  Plugin:  $pluginDll"
Write-Host ""
Write-Host "For MO2/Vortex, install the contents of Output as a mod."
Write-Host "For manual install, copy Output into Skyrim's Data folder, or rerun with -Layout Game and copy Output into the Skyrim root."

Wait-BeforeExit
