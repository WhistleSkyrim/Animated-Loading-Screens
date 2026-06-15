#pragma once

#include "Media/MediaLibrary.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ALS
{
    struct PlaylistEntry
    {
        std::filesystem::path relativePath{};
        int weight{ 1 };
        std::optional<double> durationOverrideSeconds{};
    };

    struct PlaylistLoadResult
    {
        bool valid{ false };
        std::vector<PlaylistEntry> entries{};
        std::vector<std::string> warnings{};
    };

    PlaylistLoadResult LoadPlaylist(const std::filesystem::path& playlistPath);
    std::vector<MediaFile> ResolvePlaylistMedia(
        const PlaylistLoadResult& playlist,
        const std::filesystem::path& loadingScreensFolder,
        const std::vector<std::string>& allowedExtensions,
        std::vector<std::string>& warnings);
}

