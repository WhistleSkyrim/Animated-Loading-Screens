#pragma once

#include "Config/Config.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ALS
{
    struct MediaFile
    {
        std::filesystem::path path{};
        std::uintmax_t sizeBytes{ 0 };
        int weight{ 1 };
        std::optional<double> durationOverrideSeconds{};
    };

    struct MediaScanResult
    {
        std::filesystem::path scannedFolder{};
        std::vector<MediaFile> files{};
        std::vector<std::string> warnings{};
    };

    class MediaLibrary
    {
    public:
        MediaScanResult Scan(const Config& config);

        [[nodiscard]] const std::vector<MediaFile>& Files() const noexcept { return files_; }
        [[nodiscard]] const std::filesystem::path& Folder() const noexcept { return folder_; }

    private:
        std::filesystem::path folder_{};
        std::vector<MediaFile> files_{};
    };
}

