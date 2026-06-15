#include "Media/Playlist.h"
#include "TestFramework.h"

#include <fstream>

namespace
{
    void WriteMedia(const std::filesystem::path& path)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output << "data";
    }
}

ALS_TEST(PlaylistParsesAndRejectsUnsafePaths)
{
    const auto root = ALS::Tests::MakeTempDir("PlaylistParses");
    const auto playlistPath = root / "default.txt";
    std::ofstream playlist(playlistPath);
    playlist << "# comment\n";
    playlist << "intro.mp4 | 10\n";
    playlist << "nested/dark.webm | 5 | 8.5\n";
    playlist << "../escape.mp4 | 99\n";
    playlist << "C:/absolute.mp4 | 1\n";
    playlist.close();

    const auto loaded = ALS::LoadPlaylist(playlistPath);
    ALS::Tests::Expect(loaded.valid, "playlist should have valid entries");
    ALS::Tests::ExpectEq(loaded.entries.size(), static_cast<std::size_t>(2), "unsafe entries should be rejected");

    const auto mediaRoot = root / "LoadingScreens";
    WriteMedia(mediaRoot / "intro.mp4");
    WriteMedia(mediaRoot / "nested" / "dark.webm");

    std::vector<std::string> warnings;
    const auto resolved = ALS::ResolvePlaylistMedia(loaded, mediaRoot, { ".mp4", ".webm" }, warnings);
    ALS::Tests::ExpectEq(resolved.size(), static_cast<std::size_t>(2), "playlist entries should resolve to media files");
    ALS::Tests::ExpectEq(resolved[0].weight, 10, "weight should parse");
}

