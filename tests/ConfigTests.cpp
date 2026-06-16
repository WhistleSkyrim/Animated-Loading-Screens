#include "Config/Config.h"
#include "TestFramework.h"

#include <fstream>

ALS_TEST(ConfigCreatesDefaultAndParsesValues)
{
    const auto root = ALS::Tests::MakeTempDir("ConfigCreatesDefaultAndParsesValues");
    const auto path = root / "AnimatedLoadingScreens.ini";

    const auto created = ALS::LoadOrCreateConfig(path);
    ALS::Tests::Expect(created.createdDefault, "default config should be created");
    ALS::Tests::Expect(created.config.general.enabled, "default config should enable plugin");
    ALS::Tests::Expect(!created.config.general.enableInVR, "VR should be opt-in by default");
    ALS::Tests::Expect(!created.config.display.hideVanillaLoadingSpinner, "vanilla loading spinner should remain visible by default");
    ALS::Tests::ExpectEq(ALS::ToString(created.config.playback.playbackMode), std::string("repeat_single"), "default playback mode");
    ALS::Tests::ExpectEq(created.config.playback.targetFPS, 60.0, "default target fps should preserve 60 FPS videos");

    std::ofstream output(path, std::ios::trunc);
    output << "[General]\n";
    output << "Enabled=false\n";
    output << "SelectionMode=sequential\n";
    output << "AllowedExtensions=mp4,.webm\n";
    output << "[Display]\n";
    output << "FitMode=contain\n";
    output << "Opacity=0.5\n";
    output << "HideVanillaLoadingSpinner=true\n";
    output.close();

    const auto parsed = ALS::LoadOrCreateConfig(path);
    ALS::Tests::Expect(!parsed.config.general.enabled, "Enabled=false should parse");
    ALS::Tests::ExpectEq(ALS::ToString(parsed.config.general.selectionMode), std::string("sequential"), "selection mode should parse");
    ALS::Tests::ExpectEq(parsed.config.general.allowedExtensions.size(), static_cast<std::size_t>(2), "extensions should parse");
    ALS::Tests::ExpectEq(ALS::ToString(parsed.config.display.fitMode), std::string("contain"), "fit mode should parse");
    ALS::Tests::Expect(parsed.config.display.hideVanillaLoadingSpinner, "spinner hiding should parse");
}

ALS_TEST(ConfigRejectsMalformedNumbersAndClampsBounds)
{
    const auto root = ALS::Tests::MakeTempDir("ConfigRejectsMalformedNumbersAndClampsBounds");
    const auto path = root / "AnimatedLoadingScreens.ini";

    std::ofstream output(path, std::ios::trunc);
    output << "[Playback]\n";
    output << "PlaybackSpeed=2.0x\n";
    output << "FrameQueueSize=999\n";
    output << "MaxDecodeWidth=1\n";
    output << "MaxDecodeHeight=99999\n";
    output << "TargetFPS=500\n";
    output << "[Transitions]\n";
    output << "FadeInMs=-5\n";
    output << "CrossfadeMs=99999\n";
    output << "[Performance]\n";
    output << "MaxDecoderThreads=99\n";
    output << "MaxFilesToScan=20000\n";
    output << "SkipFilesLargerThanMB=999999\n";
    output.close();

    const auto parsed = ALS::LoadOrCreateConfig(path);
    ALS::Tests::ExpectEq(parsed.config.playback.playbackSpeed, 1.0, "malformed number should keep default");
    ALS::Tests::ExpectEq(parsed.config.playback.frameQueueSize, static_cast<std::size_t>(16), "queue size should clamp");
    ALS::Tests::ExpectEq(parsed.config.playback.maxDecodeWidth, 64, "decode width should clamp up");
    ALS::Tests::ExpectEq(parsed.config.playback.maxDecodeHeight, 4096, "decode height should clamp down");
    ALS::Tests::ExpectEq(parsed.config.playback.targetFPS, 240.0, "target fps should clamp");
    ALS::Tests::ExpectEq(parsed.config.transitions.fadeInMs, 0, "fade should clamp to zero");
    ALS::Tests::ExpectEq(parsed.config.transitions.crossfadeMs, 10000, "crossfade should clamp");
    ALS::Tests::ExpectEq(parsed.config.performance.maxDecoderThreads, 4, "decoder thread cap should clamp");
    ALS::Tests::ExpectEq(parsed.config.performance.maxFilesToScan, static_cast<std::size_t>(10000), "scan cap should clamp");
    ALS::Tests::ExpectEq(parsed.config.performance.skipFilesLargerThanMB, static_cast<std::uintmax_t>(32768), "file size cap should clamp");
    ALS::Tests::Expect(!parsed.warnings.empty(), "invalid config should produce warnings");
}

ALS_TEST(ConfigRejectsUnsafeLoadingScreensFolder)
{
    const auto root = ALS::Tests::MakeTempDir("ConfigRejectsUnsafeLoadingScreensFolder");
    const auto path = root / "AnimatedLoadingScreens.ini";

    std::ofstream output(path, std::ios::trunc);
    output << "[General]\n";
    output << "LoadingScreensFolder=" << (root / "Outside").string() << "\n";
    output.close();

    const auto parsed = ALS::LoadOrCreateConfig(path);
    ALS::Tests::Expect(parsed.config.general.loadingScreensFolder == "LoadingScreens", "absolute media path should be rejected to default");
    ALS::Tests::Expect(!parsed.warnings.empty(), "path rejection should warn");
}
