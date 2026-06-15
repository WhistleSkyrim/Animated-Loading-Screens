#include "Runtime/LoadingMenuWatcher.h"

#include "Logging/Log.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    struct SuppressedLoadScreenArt
    {
        RE::TESLoadScreen::LoadNIFData* data{ nullptr };
        RE::TESBoundObject* loadNIF{ nullptr };
    };

    std::mutex g_suppressedLoadScreenArtMutex;
    std::vector<SuppressedLoadScreenArt> g_suppressedLoadScreenArt;
    std::atomic_bool g_vanillaLoadScreenArtSuppressed{ false };

    [[nodiscard]] bool IsLoadingMenu(const RE::BSFixedString& menuName)
    {
        const auto* name = menuName.c_str();
        return name && std::string_view(name) == RE::LoadingMenu::MENU_NAME;
    }

    RE::GPtr<RE::LoadingMenu> GetLoadingMenuForMutation(std::string_view reason)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            ALS::Log::diagnostic("loading_menu_lookup reason={} result=no_ui", reason);
            return {};
        }

        auto loadingMenu = ui->GetMenu<RE::LoadingMenu>();
        if (!loadingMenu) {
            ALS::Log::diagnostic("loading_menu_lookup reason={} result=no_menu", reason);
            return {};
        }
        return loadingMenu;
    }

    void EnsureLoadingMenuMovieVisible(const RE::GPtr<RE::LoadingMenu>& loadingMenu, std::string_view reason)
    {
        if (!loadingMenu) {
            ALS::Log::diagnostic("loading_menu_movie_visibility reason={} visible=true result=no_menu", reason);
            return;
        }
        if (!loadingMenu->uiMovie) {
            ALS::Log::diagnostic("loading_menu_movie_visibility reason={} visible=true result=no_movie", reason);
            return;
        }

        loadingMenu->uiMovie->SetPause(false);
        loadingMenu->uiMovie->SetVisible(true);
        ALS::Log::diagnostic(
            "loading_menu_movie_visibility reason={} visible=true paused=false result=ok menu={} movie={}",
            reason,
            static_cast<const void*>(loadingMenu.get()),
            static_cast<const void*>(loadingMenu->uiMovie.get()));
    }

    void SuppressVanillaLoadScreenArt(std::string_view reason)
    {
        auto loadingMenu = GetLoadingMenuForMutation(reason);
        EnsureLoadingMenuMovieVisible(loadingMenu, reason);
        if (!loadingMenu) {
            return;
        }

        std::scoped_lock lock(g_suppressedLoadScreenArtMutex);
        auto& runtimeData = loadingMenu->GetRuntimeData();
        std::size_t visited = 0;
        std::size_t suppressed = 0;
        for (auto* loadScreen : runtimeData.loadScreens) {
            ++visited;
            if (!loadScreen || !loadScreen->loadNIFData || !loadScreen->loadNIFData->loadNIF) {
                continue;
            }

            auto* nifData = loadScreen->loadNIFData;
            const auto alreadySuppressed = std::ranges::any_of(
                g_suppressedLoadScreenArt,
                [nifData](const SuppressedLoadScreenArt& entry) {
                    return entry.data == nifData;
                });
            if (alreadySuppressed) {
                continue;
            }

            g_suppressedLoadScreenArt.push_back({ nifData, nifData->loadNIF });
            nifData->loadNIF = nullptr;
            ++suppressed;
        }
        if (!g_suppressedLoadScreenArt.empty()) {
            g_vanillaLoadScreenArtSuppressed.store(true, std::memory_order_release);
        }
        ALS::Log::diagnostic(
            "vanilla_load_screen_art_suppression reason={} visited={} newly_suppressed={} total_suppressed={} result=ok",
            reason,
            visited,
            suppressed,
            g_suppressedLoadScreenArt.size());
    }

    void RestoreVanillaLoadScreenArt(std::string_view reason)
    {
        std::scoped_lock lock(g_suppressedLoadScreenArtMutex);
        std::size_t restored = 0;
        for (auto& entry : g_suppressedLoadScreenArt) {
            if (entry.data && !entry.data->loadNIF) {
                entry.data->loadNIF = entry.loadNIF;
                ++restored;
            }
        }
        ALS::Log::diagnostic(
            "vanilla_load_screen_art_restore reason={} restored={} total_suppressed={}",
            reason,
            restored,
            g_suppressedLoadScreenArt.size());
        g_suppressedLoadScreenArt.clear();
        g_vanillaLoadScreenArtSuppressed.store(false, std::memory_order_release);
    }
}

namespace ALS
{
    LoadingMenuWatcher& LoadingMenuWatcher::GetSingleton()
    {
        static LoadingMenuWatcher watcher;
        return watcher;
    }

    void LoadingMenuWatcher::SetController(AnimatedLoadingScreenController* controller)
    {
        controller_.store(controller, std::memory_order_release);
        Log::diagnostic("loading_menu_watcher_set_controller controller={}", static_cast<const void*>(controller));
    }

    void LoadingMenuWatcher::SetBeforeOpenCallback(BeforeOpenCallback callback)
    {
        std::lock_guard lock(callbackMutex_);
        beforeOpenCallback_ = std::move(callback);
    }

    LoadingMenuWatcher::BeforeOpenCallback LoadingMenuWatcher::CopyBeforeOpenCallback()
    {
        std::lock_guard lock(callbackMutex_);
        return beforeOpenCallback_;
    }

    bool LoadingMenuWatcher::Install()
    {
        if (installed_) {
            return true;
        }

        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            Log::warn("RE::UI singleton is not available yet. LoadingMenu watcher was not installed.");
            return false;
        }

        ui->AddEventSink<RE::MenuOpenCloseEvent>(this);
        installed_ = true;
        Log::info("LoadingMenu watcher installed.");
        Log::diagnostic("loading_menu_watcher_installed ui={} sink={}", static_cast<const void*>(ui), static_cast<const void*>(this));
        return true;
    }

    void LoadingMenuWatcher::Uninstall()
    {
        controller_.store(nullptr, std::memory_order_release);
        SetBeforeOpenCallback({});
        if (!installed_) {
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        if (ui) {
            ui->RemoveEventSink<RE::MenuOpenCloseEvent>(this);
        }
        installed_ = false;
        Log::info("LoadingMenu watcher uninstalled.");
        Log::diagnostic("loading_menu_watcher_uninstalled");
    }

    RE::BSEventNotifyControl LoadingMenuWatcher::ProcessEvent(
        const RE::MenuOpenCloseEvent* event,
        RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
    {
        auto* controller = controller_.load(std::memory_order_acquire);
        if (!event) {
            Log::diagnostic("menu_event null_event=true controller={}", static_cast<const void*>(controller));
            return RE::BSEventNotifyControl::kContinue;
        }

        const auto* menuName = event->menuName.c_str();
        const auto loadingMenu = IsLoadingMenu(event->menuName);
        Log::diagnostic(
            "menu_event name={} opening={} loading_menu={} controller={}",
            menuName ? menuName : "<null>",
            event->opening,
            loadingMenu,
            static_cast<const void*>(controller));

        if (!controller || !loadingMenu) {
            return RE::BSEventNotifyControl::kContinue;
        }

        try {
            if (event->opening) {
                bool allowAnimation = true;
                if (auto callback = CopyBeforeOpenCallback()) {
                    Log::diagnostic("loading_menu_before_open_callback_begin");
                    allowAnimation = callback();
                    Log::diagnostic(
                        "loading_menu_before_open_callback_end has_controller={} allow_animation={}",
                        static_cast<bool>(controller),
                        allowAnimation);
                }
                if (!allowAnimation) {
                    Log::diagnostic("loading_menu_open_skipped_by_callback");
                    RestoreVanillaLoadScreenArt("animation_not_allowed");
                    controller->OnLoadingMenuClose();
                    return RE::BSEventNotifyControl::kContinue;
                }
                SuppressVanillaLoadScreenArt("before_controller_open");
                Log::diagnostic("loading_menu_open_controller_begin");
                controller->OnLoadingMenuOpen();
                Log::diagnostic(
                    "loading_menu_open_controller_end overlay_active={} state={}",
                    controller->IsOverlayActive(),
                    static_cast<int>(controller->State()));
                if (controller->IsOverlayActive()) {
                    SuppressVanillaLoadScreenArt("controller_opened");
                } else {
                    RestoreVanillaLoadScreenArt("controller_open_failed");
                }
            } else {
                Log::diagnostic("loading_menu_close_controller_begin");
                RestoreVanillaLoadScreenArt("loading_menu_closing");
                controller->OnLoadingMenuClose();
                Log::diagnostic("loading_menu_close_controller_end overlay_active={}", controller->IsOverlayActive());
            }
        } catch (const std::exception& e) {
            Log::error("Exception while handling LoadingMenu event: {}", e.what());
            Log::diagnostic("loading_menu_event_exception what={}", e.what());
        } catch (...) {
            Log::error("Unknown exception while handling LoadingMenu event.");
            Log::diagnostic("loading_menu_event_exception what=<unknown>");
        }

        return RE::BSEventNotifyControl::kContinue;
    }
}
