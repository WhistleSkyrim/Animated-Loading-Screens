#include "Media/MediaLibrary.h"
#include "TestFramework.h"
#include "Utils/Paths.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    void WriteBytes(const std::filesystem::path& path, std::size_t count)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        for (std::size_t i = 0; i < count; ++i) {
            output.put(static_cast<char>('a' + (i % 26)));
        }
    }

    [[nodiscard]] bool HasWarningContaining(const std::vector<std::string>& warnings, std::string_view text)
    {
        return std::ranges::any_of(warnings, [text](const std::string& warning) {
            return warning.find(text) != std::string::npos;
        });
    }
}

ALS_TEST(MediaLibraryScansDeterministicallyAndSkipsInvalidFiles)
{
    const auto root = ALS::Tests::MakeTempDir("MediaLibraryScans");
    const ALS::Tests::ScopedCurrentPath currentPath(root);
    const auto media = root / "Data" / "SKSE" / "Plugins" / "AnimatedLoadingScreens" / "LoadingScreens";
    WriteBytes(media / "b.webm", 16);
    WriteBytes(media / "a.mp4", 16);
    WriteBytes(media / "ignored.txt", 16);
    WriteBytes(media / "empty.mkv", 0);
    WriteBytes(media / "sub" / "c.mov", 16);

    ALS::Config config;
    config.general.loadingScreensFolder = "LoadingScreens";
    config.general.scanSubfolders = true;
    config.general.allowedExtensions = { ".mp4", ".webm", ".mkv", ".mov" };
    config.performance.maxFilesToScan = 50;
    config.performance.skipFilesLargerThanMB = 64;

    ALS::MediaLibrary library;
    const auto result = library.Scan(config);

    ALS::Tests::ExpectEq(result.files.size(), static_cast<std::size_t>(3), "three valid media files should be scanned");
    ALS::Tests::Expect(result.files[0].path.filename() == "a.mp4", "files should be sorted deterministically");
    ALS::Tests::Expect(result.files[1].path.filename() == "b.webm", "files should be sorted deterministically");
    ALS::Tests::Expect(!result.warnings.empty(), "empty file should produce a warning");
}

ALS_TEST(MediaLibraryStopsAtScanCapAndRejectsAbsoluteFolder)
{
    const auto root = ALS::Tests::MakeTempDir("MediaLibraryStopsAtScanCap");
    const ALS::Tests::ScopedCurrentPath currentPath(root);
    const auto media = root / "Data" / "SKSE" / "Plugins" / "AnimatedLoadingScreens" / "LoadingScreens";
    for (int i = 0; i < 10; ++i) {
        WriteBytes(media / ("file" + std::to_string(i) + ".mp4"), 16);
    }

    ALS::Config config;
    config.general.loadingScreensFolder = "LoadingScreens";
    config.general.scanSubfolders = true;
    config.general.allowedExtensions = { ".mp4" };
    config.performance.maxFilesToScan = 2;
    config.performance.skipFilesLargerThanMB = 64;

    ALS::MediaLibrary library;
    const auto capped = library.Scan(config);
    ALS::Tests::ExpectEq(capped.files.size(), static_cast<std::size_t>(2), "scan should stop at configured cap");
    ALS::Tests::Expect(!capped.warnings.empty(), "scan cap should warn");
    ALS::Tests::Expect(capped.files[0].path.filename() == "file0.mp4", "capped files should be sorted before trimming");
    ALS::Tests::Expect(capped.files[1].path.filename() == "file1.mp4", "capped files should be sorted before trimming");

    config.general.loadingScreensFolder = root / "Outside";
    const auto rejected = library.Scan(config);
    ALS::Tests::ExpectEq(rejected.files.size(), static_cast<std::size_t>(2), "absolute folder should fall back to default plugin folder");
    ALS::Tests::Expect(!rejected.warnings.empty(), "absolute folder rejection should warn");
}

ALS_TEST(MediaLibraryFallsBackToLegacyFolderWhenDefaultIsEmpty)
{
    const auto root = ALS::Tests::MakeTempDir("MediaLibraryLegacyFallback");
    const ALS::Tests::ScopedCurrentPath currentPath(root);

    ALS::Config config;
    config.general.loadingScreensFolder = "LoadingScreens";
    config.general.scanSubfolders = true;
    config.general.allowedExtensions = { ".mp4" };
    config.performance.maxFilesToScan = 50;
    config.performance.skipFilesLargerThanMB = 64;

    ALS::Paths::EnsureBaseLayout(config.general.loadingScreensFolder);
    const auto legacy = root / "Data" / "SKSE" / "Plugins" / "AnimatedLoadingScreens" / "Loading Screens";
    WriteBytes(legacy / "legacy.mp4", 16);

    ALS::MediaLibrary library;
    const auto result = library.Scan(config);

    ALS::Tests::ExpectEq(result.files.size(), static_cast<std::size_t>(1), "legacy media should be scanned when default folder is empty");
    ALS::Tests::Expect(result.files[0].path.filename() == "legacy.mp4", "legacy file should be selected");
    ALS::Tests::Expect(result.scannedFolder.filename() == "Loading Screens", "scan result should identify the legacy folder");
}

ALS_TEST(MediaLibraryBoundsDirtyFolderScanAndWarningSpam)
{
    const auto root = ALS::Tests::MakeTempDir("MediaLibraryDirtyFolderScan");
    const ALS::Tests::ScopedCurrentPath currentPath(root);
    const auto media = root / "Data" / "SKSE" / "Plugins" / "AnimatedLoadingScreens" / "LoadingScreens";
    for (int i = 0; i < 300; ++i) {
        WriteBytes(media / ("empty" + std::to_string(i) + ".mp4"), 0);
    }

    ALS::Config config;
    config.general.loadingScreensFolder = "LoadingScreens";
    config.general.scanSubfolders = true;
    config.general.allowedExtensions = { ".mp4" };
    config.performance.maxFilesToScan = 1;
    config.performance.skipFilesLargerThanMB = 64;

    ALS::MediaLibrary library;
    const auto result = library.Scan(config);

    ALS::Tests::Expect(result.files.empty(), "empty files should not be added as media");
    ALS::Tests::Expect(result.warnings.size() <= 18, "dirty folder scan should cap detailed warning spam");
    ALS::Tests::Expect(HasWarningContaining(result.warnings, "Media scan entry cap reached"), "dirty folder scan should stop at the entry cap");
    ALS::Tests::Expect(HasWarningContaining(result.warnings, "Suppressed"), "dirty folder scan should summarize suppressed warnings");
}

ALS_TEST(TestFrameworkMakeTempDirUsesUniqueRunPaths)
{
    const auto first = ALS::Tests::MakeTempDir("MediaLibraryTempUniqueness");
    const auto second = ALS::Tests::MakeTempDir("MediaLibraryTempUniqueness");

    ALS::Tests::Expect(first != second, "temp dirs with the same test name should be unique per call");
    ALS::Tests::Expect(std::filesystem::exists(first), "first temp dir should still exist");
    ALS::Tests::Expect(std::filesystem::exists(second), "second temp dir should exist");
}
