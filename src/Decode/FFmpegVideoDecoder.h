#pragma once

#include "Decode/IVideoDecoder.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace ALS
{
    class FFmpegVideoDecoder final : public IVideoDecoder
    {
    public:
        FFmpegVideoDecoder() = default;
        ~FFmpegVideoDecoder() override;

        [[nodiscard]] static bool IsRuntimeAvailable();

        bool Open(const std::filesystem::path& path, const DecoderOptions& options) override;
        void Close() override;
        [[nodiscard]] bool IsOpen() const override;
        [[nodiscard]] bool IsFinished() const override;
        VideoFramePtr GetFrameForTime(double seconds) override;
        [[nodiscard]] VideoInfo GetInfo() const override;

    private:
        std::filesystem::path path_{};
        DecoderOptions options_{};
        std::atomic_bool stopRequested_{ false };
        std::atomic_bool open_{ false };
        std::atomic_bool finished_{ false };
        std::atomic_bool failed_{ false };
        std::thread worker_{};

        mutable std::mutex mutex_{};
        std::condition_variable queueChanged_{};
        std::deque<VideoFramePtr> queue_{};
        VideoFramePtr lastFrame_{};
        VideoInfo info_{};

        void DecodeLoop();
        bool PushFrame(VideoFramePtr frame);
    };
}
