#include "Config/Config.h"
#include "Controller/AnimatedLoadingScreenController.h"
#include "Decode/FFmpegVideoDecoder.h"
#include "Logging/Log.h"
#include "Media/MediaLibrary.h"
#include "Media/MediaSelector.h"
#include "Media/Playlist.h"
#include "Render/D3D11Hooks.h"
#include "Runtime/LoadingMenuWatcher.h"
#include "Utils/Paths.h"

#include <REL/Module.h>
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    std::unique_ptr<ALS::MediaLibrary> g_mediaLibrary;
    std::unique_ptr<ALS::AnimatedLoadingScreenController> g_controller;
    ALS::Config g_config;
    bool g_runtimeIsVR = false;
    bool g_runtimeAllowed = true;
    bool g_shutdownStarted = false;
    std::atomic_bool g_dataLoaded{ false };

    struct MediaRefreshResult
    {
        ALS::MediaScanResult scan{};
        std::vector<ALS::MediaFile> playlistMedia{};
    };

    [[nodiscard]] std::string CurrentExecutableName()
    {
        std::wstring buffer(MAX_PATH, L'\0');
        const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            return {};
        }
        buffer.resize(size);
        return ALS::Paths::ForLog(std::filesystem::path(buffer).filename());
    }

    [[nodiscard]] std::string Lower(std::string value)
    {
        std::ranges::transform(value, value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    [[nodiscard]] bool DetectVRRuntime()
    {
        return REL::Module::IsVR() || Lower(CurrentExecutableName()).find("skyrimvr") != std::string::npos;
    }

    [[nodiscard]] std::string RuntimeName(REL::Module::Runtime runtime)
    {
        switch (runtime) {
        case REL::Module::Runtime::SE:
            return "SE";
        case REL::Module::Runtime::AE:
            return "AE/GOG";
        case REL::Module::Runtime::VR:
            return "VR";
        case REL::Module::Runtime::Unknown:
        default:
            return "Unknown";
        }
    }

    void LogWarnings(const std::vector<std::string>& warnings)
    {
        for (const auto& warning : warnings) {
            ALS::Log::warn("{}", warning);
        }
    }

    void LogWarningsAtDebug(const std::vector<std::string>& warnings)
    {
        for (const auto& warning : warnings) {
            ALS::Log::debug("{}", warning);
        }
    }

    void LogMediaRefreshResult(const MediaRefreshResult& result, std::string_view reason, bool detailed)
    {
        if (!detailed) {
            ALS::Log::debug(
                "Media refresh for {}: {} scanned file(s), {} playlist file(s), folder {}.",
                reason,
                result.scan.files.size(),
                result.playlistMedia.size(),
                ALS::Paths::ForLog(result.scan.scannedFolder));
            return;
        }

        ALS::Log::info("Media scan folder: {}", ALS::Paths::ForLog(result.scan.scannedFolder));
        ALS::Log::info("Discovered {} media file(s).", result.scan.files.size());
        for (std::size_t i = 0; i < std::min<std::size_t>(result.scan.files.size(), 10); ++i) {
            ALS::Log::info(
                "Media candidate [{}]: {} ({} bytes).",
                i + 1,
                ALS::Paths::ForLog(result.scan.files[i].path),
                result.scan.files[i].sizeBytes);
        }
        if (result.scan.files.size() > 10) {
            ALS::Log::info("Media candidate list truncated after 10 entries.");
        }
        if (!result.playlistMedia.empty()) {
            ALS::Log::info("Resolved {} playlist media file(s).", result.playlistMedia.size());
        }
    }

    [[nodiscard]] MediaRefreshResult ScanMediaForPlayback(std::string_view reason, bool detailed)
    {
        if (!g_mediaLibrary) {
            g_mediaLibrary = std::make_unique<ALS::MediaLibrary>();
        }

        auto scanResult = g_mediaLibrary->Scan(g_config);
        if (detailed) {
            LogWarnings(scanResult.warnings);
        } else {
            LogWarningsAtDebug(scanResult.warnings);
        }

        auto playlistResult = ALS::LoadPlaylist(ALS::Paths::DefaultPlaylistPath());
        if (detailed) {
            LogWarnings(playlistResult.warnings);
        } else {
            LogWarningsAtDebug(playlistResult.warnings);
        }

        std::vector<std::string> playlistWarnings;
        auto playlistMedia = ALS::ResolvePlaylistMedia(
            playlistResult,
            scanResult.scannedFolder,
            g_config.general.allowedExtensions,
            playlistWarnings);
        if (detailed) {
            LogWarnings(playlistWarnings);
        } else {
            LogWarningsAtDebug(playlistWarnings);
        }

        MediaRefreshResult result{ std::move(scanResult), std::move(playlistMedia) };
        LogMediaRefreshResult(result, reason, detailed);
        ALS::Log::diagnostic(
            "media_refresh reason={} scanned_folder={} scanned_count={} playlist_count={} active_count={}",
            reason,
            ALS::Paths::ForLog(result.scan.scannedFolder),
            result.scan.files.size(),
            result.playlistMedia.size(),
            result.playlistMedia.empty() ? result.scan.files.size() : result.playlistMedia.size());
        for (std::size_t i = 0; i < std::min<std::size_t>(result.scan.files.size(), 5); ++i) {
            ALS::Log::diagnostic(
                "media_candidate index={} path={} size={}",
                i + 1,
                ALS::Paths::ForLog(result.scan.files[i].path),
                result.scan.files[i].sizeBytes);
        }
        return result;
    }

    void RefreshControllerMedia(std::string_view reason)
    {
        if (!g_controller || !g_config.general.enabled) {
            return;
        }

        auto refresh = ScanMediaForPlayback(reason, false);
        g_controller->UpdateMedia(refresh.scan.files, refresh.playlistMedia);
        ALS::Log::diagnostic(
            "controller_media_updated reason={} has_media={}",
            reason,
            g_controller->HasMedia());
        if (!g_controller->HasMedia()) {
            ALS::Log::warn(
                "No media files are available after {}. Vanilla loading screens remain active until a file is found under {}.",
                reason,
                ALS::Paths::ForLog(refresh.scan.scannedFolder));
        }
    }

    [[nodiscard]] bool RuntimeCanUseHooks()
    {
        if (!g_controller || !g_runtimeAllowed) {
            return false;
        }
        if (g_runtimeIsVR) {
            ALS::Log::warn("Skyrim VR runtime detected. Animated Loading Screens does not install D3D hooks on VR builds.");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool TryInstallRenderHook(std::string_view reason)
    {
        if (!RuntimeCanUseHooks()) {
            ALS::Log::diagnostic("render_hook_install_blocked reason={} runtime_can_use_hooks=false", reason);
            return false;
        }
        if (!g_controller->HasMedia()) {
            ALS::Log::debug("D3D11 hook installation skipped after {} because no media is available yet.", reason);
            ALS::Log::diagnostic("render_hook_install_skipped reason={} has_media=false", reason);
            return false;
        }

        if (ALS::D3D11Hooks::IsInstalled()) {
            ALS::Log::diagnostic("render_hook_already_installed reason={}", reason);
            return true;
        }

        ALS::Log::info("Attempting D3D11 hook installation: {}.", reason);
        ALS::Log::diagnostic("render_hook_install_attempt reason={}", reason);
        if (ALS::D3D11Hooks::Install(*g_controller)) {
            ALS::Log::diagnostic("render_hook_install_result reason={} success=true", reason);
            return true;
        }

        ALS::Log::warn("D3D11 hook is not ready yet ({}). Animated overlay will retry; vanilla loading screens remain available.", reason);
        ALS::Log::diagnostic("render_hook_install_result reason={} success=false", reason);
        return false;
    }

    void PrepareRuntimeHooks(std::string_view reason)
    {
        ALS::Log::diagnostic("prepare_runtime_hooks reason={}", reason);
        if (!RuntimeCanUseHooks()) {
            ALS::Log::diagnostic("prepare_runtime_hooks_blocked reason={}", reason);
            return;
        }

        auto& watcher = ALS::LoadingMenuWatcher::GetSingleton();
        watcher.SetController(g_controller.get());
        watcher.SetBeforeOpenCallback([] {
            if (!g_dataLoaded.load(std::memory_order_acquire)) {
                ALS::Log::diagnostic("loading_menu_animation_blocked reason=boot_loading_menu_before_data_loaded");
                return false;
            }
            RefreshControllerMedia("LoadingMenu open");
            if (!g_controller || !g_controller->HasMedia()) {
                ALS::Log::diagnostic("loading_menu_animation_blocked reason=no_media_after_refresh");
                return false;
            }
            if (!TryInstallRenderHook("LoadingMenu open")) {
                ALS::Log::diagnostic("loading_menu_animation_blocked reason=render_hook_unavailable");
                return false;
            }
            return true;
        });
        if (!watcher.Install()) {
            ALS::Log::warn("LoadingMenu watcher is not ready yet ({}). It will be retried on the next SKSE lifecycle message.", reason);
            ALS::Log::diagnostic("loading_menu_watcher_install_result reason={} success=false", reason);
            return;
        }
        ALS::Log::diagnostic("loading_menu_watcher_install_result reason={} success=true", reason);

        if (g_controller->HasMedia() && !TryInstallRenderHook(reason)) {
            ALS::Log::warn("D3D11 hook installation deferred after {}.", reason);
        } else if (!g_controller->HasMedia()) {
            ALS::Log::warn("No media files are available after {}. LoadingMenu watcher is installed and will rescan on the next loading screen.", reason);
        }
    }

    void ShutdownPlugin()
    {
        if (g_shutdownStarted) {
            return;
        }
        g_shutdownStarted = true;

        ALS::D3D11Hooks::Uninstall();
        ALS::LoadingMenuWatcher::GetSingleton().Uninstall();
        if (g_controller) {
            g_controller->Stop();
            g_controller.reset();
        }
        g_mediaLibrary.reset();
        ALS::Log::info("Animated Loading Screens shutdown complete.");
    }

    struct ShutdownGuard
    {
        ~ShutdownGuard()
        {
            ShutdownPlugin();
        }
    };

    ShutdownGuard g_shutdownGuard;

    void SKSEMessageHandler(SKSE::MessagingInterface::Message* message)
    {
        if (!message) {
            return;
        }

        switch (message->type) {
        case SKSE::MessagingInterface::kInputLoaded:
            ALS::Log::info("SKSE kInputLoaded received. Preparing runtime hooks.");
            PrepareRuntimeHooks("SKSE kInputLoaded");
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            ALS::Log::info("SKSE kDataLoaded received. Preparing runtime hooks.");
            g_dataLoaded.store(true, std::memory_order_release);
            PrepareRuntimeHooks("SKSE kDataLoaded");
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            ALS::Log::debug("SKSE kPreLoadGame received.");
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            ALS::Log::debug("SKSE kPostLoadGame received.");
            break;
        default:
            break;
        }
    }

    [[nodiscard]] bool InitializePlugin(const SKSE::LoadInterface* skse)
    {
        ALS::Log::InitializeFileLogger(ALS::Paths::DefaultLogPath(), "info");
        ALS::Log::InitializeDiagnosticLogger(ALS::Paths::DefaultDiagnosticLogPath());
        ALS::Log::info("Animated Loading Screens {} starting. Build {} {}.", ALS_VERSION_STRING, __DATE__, __TIME__);
        ALS::Log::diagnostic(
            "plugin_start version={} build_date={} build_time={} game_root={} plugins_dir={} diagnostic_log={}",
            ALS_VERSION_STRING,
            __DATE__,
            __TIME__,
            ALS::Paths::ForLog(ALS::Paths::GameRoot()),
            ALS::Paths::ForLog(ALS::Paths::PluginsDirectory()),
            ALS::Paths::ForLog(ALS::Paths::DefaultDiagnosticLogPath()));

        const auto executable = CurrentExecutableName();
        const auto executableLower = Lower(executable);
        const auto runtime = REL::Module::GetRuntime();
        g_runtimeIsVR = DetectVRRuntime();
        g_runtimeAllowed =
            !g_runtimeIsVR &&
            (runtime == REL::Module::Runtime::SE || runtime == REL::Module::Runtime::AE) &&
            (executableLower == "skyrimse.exe" || executableLower.empty());

        if (g_runtimeIsVR) {
            ALS::Log::error("Skyrim VR runtime detected. VR rendering is unsupported and hooks will not be installed.");
            return true;
        }

        if (!g_runtimeAllowed) {
            ALS::Log::error("Unsupported runtime/executable combination: runtime={}, executable='{}'. This plugin supports SKSE-enabled Skyrim SE/AE/GOG only.", RuntimeName(runtime), executable);
            return false;
        }

        if (skse) {
            ALS::Log::info("Runtime version: {}", skse->RuntimeVersion().string());
            ALS::Log::info("SKSE version: {}", REL::Version::unpack(skse->SKSEVersion()).string());
        }
        ALS::Log::info("Executable: {}", executable.empty() ? "<unknown>" : executable);
        ALS::Log::info("Runtime family: {}", RuntimeName(runtime));
        ALS::Log::info("Address Library/CommonLibSSE relocation is required by plugin metadata.");
        ALS::Log::diagnostic(
            "runtime executable={} runtime={} skse_runtime={} skse_version={} runtime_allowed={} runtime_vr={}",
            executable.empty() ? "<unknown>" : executable,
            RuntimeName(runtime),
            skse ? skse->RuntimeVersion().string() : "<unknown>",
            skse ? REL::Version::unpack(skse->SKSEVersion()).string() : "<unknown>",
            g_runtimeAllowed,
            g_runtimeIsVR);

        const auto configResult = ALS::LoadOrCreateConfig(ALS::Paths::DefaultConfigPath());
        g_config = configResult.config;
        ALS::Log::SetLevel(g_config.general.logLevel);
        ALS::Log::info("Configured log level from INI: {}", g_config.general.logLevel);
        if (configResult.createdDefault) {
            ALS::Log::info("Created default config: {}", ALS::Paths::ForLog(ALS::Paths::DefaultConfigPath()));
        }
        ALS::Log::info("Loaded config: {}", ALS::Paths::ForLog(ALS::Paths::DefaultConfigPath()));
        LogWarnings(configResult.warnings);

        ALS::Paths::EnsureBaseLayout(g_config.general.loadingScreensFolder);
        const auto mediaFolder = ALS::Paths::ResolvePluginOwnedPath(
            g_config.general.loadingScreensFolder,
            ALS::Paths::DefaultLoadingScreensPath(),
            "General.LoadingScreensFolder");
        if (!mediaFolder.safe) {
            ALS::Log::warn("{}", mediaFolder.warning);
        }
        ALS::Log::info("Configured media folder: {}", ALS::Paths::ForLog(mediaFolder.path));
        ALS::Log::diagnostic(
            "config enabled={} log_level={} media_folder={} media_folder_safe={} cover_vanilla={} fit_mode={} opacity={} fade_in_ms={} playback_mode={}",
            g_config.general.enabled,
            g_config.general.logLevel,
            ALS::Paths::ForLog(mediaFolder.path),
            mediaFolder.safe,
            g_config.display.coverVanillaLoadingScreen,
            ALS::ToString(g_config.display.fitMode),
            g_config.display.opacity,
            g_config.transitions.fadeInMs,
            ALS::ToString(g_config.playback.playbackMode));

        if (!g_config.general.enabled) {
            ALS::Log::warn("Plugin is disabled by INI. No hooks will be installed.");
            return true;
        }

        if (!ALS::FFmpegVideoDecoder::IsRuntimeAvailable()) {
            ALS::Log::warn("FFmpeg runtime is unavailable. Video decode will fail safely to vanilla loading screens until local FFmpeg DLLs are installed.");
        }

        g_mediaLibrary = std::make_unique<ALS::MediaLibrary>();
        auto mediaRefresh = ScanMediaForPlayback("initial load", true);

        ALS::MediaSelector selector(
            mediaRefresh.scan.files,
            mediaRefresh.playlistMedia,
            g_config.general.selectionMode,
            g_config.general.rememberLast,
            ALS::Paths::StatePath());

        g_controller = std::make_unique<ALS::AnimatedLoadingScreenController>(
            g_config,
            std::move(selector),
            [] {
                return std::make_unique<ALS::FFmpegVideoDecoder>();
            });
        if (!g_controller->HasMedia()) {
            ALS::Log::warn("No media files are currently available. The LoadingMenu watcher will keep rescanning before each loading screen.");
        }

        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging || !messaging->RegisterListener(SKSEMessageHandler)) {
            ALS::Log::error("Unable to register SKSE messaging listener.");
            return false;
        }

        ALS::Log::info("SKSE messaging listener registered.");
        return true;
    }
}

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* skse)
{
    try {
        SKSE::Init(skse);
        SKSE::log::info(
            "AnimatedLoadingScreens build {} {} MSVC {} FFmpeg {}",
            __DATE__,
            __TIME__,
#ifdef _MSC_FULL_VER
            _MSC_FULL_VER,
#else
            0,
#endif
#if ALS_ENABLE_FFMPEG
            "on"
#else
            "off"
#endif
        );
        return InitializePlugin(skse);
    } catch (const std::exception& e) {
        ALS::Log::error("Fatal exception during plugin load: {}", e.what());
    } catch (...) {
        ALS::Log::error("Unknown fatal exception during plugin load.");
    }
    return false;
}
