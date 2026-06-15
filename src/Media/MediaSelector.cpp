#include "Media/MediaSelector.h"

#include "Utils/Paths.h"

#include <algorithm>
#include <fstream>
#include <numeric>

namespace
{
    [[nodiscard]] bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right)
    {
        return left.lexically_normal() == right.lexically_normal();
    }
}

namespace ALS
{
    MediaSelector::MediaSelector(
        std::vector<MediaFile> scannedFiles,
        std::vector<MediaFile> playlistFiles,
        SelectionMode mode,
        bool rememberLast,
        std::filesystem::path statePath)
    {
        Reset(std::move(scannedFiles), std::move(playlistFiles), mode, rememberLast, std::move(statePath));
    }

    void MediaSelector::Reset(
        std::vector<MediaFile> scannedFiles,
        std::vector<MediaFile> playlistFiles,
        SelectionMode mode,
        bool rememberLast,
        std::filesystem::path statePath)
    {
        scannedFiles_ = std::move(scannedFiles);
        playlistFiles_ = std::move(playlistFiles);
        mode_ = mode;
        rememberLast_ = rememberLast;
        statePath_ = std::move(statePath);
        sequentialIndex_ = 0;
        LoadState();
    }

    bool MediaSelector::Empty() const noexcept
    {
        return ActiveFiles().empty();
    }

    std::size_t MediaSelector::Size() const noexcept
    {
        return ActiveFiles().size();
    }

    const std::vector<MediaFile>& MediaSelector::ActiveFiles() const noexcept
    {
        return FilesForMode();
    }

    std::optional<MediaFile> MediaSelector::SelectNext()
    {
        std::optional<MediaFile> selected;
        switch (mode_) {
        case SelectionMode::Sequential:
            selected = SelectSequential();
            break;
        case SelectionMode::WeightedRandom:
            selected = SelectWeightedRandom();
            break;
        case SelectionMode::Random:
        default:
            selected = SelectRandom();
            break;
        }

        return selected;
    }

    void MediaSelector::CommitSelection(const MediaFile& media) const
    {
        if (rememberLast_) {
            SaveState(media.path);
        }
    }

    const std::vector<MediaFile>& MediaSelector::FilesForMode() const noexcept
    {
        if (mode_ == SelectionMode::WeightedRandom && !playlistFiles_.empty()) {
            return playlistFiles_;
        }
        return scannedFiles_;
    }

    std::optional<MediaFile> MediaSelector::SelectRandom()
    {
        const auto& files = FilesForMode();
        if (files.empty()) {
            return std::nullopt;
        }
        std::uniform_int_distribution<std::size_t> distribution(0, files.size() - 1);
        return files[distribution(rng_)];
    }

    std::optional<MediaFile> MediaSelector::SelectSequential()
    {
        const auto& files = FilesForMode();
        if (files.empty()) {
            return std::nullopt;
        }
        const auto index = sequentialIndex_ % files.size();
        sequentialIndex_ = (sequentialIndex_ + 1) % files.size();
        return files[index];
    }

    std::optional<MediaFile> MediaSelector::SelectWeightedRandom()
    {
        const auto& files = FilesForMode();
        if (files.empty()) {
            return std::nullopt;
        }

        const auto totalWeight = std::accumulate(files.begin(), files.end(), 0LL, [](long long sum, const MediaFile& file) {
            return sum + std::max(file.weight, 0);
        });
        if (totalWeight <= 0) {
            return std::nullopt;
        }

        std::uniform_int_distribution<long long> distribution(1, totalWeight);
        auto ticket = distribution(rng_);
        for (const auto& file : files) {
            const auto weight = std::max(file.weight, 0);
            if (weight == 0) {
                continue;
            }
            ticket -= weight;
            if (ticket <= 0) {
                return file;
            }
        }
        return std::nullopt;
    }

    void MediaSelector::LoadState()
    {
        if (!rememberLast_ || statePath_.empty()) {
            return;
        }

        std::ifstream input(statePath_);
        std::string lastPath;
        if (!std::getline(input, lastPath) || lastPath.empty()) {
            return;
        }

        const auto& files = FilesForMode();
        const auto found = std::ranges::find_if(files, [&](const MediaFile& file) {
            return SamePath(file.path, lastPath);
        });
        if (found != files.end()) {
            sequentialIndex_ = (static_cast<std::size_t>(std::distance(files.begin(), found)) + 1) % files.size();
        }
    }

    void MediaSelector::SaveState(const std::filesystem::path& path) const
    {
        if (statePath_.empty()) {
            return;
        }
        if (statePath_.has_parent_path()) {
            std::filesystem::create_directories(statePath_.parent_path());
        }
        std::ofstream output(statePath_, std::ios::trunc);
        output << ALS::Paths::ForLog(path.lexically_normal()) << '\n';
    }
}
