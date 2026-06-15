#include "Decode/FFmpegVideoDecoder.h"

#include "Logging/Log.h"
#include "Utils/Paths.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if ALS_ENABLE_FFMPEG
#    include <Windows.h>
extern "C"
{
#    include <libavcodec/avcodec.h>
#    include <libavformat/avformat.h>
#    include <libavutil/imgutils.h>
#    include <libavutil/mathematics.h>
#    include <libavutil/pixfmt.h>
#    include <libswscale/swscale.h>
}
#endif

namespace
{
#if ALS_ENABLE_FFMPEG
    struct FFmpegApi
    {
        std::vector<HMODULE> modules{};

        decltype(&::av_log_set_level) av_log_set_level{};
        decltype(&::avutil_version) avutil_version{};
        decltype(&::avcodec_version) avcodec_version{};
        decltype(&::avformat_version) avformat_version{};
        decltype(&::swscale_version) swscale_version{};
        decltype(&::avformat_alloc_context) avformat_alloc_context{};
        decltype(&::avformat_free_context) avformat_free_context{};
        decltype(&::avformat_open_input) avformat_open_input{};
        decltype(&::avformat_close_input) avformat_close_input{};
        decltype(&::avformat_find_stream_info) avformat_find_stream_info{};
        decltype(&::av_find_best_stream) av_find_best_stream{};
        decltype(&::avcodec_find_decoder) avcodec_find_decoder{};
        decltype(&::avcodec_alloc_context3) avcodec_alloc_context3{};
        decltype(&::avcodec_free_context) avcodec_free_context{};
        decltype(&::avcodec_parameters_to_context) avcodec_parameters_to_context{};
        decltype(&::avcodec_open2) avcodec_open2{};
        decltype(&::avcodec_send_packet) avcodec_send_packet{};
        decltype(&::avcodec_receive_frame) avcodec_receive_frame{};
        decltype(&::avcodec_flush_buffers) avcodec_flush_buffers{};
        decltype(&::av_frame_alloc) av_frame_alloc{};
        decltype(&::av_frame_free) av_frame_free{};
        decltype(&::av_frame_unref) av_frame_unref{};
        decltype(&::av_packet_alloc) av_packet_alloc{};
        decltype(&::av_packet_free) av_packet_free{};
        decltype(&::av_packet_unref) av_packet_unref{};
        decltype(&::av_read_frame) av_read_frame{};
        decltype(&::av_seek_frame) av_seek_frame{};
        decltype(&::sws_getContext) sws_getContext{};
        decltype(&::sws_freeContext) sws_freeContext{};
        decltype(&::sws_scale) sws_scale{};

        ~FFmpegApi()
        {
            for (auto it = modules.rbegin(); it != modules.rend(); ++it) {
                if (*it) {
                    FreeLibrary(*it);
                }
            }
        }
    };

    struct AVFormatCloser
    {
        FFmpegApi* api{};

        void operator()(AVFormatContext* context) const noexcept
        {
            if (context && api) {
                api->avformat_close_input(&context);
            }
        }
    };

    template <class T>
    struct AVFreePtr
    {
        using FreeFn = void (*)(T**);
        FreeFn freeFn{};

        void operator()(T* value) const noexcept
        {
            if (value && freeFn) {
                freeFn(&value);
            }
        }
    };

    struct SwsCloser
    {
        FFmpegApi* api{};

        void operator()(SwsContext* context) const noexcept
        {
            if (context && api) {
                api->sws_freeContext(context);
            }
        }
    };

    using FormatPtr = std::unique_ptr<AVFormatContext, AVFormatCloser>;
    using CodecContextPtr = std::unique_ptr<AVCodecContext, AVFreePtr<AVCodecContext>>;
    using FramePtr = std::unique_ptr<AVFrame, AVFreePtr<AVFrame>>;
    using PacketPtr = std::unique_ptr<AVPacket, AVFreePtr<AVPacket>>;
    using SwsContextPtr = std::unique_ptr<SwsContext, SwsCloser>;

    [[nodiscard]] std::optional<std::filesystem::path> FindDllByPrefix(const std::filesystem::path& directory, std::wstring_view prefix)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec)) {
            return std::nullopt;
        }

        auto lowerPrefix = std::wstring(prefix);
        std::ranges::transform(lowerPrefix, lowerPrefix.begin(), [](wchar_t c) {
            return static_cast<wchar_t>(std::towlower(c));
        });

        std::vector<std::filesystem::path> matches;
        for (std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
            if (!it->is_regular_file(ec)) {
                continue;
            }
            auto filename = it->path().filename().wstring();
            std::ranges::transform(filename, filename.begin(), [](wchar_t c) {
                return static_cast<wchar_t>(std::towlower(c));
            });
            if (filename == lowerPrefix + L".dll" ||
                (filename.starts_with(lowerPrefix + L"-") && filename.ends_with(L".dll"))) {
                matches.push_back(it->path());
            }
        }

        if (matches.empty()) {
            return std::nullopt;
        }
        std::ranges::sort(matches);
        return matches.front();
    }

    [[nodiscard]] std::vector<std::filesystem::path> FFmpegSearchDirectories()
    {
        std::vector<std::filesystem::path> directories{
            ALS::Paths::ModDirectory() / "FFmpeg"
        };
        return directories;
    }

    [[nodiscard]] std::optional<std::filesystem::path> FindRequiredDll(std::wstring_view prefix)
    {
        for (const auto& directory : FFmpegSearchDirectories()) {
            if (auto path = FindDllByPrefix(directory, prefix)) {
                return path;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::filesystem::path PathForCompare(const std::filesystem::path& path)
    {
        std::error_code ec;
        auto comparable = std::filesystem::weakly_canonical(path, ec);
        if (!ec) {
            return comparable;
        }

        comparable = std::filesystem::absolute(path, ec);
        return ec ? path : comparable;
    }

    [[nodiscard]] std::wstring LowerWide(std::wstring value)
    {
        std::ranges::transform(value, value.begin(), [](wchar_t c) {
            return static_cast<wchar_t>(std::towlower(c));
        });
        return value;
    }

    [[nodiscard]] bool SamePathIgnoreCase(const std::filesystem::path& left, const std::filesystem::path& right)
    {
        return LowerWide(PathForCompare(left).wstring()) == LowerWide(PathForCompare(right).wstring());
    }

    [[nodiscard]] std::optional<std::filesystem::path> ModulePath(HMODULE module)
    {
        if (!module) {
            return std::nullopt;
        }

        std::wstring buffer(32768, L'\0');
        const auto size = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0 || size >= buffer.size()) {
            return std::nullopt;
        }

        buffer.resize(size);
        return std::filesystem::path(buffer);
    }

    [[nodiscard]] HMODULE LoadDll(const std::filesystem::path& path)
    {
        auto module = LoadLibraryExW(
            path.c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module && GetLastError() == ERROR_INVALID_PARAMETER) {
            module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
        if (module) {
            const auto loadedPath = ModulePath(module);
            if (!loadedPath || !SamePathIgnoreCase(*loadedPath, path)) {
                ALS::Log::error(
                    "FFmpeg DLL path mismatch. Expected '{}', but Windows returned '{}'.",
                    ALS::Paths::ForLog(path),
                    loadedPath ? loadedPath->string() : std::string("<unknown>"));
                FreeLibrary(module);
                return nullptr;
            }
        }
        return module;
    }

    template <class T>
    [[nodiscard]] bool LoadSymbol(FFmpegApi& api, T& target, const char* name)
    {
        for (auto module : api.modules) {
            if (auto* symbol = GetProcAddress(module, name)) {
                target = reinterpret_cast<T>(symbol);
                return true;
            }
        }

        ALS::Log::error("FFmpeg runtime is missing required symbol '{}'.", name);
        return false;
    }

    [[nodiscard]] unsigned FFmpegMajorVersion(unsigned version)
    {
        return version >> 16U;
    }

    [[nodiscard]] bool ValidateFFmpegComponent(
        std::string_view name,
        unsigned runtimeVersion,
        unsigned expectedMajor)
    {
        const auto runtimeMajor = FFmpegMajorVersion(runtimeVersion);
        if (runtimeMajor != expectedMajor) {
            ALS::Log::error(
                "FFmpeg runtime component '{}' has ABI major {} but this build was compiled against major {}.",
                name,
                runtimeMajor,
                expectedMajor);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ValidateFFmpegRuntime(const FFmpegApi& api)
    {
        return
            ValidateFFmpegComponent("avutil", api.avutil_version(), LIBAVUTIL_VERSION_MAJOR) &&
            ValidateFFmpegComponent("avcodec", api.avcodec_version(), LIBAVCODEC_VERSION_MAJOR) &&
            ValidateFFmpegComponent("avformat", api.avformat_version(), LIBAVFORMAT_VERSION_MAJOR) &&
            ValidateFFmpegComponent("swscale", api.swscale_version(), LIBSWSCALE_VERSION_MAJOR);
    }

    [[nodiscard]] std::shared_ptr<FFmpegApi> LoadFFmpegApi()
    {
        static std::mutex mutex;
        static std::shared_ptr<FFmpegApi> cached;

        std::lock_guard lock(mutex);
        if (cached) {
            return cached;
        }

        auto api = std::make_shared<FFmpegApi>();
        const std::array requiredDlls{
            L"avutil",
            L"swscale",
            L"avcodec",
            L"avformat"
        };
        const std::array optionalDlls{
            L"swresample",
            L"avfilter",
            L"avdevice",
            L"postproc"
        };

        for (const auto* prefix : optionalDlls) {
            if (auto path = FindRequiredDll(prefix)) {
                if (auto module = LoadDll(*path)) {
                    api->modules.push_back(module);
                }
            }
        }

        for (const auto* prefix : requiredDlls) {
            const auto path = FindRequiredDll(prefix);
            if (!path) {
                ALS::Log::error(
                    "FFmpeg runtime DLL '{}-*.dll' was not found. Expected it under {}.",
                    ALS::Paths::ForLog(std::filesystem::path(std::wstring(prefix))),
                    ALS::Paths::ForLog(ALS::Paths::ModDirectory() / "FFmpeg"));
                return nullptr;
            }

            auto module = LoadDll(*path);
            if (!module) {
                ALS::Log::error("Unable to load FFmpeg DLL: {}", path->string());
                return nullptr;
            }
            api->modules.push_back(module);
        }

#    define ALS_LOAD_FFMPEG_SYMBOL(name) \
        if (!LoadSymbol(*api, api->name, #name)) { \
            return nullptr; \
        }

        ALS_LOAD_FFMPEG_SYMBOL(av_log_set_level)
        ALS_LOAD_FFMPEG_SYMBOL(avutil_version)
        ALS_LOAD_FFMPEG_SYMBOL(avcodec_version)
        ALS_LOAD_FFMPEG_SYMBOL(avformat_version)
        ALS_LOAD_FFMPEG_SYMBOL(swscale_version)
        ALS_LOAD_FFMPEG_SYMBOL(avformat_alloc_context)
        ALS_LOAD_FFMPEG_SYMBOL(avformat_free_context)
        ALS_LOAD_FFMPEG_SYMBOL(avformat_open_input)
        ALS_LOAD_FFMPEG_SYMBOL(avformat_close_input)
        ALS_LOAD_FFMPEG_SYMBOL(avformat_find_stream_info)
        ALS_LOAD_FFMPEG_SYMBOL(av_find_best_stream)
        ALS_LOAD_FFMPEG_SYMBOL(avcodec_find_decoder)
        ALS_LOAD_FFMPEG_SYMBOL(avcodec_alloc_context3)
        ALS_LOAD_FFMPEG_SYMBOL(avcodec_free_context)
        ALS_LOAD_FFMPEG_SYMBOL(avcodec_parameters_to_context)
        ALS_LOAD_FFMPEG_SYMBOL(avcodec_open2)
        ALS_LOAD_FFMPEG_SYMBOL(avcodec_send_packet)
        ALS_LOAD_FFMPEG_SYMBOL(avcodec_receive_frame)
        ALS_LOAD_FFMPEG_SYMBOL(avcodec_flush_buffers)
        ALS_LOAD_FFMPEG_SYMBOL(av_frame_alloc)
        ALS_LOAD_FFMPEG_SYMBOL(av_frame_free)
        ALS_LOAD_FFMPEG_SYMBOL(av_frame_unref)
        ALS_LOAD_FFMPEG_SYMBOL(av_packet_alloc)
        ALS_LOAD_FFMPEG_SYMBOL(av_packet_free)
        ALS_LOAD_FFMPEG_SYMBOL(av_packet_unref)
        ALS_LOAD_FFMPEG_SYMBOL(av_read_frame)
        ALS_LOAD_FFMPEG_SYMBOL(av_seek_frame)
        ALS_LOAD_FFMPEG_SYMBOL(sws_getContext)
        ALS_LOAD_FFMPEG_SYMBOL(sws_freeContext)
        ALS_LOAD_FFMPEG_SYMBOL(sws_scale)

#    undef ALS_LOAD_FFMPEG_SYMBOL

        if (!ValidateFFmpegRuntime(*api)) {
            return nullptr;
        }

        cached = api;
        ALS::Log::info("FFmpeg runtime loaded from plugin-owned FFmpeg directory.");
        return cached;
    }

    [[nodiscard]] double RationalToDouble(AVRational value)
    {
        if (value.num == 0 || value.den == 0) {
            return 0.0;
        }
        return av_q2d(value);
    }

    [[nodiscard]] int ThreadPriorityToWindows(ALS::DecoderThreadPriority priority)
    {
        switch (priority) {
        case ALS::DecoderThreadPriority::Low:
            return THREAD_PRIORITY_LOWEST;
        case ALS::DecoderThreadPriority::BelowNormal:
            return THREAD_PRIORITY_BELOW_NORMAL;
        case ALS::DecoderThreadPriority::Normal:
        default:
            return THREAD_PRIORITY_NORMAL;
        }
    }

    void ApplyThreadPriority(ALS::DecoderThreadPriority priority)
    {
        (void)SetThreadPriority(GetCurrentThread(), ThreadPriorityToWindows(priority));
    }

    [[nodiscard]] std::pair<int, int> CalculateOutputSize(int sourceWidth, int sourceHeight, int maxWidth, int maxHeight)
    {
        if (sourceWidth <= 0 || sourceHeight <= 0) {
            return { 0, 0 };
        }

        auto outputWidth = sourceWidth;
        auto outputHeight = sourceHeight;
        if (maxWidth > 0 && maxHeight > 0 && (sourceWidth > maxWidth || sourceHeight > maxHeight)) {
            const auto scale = std::min(
                static_cast<double>(maxWidth) / static_cast<double>(sourceWidth),
                static_cast<double>(maxHeight) / static_cast<double>(sourceHeight));
            outputWidth = std::max(1, static_cast<int>(std::lround(sourceWidth * scale)));
            outputHeight = std::max(1, static_cast<int>(std::lround(sourceHeight * scale)));
        }
        return { outputWidth, outputHeight };
    }
#endif
}

namespace ALS
{
    FFmpegVideoDecoder::~FFmpegVideoDecoder()
    {
        Close();
    }

    bool FFmpegVideoDecoder::IsRuntimeAvailable()
    {
#if !ALS_ENABLE_FFMPEG
        return false;
#else
        return static_cast<bool>(LoadFFmpegApi());
#endif
    }

    bool FFmpegVideoDecoder::Open(const std::filesystem::path& path, const DecoderOptions& options)
    {
        Close();

#if !ALS_ENABLE_FFMPEG
        (void)path;
        (void)options;
        Log::error("FFmpeg support is disabled at compile time. Build with ALS_ENABLE_FFMPEG=ON.");
        return false;
#else
        if (!LoadFFmpegApi()) {
            Log::error("FFmpeg runtime is unavailable. Video playback is disabled for this media.");
            return false;
        }

        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || !std::filesystem::is_regular_file(path, ec)) {
            Log::warn("Media file does not exist: {}", Paths::ForLog(path));
            return false;
        }
        if (std::filesystem::file_size(path, ec) == 0 || ec) {
            Log::warn("Media file is empty or unreadable: {}", Paths::ForLog(path));
            return false;
        }

        path_ = path;
        options_ = DecoderOptions{
            std::clamp<std::size_t>(options.frameQueueSize, 1, 16),
            std::clamp(options.maxDecodeWidth, 64, 4096),
            std::clamp(options.maxDecodeHeight, 64, 4096),
            std::clamp(options.targetFPS, 1.0, 240.0),
            options.loopVideo,
            options.useHardwareDecoding,
            options.threadPriority,
            std::clamp(options.maxDecoderThreads, 1, 4)
        };
        stopRequested_ = false;
        open_ = false;
        finished_ = false;
        failed_ = false;

        {
            std::lock_guard lock(mutex_);
            queue_.clear();
            lastFrame_.reset();
            info_ = {};
        }

        worker_ = std::thread([this] {
            try {
                DecodeLoop();
            } catch (const std::exception& e) {
                Log::error("Unhandled exception in FFmpeg worker for {}: {}", Paths::ForLog(path_), e.what());
                failed_ = true;
                open_ = false;
                finished_ = true;
                queueChanged_.notify_all();
            } catch (...) {
                Log::error("Unknown unhandled exception in FFmpeg worker for {}.", Paths::ForLog(path_));
                failed_ = true;
                open_ = false;
                finished_ = true;
                queueChanged_.notify_all();
            }
        });
        return true;
#endif
    }

    void FFmpegVideoDecoder::Close()
    {
        try {
            stopRequested_ = true;
            queueChanged_.notify_all();
            if (worker_.joinable()) {
                if (worker_.get_id() == std::this_thread::get_id()) {
                    worker_.detach();
                } else {
                    worker_.join();
                }
            }

            open_ = false;
            finished_ = true;
            {
                std::lock_guard lock(mutex_);
                queue_.clear();
                lastFrame_.reset();
            }
        } catch (const std::exception& e) {
            Log::error("Exception while closing FFmpeg decoder: {}", e.what());
            open_ = false;
            finished_ = true;
            failed_ = true;
        } catch (...) {
            Log::error("Unknown exception while closing FFmpeg decoder.");
            open_ = false;
            finished_ = true;
            failed_ = true;
        }
    }

    bool FFmpegVideoDecoder::IsOpen() const
    {
        return open_ && !failed_;
    }

    bool FFmpegVideoDecoder::IsFinished() const
    {
        if (failed_) {
            return true;
        }
        if (!finished_) {
            return false;
        }

        std::lock_guard lock(mutex_);
        return queue_.empty();
    }

    VideoFramePtr FFmpegVideoDecoder::GetFrameForTime(double seconds)
    {
        std::unique_lock lock(mutex_);

        bool consumed = false;
        while (!queue_.empty() && queue_.front() && queue_.front()->ptsSeconds <= seconds + 0.001) {
            lastFrame_ = std::move(queue_.front());
            queue_.pop_front();
            consumed = true;
        }

        if (consumed) {
            lock.unlock();
            queueChanged_.notify_one();
            lock.lock();
        }

        if (lastFrame_) {
            return lastFrame_;
        }
        if (!queue_.empty()) {
            return queue_.front();
        }
        return {};
    }

    VideoInfo FFmpegVideoDecoder::GetInfo() const
    {
        std::lock_guard lock(mutex_);
        return info_;
    }

    bool FFmpegVideoDecoder::PushFrame(VideoFramePtr frame)
    {
        if (!frame) {
            return false;
        }

        std::unique_lock lock(mutex_);
        queueChanged_.wait(lock, [&] {
            return stopRequested_ || queue_.size() < std::max<std::size_t>(1, options_.frameQueueSize);
        });

        if (stopRequested_) {
            return false;
        }

        queue_.push_back(std::move(frame));
        return true;
    }

    void FFmpegVideoDecoder::DecodeLoop()
    {
        struct FinishGuard
        {
            FFmpegVideoDecoder& decoder;

            ~FinishGuard()
            {
                decoder.open_ = false;
                decoder.finished_ = true;
                decoder.queueChanged_.notify_all();
            }
        } finish{ *this };

#if !ALS_ENABLE_FFMPEG
        failed_ = true;
#else
        auto api = LoadFFmpegApi();
        if (!api) {
            Log::error("FFmpeg runtime is unavailable. Video playback is disabled for this media.");
            failed_ = true;
            return;
        }

        ApplyThreadPriority(options_.threadPriority);

        if (options_.useHardwareDecoding) {
            Log::warn("UseHardwareDecoding=true was requested, but hardware decoding is not implemented yet. Falling back to software FFmpeg decode.");
        }

        api->av_log_set_level(AV_LOG_ERROR);

        AVFormatContext* rawFormat = api->avformat_alloc_context();
        if (!rawFormat) {
            Log::warn("Unable to allocate FFmpeg format context: {}", Paths::ForLog(path_));
            failed_ = true;
            return;
        }
        rawFormat->interrupt_callback.callback = [](void* opaque) -> int {
            const auto* stopRequested = static_cast<const std::atomic_bool*>(opaque);
            return stopRequested && stopRequested->load() ? 1 : 0;
        };
        rawFormat->interrupt_callback.opaque = &stopRequested_;

        const auto utf8Path = path_.u8string();
        if (api->avformat_open_input(&rawFormat, reinterpret_cast<const char*>(utf8Path.c_str()), nullptr, nullptr) < 0) {
            Log::warn("FFmpeg could not open media: {}", Paths::ForLog(path_));
            failed_ = true;
            return;
        }
        FormatPtr format(rawFormat, AVFormatCloser{ api.get() });

        if (api->avformat_find_stream_info(format.get(), nullptr) < 0) {
            Log::warn("FFmpeg could not read stream info: {}", Paths::ForLog(path_));
            failed_ = true;
            return;
        }

        const auto streamIndex = api->av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (streamIndex < 0) {
            Log::warn("No video stream found: {}", Paths::ForLog(path_));
            failed_ = true;
            return;
        }

        AVStream* stream = format->streams[streamIndex];
        const AVCodec* codec = api->avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            Log::warn("No FFmpeg decoder available for codec id {} in {}", static_cast<int>(stream->codecpar->codec_id), Paths::ForLog(path_));
            failed_ = true;
            return;
        }

        CodecContextPtr codecContext(api->avcodec_alloc_context3(codec), AVFreePtr<AVCodecContext>{ api->avcodec_free_context });
        if (!codecContext || api->avcodec_parameters_to_context(codecContext.get(), stream->codecpar) < 0) {
            Log::warn("Unable to create FFmpeg codec context: {}", Paths::ForLog(path_));
            failed_ = true;
            return;
        }

        codecContext->thread_count = options_.maxDecoderThreads;
        if (api->avcodec_open2(codecContext.get(), codec, nullptr) < 0) {
            Log::warn("Unable to open FFmpeg codec {} for {}", codec->name ? codec->name : "unknown", Paths::ForLog(path_));
            failed_ = true;
            return;
        }

        const auto [outputWidth, outputHeight] = CalculateOutputSize(
            codecContext->width,
            codecContext->height,
            options_.maxDecodeWidth,
            options_.maxDecodeHeight);
        if (outputWidth <= 0 || outputHeight <= 0) {
            Log::warn("Invalid video dimensions in {}", Paths::ForLog(path_));
            failed_ = true;
            return;
        }

        SwsContextPtr sws(
            api->sws_getContext(
                codecContext->width,
                codecContext->height,
                codecContext->pix_fmt,
                outputWidth,
                outputHeight,
                AV_PIX_FMT_BGRA,
                SWS_FAST_BILINEAR,
                nullptr,
                nullptr,
                nullptr),
            SwsCloser{ api.get() });
        if (!sws) {
            Log::warn("Unable to create FFmpeg scaling context: {}", Paths::ForLog(path_));
            failed_ = true;
            return;
        }

        auto fps = RationalToDouble(stream->avg_frame_rate);
        if (fps <= 0.0) {
            fps = RationalToDouble(stream->r_frame_rate);
        }
        if (fps <= 0.0) {
            fps = options_.targetFPS;
        }

        auto duration = 0.0;
        if (format->duration > 0) {
            duration = static_cast<double>(format->duration) / static_cast<double>(AV_TIME_BASE);
        } else if (stream->duration > 0) {
            duration = static_cast<double>(stream->duration) * RationalToDouble(stream->time_base);
        }

        {
            std::lock_guard lock(mutex_);
            info_ = VideoInfo{
                outputWidth,
                outputHeight,
                fps,
                duration,
                codec->name ? codec->name : ""
            };
        }

        Log::info(
            "FFmpeg opened {} ({}x{}, {:.3f} fps, {:.3f}s, codec {})",
            Paths::ForLog(path_),
            outputWidth,
            outputHeight,
            fps,
            duration,
            codec->name ? codec->name : "unknown");

        FramePtr frame(api->av_frame_alloc(), AVFreePtr<AVFrame>{ api->av_frame_free });
        PacketPtr packet(api->av_packet_alloc(), AVFreePtr<AVPacket>{ api->av_packet_free });
        if (!frame || !packet) {
            failed_ = true;
            return;
        }

        open_ = true;

        auto syntheticPts = 0.0;
        auto lastPts = 0.0;
        auto loopOffset = 0.0;
        std::optional<double> firstPtsSeconds;
        const auto streamTimeBase = RationalToDouble(stream->time_base);
        const auto frameDuration = fps > 0.0 ? 1.0 / fps : 1.0 / std::max(1.0, options_.targetFPS);
        const auto outputFrameDuration = 1.0 / std::max(1.0, options_.targetFPS);
        auto nextOutputPts = 0.0;

        const auto receiveFrames = [&]() -> bool {
            while (!stopRequested_) {
                const auto receiveResult = api->avcodec_receive_frame(codecContext.get(), frame.get());
                if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
                    return true;
                }
                if (receiveResult < 0) {
                    Log::warn("FFmpeg failed while decoding frame from {}", Paths::ForLog(path_));
                    return false;
                }

                auto localPts = syntheticPts;
                if (frame->best_effort_timestamp != AV_NOPTS_VALUE && streamTimeBase > 0.0) {
                    const auto rawPts = static_cast<double>(frame->best_effort_timestamp) * streamTimeBase;
                    if (!firstPtsSeconds) {
                        firstPtsSeconds = rawPts;
                    }
                    localPts = std::max(0.0, rawPts - *firstPtsSeconds);
                }
                syntheticPts = std::max(syntheticPts, localPts + frameDuration);
                const auto pts = localPts + loopOffset;
                lastPts = std::max(lastPts, pts);

                if (pts + 0.0005 < nextOutputPts) {
                    api->av_frame_unref(frame.get());
                    continue;
                }
                nextOutputPts = pts + outputFrameDuration;

                auto output = std::make_shared<VideoFrame>();
                output->width = outputWidth;
                output->height = outputHeight;
                output->ptsSeconds = pts;
                const auto pixels = static_cast<std::size_t>(outputWidth) * static_cast<std::size_t>(outputHeight);
                constexpr auto maxFrameBytes = static_cast<std::size_t>(4096) * static_cast<std::size_t>(4096) * 4U;
                if (pixels == 0 || pixels > (maxFrameBytes / 4U)) {
                    Log::warn("Rejecting oversized decoded frame from {}", Paths::ForLog(path_));
                    return false;
                }
                output->bgra.resize(pixels * 4U);

                uint8_t* dstData[4] = { output->bgra.data(), nullptr, nullptr, nullptr };
                int dstLinesize[4] = { outputWidth * 4, 0, 0, 0 };
                api->sws_scale(
                    sws.get(),
                    frame->data,
                    frame->linesize,
                    0,
                    codecContext->height,
                    dstData,
                    dstLinesize);

                api->av_frame_unref(frame.get());
                if (!PushFrame(std::move(output))) {
                    return false;
                }
            }
            return false;
        };

        while (!stopRequested_) {
            const auto readResult = api->av_read_frame(format.get(), packet.get());
            if (readResult == AVERROR_EOF) {
                (void)api->avcodec_send_packet(codecContext.get(), nullptr);
                (void)receiveFrames();

                if (options_.loopVideo && !stopRequested_) {
                    const auto loopDuration = duration > 0.0 ? duration : (lastPts + frameDuration - loopOffset);
                    loopOffset += std::max(loopDuration, frameDuration);
                    syntheticPts = 0.0;
                    firstPtsSeconds.reset();
                    api->avcodec_flush_buffers(codecContext.get());
                    if (api->av_seek_frame(format.get(), streamIndex, 0, AVSEEK_FLAG_BACKWARD) < 0) {
                        Log::warn("FFmpeg failed to seek for looping: {}", Paths::ForLog(path_));
                        break;
                    }
                    continue;
                }
                break;
            }

            if (readResult < 0) {
                Log::warn("FFmpeg read error in {}", Paths::ForLog(path_));
                break;
            }

            if (packet->stream_index == streamIndex) {
                auto packetSent = false;
                while (!packetSent && !stopRequested_) {
                    const auto sendResult = api->avcodec_send_packet(codecContext.get(), packet.get());
                    if (sendResult == 0) {
                        packetSent = true;
                        break;
                    }
                    if (sendResult == AVERROR(EAGAIN)) {
                        if (!receiveFrames()) {
                            break;
                        }
                        continue;
                    }
                    Log::warn("FFmpeg failed to send packet from {}", Paths::ForLog(path_));
                    break;
                }
                api->av_packet_unref(packet.get());
                if (!packetSent) {
                    break;
                }
                if (!receiveFrames()) {
                    break;
                }
            } else {
                api->av_packet_unref(packet.get());
            }
        }
#endif
    }
}
