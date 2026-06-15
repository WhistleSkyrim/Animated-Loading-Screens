# Building

## Toolchain

- Visual Studio 2022.
- CMake 3.26 or newer.
- x64 generator or preset.
- vcpkg with the manifest in this repository and `VCPKG_ROOT` set.
- FFmpeg development files if `ALS_ENABLE_FFMPEG=ON`. The `release-x64` preset enables the manifest `ffmpeg` feature automatically.

## Configure

Recommended preset for the full plugin build:

```powershell
cmake --preset release-x64
```

The presets use the repository's `x64-windows-v143` overlay triplet so vcpkg dependencies and the plugin are built with the same Visual Studio 2022 C++ runtime ABI. The triplet links normal vcpkg libraries statically while keeping the MSVC runtime dynamic; this avoids SKSE load-time failures from missing adjacent `minhook`, `spdlog`, or `fmt` DLLs. The `ffmpeg` port stays dynamic because the plugin loads FFmpeg DLLs explicitly from its own `AnimatedLoadingScreens\FFmpeg` runtime folder. The presets also place vcpkg's installed tree under `%VCPKG_ROOT%\installed-animated-loading-screens` so FFmpeg's MSYS configure scripts do not inherit the repository path. This avoids known quoting failures when the checkout path contains spaces.

Unit-test-only preset, no Skyrim or FFmpeg dependencies:

```powershell
cmake --preset tests-release-x64
```

Equivalent manual full-video configure:

```powershell
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake -DVCPKG_MANIFEST_FEATURES=ffmpeg -DALS_ENABLE_FFMPEG=ON
```

The plugin uses FFmpeg headers/libs only to build against the ABI; FFmpeg functions are resolved with `LoadLibraryExW`/`GetProcAddress` at runtime so missing FFmpeg DLLs do not block `AnimatedLoadingScreens.dll` from loading. Runtime FFmpeg DLLs must be placed in `Data\SKSE\Plugins\AnimatedLoadingScreens\FFmpeg`.

If FFmpeg is not found while `ALS_ENABLE_FFMPEG=ON`, configuration fails clearly. This prevents a build that appears to support video while shipping without a decoder.

For a no-video fallback build that still produces the SKSE DLL and logs a safe decode-disabled message:

```powershell
cmake -S . -B build-novideo -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake -DALS_ENABLE_FFMPEG=OFF
cmake --build build-novideo --config Release
```

## Build

```powershell
cmake --build --preset release-x64
ctest --preset tests-release-x64
```

The plugin DLL is emitted as `AnimatedLoadingScreens.dll`.

## Package Layout

```powershell
cmake --install build-release-v143-x64 --config Release --prefix package
```

Then copy or archive the generated `package\Data` folder as a Skyrim mod.

You can also build and package the plugin into the repository-level `Output`
folder with:

```powershell
.\Build-Output.ps1
```

By default, `Output` is laid out as a mod root:

```text
Output/
  SKSE/
    Plugins/
      AnimatedLoadingScreens.dll
      AnimatedLoadingScreens.ini
      AnimatedLoadingScreens/
        LoadingScreens/
        FFmpeg/
        Playlists/
```

For a direct Skyrim-root mirror, use `.\Build-Output.ps1 -Layout Game`, which
creates `Output\Data\SKSE\Plugins`.

## FFmpeg Licensing

This project loads FFmpeg dynamically from `Data\SKSE\Plugins\AnimatedLoadingScreens\FFmpeg`. For ordinary mod distribution, use an LGPL-compatible FFmpeg build. GPL-enabled or nonfree FFmpeg builds can impose additional obligations on the whole distribution and must be documented by the distributor.
