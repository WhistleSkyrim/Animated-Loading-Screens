#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ALS
{
    enum class SelectionMode
    {
        Random,
        Sequential,
        WeightedRandom
    };

    enum class PlaybackMode
    {
        RepeatSingle,
        NextAfterEnd,
        CrossfadePlaylist
    };

    enum class FitMode
    {
        Cover,
        Contain,
        Stretch
    };

    enum class DecoderThreadPriority
    {
        Low,
        BelowNormal,
        Normal
    };

    struct Color
    {
        float r{ 0.0F };
        float g{ 0.0F };
        float b{ 0.0F };
        float a{ 1.0F };
    };

    struct GeneralConfig
    {
        bool enabled{ true };
        std::filesystem::path loadingScreensFolder{ "LoadingScreens" };
        bool scanSubfolders{ true };
        std::vector<std::string> allowedExtensions{ ".mp4", ".mkv", ".webm", ".mov", ".avi", ".gif", ".apng" };
        SelectionMode selectionMode{ SelectionMode::Random };
        bool rememberLast{ false };
        bool enableInVR{ false };
        std::string logLevel{ "info" };
    };

    struct PlaybackConfig
    {
        PlaybackMode playbackMode{ PlaybackMode::RepeatSingle };
        double playbackSpeed{ 1.0 };
        bool mute{ true };
        bool loopVideo{ true };
        bool preloadNext{ true };
        std::size_t frameQueueSize{ 4 };
        int maxDecodeWidth{ 1920 };
        int maxDecodeHeight{ 1080 };
        double targetFPS{ 60.0 };
        bool pauseWhenMenuClosed{ true };
    };

    struct TransitionConfig
    {
        int fadeInMs{ 350 };
        int fadeOutMs{ 250 };
        bool enableCrossfade{ true };
        int crossfadeMs{ 700 };
        bool fadeBetweenDifferentFiles{ true };
        bool fadeToBlackOnMenuClose{ true };
    };

    struct DisplayConfig
    {
        FitMode fitMode{ FitMode::Cover };
        float opacity{ 1.0F };
        Color backgroundColor{};
        bool coverVanillaLoadingScreen{ true };
        bool showDebugOverlay{ false };
    };

    struct PerformanceConfig
    {
        DecoderThreadPriority decoderThreadPriority{ DecoderThreadPriority::BelowNormal };
        int maxDecoderThreads{ 1 };
        std::size_t maxFilesToScan{ 500 };
        std::uintmax_t skipFilesLargerThanMB{ 2048 };
        bool useHardwareDecoding{ false };
    };

    struct CompatibilityConfig
    {
        bool disableWhenENBMenuOpen{ true };
        bool disableWhenConsoleOpen{ false };
        bool failSafeVanillaFallback{ true };
    };

    struct Config
    {
        GeneralConfig general{};
        PlaybackConfig playback{};
        TransitionConfig transitions{};
        DisplayConfig display{};
        PerformanceConfig performance{};
        CompatibilityConfig compatibility{};
    };

    struct ConfigLoadResult
    {
        Config config{};
        bool createdDefault{ false };
        std::vector<std::string> warnings{};
    };

    [[nodiscard]] std::string DefaultConfigText();
    [[nodiscard]] ConfigLoadResult LoadOrCreateConfig(const std::filesystem::path& path);
    void WriteDefaultConfig(const std::filesystem::path& path);

    [[nodiscard]] std::string ToString(SelectionMode value);
    [[nodiscard]] std::string ToString(PlaybackMode value);
    [[nodiscard]] std::string ToString(FitMode value);
}
