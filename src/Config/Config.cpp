#include "Config/Config.h"

#include "Utils/Paths.h"

#include "Utils/Paths.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <sstream>

namespace
{
    using IniData = std::unordered_map<std::string, std::unordered_map<std::string, std::string>>;

    [[nodiscard]] std::string Trim(std::string_view value)
    {
        auto begin = value.begin();
        auto end = value.end();
        while (begin != end && std::isspace(static_cast<unsigned char>(*begin)) != 0) {
            ++begin;
        }
        while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1))) != 0) {
            --end;
        }
        return std::string(begin, end);
    }

    [[nodiscard]] std::string Lower(std::string value)
    {
        std::ranges::transform(value, value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    [[nodiscard]] bool ParseBool(std::string value, bool fallback, std::vector<std::string>& warnings, std::string_view key)
    {
        value = Lower(Trim(value));
        if (value == "true" || value == "1" || value == "yes" || value == "on") {
            return true;
        }
        if (value == "false" || value == "0" || value == "no" || value == "off") {
            return false;
        }
        warnings.emplace_back("Invalid boolean for " + std::string(key) + ": " + value);
        return fallback;
    }

    template <class T>
    [[nodiscard]] T ParseNumber(std::string value, T fallback, std::vector<std::string>& warnings, std::string_view key)
    {
        value = Trim(value);
        T parsed{};
        const auto* begin = value.data();
        const auto* end = begin + value.size();
        const auto result = std::from_chars(begin, end, parsed);
        if (value.empty() || result.ec != std::errc{} || result.ptr != end) {
            warnings.emplace_back("Invalid numeric value for " + std::string(key) + ": " + value);
            return fallback;
        }
        if constexpr (std::is_floating_point_v<T>) {
            if (!std::isfinite(parsed)) {
                warnings.emplace_back("Invalid numeric value for " + std::string(key) + ": " + value);
                return fallback;
            }
        }
        return parsed;
    }

    template <class T>
    [[nodiscard]] T ClampValue(T value, T minimum, T maximum, std::vector<std::string>& warnings, std::string_view key)
    {
        if (value < minimum) {
            warnings.emplace_back(std::string(key) + " below safe minimum; clamped to " + std::to_string(minimum));
            return minimum;
        }
        if (value > maximum) {
            warnings.emplace_back(std::string(key) + " above safe maximum; clamped to " + std::to_string(maximum));
            return maximum;
        }
        return value;
    }

    [[nodiscard]] ALS::SelectionMode ParseSelectionMode(const std::string& value, ALS::SelectionMode fallback, std::vector<std::string>& warnings)
    {
        const auto lower = Lower(Trim(value));
        if (lower == "random") {
            return ALS::SelectionMode::Random;
        }
        if (lower == "sequential") {
            return ALS::SelectionMode::Sequential;
        }
        if (lower == "weighted_random") {
            return ALS::SelectionMode::WeightedRandom;
        }
        warnings.emplace_back("Invalid SelectionMode: " + value);
        return fallback;
    }

    [[nodiscard]] ALS::PlaybackMode ParsePlaybackMode(const std::string& value, ALS::PlaybackMode fallback, std::vector<std::string>& warnings)
    {
        const auto lower = Lower(Trim(value));
        if (lower == "repeat_single") {
            return ALS::PlaybackMode::RepeatSingle;
        }
        if (lower == "next_after_end") {
            return ALS::PlaybackMode::NextAfterEnd;
        }
        if (lower == "crossfade_playlist") {
            return ALS::PlaybackMode::CrossfadePlaylist;
        }
        warnings.emplace_back("Invalid PlaybackMode: " + value);
        return fallback;
    }

    [[nodiscard]] ALS::FitMode ParseFitMode(const std::string& value, ALS::FitMode fallback, std::vector<std::string>& warnings)
    {
        const auto lower = Lower(Trim(value));
        if (lower == "cover") {
            return ALS::FitMode::Cover;
        }
        if (lower == "contain") {
            return ALS::FitMode::Contain;
        }
        if (lower == "stretch") {
            return ALS::FitMode::Stretch;
        }
        warnings.emplace_back("Invalid FitMode: " + value);
        return fallback;
    }

    [[nodiscard]] ALS::DecoderThreadPriority ParsePriority(const std::string& value, ALS::DecoderThreadPriority fallback, std::vector<std::string>& warnings)
    {
        const auto lower = Lower(Trim(value));
        if (lower == "low") {
            return ALS::DecoderThreadPriority::Low;
        }
        if (lower == "below_normal") {
            return ALS::DecoderThreadPriority::BelowNormal;
        }
        if (lower == "normal") {
            return ALS::DecoderThreadPriority::Normal;
        }
        warnings.emplace_back("Invalid DecoderThreadPriority: " + value);
        return fallback;
    }

    [[nodiscard]] std::vector<std::string> ParseExtensions(const std::string& csv)
    {
        std::vector<std::string> result;
        std::stringstream stream(csv);
        std::string item;
        while (std::getline(stream, item, ',')) {
            item = Lower(Trim(item));
            if (item.empty()) {
                continue;
            }
            if (item.front() != '.') {
                item.insert(item.begin(), '.');
            }
            result.push_back(item);
        }
        return result;
    }

    [[nodiscard]] ALS::Color ParseColor(const std::string& text, ALS::Color fallback, std::vector<std::string>& warnings)
    {
        auto value = Trim(text);
        if (value.size() != 7 || value.front() != '#') {
            warnings.emplace_back("Invalid BackgroundColor: " + text);
            return fallback;
        }

        const auto parseChannel = [&](std::size_t offset) -> std::optional<int> {
            int parsed = 0;
            const auto* begin = value.data() + offset;
            const auto* end = begin + 2;
            const auto result = std::from_chars(begin, end, parsed, 16);
            if (result.ec != std::errc{} || result.ptr != end) {
                return std::nullopt;
            }
            return parsed;
        };

        const auto r = parseChannel(1);
        const auto g = parseChannel(3);
        const auto b = parseChannel(5);
        if (!r || !g || !b) {
            warnings.emplace_back("Invalid BackgroundColor: " + text);
            return fallback;
        }

        return ALS::Color{
            static_cast<float>(*r) / 255.0F,
            static_cast<float>(*g) / 255.0F,
            static_cast<float>(*b) / 255.0F,
            1.0F
        };
    }

    void ApplyLoadingScreensFolder(std::string value, ALS::Config& config, std::vector<std::string>& warnings)
    {
        const auto path = std::filesystem::path(Trim(value));
        const auto resolved = ALS::Paths::ResolvePluginOwnedPath(
            path,
            ALS::Paths::DefaultLoadingScreensPath(),
            "General.LoadingScreensFolder");
        if (!resolved.safe) {
            warnings.emplace_back(resolved.warning);
            config.general.loadingScreensFolder = "LoadingScreens";
            return;
        }
        config.general.loadingScreensFolder = path.lexically_normal();
    }

    void ApplyLogLevel(std::string value, ALS::Config& config, std::vector<std::string>& warnings)
    {
        value = Lower(Trim(value));
        if (value == "trace" || value == "debug" || value == "info" || value == "warn" ||
            value == "warning" || value == "error" || value == "critical" || value == "off") {
            config.general.logLevel = value;
            return;
        }
        warnings.emplace_back("Invalid LogLevel: " + value);
    }

    [[nodiscard]] IniData ParseIni(std::istream& input)
    {
        IniData data;
        std::string section;
        std::string line;

        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }

            const auto trimmed = Trim(line);
            if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#') {
                continue;
            }
            if (trimmed.front() == '[' && trimmed.back() == ']') {
                section = Trim(std::string_view(trimmed).substr(1, trimmed.size() - 2));
                continue;
            }

            const auto equals = trimmed.find('=');
            if (equals == std::string::npos) {
                continue;
            }

            auto key = Trim(std::string_view(trimmed).substr(0, equals));
            auto value = Trim(std::string_view(trimmed).substr(equals + 1));
            data[section][key] = value;
        }

        return data;
    }

    [[nodiscard]] std::optional<std::string> Get(const IniData& data, std::string_view section, std::string_view key)
    {
        const auto sectionIt = data.find(std::string(section));
        if (sectionIt == data.end()) {
            return std::nullopt;
        }
        const auto keyIt = sectionIt->second.find(std::string(key));
        if (keyIt == sectionIt->second.end()) {
            return std::nullopt;
        }
        return keyIt->second;
    }

    void ApplyIni(const IniData& data, ALS::Config& config, std::vector<std::string>& warnings)
    {
        if (auto value = Get(data, "General", "Enabled")) {
            config.general.enabled = ParseBool(*value, config.general.enabled, warnings, "General.Enabled");
        }
        if (auto value = Get(data, "General", "LoadingScreensFolder")) {
            ApplyLoadingScreensFolder(*value, config, warnings);
        }
        if (auto value = Get(data, "General", "ScanSubfolders")) {
            config.general.scanSubfolders = ParseBool(*value, config.general.scanSubfolders, warnings, "General.ScanSubfolders");
        }
        if (auto value = Get(data, "General", "AllowedExtensions")) {
            const auto parsed = ParseExtensions(*value);
            if (!parsed.empty()) {
                config.general.allowedExtensions = parsed;
            }
        }
        if (auto value = Get(data, "General", "SelectionMode")) {
            config.general.selectionMode = ParseSelectionMode(*value, config.general.selectionMode, warnings);
        }
        if (auto value = Get(data, "General", "RememberLast")) {
            config.general.rememberLast = ParseBool(*value, config.general.rememberLast, warnings, "General.RememberLast");
        }
        if (auto value = Get(data, "General", "EnableInVR")) {
            config.general.enableInVR = ParseBool(*value, config.general.enableInVR, warnings, "General.EnableInVR");
        }
        if (auto value = Get(data, "General", "LogLevel")) {
            ApplyLogLevel(*value, config, warnings);
        }

        if (auto value = Get(data, "Playback", "PlaybackMode")) {
            config.playback.playbackMode = ParsePlaybackMode(*value, config.playback.playbackMode, warnings);
        }
        if (auto value = Get(data, "Playback", "PlaybackSpeed")) {
            config.playback.playbackSpeed = ClampValue(
                ParseNumber<double>(*value, config.playback.playbackSpeed, warnings, "Playback.PlaybackSpeed"),
                0.10,
                4.0,
                warnings,
                "Playback.PlaybackSpeed");
        }
        if (auto value = Get(data, "Playback", "Mute")) {
            config.playback.mute = ParseBool(*value, config.playback.mute, warnings, "Playback.Mute");
        }
        if (auto value = Get(data, "Playback", "LoopVideo")) {
            config.playback.loopVideo = ParseBool(*value, config.playback.loopVideo, warnings, "Playback.LoopVideo");
        }
        if (auto value = Get(data, "Playback", "PreloadNext")) {
            config.playback.preloadNext = ParseBool(*value, config.playback.preloadNext, warnings, "Playback.PreloadNext");
        }
        if (auto value = Get(data, "Playback", "FrameQueueSize")) {
            config.playback.frameQueueSize = ClampValue<std::size_t>(
                ParseNumber<std::size_t>(*value, config.playback.frameQueueSize, warnings, "Playback.FrameQueueSize"),
                1,
                16,
                warnings,
                "Playback.FrameQueueSize");
        }
        if (auto value = Get(data, "Playback", "MaxDecodeWidth")) {
            config.playback.maxDecodeWidth = ClampValue(
                ParseNumber<int>(*value, config.playback.maxDecodeWidth, warnings, "Playback.MaxDecodeWidth"),
                64,
                4096,
                warnings,
                "Playback.MaxDecodeWidth");
        }
        if (auto value = Get(data, "Playback", "MaxDecodeHeight")) {
            config.playback.maxDecodeHeight = ClampValue(
                ParseNumber<int>(*value, config.playback.maxDecodeHeight, warnings, "Playback.MaxDecodeHeight"),
                64,
                4096,
                warnings,
                "Playback.MaxDecodeHeight");
        }
        if (auto value = Get(data, "Playback", "TargetFPS")) {
            config.playback.targetFPS = ClampValue(
                ParseNumber<double>(*value, config.playback.targetFPS, warnings, "Playback.TargetFPS"),
                1.0,
                240.0,
                warnings,
                "Playback.TargetFPS");
        }
        if (auto value = Get(data, "Playback", "PauseWhenMenuClosed")) {
            config.playback.pauseWhenMenuClosed = ParseBool(*value, config.playback.pauseWhenMenuClosed, warnings, "Playback.PauseWhenMenuClosed");
        }

        if (auto value = Get(data, "Transitions", "FadeInMs")) {
            config.transitions.fadeInMs = ClampValue(
                ParseNumber<int>(*value, config.transitions.fadeInMs, warnings, "Transitions.FadeInMs"),
                0,
                10000,
                warnings,
                "Transitions.FadeInMs");
        }
        if (auto value = Get(data, "Transitions", "FadeOutMs")) {
            config.transitions.fadeOutMs = ClampValue(
                ParseNumber<int>(*value, config.transitions.fadeOutMs, warnings, "Transitions.FadeOutMs"),
                0,
                10000,
                warnings,
                "Transitions.FadeOutMs");
        }
        if (auto value = Get(data, "Transitions", "EnableCrossfade")) {
            config.transitions.enableCrossfade = ParseBool(*value, config.transitions.enableCrossfade, warnings, "Transitions.EnableCrossfade");
        }
        if (auto value = Get(data, "Transitions", "CrossfadeMs")) {
            config.transitions.crossfadeMs = ClampValue(
                ParseNumber<int>(*value, config.transitions.crossfadeMs, warnings, "Transitions.CrossfadeMs"),
                0,
                10000,
                warnings,
                "Transitions.CrossfadeMs");
        }
        if (auto value = Get(data, "Transitions", "FadeBetweenDifferentFiles")) {
            config.transitions.fadeBetweenDifferentFiles = ParseBool(*value, config.transitions.fadeBetweenDifferentFiles, warnings, "Transitions.FadeBetweenDifferentFiles");
        }
        if (auto value = Get(data, "Transitions", "FadeToBlackOnMenuClose")) {
            config.transitions.fadeToBlackOnMenuClose = ParseBool(*value, config.transitions.fadeToBlackOnMenuClose, warnings, "Transitions.FadeToBlackOnMenuClose");
        }

        if (auto value = Get(data, "Display", "FitMode")) {
            config.display.fitMode = ParseFitMode(*value, config.display.fitMode, warnings);
        }
        if (auto value = Get(data, "Display", "Opacity")) {
            config.display.opacity = std::clamp(ParseNumber<float>(*value, config.display.opacity, warnings, "Display.Opacity"), 0.0F, 1.0F);
        }
        if (auto value = Get(data, "Display", "BackgroundColor")) {
            config.display.backgroundColor = ParseColor(*value, config.display.backgroundColor, warnings);
        }
        if (auto value = Get(data, "Display", "CoverVanillaLoadingScreen")) {
            config.display.coverVanillaLoadingScreen = ParseBool(*value, config.display.coverVanillaLoadingScreen, warnings, "Display.CoverVanillaLoadingScreen");
        }
        if (auto value = Get(data, "Display", "ShowDebugOverlay")) {
            config.display.showDebugOverlay = ParseBool(*value, config.display.showDebugOverlay, warnings, "Display.ShowDebugOverlay");
        }

        if (auto value = Get(data, "Performance", "DecoderThreadPriority")) {
            config.performance.decoderThreadPriority = ParsePriority(*value, config.performance.decoderThreadPriority, warnings);
        }
        if (auto value = Get(data, "Performance", "MaxDecoderThreads")) {
            config.performance.maxDecoderThreads = ClampValue(
                ParseNumber<int>(*value, config.performance.maxDecoderThreads, warnings, "Performance.MaxDecoderThreads"),
                1,
                4,
                warnings,
                "Performance.MaxDecoderThreads");
        }
        if (auto value = Get(data, "Performance", "MaxFilesToScan")) {
            config.performance.maxFilesToScan = ClampValue<std::size_t>(
                ParseNumber<std::size_t>(*value, config.performance.maxFilesToScan, warnings, "Performance.MaxFilesToScan"),
                1,
                10000,
                warnings,
                "Performance.MaxFilesToScan");
        }
        if (auto value = Get(data, "Performance", "SkipFilesLargerThanMB")) {
            config.performance.skipFilesLargerThanMB = ClampValue<std::uintmax_t>(
                ParseNumber<std::uintmax_t>(*value, config.performance.skipFilesLargerThanMB, warnings, "Performance.SkipFilesLargerThanMB"),
                0,
                32768,
                warnings,
                "Performance.SkipFilesLargerThanMB");
        }
        if (auto value = Get(data, "Performance", "UseHardwareDecoding")) {
            config.performance.useHardwareDecoding = ParseBool(*value, config.performance.useHardwareDecoding, warnings, "Performance.UseHardwareDecoding");
        }

        if (auto value = Get(data, "Compatibility", "DisableWhenENBMenuOpen")) {
            config.compatibility.disableWhenENBMenuOpen = ParseBool(*value, config.compatibility.disableWhenENBMenuOpen, warnings, "Compatibility.DisableWhenENBMenuOpen");
        }
        if (auto value = Get(data, "Compatibility", "DisableWhenConsoleOpen")) {
            config.compatibility.disableWhenConsoleOpen = ParseBool(*value, config.compatibility.disableWhenConsoleOpen, warnings, "Compatibility.DisableWhenConsoleOpen");
        }
        if (auto value = Get(data, "Compatibility", "FailSafeVanillaFallback")) {
            config.compatibility.failSafeVanillaFallback = ParseBool(*value, config.compatibility.failSafeVanillaFallback, warnings, "Compatibility.FailSafeVanillaFallback");
        }
    }
}

namespace ALS
{
    std::string DefaultConfigText()
    {
        return R"(; Animated Loading Screens default configuration.
; LoadingScreensFolder is confined to Data\SKSE\Plugins\AnimatedLoadingScreens.

[General]
Enabled=true
LoadingScreensFolder=LoadingScreens
ScanSubfolders=true
AllowedExtensions=.mp4,.mkv,.webm,.mov,.avi,.gif,.apng
SelectionMode=random
RememberLast=false
; Retained for compatibility only. Skyrim VR is unsupported and hooks are never installed there.
EnableInVR=false
LogLevel=info

[Playback]
PlaybackMode=repeat_single
; repeat_single = loop the same media until LoadingMenu closes
; next_after_end = when media ends, switch to another media
; crossfade_playlist = crossfade to another media when current media ends
PlaybackSpeed=1.0
; Reserved: video playback is silent in this build.
Mute=true
; LoopVideo only affects repeat_single. If false, playback stops after one pass.
LoopVideo=true
PreloadNext=true
FrameQueueSize=4
MaxDecodeWidth=1920
MaxDecodeHeight=1080
TargetFPS=60
PauseWhenMenuClosed=true

[Transitions]
FadeInMs=350
FadeOutMs=250
EnableCrossfade=true
CrossfadeMs=700
; Reserved for future transition tuning.
FadeBetweenDifferentFiles=true
FadeToBlackOnMenuClose=true

[Display]
FitMode=cover
; cover, contain, stretch
Opacity=1.0
BackgroundColor=#000000
CoverVanillaLoadingScreen=true
; Reserved for future on-screen diagnostics.
ShowDebugOverlay=false

[Performance]
DecoderThreadPriority=below_normal
MaxDecoderThreads=1
MaxFilesToScan=500
SkipFilesLargerThanMB=2048
; Reserved: software FFmpeg decoding is used.
UseHardwareDecoding=false

[Compatibility]
; Reserved: these detection paths are not enforced yet.
DisableWhenENBMenuOpen=true
DisableWhenConsoleOpen=false
; Reserved: the implementation always uses fail-safe vanilla fallback.
FailSafeVanillaFallback=true
)";
    }

    void WriteDefaultConfig(const std::filesystem::path& path)
    {
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }

        std::ofstream output(path, std::ios::binary);
        if (!output) {
            throw std::runtime_error("Unable to create default config: " + Paths::ForLog(path));
        }
        output << DefaultConfigText();
    }

    ConfigLoadResult LoadOrCreateConfig(const std::filesystem::path& path)
    {
        ConfigLoadResult result;

        if (!std::filesystem::exists(path)) {
            WriteDefaultConfig(path);
            result.createdDefault = true;
        }

        std::ifstream input(path, std::ios::binary);
        if (!input) {
            result.warnings.emplace_back("Unable to open config file: " + Paths::ForLog(path));
            return result;
        }

        const auto ini = ParseIni(input);
        ApplyIni(ini, result.config, result.warnings);
        return result;
    }

    std::string ToString(SelectionMode value)
    {
        switch (value) {
        case SelectionMode::Random:
            return "random";
        case SelectionMode::Sequential:
            return "sequential";
        case SelectionMode::WeightedRandom:
            return "weighted_random";
        }
        return "unknown";
    }

    std::string ToString(PlaybackMode value)
    {
        switch (value) {
        case PlaybackMode::RepeatSingle:
            return "repeat_single";
        case PlaybackMode::NextAfterEnd:
            return "next_after_end";
        case PlaybackMode::CrossfadePlaylist:
            return "crossfade_playlist";
        }
        return "unknown";
    }

    std::string ToString(FitMode value)
    {
        switch (value) {
        case FitMode::Cover:
            return "cover";
        case FitMode::Contain:
            return "contain";
        case FitMode::Stretch:
            return "stretch";
        }
        return "unknown";
    }
}
