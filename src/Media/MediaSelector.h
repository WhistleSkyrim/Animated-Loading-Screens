#pragma once

#include "Config/Config.h"
#include "Media/MediaLibrary.h"

#include <filesystem>
#include <optional>
#include <random>
#include <vector>

namespace ALS
{
    class MediaSelector
    {
    public:
        MediaSelector() = default;
        MediaSelector(
            std::vector<MediaFile> scannedFiles,
            std::vector<MediaFile> playlistFiles,
            SelectionMode mode,
            bool rememberLast,
            std::filesystem::path statePath);

        [[nodiscard]] bool Empty() const noexcept;
        [[nodiscard]] std::size_t Size() const noexcept;
        [[nodiscard]] const std::vector<MediaFile>& ActiveFiles() const noexcept;

        std::optional<MediaFile> SelectNext();
        void CommitSelection(const MediaFile& media) const;
        void Reset(
            std::vector<MediaFile> scannedFiles,
            std::vector<MediaFile> playlistFiles,
            SelectionMode mode,
            bool rememberLast,
            std::filesystem::path statePath);

    private:
        std::vector<MediaFile> scannedFiles_{};
        std::vector<MediaFile> playlistFiles_{};
        SelectionMode mode_{ SelectionMode::Random };
        bool rememberLast_{ false };
        std::filesystem::path statePath_{};
        std::size_t sequentialIndex_{ 0 };
        std::mt19937 rng_{ std::random_device{}() };

        [[nodiscard]] const std::vector<MediaFile>& FilesForMode() const noexcept;
        std::optional<MediaFile> SelectRandom();
        std::optional<MediaFile> SelectSequential();
        std::optional<MediaFile> SelectWeightedRandom();
        void LoadState();
        void SaveState(const std::filesystem::path& path) const;
    };
}
