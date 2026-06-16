#pragma once

#include "Config/Config.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ALS
{
    struct VideoInfo
    {
        int width{ 0 };
        int height{ 0 };
        double fps{ 0.0 };
        double durationSeconds{ 0.0 };
        std::string codecName{};
    };

    struct VideoFrame
    {
        int width{ 0 };
        int height{ 0 };
        double ptsSeconds{ 0.0 };
        std::vector<std::uint8_t> bgra{};
    };

    using VideoFramePtr = std::shared_ptr<const VideoFrame>;

    struct DecoderOptions
    {
        std::size_t frameQueueSize{ 4 };
        int maxDecodeWidth{ 1920 };
        int maxDecodeHeight{ 1080 };
        double targetFPS{ 60.0 };
        bool loopVideo{ true };
        bool useHardwareDecoding{ false };
        DecoderThreadPriority threadPriority{ DecoderThreadPriority::BelowNormal };
        int maxDecoderThreads{ 2 };
    };

    class IVideoDecoder
    {
    public:
        virtual ~IVideoDecoder() = default;

        virtual bool Open(const std::filesystem::path& path, const DecoderOptions& options) = 0;
        virtual void Close() = 0;
        [[nodiscard]] virtual bool IsOpen() const = 0;
        [[nodiscard]] virtual bool IsFinished() const = 0;
        virtual VideoFramePtr GetFrameForTime(double seconds) = 0;
        [[nodiscard]] virtual VideoInfo GetInfo() const = 0;
    };
}
