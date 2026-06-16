#include "Controller/AnimatedLoadingScreenController.h"

#include "Logging/Log.h"
#include "Utils/Paths.h"
#include "Utils/Math.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace
{
    [[nodiscard]] double SteadySeconds()
    {
        using clock = std::chrono::steady_clock;
        return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    }

    [[nodiscard]] bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right)
    {
        return left.lexically_normal().generic_string() == right.lexically_normal().generic_string();
    }

    void CloseDecoderNow(std::unique_ptr<ALS::IVideoDecoder> decoder) noexcept
    {
        if (!decoder) {
            return;
        }

        try {
            decoder->Close();
        } catch (const std::exception& e) {
            ALS::Log::error("Exception while retiring decoder: {}", e.what());
        } catch (...) {
            ALS::Log::error("Unknown exception while retiring decoder.");
        }
    }

    class DecoderCloseQueue
    {
    public:
        DecoderCloseQueue() :
            worker_([this] {
                Run();
            })
        {}

        ~DecoderCloseQueue()
        {
            {
                std::lock_guard lock(mutex_);
                stopping_ = true;
            }
            queueChanged_.notify_one();
            if (worker_.joinable()) {
                worker_.join();
            }
        }

        DecoderCloseQueue(const DecoderCloseQueue&) = delete;
        DecoderCloseQueue& operator=(const DecoderCloseQueue&) = delete;

        void Retire(std::unique_ptr<ALS::IVideoDecoder> decoder)
        {
            if (!decoder) {
                return;
            }

            auto closeInline = false;
            {
                std::lock_guard lock(mutex_);
                if (stopping_) {
                    closeInline = true;
                } else {
                    decoders_.push_back(std::move(decoder));
                }
            }

            if (closeInline) {
                CloseDecoderNow(std::move(decoder));
                return;
            }
            queueChanged_.notify_one();
        }

    private:
        void Run()
        {
            while (true) {
                std::unique_ptr<ALS::IVideoDecoder> decoder;
                {
                    std::unique_lock lock(mutex_);
                    queueChanged_.wait(lock, [&] {
                        return stopping_ || !decoders_.empty();
                    });
                    if (decoders_.empty()) {
                        if (stopping_) {
                            return;
                        }
                        continue;
                    }
                    decoder = std::move(decoders_.front());
                    decoders_.pop_front();
                }

                CloseDecoderNow(std::move(decoder));
            }
        }

        std::mutex mutex_{};
        std::condition_variable queueChanged_{};
        std::deque<std::unique_ptr<ALS::IVideoDecoder>> decoders_{};
        std::thread worker_{};
        bool stopping_{ false };
    };

    DecoderCloseQueue& DecoderRetirementQueue()
    {
        static DecoderCloseQueue queue;
        return queue;
    }

    void RetireDecoder(std::unique_ptr<ALS::IVideoDecoder> decoder)
    {
        DecoderRetirementQueue().Retire(std::move(decoder));
    }
}

namespace ALS
{
    AnimatedLoadingScreenController::AnimatedLoadingScreenController(
        Config config,
        MediaSelector selector,
        DecoderFactory decoderFactory,
        ClockFn clock) :
        config_(std::move(config)),
        selector_(std::move(selector)),
        decoderFactory_(std::move(decoderFactory)),
        clock_(std::move(clock))
    {
        if (!clock_) {
            clock_ = SteadySeconds;
        }
        (void)DecoderRetirementQueue();
    }

    AnimatedLoadingScreenController::~AnimatedLoadingScreenController()
    {
        Stop();
    }

    void AnimatedLoadingScreenController::OnLoadingMenuOpen()
    {
        std::lock_guard lock(mutex_);
        if (!config_.general.enabled) {
            Log::diagnostic("controller_open_ignored enabled=false");
            return;
        }
        if (loadingMenuOpen_ && state_ != PlaybackState::Idle) {
            Log::diagnostic(
                "controller_open_ignored already_open=true state={} overlay_active={}",
                static_cast<int>(state_),
                overlayActive_.load(std::memory_order_acquire));
            return;
        }

        Log::diagnostic(
            "controller_open_begin state={} has_media={} overlay_active={}",
            static_cast<int>(state_),
            !selector_.Empty(),
            overlayActive_.load(std::memory_order_acquire));
        StopLocked();
        loadingMenuOpen_ = true;
        failuresThisMenu_ = 0;

        const auto now = Now();
        if (!TryStartSelectedLocked(now)) {
            loadingMenuOpen_ = false;
            Log::warn("LoadingMenu opened, but no playable media could be started. Vanilla fallback remains active.");
            Log::diagnostic("controller_open_failed no_playable_media=true");
            return;
        }

        state_ = PlaybackState::FadingIn;
        transitionStartSeconds_ = now;
        overlayActive_.store(true, std::memory_order_release);
        Log::info("LoadingMenu opened. Selected media: {}", Paths::ForLog(current_->media.path));
        Log::diagnostic(
            "controller_open_success media={} state={} overlay_active=true fade_in_ms={} cover_vanilla={} opacity={}",
            Paths::ForLog(current_->media.path),
            static_cast<int>(state_),
            config_.transitions.fadeInMs,
            config_.display.coverVanillaLoadingScreen,
            config_.display.opacity);
    }

    void AnimatedLoadingScreenController::OnLoadingMenuClose()
    {
        std::lock_guard lock(mutex_);
        loadingMenuOpen_ = false;
        if (state_ == PlaybackState::Idle) {
            return;
        }

        Log::info("LoadingMenu closed.");
        Log::diagnostic("controller_close state={} overlay_active={}", static_cast<int>(state_), overlayActive_.load(std::memory_order_acquire));
        StopLocked();
    }

    OverlayRenderData AnimatedLoadingScreenController::BuildRenderFrame()
    {
        if (!overlayActive_.load(std::memory_order_acquire)) {
            return {};
        }

        std::lock_guard lock(mutex_);
        return BuildRenderFrameLocked();
    }

    OverlayRenderData AnimatedLoadingScreenController::TryBuildRenderFrame()
    {
        if (!overlayActive_.load(std::memory_order_acquire)) {
            return {};
        }

        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            return {};
        }

        return BuildRenderFrameLocked();
    }

    void AnimatedLoadingScreenController::UpdateMedia(std::vector<MediaFile> scannedFiles, std::vector<MediaFile> playlistFiles)
    {
        std::lock_guard lock(mutex_);
        const auto scannedCount = scannedFiles.size();
        const auto playlistCount = playlistFiles.size();
        selector_.Reset(
            std::move(scannedFiles),
            std::move(playlistFiles),
            config_.general.selectionMode,
            config_.general.rememberLast,
            Paths::StatePath());
        Log::diagnostic(
            "controller_update_media scanned_count={} playlist_count={} active_count={}",
            scannedCount,
            playlistCount,
            selector_.Size());
    }

    OverlayRenderData AnimatedLoadingScreenController::BuildRenderFrameLocked()
    {
        if (state_ == PlaybackState::Idle || !current_) {
            return {};
        }

        const auto now = Now();
        auto renderData = BaseRenderDataLocked();

        if (state_ == PlaybackState::FadingIn) {
            const auto elapsedMs = (now - transitionStartSeconds_) * 1000.0;
            if (elapsedMs >= config_.transitions.fadeInMs) {
                state_ = PlaybackState::Playing;
            }
        }

        const auto currentFinished = CurrentDecoderFinishedLocked();
        if (state_ == PlaybackState::Idle || !current_) {
            return {};
        }

        if (currentFinished && (!current_->lastFrame || state_ == PlaybackState::Playing)) {
            HandleCurrentFinishedLocked(now);
            if (state_ == PlaybackState::Idle || !current_) {
                return {};
            }
        }

        if (state_ == PlaybackState::Playing) {
            HandlePlaybackProgressLocked(now);
            if (state_ == PlaybackState::Idle || !current_) {
                return {};
            }
        }

        if (state_ == PlaybackState::Crossfading) {
            const auto elapsedMs = (now - transitionStartSeconds_) * 1000.0;
            const auto crossfadeMs = current_ ? EffectiveCrossfadeMsLocked(*current_) : static_cast<double>(config_.transitions.crossfadeMs);
            if (elapsedMs >= crossfadeMs) {
                PromoteNextLocked(now);
            }
        }

        if (state_ == PlaybackState::FadingOut) {
            const auto elapsedMs = (now - transitionStartSeconds_) * 1000.0;
            if (elapsedMs >= config_.transitions.fadeOutMs) {
                StopLocked();
                return {};
            }
        }

        const auto currentFrame = FrameForSlotLocked(*current_, now);
        if (currentFrame) {
            if (!current_->firstFrameLogged) {
                current_->firstFrameLogged = true;
                Log::info(
                    "First decoded frame is ready for {} ({}x{}, pts {:.3f}s).",
                    Paths::ForLog(current_->media.path),
                    currentFrame->width,
                    currentFrame->height,
                    currentFrame->ptsSeconds);
            }
            renderData.current.frame = currentFrame;
            EnsureNextPreloadedLocked(now);
        } else if (current_->lastFrame) {
            renderData.current.frame = current_->lastFrame;
            EnsureNextPreloadedLocked(now);
        } else if (!current_->waitingForFirstFrameLogged) {
            current_->waitingForFirstFrameLogged = true;
            Log::info("Overlay is active and waiting for the first decoded frame from {}.", Paths::ForLog(current_->media.path));
        }

        auto currentAlpha = 1.0F;
        if (state_ == PlaybackState::FadingIn) {
            currentAlpha = FadeInAlpha((now - transitionStartSeconds_) * 1000.0, config_.transitions.fadeInMs);
        } else if (state_ == PlaybackState::FadingOut) {
            currentAlpha = FadeOutAlpha((now - transitionStartSeconds_) * 1000.0, config_.transitions.fadeOutMs);
        } else if (state_ == PlaybackState::Crossfading && next_) {
            const auto t = FadeInAlpha((now - transitionStartSeconds_) * 1000.0, EffectiveCrossfadeMsLocked(*current_));
            currentAlpha = 1.0F - t;
            auto nextFrame = FrameForSlotLocked(*next_, now);
            if (nextFrame) {
                renderData.next.frame = std::move(nextFrame);
            } else if (next_->lastFrame) {
                renderData.next.frame = next_->lastFrame;
            }
            renderData.next.alpha = t;
        }

        renderData.current.alpha = currentAlpha;
        renderData.shouldRender =
            renderData.drawBackground ||
            static_cast<bool>(renderData.current.frame) ||
            static_cast<bool>(renderData.next.frame);
        static std::atomic_uint renderDiagnostics{ 0 };
        const auto diagnosticIndex = renderDiagnostics.fetch_add(1, std::memory_order_relaxed);
        if (diagnosticIndex < 20 || (diagnosticIndex % 120) == 0) {
            Log::diagnostic(
                "controller_render_frame index={} state={} should_render={} draw_background={} current_frame={} next_frame={} current_alpha={} next_alpha={} opacity={} current_size={}x{} next_size={}x{}",
                diagnosticIndex,
                static_cast<int>(state_),
                renderData.shouldRender,
                renderData.drawBackground,
                static_cast<bool>(renderData.current.frame),
                static_cast<bool>(renderData.next.frame),
                renderData.current.alpha,
                renderData.next.alpha,
                renderData.opacity,
                renderData.current.frame ? renderData.current.frame->width : 0,
                renderData.current.frame ? renderData.current.frame->height : 0,
                renderData.next.frame ? renderData.next.frame->width : 0,
                renderData.next.frame ? renderData.next.frame->height : 0);
        }
        return renderData;
    }

    void AnimatedLoadingScreenController::Stop()
    {
        std::lock_guard lock(mutex_);
        StopLocked();
    }

    PlaybackState AnimatedLoadingScreenController::State() const
    {
        std::lock_guard lock(mutex_);
        return state_;
    }

    bool AnimatedLoadingScreenController::HasMedia() const
    {
        std::lock_guard lock(mutex_);
        return !selector_.Empty();
    }

    bool AnimatedLoadingScreenController::IsOverlayActive() const noexcept
    {
        return overlayActive_.load(std::memory_order_acquire);
    }

    double AnimatedLoadingScreenController::Now() const
    {
        return clock_ ? clock_() : SteadySeconds();
    }

    DecoderOptions AnimatedLoadingScreenController::MakeDecoderOptions() const
    {
        return DecoderOptions{
            config_.playback.frameQueueSize,
            config_.playback.maxDecodeWidth,
            config_.playback.maxDecodeHeight,
            config_.playback.targetFPS,
            config_.playback.playbackMode == PlaybackMode::RepeatSingle && config_.playback.loopVideo,
            config_.performance.useHardwareDecoding,
            config_.performance.decoderThreadPriority,
            config_.performance.maxDecoderThreads
        };
    }

    bool AnimatedLoadingScreenController::StartCurrentLocked(const MediaFile& media, double now)
    {
        if (!decoderFactory_) {
            Log::error("No decoder factory is available.");
            return false;
        }

        auto decoder = decoderFactory_();
        if (!decoder) {
            Log::error("Decoder factory returned null.");
            return false;
        }

        if (!decoder->Open(media.path, MakeDecoderOptions())) {
            Log::warn("Decoder rejected media: {}", Paths::ForLog(media.path));
            Log::diagnostic("controller_start_current_decoder_rejected media={}", Paths::ForLog(media.path));
            return false;
        }

        current_ = PlayerSlot{ media, std::move(decoder), now, {} };
        selector_.CommitSelection(media);
        Log::diagnostic("controller_start_current_success media={} now={}", Paths::ForLog(media.path), now);
        return true;
    }

    bool AnimatedLoadingScreenController::StartNextLocked(double now, bool allowSameAsCurrent)
    {
        if (next_) {
            return true;
        }

        constexpr int maxAttempts = 5;
        for (int attempt = 0; attempt < maxAttempts; ++attempt) {
            auto selected = selector_.SelectNext();
            if (!selected) {
                return false;
            }
            if (!allowSameAsCurrent && current_ && SamePath(selected->path, current_->media.path)) {
                continue;
            }

            auto decoder = decoderFactory_ ? decoderFactory_() : nullptr;
            if (!decoder) {
                return false;
            }

            if (!decoder->Open(selected->path, MakeDecoderOptions())) {
                Log::warn("Unable to preload media: {}", Paths::ForLog(selected->path));
                continue;
            }

            next_ = PlayerSlot{ *selected, std::move(decoder), now, {} };
            Log::debug("Preloaded next media: {}", Paths::ForLog(next_->media.path));
            return true;
        }
        return false;
    }

    bool AnimatedLoadingScreenController::TryStartSelectedLocked(double now)
    {
        constexpr int maxAttempts = 5;
        for (int attempt = 0; attempt < maxAttempts; ++attempt) {
            auto selected = selector_.SelectNext();
            if (!selected) {
                Log::diagnostic("controller_try_start_selected_no_selection attempt={}", attempt + 1);
                return false;
            }
            Log::diagnostic(
                "controller_try_start_selected attempt={} media={}",
                attempt + 1,
                Paths::ForLog(selected->path));
            if (StartCurrentLocked(*selected, now)) {
                return true;
            }
        }
        return false;
    }

    void AnimatedLoadingScreenController::EnsureNextPreloadedLocked(double now)
    {
        if (!config_.playback.preloadNext) {
            return;
        }
        if (config_.playback.playbackMode == PlaybackMode::RepeatSingle) {
            return;
        }
        if (selector_.Size() < 2) {
            return;
        }
        (void)StartNextLocked(now, false);
    }

    void AnimatedLoadingScreenController::HandleCurrentFinishedLocked(double now)
    {
        const auto hadFrame = current_ && current_->lastFrame;
        if (!hadFrame) {
            ++failuresThisMenu_;
            Log::warn("Selected media finished before producing a frame. Falling back to vanilla loading screen.");
            StopLocked();
            return;
        }

        failuresThisMenu_ = 0;

        if (config_.playback.playbackMode == PlaybackMode::RepeatSingle) {
            if (!config_.playback.loopVideo) {
                Log::debug("Repeat-single media finished and LoopVideo=false. Returning to vanilla loading screen.");
                StopLocked();
                return;
            }

            const auto media = current_->media;
            Log::debug("Restarting media after decoder finished: {}", Paths::ForLog(media.path));
            RetireDecoder(std::move(current_->decoder));
            current_.reset();
            if (!StartCurrentLocked(media, now)) {
                Log::warn("Repeat-single media could not be reopened. Falling back to vanilla loading screen.");
                StopLocked();
            }
            return;
        }

        if (config_.playback.playbackMode == PlaybackMode::CrossfadePlaylist &&
            config_.transitions.enableCrossfade &&
            config_.transitions.crossfadeMs > 0 &&
            selector_.Size() > 1) {
            if (!next_) {
                (void)StartNextLocked(now, false);
            }
            if (next_) {
                state_ = PlaybackState::Crossfading;
                transitionStartSeconds_ = now;
                next_->startSeconds = now;
                Log::info("Crossfading to media: {}", Paths::ForLog(next_->media.path));
                return;
            }
        }

        if (!next_) {
            (void)StartNextLocked(now, true);
        }
        if (next_) {
            PromoteNextLocked(now);
            return;
        }

        StopLocked();
    }

    void AnimatedLoadingScreenController::HandlePlaybackProgressLocked(double now)
    {
        if (!current_ || config_.playback.playbackMode != PlaybackMode::CrossfadePlaylist ||
            !config_.transitions.enableCrossfade || config_.transitions.crossfadeMs <= 0 ||
            selector_.Size() < 2) {
            return;
        }

        const auto durationSeconds = SlotDurationSecondsLocked(*current_);
        if (durationSeconds <= 0.0) {
            return;
        }

        const auto crossfadeSeconds = EffectiveCrossfadeMsLocked(*current_) / 1000.0;
        const auto elapsedSeconds = SlotElapsedSecondsLocked(*current_, now);
        if (elapsedSeconds + 0.001 < std::max(0.0, durationSeconds - crossfadeSeconds)) {
            return;
        }

        if (!next_) {
            (void)StartNextLocked(now, false);
        }
        if (!next_) {
            return;
        }

        state_ = PlaybackState::Crossfading;
        transitionStartSeconds_ = now;
        next_->startSeconds = now;
        Log::info("Crossfading to media: {}", Paths::ForLog(next_->media.path));
    }

    void AnimatedLoadingScreenController::PromoteNextLocked(double now)
    {
        if (!next_) {
            return;
        }

        if (current_ && current_->decoder) {
            RetireDecoder(std::move(current_->decoder));
        }
        current_.reset();
        current_ = std::move(next_);
        current_->startSeconds = now;
        selector_.CommitSelection(current_->media);
        state_ = loadingMenuOpen_ ? PlaybackState::Playing : PlaybackState::FadingOut;
        transitionStartSeconds_ = now;
        failuresThisMenu_ = 0;
        EnsureNextPreloadedLocked(now);
    }

    void AnimatedLoadingScreenController::StopLocked()
    {
        if (current_ && current_->decoder) {
            RetireDecoder(std::move(current_->decoder));
        }
        if (next_ && next_->decoder) {
            RetireDecoder(std::move(next_->decoder));
        }
        current_.reset();
        next_.reset();
        state_ = PlaybackState::Idle;
        loadingMenuOpen_ = false;
        overlayActive_.store(false, std::memory_order_release);
    }

    bool AnimatedLoadingScreenController::CurrentDecoderFinishedLocked()
    {
        if (!current_ || !current_->decoder) {
            return false;
        }

        try {
            return current_->decoder->IsFinished();
        } catch (const std::exception& e) {
            Log::error("Decoder failed while polling finish state: {}", e.what());
        } catch (...) {
            Log::error("Unknown decoder failure while polling finish state.");
        }

        StopLocked();
        return false;
    }

    VideoFramePtr AnimatedLoadingScreenController::FrameForSlotLocked(PlayerSlot& slot, double now)
    {
        if (!slot.decoder) {
            return {};
        }

        const auto elapsed = SlotElapsedSecondsLocked(slot, now);
        auto frame = slot.decoder->GetFrameForTime(elapsed);
        if (frame) {
            slot.lastFrame = frame;
        }
        return frame;
    }

    double AnimatedLoadingScreenController::SlotElapsedSecondsLocked(const PlayerSlot& slot, double now) const
    {
        return std::max(0.0, now - slot.startSeconds) * config_.playback.playbackSpeed;
    }

    double AnimatedLoadingScreenController::SlotDurationSecondsLocked(const PlayerSlot& slot) const
    {
        if (slot.media.durationOverrideSeconds && *slot.media.durationOverrideSeconds > 0.0) {
            return *slot.media.durationOverrideSeconds;
        }
        if (!slot.decoder) {
            return 0.0;
        }
        const auto info = slot.decoder->GetInfo();
        return std::max(0.0, info.durationSeconds);
    }

    double AnimatedLoadingScreenController::EffectiveCrossfadeMsLocked(const PlayerSlot& slot) const
    {
        auto crossfadeMs = static_cast<double>(std::max(0, config_.transitions.crossfadeMs));
        const auto durationSeconds = SlotDurationSecondsLocked(slot);
        if (durationSeconds > 0.0) {
            crossfadeMs = std::min(crossfadeMs, std::max(1.0, durationSeconds * 1000.0));
        }
        return crossfadeMs;
    }

    OverlayRenderData AnimatedLoadingScreenController::BaseRenderDataLocked() const
    {
        auto drawBackground = config_.display.coverVanillaLoadingScreen;
        if (state_ == PlaybackState::FadingOut && !config_.transitions.fadeToBlackOnMenuClose) {
            drawBackground = false;
        }

        return OverlayRenderData{
            false,
            drawBackground,
            1.0F,
            config_.display.backgroundColor,
            config_.display.fitMode,
            config_.display.opacity,
            {},
            {}
        };
    }
}
