#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace ALS::Paths
{
    struct ResolvedPluginPath
    {
        std::filesystem::path path{};
        bool safe{ true };
        std::string warning{};
    };

    [[nodiscard]] std::filesystem::path GameRoot();
    [[nodiscard]] std::filesystem::path PluginsDirectory();
    [[nodiscard]] std::filesystem::path ModDirectory();
    [[nodiscard]] std::filesystem::path DefaultConfigPath();
    [[nodiscard]] std::filesystem::path DefaultLogPath();
    [[nodiscard]] std::filesystem::path DefaultDiagnosticLogPath();
    [[nodiscard]] std::filesystem::path DefaultLoadingScreensPath();
    [[nodiscard]] std::filesystem::path LegacyLoadingScreensPath();
    [[nodiscard]] std::filesystem::path DefaultPlaylistPath();
    [[nodiscard]] std::filesystem::path StatePath();
    [[nodiscard]] std::string ForLog(const std::filesystem::path& path);

    [[nodiscard]] std::filesystem::path ResolveGameRelativePath(const std::filesystem::path& path);
    [[nodiscard]] bool IsPathInside(const std::filesystem::path& path, const std::filesystem::path& parent);
    [[nodiscard]] bool ContainsParentTraversal(const std::filesystem::path& path);
    [[nodiscard]] ResolvedPluginPath ResolvePluginOwnedPath(
        const std::filesystem::path& configuredPath,
        const std::filesystem::path& fallbackPath,
        std::string_view settingName);
    void EnsureBaseLayout(const std::filesystem::path& loadingScreensFolder);
}
