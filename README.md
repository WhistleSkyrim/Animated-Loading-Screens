# Animated Loading Screens

Animated Loading Screens is an SKSE plugin for Skyrim Special Edition, Anniversary Edition, and GOG AE. It draws video-backed loading screens over Skyrim's vanilla `LoadingMenu`, using media files from a mod-owned folder.

The plugin is built with CommonLibSSE NG, renders through D3D11, and decodes video through dynamically loaded FFmpeg DLLs. If FFmpeg is missing or media cannot be decoded, the plugin fails safely and leaves the vanilla loading screen available.

## Requirements

- Skyrim SE 1.5.97, AE 1.6.x Steam, or AE GOG 1.6.x.
- Matching SKSE/SKSE64 for the game runtime.
- Address Library for SKSE Plugins.
- Visual Studio 2022, CMake 3.26+, and vcpkg for building.
- FFmpeg runtime DLLs for video playback.

Skyrim VR is not supported. Microsoft Store/Game Pass and Epic Games Store builds are not supported unless SKSE officially supports them.

## Installation

Install the packaged output as a Skyrim mod with this layout:

```text
Data/
  SKSE/
    Plugins/
      AnimatedLoadingScreens.dll
      AnimatedLoadingScreens.ini
      AnimatedLoadingScreens/
        LoadingScreens/
          put_your_videos_here.txt
        FFmpeg/
          avcodec-*.dll
          avformat-*.dll
          avutil-*.dll
          swscale-*.dll
        Playlists/
          default.txt
```

Put your videos in:

```text
Data\SKSE\Plugins\AnimatedLoadingScreens\LoadingScreens
```

MP4, WebM, and MKV are recommended. Actual codec support depends on the FFmpeg build you ship.

## Build

Set `VCPKG_ROOT` to your vcpkg checkout, then configure and build the release preset:

```powershell
cmake --preset release-x64
cmake --build --preset release-x64
```

To run the unit tests:

```powershell
cmake --preset tests-release-x64
cmake --build --preset tests-release-x64
ctest --preset tests-release-x64
```

To compile and package the mod into the repository-level `Output` folder, use the helper script:

```powershell
.\Build-Output.ps1 -NoPause
```

Install the contents of `Output` with MO2/Vortex, or copy it into Skyrim's `Data` folder. For a direct Skyrim-root layout, run:

```powershell
.\Build-Output.ps1 -Layout Game -NoPause
```

## Configuration

The main config file is:

```text
Data\SKSE\Plugins\AnimatedLoadingScreens.ini
```

If the file is missing, the plugin creates it with safe defaults. A small display-focused example:

```ini
[Display]
FitMode=cover
Opacity=1.0
BackgroundColor=#000000
CoverVanillaLoadingScreen=true
HideVanillaLoadingSpinner=false
```

`HideVanillaLoadingSpinner=true` hides Skyrim's bottom-right `LoadWaitSpinner` while the animated overlay is active. It is `false` by default so vanilla behavior is preserved unless you opt in.

INI settings are grouped by section:

- `[General]`: `Enabled`, `LoadingScreensFolder`, `ScanSubfolders`, `AllowedExtensions`, `SelectionMode`, `RememberLast`, `EnableInVR`, `LogLevel`.
- `[Playback]`: `PlaybackMode`, `PlaybackSpeed`, `Mute`, `LoopVideo`, `PreloadNext`, `FrameQueueSize`, `MaxDecodeWidth`, `MaxDecodeHeight`, `TargetFPS`, `PauseWhenMenuClosed`.
- `[Transitions]`: `FadeInMs`, `FadeOutMs`, `EnableCrossfade`, `CrossfadeMs`, `FadeBetweenDifferentFiles`, `FadeToBlackOnMenuClose`.
- `[Display]`: `FitMode`, `Opacity`, `BackgroundColor`, `CoverVanillaLoadingScreen`, `HideVanillaLoadingSpinner`, `ShowDebugOverlay`.
- `[Performance]`: `DecoderThreadPriority`, `MaxDecoderThreads`, `MaxFilesToScan`, `SkipFilesLargerThanMB`, `UseHardwareDecoding`.
- `[Compatibility]`: `DisableWhenENBMenuOpen`, `DisableWhenConsoleOpen`, `FailSafeVanillaFallback`.

The default playlist is:

```text
Data\SKSE\Plugins\AnimatedLoadingScreens\Playlists\default.txt
```

External plugins can toggle the vanilla loading spinner at runtime through the exported DLL functions:

```cpp
AnimatedLoadingScreens_SetVanillaLoadingSpinnerHidden(true);
AnimatedLoadingScreens_SetVanillaLoadingSpinnerHidden(false);
```

This runtime toggle affects the current session only; the INI value is still the startup default.

See [docs/BUILDING.md](docs/BUILDING.md) and [docs/CONFIGURATION.md](docs/CONFIGURATION.md) for more details.
