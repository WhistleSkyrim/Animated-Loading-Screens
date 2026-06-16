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

Common things to customize:

| Goal | Setting |
| --- | --- |
| Disable the plugin without uninstalling it | `General.Enabled=false` |
| Use a different media folder under the plugin folder | `General.LoadingScreensFolder=MyFolder` |
| Pick files in order instead of randomly | `General.SelectionMode=sequential` |
| Keep one video looping for the whole loading screen | `Playback.PlaybackMode=repeat_single` |
| Move to another file when the current file ends | `Playback.PlaybackMode=next_after_end` |
| Crossfade between playlist entries | `Playback.PlaybackMode=crossfade_playlist` and `Transitions.EnableCrossfade=true` |
| Lower decoder/render workload | reduce `Playback.TargetFPS`, `Playback.MaxDecodeWidth`, `Playback.MaxDecodeHeight`, or `Performance.MaxDecoderThreads` |
| Show the vanilla 3D loading art behind the video | `Display.CoverVanillaLoadingScreen=false` |
| Hide the vanilla loading spinner | `Display.HideVanillaLoadingSpinner=true` |
| Make the overlay partially transparent | reduce `Display.Opacity` |

INI settings are grouped by section:

| Section | Settings |
| --- | --- |
| `[General]` | Enables/disables the plugin, chooses the media folder, scan behavior, file extensions, selection mode, VR compatibility flag, and log level. |
| `[Playback]` | Controls playback mode, speed, looping, preloading, frame queue size, decode resolution limits, target FPS, and close behavior. |
| `[Transitions]` | Controls fade-in, fade-out, crossfade, and close fade behavior. |
| `[Display]` | Controls fit mode, opacity, background color, vanilla screen coverage, vanilla spinner hiding, and debug overlay reservation. |
| `[Performance]` | Controls decoder thread priority, FFmpeg thread cap, scan/file-size limits, and reserved hardware decoding flag. |
| `[Compatibility]` | Reserved compatibility toggles and fail-safe fallback flag. |

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
