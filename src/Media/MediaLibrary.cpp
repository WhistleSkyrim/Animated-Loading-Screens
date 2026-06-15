#include "Media/MediaLibrary.h"

#include "Utils/Paths.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace
{
    [[nodiscard]] std::string Lower(std::string value)
    {
        std::ranges::transform(value, value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    [[nodiscard]] std::unordered_set<std::string> BuildExtensionSet(const std::vector<std::string>& extensions)
    {
        std::unordered_set<std::string> result;
        for (auto extension : extensions) {
            extension = Lower(extension);
            if (!extension.empty() && extension.front() != '.') {
                extension.insert(extension.begin(), '.');
            }
            if (!extension.empty()) {
                result.insert(extension);
            }
        }
        return result;
    }

    [[nodiscard]] bool HasAllowedExtension(const std::filesystem::path& path, const std::unordered_set<std::string>& allowed)
    {
        return allowed.contains(Lower(ALS::Paths::ForLog(path.extension())));
    }

    [[nodiscard]] std::size_t ComputeMaxEntriesToVisit(std::size_t maxFilesToScan)
    {
        constexpr std::size_t EntryMultiplier = 8;
        constexpr std::size_t MinEntriesToVisit = 256;
        constexpr std::size_t MaxEntriesToVisit = 100000;

        if (maxFilesToScan == 0) {
            return 0;
        }

        const auto multiplied = maxFilesToScan > (std::numeric_limits<std::size_t>::max() / EntryMultiplier) ?
            std::numeric_limits<std::size_t>::max() :
            maxFilesToScan * EntryMultiplier;
        return std::min(std::max(multiplied, MinEntriesToVisit), MaxEntriesToVisit);
    }

    [[nodiscard]] bool IsDefaultLoadingScreensPath(const std::filesystem::path& path)
    {
        return Lower(ALS::Paths::ForLog(path.lexically_normal())) ==
            Lower(ALS::Paths::ForLog(ALS::Paths::DefaultLoadingScreensPath().lexically_normal()));
    }

    [[nodiscard]] bool IsUsableMediaCandidate(
        const std::filesystem::directory_entry& entry,
        const std::unordered_set<std::string>& allowed)
    {
        std::error_code ec;
        if (!entry.is_regular_file(ec)) {
            return false;
        }
        if (!HasAllowedExtension(entry.path(), allowed)) {
            return false;
        }
        const auto size = entry.file_size(ec);
        return !ec && size > 0;
    }

    [[nodiscard]] bool ContainsAllowedMedia(
        const std::filesystem::path& folder,
        const std::unordered_set<std::string>& allowed,
        bool scanSubfolders,
        std::size_t maxEntriesToVisit)
    {
        if (allowed.empty() || maxEntriesToVisit == 0) {
            return false;
        }

        std::error_code ec;
        if (!std::filesystem::is_directory(folder, ec)) {
            return false;
        }

        std::size_t visitedEntries = 0;
        const auto shouldStop = [&] {
            ++visitedEntries;
            return visitedEntries >= maxEntriesToVisit;
        };

        const auto options = std::filesystem::directory_options::skip_permission_denied;
        try {
            if (scanSubfolders) {
                for (std::filesystem::recursive_directory_iterator it(folder, options, ec), end; it != end && !ec; it.increment(ec)) {
                    if (IsUsableMediaCandidate(*it, allowed)) {
                        return true;
                    }
                    if (shouldStop()) {
                        break;
                    }
                }
            } else {
                for (std::filesystem::directory_iterator it(folder, options, ec), end; it != end && !ec; it.increment(ec)) {
                    if (IsUsableMediaCandidate(*it, allowed)) {
                        return true;
                    }
                    if (shouldStop()) {
                        break;
                    }
                }
            }
        } catch (const std::filesystem::filesystem_error&) {
            return false;
        }

        return false;
    }

    [[nodiscard]] std::filesystem::path SelectFolder(
        const ALS::Config& config,
        const std::unordered_set<std::string>& allowedExtensions,
        std::vector<std::string>& warnings)
    {
        auto resolved = ALS::Paths::ResolvePluginOwnedPath(
            config.general.loadingScreensFolder,
            ALS::Paths::DefaultLoadingScreensPath(),
            "General.LoadingScreensFolder");
        if (!resolved.safe) {
            warnings.emplace_back(resolved.warning);
        }
        auto configured = resolved.path;
        const auto legacy = ALS::Paths::LegacyLoadingScreensPath();

        if (std::filesystem::exists(configured)) {
            if (IsDefaultLoadingScreensPath(configured) && std::filesystem::exists(legacy)) {
                const auto maxEntriesToVisit = ComputeMaxEntriesToVisit(config.performance.maxFilesToScan);
                const auto configuredHasMedia = ContainsAllowedMedia(
                    configured,
                    allowedExtensions,
                    config.general.scanSubfolders,
                    maxEntriesToVisit);
                const auto legacyHasMedia = ContainsAllowedMedia(
                    legacy,
                    allowedExtensions,
                    config.general.scanSubfolders,
                    maxEntriesToVisit);
                if (!configuredHasMedia && legacyHasMedia) {
                    warnings.emplace_back("Using legacy media folder because default LoadingScreens has no media: " + ALS::Paths::ForLog(legacy));
                    return legacy;
                }
            }
            return configured;
        }
        if (std::filesystem::exists(legacy)) {
            return legacy;
        }
        return configured;
    }

    [[nodiscard]] std::string SortKey(const std::filesystem::path& path)
    {
        return Lower(ALS::Paths::ForLog(path.lexically_normal()));
    }

}

namespace ALS
{
    MediaScanResult MediaLibrary::Scan(const Config& config)
    {
        MediaScanResult result;
        const auto allowedExtensions = BuildExtensionSet(config.general.allowedExtensions);
        result.scannedFolder = SelectFolder(config, allowedExtensions, result.warnings);
        folder_ = result.scannedFolder;
        files_.clear();

        if (allowedExtensions.empty()) {
            result.warnings.emplace_back("AllowedExtensions is empty. No media will be scanned.");
            return result;
        }

        std::error_code ec;
        if (!std::filesystem::exists(result.scannedFolder, ec)) {
            result.warnings.emplace_back("Media folder does not exist: " + Paths::ForLog(result.scannedFolder));
            return result;
        }
        if (!std::filesystem::is_directory(result.scannedFolder, ec)) {
            result.warnings.emplace_back("Media folder is not a directory: " + Paths::ForLog(result.scannedFolder));
            return result;
        }

        const auto maxBytes = config.performance.skipFilesLargerThanMB > 0 ?
            config.performance.skipFilesLargerThanMB * 1024ULL * 1024ULL :
            0ULL;
        constexpr std::size_t MaxDetailedScanWarnings = 16;
        std::size_t detailedScanWarnings = 0;
        std::size_t suppressedScanWarnings = 0;
        const auto addDetailedScanWarning = [&](std::string warning) {
            if (detailedScanWarnings < MaxDetailedScanWarnings) {
                result.warnings.emplace_back(std::move(warning));
                ++detailedScanWarnings;
                return;
            }
            ++suppressedScanWarnings;
        };
        const auto addStatusWarning = [&](std::string warning) {
            result.warnings.emplace_back(std::move(warning));
        };
        const auto addCandidate = [&](const std::filesystem::directory_entry& entry) {
            std::error_code entryEc;
            if (!entry.is_regular_file(entryEc)) {
                return;
            }
            const auto path = entry.path();
            if (!HasAllowedExtension(path, allowedExtensions)) {
                return;
            }
            const auto size = entry.file_size(entryEc);
            if (entryEc) {
                addDetailedScanWarning("Unable to read file size: " + Paths::ForLog(path));
                return;
            }
            if (size == 0) {
                addDetailedScanWarning("Skipping empty media file: " + Paths::ForLog(path));
                return;
            }
            if (maxBytes > 0 && size > maxBytes) {
                addDetailedScanWarning("Skipping oversized media file: " + Paths::ForLog(path));
                return;
            }

            files_.push_back(MediaFile{ path.lexically_normal(), size, 1, std::nullopt });
        };

        const auto options = std::filesystem::directory_options::skip_permission_denied;
        const auto maxEntriesToVisit = ComputeMaxEntriesToVisit(config.performance.maxFilesToScan);
        if (maxEntriesToVisit == 0) {
            addStatusWarning("Media scan cap reached at 0 file(s).");
            result.files = files_;
            return result;
        }

        std::size_t visitedEntries = 0;
        bool reachedEntryCap = false;
        const auto shouldStopAfterEntry = [&]() {
            ++visitedEntries;
            if (maxEntriesToVisit > 0 && visitedEntries >= maxEntriesToVisit) {
                reachedEntryCap = true;
                return true;
            }
            return false;
        };
        try {
            if (config.general.scanSubfolders) {
                for (std::filesystem::recursive_directory_iterator it(result.scannedFolder, options, ec), end; it != end && !ec; it.increment(ec)) {
                    addCandidate(*it);
                    if (shouldStopAfterEntry()) {
                        break;
                    }
                }
            } else {
                for (std::filesystem::directory_iterator it(result.scannedFolder, options, ec), end; it != end && !ec; it.increment(ec)) {
                    addCandidate(*it);
                    if (shouldStopAfterEntry()) {
                        break;
                    }
                }
            }
        } catch (const std::filesystem::filesystem_error& e) {
            addStatusWarning("Media scan filesystem error: " + std::string(e.what()));
        }

        if (reachedEntryCap) {
            addStatusWarning("Media scan entry cap reached at " + std::to_string(visitedEntries) + " visited entries.");
        }
        if (ec) {
            addStatusWarning("Media scan stopped early: " + ec.message());
        }

        std::ranges::sort(files_, [](const MediaFile& left, const MediaFile& right) {
            return SortKey(left.path) < SortKey(right.path);
        });
        if (files_.size() > config.performance.maxFilesToScan) {
            files_.resize(config.performance.maxFilesToScan);
            addStatusWarning("Media scan cap reached at " + std::to_string(config.performance.maxFilesToScan) + " file(s).");
        }
        if (suppressedScanWarnings > 0) {
            addStatusWarning("Suppressed " + std::to_string(suppressedScanWarnings) + " additional media scan warning(s).");
        }

        result.files = files_;
        return result;
    }
}
