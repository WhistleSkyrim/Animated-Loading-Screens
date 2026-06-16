#pragma once

#include "Controller/AnimatedLoadingScreenController.h"

#include <RE/Skyrim.h>

#include <atomic>
#include <functional>
#include <mutex>

namespace ALS
{
    class LoadingMenuWatcher final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        using BeforeOpenCallback = std::function<bool()>;

        static LoadingMenuWatcher& GetSingleton();

        void SetController(AnimatedLoadingScreenController* controller);
        void SetBeforeOpenCallback(BeforeOpenCallback callback);
        void SetHideVanillaLoadingSpinner(bool hide);
        [[nodiscard]] bool HideVanillaLoadingSpinner() const noexcept;
        bool Install();
        void Uninstall();

        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent* event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>* source) override;

    private:
        BeforeOpenCallback CopyBeforeOpenCallback();

        std::atomic<AnimatedLoadingScreenController*> controller_{ nullptr };
        std::atomic_bool hideVanillaLoadingSpinner_{ false };
        std::atomic_bool loadingMenuOpen_{ false };
        std::mutex callbackMutex_{};
        BeforeOpenCallback beforeOpenCallback_{};
        bool installed_{ false };
    };
}
