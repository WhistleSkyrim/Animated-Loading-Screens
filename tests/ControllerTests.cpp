#include "Controller/AnimatedLoadingScreenController.h"
#include "TestFramework.h"

#include <algorithm>
#include <vector>

namespace
{
    struct OpenRecord
    {
        std::filesystem::path path{};
        bool loopVideo{ false };
    };

    class FakeDecoder final : public ALS::IVideoDecoder
    {
    public:
        explicit FakeDecoder(
            double durationSeconds = 1.0,
            std::vector<OpenRecord>* records = nullptr,
            bool failOpen = false,
            bool finishImmediately = false) :
            durationSeconds_(durationSeconds),
            records_(records),
            failOpen_(failOpen),
            finishImmediately_(finishImmediately)
        {}

        bool Open(const std::filesystem::path& path, const ALS::DecoderOptions& options) override
        {
            if (failOpen_) {
                return false;
            }
            if (records_) {
                records_->push_back(OpenRecord{ path, options.loopVideo });
            }
            open_ = true;
            finished_ = finishImmediately_;
            return true;
        }

        void Close() override
        {
            open_ = false;
            finished_ = true;
        }

        [[nodiscard]] bool IsOpen() const override { return open_; }
        [[nodiscard]] bool IsFinished() const override { return finished_; }

        ALS::VideoFramePtr GetFrameForTime(double seconds) override
        {
            if (finishImmediately_) {
                return {};
            }
            if (seconds >= durationSeconds_) {
                finished_ = true;
            }
            auto frame = std::make_shared<ALS::VideoFrame>();
            frame->width = 2;
            frame->height = 2;
            frame->ptsSeconds = std::min(seconds, durationSeconds_);
            frame->bgra.assign(16, 255);
            return frame;
        }

        [[nodiscard]] ALS::VideoInfo GetInfo() const override
        {
            return ALS::VideoInfo{ 2, 2, 60.0, durationSeconds_, "fake" };
        }

    private:
        double durationSeconds_{ 1.0 };
        std::vector<OpenRecord>* records_{ nullptr };
        bool failOpen_{ false };
        bool finishImmediately_{ false };
        bool open_{ false };
        bool finished_{ false };
    };

    class FakeNoFrameDecoder final : public ALS::IVideoDecoder
    {
    public:
        explicit FakeNoFrameDecoder(bool finishedImmediately) :
            finishedImmediately_(finishedImmediately),
            finished_(finishedImmediately)
        {}

        bool Open(const std::filesystem::path& path, const ALS::DecoderOptions& options) override
        {
            (void)path;
            (void)options;
            open_ = true;
            finished_ = finishedImmediately_;
            return true;
        }

        void Close() override
        {
            open_ = false;
            finished_ = true;
        }

        [[nodiscard]] bool IsOpen() const override { return open_; }
        [[nodiscard]] bool IsFinished() const override { return finished_; }

        ALS::VideoFramePtr GetFrameForTime(double seconds) override
        {
            (void)seconds;
            return {};
        }

        [[nodiscard]] ALS::VideoInfo GetInfo() const override
        {
            return ALS::VideoInfo{ 2, 2, 60.0, 1.0, "fake-no-frame" };
        }

    private:
        bool finishedImmediately_{ false };
        bool open_{ false };
        bool finished_{ false };
    };
}

ALS_TEST(ControllerRepeatSingleHonorsLoopVideoSetting)
{
    double now = 0.0;
    std::vector<OpenRecord> records;

    ALS::Config config;
    config.transitions.fadeInMs = 0;
    config.playback.playbackMode = ALS::PlaybackMode::RepeatSingle;
    config.playback.loopVideo = false;

    std::vector<ALS::MediaFile> media{
        { "loop.mp4", 1, 1, std::nullopt }
    };
    ALS::MediaSelector selector(media, {}, ALS::SelectionMode::Sequential, false, {});

    ALS::AnimatedLoadingScreenController controller(
        config,
        std::move(selector),
        [&] {
            return std::make_unique<FakeDecoder>(1.0, &records);
        },
        [&] {
            return now;
        });

    controller.OnLoadingMenuOpen();
    (void)controller.BuildRenderFrame();

    ALS::Tests::ExpectEq(records.size(), static_cast<std::size_t>(1), "repeat_single should open one decoder");
    ALS::Tests::Expect(!records[0].loopVideo, "repeat_single should pass LoopVideo=false to decoder");

    now = 1.2;
    (void)controller.BuildRenderFrame();
    now = 1.3;
    (void)controller.BuildRenderFrame();

    ALS::Tests::ExpectEq(records.size(), static_cast<std::size_t>(1), "LoopVideo=false should not reopen repeat_single media");
    ALS::Tests::Expect(controller.State() == ALS::PlaybackState::Idle, "LoopVideo=false should stop after the single playthrough");
}

ALS_TEST(ControllerRepeatSingleStopsWhenReopenFails)
{
    double now = 0.0;
    int openAttempts = 0;

    ALS::Config config;
    config.transitions.fadeInMs = 0;
    config.playback.playbackMode = ALS::PlaybackMode::RepeatSingle;

    std::vector<ALS::MediaFile> media{
        { "loop.mp4", 1, 1, std::nullopt }
    };
    ALS::MediaSelector selector(media, {}, ALS::SelectionMode::Sequential, false, {});

    ALS::AnimatedLoadingScreenController controller(
        config,
        std::move(selector),
        [&] {
            ++openAttempts;
            return std::make_unique<FakeDecoder>(1.0, nullptr, openAttempts > 1);
        },
        [&] {
            return now;
        });

    controller.OnLoadingMenuOpen();
    (void)controller.BuildRenderFrame();

    now = 1.2;
    (void)controller.BuildRenderFrame();
    now = 1.3;
    (void)controller.BuildRenderFrame();

    ALS::Tests::ExpectEq(openAttempts, 2, "repeat_single should attempt to reopen finished media");
    ALS::Tests::Expect(controller.State() == ALS::PlaybackState::Idle, "repeat_single should stop after reopen failure");
}

ALS_TEST(ControllerNextAfterEndIgnoresLegacyLoopVideo)
{
    double now = 0.0;
    std::vector<OpenRecord> records;

    ALS::Config config;
    config.transitions.fadeInMs = 0;
    config.playback.playbackMode = ALS::PlaybackMode::NextAfterEnd;
    config.playback.loopVideo = true;
    config.playback.preloadNext = true;

    std::vector<ALS::MediaFile> media{
        { "a.mp4", 1, 1, std::nullopt },
        { "b.mp4", 1, 1, std::nullopt }
    };
    ALS::MediaSelector selector(media, {}, ALS::SelectionMode::Sequential, false, {});

    ALS::AnimatedLoadingScreenController controller(
        config,
        std::move(selector),
        [&] {
            return std::make_unique<FakeDecoder>(1.0, &records);
        },
        [&] {
            return now;
        });

    controller.OnLoadingMenuOpen();
    (void)controller.BuildRenderFrame();
    ALS::Tests::ExpectEq(records.size(), static_cast<std::size_t>(2), "next_after_end should preload next media");
    ALS::Tests::Expect(!records[0].loopVideo && !records[1].loopVideo, "next_after_end should not enable decoder looping");

    now = 1.2;
    (void)controller.BuildRenderFrame();
    now = 1.3;
    auto render = controller.BuildRenderFrame();
    ALS::Tests::Expect(controller.State() == ALS::PlaybackState::Playing, "next_after_end should promote next media");
    ALS::Tests::Expect(static_cast<bool>(render.current.frame), "promoted media should render");
    ALS::Tests::Expect(render.current.frame->ptsSeconds < 0.2, "promoted media should restart its timeline");
}

ALS_TEST(ControllerNextAfterEndDoesNotPreloadSingleFile)
{
    double now = 0.0;
    std::vector<OpenRecord> records;

    ALS::Config config;
    config.transitions.fadeInMs = 0;
    config.playback.playbackMode = ALS::PlaybackMode::NextAfterEnd;
    config.playback.preloadNext = true;

    std::vector<ALS::MediaFile> media{
        { "solo.mp4", 1, 1, std::nullopt }
    };
    ALS::MediaSelector selector(media, {}, ALS::SelectionMode::Sequential, false, {});

    ALS::AnimatedLoadingScreenController controller(
        config,
        std::move(selector),
        [&] {
            return std::make_unique<FakeDecoder>(1.0, &records);
        },
        [&] {
            return now;
        });

    controller.OnLoadingMenuOpen();
    (void)controller.BuildRenderFrame();
    ALS::Tests::ExpectEq(records.size(), static_cast<std::size_t>(1), "single-file next_after_end should not keep a preloaded duplicate decoder");

    now = 1.2;
    (void)controller.BuildRenderFrame();
    now = 1.3;
    (void)controller.BuildRenderFrame();

    ALS::Tests::ExpectEq(records.size(), static_cast<std::size_t>(2), "single-file next_after_end should reopen only after the current decoder finishes");
    ALS::Tests::Expect(controller.State() == ALS::PlaybackState::Playing, "single-file next_after_end should continue after reopening");
}

ALS_TEST(ControllerCrossfadeRendersCurrentAndNext)
{
    double now = 0.0;
    std::vector<OpenRecord> records;

    ALS::Config config;
    config.transitions.fadeInMs = 0;
    config.transitions.crossfadeMs = 500;
    config.transitions.enableCrossfade = true;
    config.playback.playbackMode = ALS::PlaybackMode::CrossfadePlaylist;
    config.playback.loopVideo = true;
    config.playback.preloadNext = true;

    std::vector<ALS::MediaFile> media{
        { "a.mp4", 1, 1, std::nullopt },
        { "b.mp4", 1, 1, std::nullopt }
    };
    ALS::MediaSelector selector(media, {}, ALS::SelectionMode::Sequential, false, {});

    ALS::AnimatedLoadingScreenController controller(
        config,
        std::move(selector),
        [&] {
            return std::make_unique<FakeDecoder>(1.0, &records);
        },
        [&] {
            return now;
        });

    controller.OnLoadingMenuOpen();
    (void)controller.BuildRenderFrame();
    ALS::Tests::ExpectEq(records.size(), static_cast<std::size_t>(2), "crossfade should preload next media");
    ALS::Tests::Expect(!records[0].loopVideo && !records[1].loopVideo, "crossfade should not enable decoder looping");

    now = 0.6;
    (void)controller.BuildRenderFrame();
    ALS::Tests::Expect(controller.State() == ALS::PlaybackState::Crossfading, "crossfade should begin before media ends");

    now = 0.85;
    const auto render = controller.BuildRenderFrame();
    ALS::Tests::Expect(static_cast<bool>(render.current.frame), "crossfade should render current frame");
    ALS::Tests::Expect(static_cast<bool>(render.next.frame), "crossfade should render next frame");
    ALS::Tests::Expect(render.current.alpha < 1.0F && render.next.alpha > 0.0F, "crossfade should blend both streams");
}

ALS_TEST(ControllerClampsVeryShortCrossfade)
{
    double now = 0.0;

    ALS::Config config;
    config.transitions.fadeInMs = 0;
    config.transitions.crossfadeMs = 1000;
    config.transitions.enableCrossfade = true;
    config.playback.playbackMode = ALS::PlaybackMode::CrossfadePlaylist;

    std::vector<ALS::MediaFile> media{
        { "a.mp4", 1, 1, 0.2 },
        { "b.mp4", 1, 1, 0.2 }
    };
    ALS::MediaSelector selector(media, {}, ALS::SelectionMode::Sequential, false, {});

    ALS::AnimatedLoadingScreenController controller(
        config,
        std::move(selector),
        [] {
            return std::make_unique<FakeDecoder>(0.2);
        },
        [&] {
            return now;
        });

    controller.OnLoadingMenuOpen();
    (void)controller.BuildRenderFrame();
    ALS::Tests::Expect(controller.State() == ALS::PlaybackState::Crossfading, "short media should start clamped crossfade immediately");

    now = 0.3;
    (void)controller.BuildRenderFrame();
    ALS::Tests::Expect(controller.State() == ALS::PlaybackState::Playing, "short clamped crossfade should complete");
}

ALS_TEST(ControllerTransitionsThroughOpenPlayAndClose)
{
    double now = 0.0;

    ALS::Config config;
    config.transitions.fadeInMs = 100;
    config.transitions.fadeOutMs = 100;
    config.playback.playbackMode = ALS::PlaybackMode::RepeatSingle;

    std::vector<ALS::MediaFile> media{
        { "loop.mp4", 1, 1, std::nullopt }
    };
    ALS::MediaSelector selector(media, {}, ALS::SelectionMode::Sequential, false, {});

    ALS::AnimatedLoadingScreenController controller(
        config,
        std::move(selector),
        [] {
            return std::make_unique<FakeDecoder>();
        },
        [&] {
            return now;
        });

    controller.OnLoadingMenuOpen();
    ALS::Tests::Expect(controller.State() == ALS::PlaybackState::FadingIn, "controller should fade in on open");
    ALS::Tests::Expect(controller.IsOverlayActive(), "overlay should become active after media starts");

    auto render = controller.BuildRenderFrame();
    ALS::Tests::Expect(render.shouldRender, "controller should produce render data");
    ALS::Tests::ExpectEq(render.current.alpha, 0.0F, "fade begins transparent");

    now = 0.2;
    render = controller.BuildRenderFrame();
    ALS::Tests::Expect(controller.State() == ALS::PlaybackState::Playing, "controller should transition to playing");
    ALS::Tests::ExpectEq(render.current.alpha, 1.0F, "playing should be opaque");

    controller.OnLoadingMenuClose();
    ALS::Tests::Expect(controller.State() == ALS::PlaybackState::Idle, "controller should stop immediately on close");
    ALS::Tests::Expect(!controller.IsOverlayActive(), "overlay should become inactive on close");

    now = 0.4;
    (void)controller.BuildRenderFrame();
    ALS::Tests::Expect(controller.State() == ALS::PlaybackState::Idle, "controller should stay stopped after close");
}

ALS_TEST(ControllerStartsAfterRuntimeMediaUpdate)
{
    double now = 0.0;

    ALS::Config config;
    config.transitions.fadeInMs = 0;
    config.playback.playbackMode = ALS::PlaybackMode::RepeatSingle;

    ALS::MediaSelector selector({}, {}, ALS::SelectionMode::Sequential, false, {});
    ALS::AnimatedLoadingScreenController controller(
        config,
        std::move(selector),
        [] {
            return std::make_unique<FakeDecoder>();
        },
        [&] {
            return now;
        });

    ALS::Tests::Expect(!controller.HasMedia(), "empty controller should report no media");
    controller.OnLoadingMenuOpen();
    ALS::Tests::Expect(!controller.IsOverlayActive(), "empty controller should fall back to vanilla");

    controller.UpdateMedia({ { "runtime-added.mp4", 1, 1, std::nullopt } }, {});
    ALS::Tests::Expect(controller.HasMedia(), "runtime refresh should make media available");

    controller.OnLoadingMenuOpen();
    const auto render = controller.BuildRenderFrame();

    ALS::Tests::Expect(controller.IsOverlayActive(), "controller should activate overlay after runtime media refresh");
    ALS::Tests::Expect(render.shouldRender, "refreshed media should render on the next LoadingMenu open");
}

ALS_TEST(ControllerDoesNotActivateOverlayWhenMediaCannotStart)
{
    double now = 0.0;

    ALS::Config config;
    config.transitions.fadeInMs = 0;
    config.playback.playbackMode = ALS::PlaybackMode::RepeatSingle;

    std::vector<ALS::MediaFile> media{
        { "broken.mp4", 1, 1, std::nullopt }
    };
    ALS::MediaSelector selector(media, {}, ALS::SelectionMode::Sequential, false, {});

    ALS::AnimatedLoadingScreenController controller(
        config,
        std::move(selector),
        [] {
            return std::make_unique<FakeDecoder>(1.0, nullptr, true);
        },
        [&] {
            return now;
        });

    controller.OnLoadingMenuOpen();

    ALS::Tests::Expect(controller.State() == ALS::PlaybackState::Idle, "failed media start should leave controller idle");
    ALS::Tests::Expect(!controller.IsOverlayActive(), "failed media start should not activate the overlay path");
}

ALS_TEST(ControllerHandlesUnicodeMediaPathInLoadingMenuLog)
{
    double now = 0.0;

    ALS::Config config;
    config.transitions.fadeInMs = 0;
    config.playback.playbackMode = ALS::PlaybackMode::RepeatSingle;

    std::filesystem::path unicodePath = std::filesystem::path(L"kling_\x041F\x0440\x0438\x0432\x0435\x0442.mp4");
    std::vector<ALS::MediaFile> media{
        { unicodePath, 1, 1, std::nullopt }
    };
    ALS::MediaSelector selector(media, {}, ALS::SelectionMode::Sequential, false, {});

    ALS::AnimatedLoadingScreenController controller(
        config,
        std::move(selector),
        [] {
            return std::make_unique<FakeDecoder>();
        },
        [&] {
            return now;
        });

    controller.OnLoadingMenuOpen();
    const auto render = controller.BuildRenderFrame();

    ALS::Tests::Expect(controller.IsOverlayActive(), "unicode media path should not break LoadingMenu open");
    ALS::Tests::Expect(render.shouldRender, "unicode media path should still render normally");
}

ALS_TEST(ControllerRendersBackgroundWhileWaitingForFirstFrame)
{
    double now = 0.0;

    ALS::Config config;
    config.display.coverVanillaLoadingScreen = true;
    config.transitions.fadeInMs = 0;
    config.playback.playbackMode = ALS::PlaybackMode::RepeatSingle;

    std::vector<ALS::MediaFile> media{
        { "warming-up.mp4", 1, 1, std::nullopt }
    };
    ALS::MediaSelector selector(media, {}, ALS::SelectionMode::Sequential, false, {});

    ALS::AnimatedLoadingScreenController controller(
        config,
        std::move(selector),
        [] {
            return std::make_unique<FakeNoFrameDecoder>(false);
        },
        [&] {
            return now;
        });

    controller.OnLoadingMenuOpen();
    const auto render = controller.BuildRenderFrame();

    ALS::Tests::Expect(render.shouldRender, "controller should render the cover background before the first decoded frame");
    ALS::Tests::Expect(render.drawBackground, "background intent should remain set while waiting for media");
    ALS::Tests::Expect(controller.State() == ALS::PlaybackState::Playing, "controller should keep waiting while the decoder is still running");
    ALS::Tests::Expect(controller.IsOverlayActive(), "overlay should remain active while waiting for the first frame");
}

ALS_TEST(ControllerFallsBackWithoutRenderingBackgroundWhenAsyncDecoderFailsWithoutFrames)
{
    double now = 0.0;

    ALS::Config config;
    config.display.coverVanillaLoadingScreen = true;
    config.transitions.fadeInMs = 0;
    config.playback.playbackMode = ALS::PlaybackMode::RepeatSingle;

    std::vector<ALS::MediaFile> media{
        { "async-fail.mp4", 1, 1, std::nullopt }
    };
    ALS::MediaSelector selector(media, {}, ALS::SelectionMode::Sequential, false, {});

    ALS::AnimatedLoadingScreenController controller(
        config,
        std::move(selector),
        [] {
            return std::make_unique<FakeNoFrameDecoder>(true);
        },
        [&] {
            return now;
        });

    controller.OnLoadingMenuOpen();
    const auto render = controller.BuildRenderFrame();

    ALS::Tests::Expect(!render.shouldRender, "async no-frame failure should not render a background-only overlay");
    ALS::Tests::Expect(controller.State() == ALS::PlaybackState::Idle, "async no-frame failure should fall back to vanilla");
    ALS::Tests::Expect(!controller.IsOverlayActive(), "async no-frame failure should deactivate overlay work");
}

ALS_TEST(ControllerFallsBackDuringFadeInWhenAsyncDecoderFailsWithoutFrames)
{
    double now = 0.0;

    ALS::Config config;
    config.display.coverVanillaLoadingScreen = true;
    config.transitions.fadeInMs = 350;
    config.playback.playbackMode = ALS::PlaybackMode::RepeatSingle;

    std::vector<ALS::MediaFile> media{
        { "fade-in-fail.mp4", 1, 1, std::nullopt }
    };
    ALS::MediaSelector selector(media, {}, ALS::SelectionMode::Sequential, false, {});

    ALS::AnimatedLoadingScreenController controller(
        config,
        std::move(selector),
        [] {
            return std::make_unique<FakeNoFrameDecoder>(true);
        },
        [&] {
            return now;
        });

    controller.OnLoadingMenuOpen();
    const auto render = controller.BuildRenderFrame();

    ALS::Tests::Expect(!render.shouldRender, "fade-in async failure should not render a background-only overlay");
    ALS::Tests::Expect(controller.State() == ALS::PlaybackState::Idle, "fade-in async no-frame failure should fall back immediately");
    ALS::Tests::Expect(!controller.IsOverlayActive(), "fade-in async no-frame failure should deactivate overlay work");
}
