#pragma once

#include "Config/Config.h"
#include "Decode/IVideoDecoder.h"
#include "Media/MediaSelector.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace ALS
{
    enum class PlaybackState
    {
        Idle,
        FadingIn,
        Playing,
        Crossfading,
        FadingOut
    };

    struct OverlayFrame
    {
        VideoFramePtr frame{};
        float alpha{ 0.0F };
    };

    struct OverlayRenderData
    {
        bool shouldRender{ false };
        bool drawBackground{ false };
        float backgroundAlpha{ 1.0F };
        Color backgroundColor{};
        FitMode fitMode{ FitMode::Cover };
        float opacity{ 1.0F };
        OverlayFrame current{};
        OverlayFrame next{};
    };

    class AnimatedLoadingScreenController
    {
    public:
        using DecoderFactory = std::function<std::unique_ptr<IVideoDecoder>()>;
        using ClockFn = std::function<double()>;

        AnimatedLoadingScreenController(Config config, MediaSelector selector, DecoderFactory decoderFactory, ClockFn clock = {});
        ~AnimatedLoadingScreenController();

        void OnLoadingMenuOpen();
        void OnLoadingMenuClose();
        OverlayRenderData BuildRenderFrame();
        OverlayRenderData TryBuildRenderFrame();
        void UpdateMedia(std::vector<MediaFile> scannedFiles, std::vector<MediaFile> playlistFiles);
        void Stop();

        [[nodiscard]] PlaybackState State() const;
        [[nodiscard]] bool HasMedia() const;
        [[nodiscard]] bool IsOverlayActive() const noexcept;

    private:
        struct PlayerSlot
        {
            MediaFile media{};
            std::unique_ptr<IVideoDecoder> decoder{};
            double startSeconds{ 0.0 };
            VideoFramePtr lastFrame{};
            bool waitingForFirstFrameLogged{ false };
            bool firstFrameLogged{ false };
        };

        std::atomic_bool overlayActive_{ false };
        mutable std::mutex mutex_;
        Config config_{};
        MediaSelector selector_{};
        DecoderFactory decoderFactory_{};
        ClockFn clock_{};

        PlaybackState state_{ PlaybackState::Idle };
        bool loadingMenuOpen_{ false };
        int failuresThisMenu_{ 0 };
        double transitionStartSeconds_{ 0.0 };
        std::optional<PlayerSlot> current_{};
        std::optional<PlayerSlot> next_{};

        [[nodiscard]] double Now() const;
        [[nodiscard]] DecoderOptions MakeDecoderOptions() const;
        bool StartCurrentLocked(const MediaFile& media, double now);
        bool StartNextLocked(double now, bool allowSameAsCurrent);
        bool TryStartSelectedLocked(double now);
        void EnsureNextPreloadedLocked(double now);
        void HandleCurrentFinishedLocked(double now);
        void HandlePlaybackProgressLocked(double now);
        void PromoteNextLocked(double now);
        void StopLocked();
        OverlayRenderData BuildRenderFrameLocked();
        [[nodiscard]] bool CurrentDecoderFinishedLocked();
        VideoFramePtr FrameForSlotLocked(PlayerSlot& slot, double now);
        [[nodiscard]] double SlotElapsedSecondsLocked(const PlayerSlot& slot, double now) const;
        [[nodiscard]] double SlotDurationSecondsLocked(const PlayerSlot& slot) const;
        [[nodiscard]] double EffectiveCrossfadeMsLocked(const PlayerSlot& slot) const;
        [[nodiscard]] OverlayRenderData BaseRenderDataLocked() const;
    };
}
