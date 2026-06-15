#include "Media/Playlist.h"

#include "Utils/Paths.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <unordered_set>

namespace
{
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

    [[nodiscard]] bool ContainsParentEscape(const std::filesystem::path& path)
    {
        for (const auto& component : path) {
            if (component == "..") {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::vector<std::string> SplitPipe(const std::string& line)
    {
        std::vector<std::string> parts;
        std::stringstream stream(line);
        std::string part;
        while (std::getline(stream, part, '|')) {
            parts.push_back(Trim(part));
        }
        return parts;
    }

    [[nodiscard]] std::unordered_set<std::string> BuildExtensionSet(const std::vector<std::string>& extensions)
    {
        std::unordered_set<std::string> result;
        for (auto extension : extensions) {
            extension = Lower(extension);
            if (!extension.empty() && extension.front() != '.') {
                extension.insert(extension.begin(), '.');
            }
            result.insert(extension);
        }
        return result;
    }

    template <class T>
    [[nodiscard]] std::optional<T> ParseStrictNumber(const std::string& text)
    {
        const auto value = Trim(text);
        T parsed{};
        const auto* begin = value.data();
        const auto* end = begin + value.size();
        const auto result = std::from_chars(begin, end, parsed);
        if (value.empty() || result.ec != std::errc{} || result.ptr != end) {
            return std::nullopt;
        }
        if constexpr (std::is_floating_point_v<T>) {
            if (!std::isfinite(parsed)) {
                return std::nullopt;
            }
        }
        return parsed;
    }
}

namespace ALS
{
    PlaylistLoadResult LoadPlaylist(const std::filesystem::path& playlistPath)
    {
        PlaylistLoadResult result;
        std::ifstream input(playlistPath);
        if (!input) {
            result.warnings.emplace_back("Playlist not found: " + Paths::ForLog(playlistPath));
            return result;
        }

        std::string line;
        std::size_t lineNumber = 0;
        while (std::getline(input, line)) {
            ++lineNumber;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            const auto trimmed = Trim(line);
            if (trimmed.empty() || trimmed.front() == '#') {
                continue;
            }

            const auto parts = SplitPipe(trimmed);
            if (parts.size() < 2 || parts.size() > 3) {
                result.warnings.emplace_back("Invalid playlist line " + std::to_string(lineNumber));
                continue;
            }

            std::filesystem::path relativePath(parts[0]);
            if (relativePath.is_absolute() || relativePath.has_root_name()) {
                result.warnings.emplace_back("Rejecting absolute playlist path on line " + std::to_string(lineNumber));
                continue;
            }
            if (ContainsParentEscape(relativePath)) {
                result.warnings.emplace_back("Rejecting playlist path escape on line " + std::to_string(lineNumber));
                continue;
            }

            const auto parsedWeight = ParseStrictNumber<int>(parts[1]);
            if (!parsedWeight) {
                result.warnings.emplace_back("Invalid playlist weight on line " + std::to_string(lineNumber));
                continue;
            }
            const auto weight = std::clamp(*parsedWeight, -100000, 100000);

            std::optional<double> durationOverride;
            if (parts.size() == 3 && !parts[2].empty()) {
                durationOverride = ParseStrictNumber<double>(parts[2]);
                if (!durationOverride) {
                    result.warnings.emplace_back("Invalid duration override on line " + std::to_string(lineNumber));
                    continue;
                }
                if (*durationOverride <= 0.0) {
                    result.warnings.emplace_back("Ignoring non-positive duration override on line " + std::to_string(lineNumber));
                    durationOverride.reset();
                }
            }

            if (*parsedWeight != weight) {
                result.warnings.emplace_back("Playlist weight clamped on line " + std::to_string(lineNumber));
            }

            result.entries.push_back(PlaylistEntry{ relativePath.lexically_normal(), weight, durationOverride });
        }

        result.valid = !result.entries.empty();
        if (!result.valid) {
            result.warnings.emplace_back("Playlist has no valid entries: " + Paths::ForLog(playlistPath));
        }
        return result;
    }

    std::vector<MediaFile> ResolvePlaylistMedia(
        const PlaylistLoadResult& playlist,
        const std::filesystem::path& loadingScreensFolder,
        const std::vector<std::string>& allowedExtensions,
        std::vector<std::string>& warnings)
    {
        std::vector<MediaFile> result;
        if (!playlist.valid) {
            return result;
        }

        const auto allowed = BuildExtensionSet(allowedExtensions);
        for (const auto& entry : playlist.entries) {
            const auto path = (loadingScreensFolder / entry.relativePath).lexically_normal();
            if (!ALS::Paths::IsPathInside(path, loadingScreensFolder)) {
                warnings.emplace_back("Playlist entry escapes loading screen folder: " + Paths::ForLog(entry.relativePath));
                continue;
            }
            const auto extension = Lower(Paths::ForLog(path.extension()));
            if (!allowed.contains(extension)) {
                warnings.emplace_back("Playlist entry has disallowed extension: " + Paths::ForLog(path));
                continue;
            }

            std::error_code ec;
            if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec)) {
                warnings.emplace_back("Playlist entry does not exist: " + Paths::ForLog(path));
                continue;
            }

            const auto size = std::filesystem::file_size(path, ec);
            if (ec || size == 0) {
                warnings.emplace_back("Playlist entry is empty or unreadable: " + Paths::ForLog(path));
                continue;
            }

            result.push_back(MediaFile{ path, size, entry.weight, entry.durationOverrideSeconds });
        }

        std::ranges::sort(result, [](const MediaFile& left, const MediaFile& right) {
            return Paths::ForLog(left.path) < Paths::ForLog(right.path);
        });
        return result;
    }
}
