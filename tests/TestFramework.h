#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace ALS::Tests
{
    struct TestCase
    {
        std::string name;
        std::function<void()> run;
    };

    inline std::vector<TestCase>& Registry()
    {
        static std::vector<TestCase> tests;
        return tests;
    }

    struct Registrar
    {
        Registrar(std::string name, std::function<void()> run)
        {
            Registry().push_back(TestCase{ std::move(name), std::move(run) });
        }
    };

    inline void Expect(bool condition, std::string_view message)
    {
        if (!condition) {
            throw std::runtime_error(std::string(message));
        }
    }

    template <class T, class U>
    void ExpectEq(const T& actual, const U& expected, std::string_view message)
    {
        if (!(actual == expected)) {
            throw std::runtime_error(std::string(message));
        }
    }

    inline std::uint64_t CurrentProcessId()
    {
#if defined(_WIN32)
        return static_cast<std::uint64_t>(_getpid());
#else
        return static_cast<std::uint64_t>(getpid());
#endif
    }

    inline const std::string& TempRunSuffix()
    {
        static const std::string suffix = [] {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
            return std::to_string(CurrentProcessId()) + "-" + std::to_string(ticks);
        }();
        return suffix;
    }

    inline std::filesystem::path MakeTempDir(std::string_view name)
    {
        static std::atomic_uint64_t counter{ 0 };
        const auto uniqueName = std::string(name) + "-" + TempRunSuffix() + "-" + std::to_string(++counter);
        const auto root = std::filesystem::temp_directory_path() / "AnimatedLoadingScreensTests" / uniqueName;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        return root;
    }

    class ScopedCurrentPath
    {
    public:
        explicit ScopedCurrentPath(const std::filesystem::path& path) :
            previous_(std::filesystem::current_path())
        {
            std::filesystem::current_path(path);
        }

        ~ScopedCurrentPath()
        {
            std::filesystem::current_path(previous_);
        }

        ScopedCurrentPath(const ScopedCurrentPath&) = delete;
        ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;

    private:
        std::filesystem::path previous_;
    };
}

#define ALS_TEST(name)                                                                 \
    static void name();                                                                \
    static ::ALS::Tests::Registrar registrar_##name(#name, name);                      \
    static void name()
