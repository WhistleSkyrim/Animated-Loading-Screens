# Configuration

The plugin reads:

```text
Data\SKSE\Plugins\AnimatedLoadingScreens.ini
```

If the file is missing, a commented default INI is created.

## General

- `Enabled`: enables or disables the plugin.
- `LoadingScreensFolder`: folder scanned for media. Relative paths are resolved under `Data\SKSE\Plugins\AnimatedLoadingScreens`; absolute paths and `..` escapes are rejected.
- `ScanSubfolders`: includes nested folders.
- `AllowedExtensions`: comma-separated extension filter used during scanning.
- `SelectionMode`: `random`, `sequential`, or `weighted_random`.
- `RememberLast`: remembers the last selected file for sequential playback.
- `EnableInVR`: retained for config compatibility only. VR rendering is unsupported, and D3D hooks are never installed on Skyrim VR.
- `LogLevel`: `trace`, `debug`, `info`, `warn`, `error`, or `critical`.

## Playback

- `PlaybackMode`: `repeat_single`, `next_after_end`, or `crossfade_playlist`.
- `PlaybackSpeed`: playback multiplier.
- `Mute`: reserved for future audio support. Video playback is silent in this implementation.
- `LoopVideo`: legacy compatibility setting. High-level repeat ownership belongs to `PlaybackMode`; `next_after_end` and `crossfade_playlist` never enable decoder looping.
- `PreloadNext`: opens the next decoder before it is needed.
- `FrameQueueSize`: maximum decoded video frames queued in memory.
- `MaxDecodeWidth` and `MaxDecodeHeight`: downscale large sources while preserving aspect ratio.
- `TargetFPS`: output frame-rate cap and fallback frame rate when media timestamps are missing. The default is `60` for smooth 60 FPS videos; lower values reduce decode and texture-upload work.
- `PauseWhenMenuClosed`: reserved. Playback currently fades/stops when `LoadingMenu` closes.

## Transitions

- `FadeInMs`: fade-in duration.
- `FadeOutMs`: fade-out duration.
- `EnableCrossfade`: enables crossfade when `PlaybackMode=crossfade_playlist`.
- `CrossfadeMs`: crossfade duration.
- `FadeBetweenDifferentFiles`: reserved for future transition tuning.
- `FadeToBlackOnMenuClose`: draws the configured background during close fade.

## Display

- `FitMode`: `cover`, `contain`, or `stretch`.
- `Opacity`: global opacity from `0.0` to `1.0`.
- `BackgroundColor`: hex RGB color.
- `CoverVanillaLoadingScreen`: fills the screen before drawing video.
- `HideVanillaLoadingSpinner`: hides Skyrim's bottom-right `LoadWaitSpinner` while the animated overlay is active. Default is `false`.
- `ShowDebugOverlay`: reserved for future on-screen diagnostics.

## Performance

- `DecoderThreadPriority`: `low`, `below_normal`, or `normal`.
- `MaxDecoderThreads`: caps FFmpeg codec worker threads from `1` to `4`; the default is `1` to protect game frame pacing.
- `MaxFilesToScan`: hard cap on media files to accept; traversal stops after the cap is reached.
- `SkipFilesLargerThanMB`: skips oversized media files. `0` disables size filtering.
- `UseHardwareDecoding`: reserved. Software FFmpeg decoding is used.

## Compatibility

- `DisableWhenENBMenuOpen`: reserved. ENB menu detection is not enforced yet.
- `DisableWhenConsoleOpen`: reserved. Console detection is not enforced yet.
- `FailSafeVanillaFallback`: reserved. The implementation always uses fail-safe vanilla fallback on decoder/init failure.

## Playlist

Optional playlist:

```text
Data\SKSE\Plugins\AnimatedLoadingScreens\Playlists\default.txt
```

Format:

```text
# path | weight | display duration override in seconds, optional
intro.mp4 | 10
dark_forest.webm | 5
logo_loop.mp4 | 20 | 8.0
```

Playlist paths are relative to `LoadingScreensFolder`. Absolute paths and parent-directory escapes are rejected for safety.

## Runtime Integration

External plugins can toggle the vanilla loading spinner for the current session without editing the INI:

```cpp
AnimatedLoadingScreens_SetVanillaLoadingSpinnerHidden(true);
AnimatedLoadingScreens_SetVanillaLoadingSpinnerHidden(false);
```

`AnimatedLoadingScreens_GetVanillaLoadingSpinnerHidden()` returns the current runtime value. The INI value is still used as the default on plugin load.
