#include "Utils/Paths.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string_view>

namespace ALS::Paths
{
    namespace
    {
        [[nodiscard]] std::string Lower(std::string value)
        {
            std::ranges::transform(value, value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        [[nodiscard]] std::string ToUtf8String(const std::u8string& value)
        {
            std::string result;
            result.reserve(value.size());
            for (const auto ch : value) {
                result.push_back(static_cast<char>(ch));
            }
            return result;
        }

        [[nodiscard]] std::string ComponentForCompare(const std::filesystem::path& path)
        {
            try {
                return Lower(ToUtf8String(path.generic_u8string()));
            } catch (...) {
                return {};
            }
        }

        [[nodiscard]] bool StartsWithDataComponent(const std::filesystem::path& path)
        {
            auto it = path.begin();
            if (it == path.end()) {
                return false;
            }
            return ComponentForCompare(*it) == "data";
        }
    }

    std::filesystem::path GameRoot()
    {
        return std::filesystem::current_path();
    }

    std::filesystem::path PluginsDirectory()
    {
        return GameRoot() / "Data" / "SKSE" / "Plugins";
    }

    std::filesystem::path ModDirectory()
    {
        return PluginsDirectory() / "AnimatedLoadingScreens";
    }

    std::filesystem::path DefaultConfigPath()
    {
        return PluginsDirectory() / "AnimatedLoadingScreens.ini";
    }

    std::filesystem::path DefaultLogPath()
    {
        return PluginsDirectory() / "AnimatedLoadingScreens.log";
    }

    std::filesystem::path DefaultDiagnosticLogPath()
    {
        return PluginsDirectory() / "AnimatedLoadingScreens.debug.log";
    }

    std::filesystem::path DefaultLoadingScreensPath()
    {
        return ModDirectory() / "LoadingScreens";
    }

    std::filesystem::path LegacyLoadingScreensPath()
    {
        return ModDirectory() / "Loading Screens";
    }

    std::filesystem::path DefaultPlaylistPath()
    {
        return ModDirectory() / "Playlists" / "default.txt";
    }

    std::filesystem::path StatePath()
    {
        return ModDirectory() / "state.txt";
    }

    std::string ForLog(const std::filesystem::path& path)
    {
        try {
            return ToUtf8String(path.lexically_normal().generic_u8string());
        } catch (...) {
            return "<path unavailable>";
        }
    }

    std::filesystem::path ResolveGameRelativePath(const std::filesystem::path& path)
    {
        if (path.is_absolute()) {
            return path;
        }
        return (GameRoot() / path).lexically_normal();
    }

    bool IsPathInside(const std::filesystem::path& path, const std::filesystem::path& parent)
    {
        const auto normalizedPath = path.lexically_normal();
        const auto normalizedParent = parent.lexically_normal();

        auto pathIt = normalizedPath.begin();
        const auto pathEnd = normalizedPath.end();
        for (auto parentIt = normalizedParent.begin(); parentIt != normalizedParent.end(); ++parentIt) {
            if (pathIt == pathEnd) {
                return false;
            }
            if (ComponentForCompare(*pathIt) != ComponentForCompare(*parentIt)) {
                return false;
            }
            ++pathIt;
        }
        return true;
    }

    bool ContainsParentTraversal(const std::filesystem::path& path)
    {
        for (const auto& component : path.lexically_normal()) {
            if (component == "..") {
                return true;
            }
        }
        return false;
    }

    ResolvedPluginPath ResolvePluginOwnedPath(
        const std::filesystem::path& configuredPath,
        const std::filesystem::path& fallbackPath,
        std::string_view settingName)
    {
        const auto fallback = fallbackPath.is_absolute() ? fallbackPath.lexically_normal() : ResolveGameRelativePath(fallbackPath);
        const auto pluginRoot = ModDirectory().lexically_normal();

        if (configuredPath.empty()) {
            return { fallback, false, std::string(settingName) + " is empty; using default plugin folder." };
        }
        if (configuredPath.is_absolute()) {
            return { fallback, false, std::string(settingName) + " rejected absolute path '" + ForLog(configuredPath) + "'; using default plugin folder." };
        }
        if (ContainsParentTraversal(configuredPath)) {
            return { fallback, false, std::string(settingName) + " rejected parent-directory traversal '" + ForLog(configuredPath) + "'; using default plugin folder." };
        }

        const auto candidate = StartsWithDataComponent(configuredPath) ?
            (GameRoot() / configuredPath).lexically_normal() :
            (pluginRoot / configuredPath).lexically_normal();

        if (!IsPathInside(candidate, pluginRoot)) {
            return { fallback, false, std::string(settingName) + " must stay under " + ForLog(pluginRoot) + "; using default plugin folder." };
        }

        return { candidate, true, {} };
    }

    void EnsureBaseLayout(const std::filesystem::path& loadingScreensFolder)
    {
        const auto resolvedLoadingScreens = ResolvePluginOwnedPath(
            loadingScreensFolder,
            DefaultLoadingScreensPath(),
            "General.LoadingScreensFolder")
                                                .path;
        std::filesystem::create_directories(PluginsDirectory());
        std::filesystem::create_directories(ModDirectory() / "FFmpeg");
        std::filesystem::create_directories(ModDirectory() / "Playlists");
        std::filesystem::create_directories(resolvedLoadingScreens);

        const auto placeholder = resolvedLoadingScreens / "put_your_videos_here.txt";
        if (!std::filesystem::exists(placeholder)) {
            std::ofstream output(placeholder);
            output << "Drop MP4, WebM, MKV, MOV, AVI, GIF, or APNG files in this folder.\n";
        }

        const auto playlist = DefaultPlaylistPath();
        if (!std::filesystem::exists(playlist)) {
            std::ofstream output(playlist);
            output << "# path | weight | display duration override in seconds, optional\n";
            output << "# intro.mp4 | 10\n";
        }
    }
}
