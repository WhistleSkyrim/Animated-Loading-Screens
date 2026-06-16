#include "Runtime/LoadingMenuWatcher.h"

#include "Logging/Log.h"

#include <RE/L/LoadWaitSpinner.h>

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

    struct SuppressedLoadWaitSpinner
    {
        RE::GPtr<RE::LoadWaitSpinner> menu{};
        bool rootVisibleKnown{ false };
        bool originalRootVisible{ true };
        bool movieVisibilityTouched{ false };
    };

    std::mutex g_suppressedLoadScreenArtMutex;
    std::vector<SuppressedLoadScreenArt> g_suppressedLoadScreenArt;
    std::atomic_bool g_vanillaLoadScreenArtSuppressed{ false };
    std::mutex g_loadingMenuMovieStateMutex;
    RE::GPtr<RE::GFxMovieView> g_suppressedLoadingMenuMovie;
    float g_originalLoadingMenuMovieBackgroundAlpha{ 0.0F };
    bool g_loadingMenuMovieBackgroundSuppressed{ false };
    std::mutex g_loadWaitSpinnerStateMutex;
    SuppressedLoadWaitSpinner g_suppressedLoadWaitSpinner;

    [[nodiscard]] bool IsLoadingMenu(const RE::BSFixedString& menuName)
    {
        const auto* name = menuName.c_str();
        return name && std::string_view(name) == RE::LoadingMenu::MENU_NAME;
    }

    [[nodiscard]] bool IsLoadWaitSpinner(const RE::BSFixedString& menuName)
    {
        const auto* name = menuName.c_str();
        return name && std::string_view(name) == RE::LoadWaitSpinner::MENU_NAME;
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

    RE::GPtr<RE::LoadWaitSpinner> GetLoadWaitSpinnerForMutation(std::string_view reason)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            ALS::Log::diagnostic("load_wait_spinner_lookup reason={} result=no_ui", reason);
            return {};
        }

        auto spinner = ui->GetMenu<RE::LoadWaitSpinner>();
        if (!spinner) {
            ALS::Log::diagnostic("load_wait_spinner_lookup reason={} result=no_menu", reason);
            return {};
        }
        return spinner;
    }

    void RestoreLoadWaitSpinnerLocked(std::string_view reason)
    {
        if (!g_suppressedLoadWaitSpinner.menu) {
            return;
        }

        auto spinner = g_suppressedLoadWaitSpinner.menu;
        auto restoredRoot = false;
        auto restoredMovie = false;
        if (spinner) {
            if (g_suppressedLoadWaitSpinner.rootVisibleKnown) {
                auto& root = spinner->GetRuntimeData().root;
                RE::GFxValue::DisplayInfo displayInfo;
                if (root.GetDisplayInfo(&displayInfo)) {
                    displayInfo.SetVisible(g_suppressedLoadWaitSpinner.originalRootVisible);
                    restoredRoot = root.SetDisplayInfo(displayInfo);
                }
            }
            if (g_suppressedLoadWaitSpinner.movieVisibilityTouched && spinner->uiMovie) {
                spinner->uiMovie->SetVisible(true);
                restoredMovie = true;
            }
        }

        ALS::Log::diagnostic(
            "load_wait_spinner_restore reason={} menu={} restored_root={} original_root_visible={} restored_movie={}",
            reason,
            static_cast<const void*>(spinner.get()),
            restoredRoot,
            g_suppressedLoadWaitSpinner.originalRootVisible,
            restoredMovie);
        g_suppressedLoadWaitSpinner = {};
    }

    void RestoreLoadWaitSpinner(std::string_view reason)
    {
        std::scoped_lock lock(g_loadWaitSpinnerStateMutex);
        RestoreLoadWaitSpinnerLocked(reason);
    }

    void SuppressLoadWaitSpinner(std::string_view reason)
    {
        auto spinner = GetLoadWaitSpinnerForMutation(reason);
        if (!spinner) {
            return;
        }

        std::scoped_lock lock(g_loadWaitSpinnerStateMutex);
        if (g_suppressedLoadWaitSpinner.menu &&
            g_suppressedLoadWaitSpinner.menu.get() != spinner.get()) {
            RestoreLoadWaitSpinnerLocked("load_wait_spinner_replaced");
        }
        if (!g_suppressedLoadWaitSpinner.menu) {
            g_suppressedLoadWaitSpinner.menu = spinner;
        }

        auto rootSuppressed = false;
        auto movieSuppressed = false;
        auto& root = spinner->GetRuntimeData().root;
        RE::GFxValue::DisplayInfo displayInfo;
        if (root.GetDisplayInfo(&displayInfo)) {
            if (!g_suppressedLoadWaitSpinner.rootVisibleKnown) {
                g_suppressedLoadWaitSpinner.rootVisibleKnown = true;
                g_suppressedLoadWaitSpinner.originalRootVisible = displayInfo.GetVisible();
            }
            displayInfo.SetVisible(false);
            rootSuppressed = root.SetDisplayInfo(displayInfo);
        }

        if (!rootSuppressed && spinner->uiMovie) {
            spinner->uiMovie->SetVisible(false);
            g_suppressedLoadWaitSpinner.movieVisibilityTouched = true;
            movieSuppressed = true;
        }

        ALS::Log::diagnostic(
            "load_wait_spinner_suppression reason={} menu={} root_suppressed={} original_root_visible={} movie_suppressed={} result={}",
            reason,
            static_cast<const void*>(spinner.get()),
            rootSuppressed,
            g_suppressedLoadWaitSpinner.originalRootVisible,
            movieSuppressed,
            rootSuppressed || movieSuppressed ? "ok" : "no_display_object");
    }

    void RestoreLoadingMenuMovieBackground(std::string_view reason)
    {
        std::scoped_lock lock(g_loadingMenuMovieStateMutex);
        if (!g_loadingMenuMovieBackgroundSuppressed) {
            return;
        }

        if (g_suppressedLoadingMenuMovie) {
            g_suppressedLoadingMenuMovie->SetBackgroundAlpha(g_originalLoadingMenuMovieBackgroundAlpha);
        }
        ALS::Log::diagnostic(
            "loading_menu_movie_background_restore reason={} movie={} alpha={}",
            reason,
            static_cast<const void*>(g_suppressedLoadingMenuMovie.get()),
            g_originalLoadingMenuMovieBackgroundAlpha);

        g_suppressedLoadingMenuMovie = nullptr;
        g_originalLoadingMenuMovieBackgroundAlpha = 0.0F;
        g_loadingMenuMovieBackgroundSuppressed = false;
    }

    void PrepareLoadingMenuMovieForOverlayText(const RE::GPtr<RE::LoadingMenu>& loadingMenu, std::string_view reason)
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
        {
            std::scoped_lock lock(g_loadingMenuMovieStateMutex);
            if (!g_loadingMenuMovieBackgroundSuppressed ||
                g_suppressedLoadingMenuMovie.get() != loadingMenu->uiMovie.get()) {
                if (g_loadingMenuMovieBackgroundSuppressed && g_suppressedLoadingMenuMovie) {
                    g_suppressedLoadingMenuMovie->SetBackgroundAlpha(g_originalLoadingMenuMovieBackgroundAlpha);
                }

                g_suppressedLoadingMenuMovie = loadingMenu->uiMovie;
                g_originalLoadingMenuMovieBackgroundAlpha = loadingMenu->uiMovie->GetBackgroundAlpha();
                g_loadingMenuMovieBackgroundSuppressed = true;
            }
            loadingMenu->uiMovie->SetBackgroundAlpha(0.0F);
        }
        ALS::Log::diagnostic(
            "loading_menu_movie_visibility reason={} visible=true paused=false background_alpha=0 result=ok menu={} movie={}",
            reason,
            static_cast<const void*>(loadingMenu.get()),
            static_cast<const void*>(loadingMenu->uiMovie.get()));
    }

    void SuppressVanillaLoadScreenArt(std::string_view reason)
    {
        auto loadingMenu = GetLoadingMenuForMutation(reason);
        PrepareLoadingMenuMovieForOverlayText(loadingMenu, reason);
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
        RestoreLoadingMenuMovieBackground(reason);
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

    void LoadingMenuWatcher::SetHideVanillaLoadingSpinner(bool hide)
    {
        hideVanillaLoadingSpinner_.store(hide, std::memory_order_release);
        Log::diagnostic("loading_menu_watcher_set_hide_spinner hide={}", hide);
        if (!hide) {
            RestoreLoadWaitSpinner("hide_spinner_disabled");
            return;
        }

        auto* controller = controller_.load(std::memory_order_acquire);
        if (loadingMenuOpen_.load(std::memory_order_acquire) && controller && controller->IsOverlayActive()) {
            SuppressLoadWaitSpinner("hide_spinner_enabled_runtime");
        }
    }

    bool LoadingMenuWatcher::HideVanillaLoadingSpinner() const noexcept
    {
        return hideVanillaLoadingSpinner_.load(std::memory_order_acquire);
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
        loadingMenuOpen_.store(false, std::memory_order_release);
        SetBeforeOpenCallback({});
        RestoreLoadWaitSpinner("watcher_uninstall");
        RestoreVanillaLoadScreenArt("watcher_uninstall");
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
        const auto loadWaitSpinner = IsLoadWaitSpinner(event->menuName);
        Log::diagnostic(
            "menu_event name={} opening={} loading_menu={} load_wait_spinner={} controller={}",
            menuName ? menuName : "<null>",
            event->opening,
            loadingMenu,
            loadWaitSpinner,
            static_cast<const void*>(controller));

        if (loadWaitSpinner) {
            if (event->opening) {
                if (HideVanillaLoadingSpinner() &&
                    loadingMenuOpen_.load(std::memory_order_acquire) &&
                    controller &&
                    controller->IsOverlayActive()) {
                    SuppressLoadWaitSpinner("load_wait_spinner_opened");
                }
            } else {
                RestoreLoadWaitSpinner("load_wait_spinner_closing");
            }
            return RE::BSEventNotifyControl::kContinue;
        }

        if (!controller || !loadingMenu) {
            return RE::BSEventNotifyControl::kContinue;
        }

        try {
            if (event->opening) {
                loadingMenuOpen_.store(true, std::memory_order_release);
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
                    RestoreLoadWaitSpinner("animation_not_allowed");
                    RestoreVanillaLoadScreenArt("animation_not_allowed");
                    controller->OnLoadingMenuClose();
                    return RE::BSEventNotifyControl::kContinue;
                }
                SuppressVanillaLoadScreenArt("before_controller_open");
                if (HideVanillaLoadingSpinner()) {
                    SuppressLoadWaitSpinner("before_controller_open");
                }
                Log::diagnostic("loading_menu_open_controller_begin");
                controller->OnLoadingMenuOpen();
                Log::diagnostic(
                    "loading_menu_open_controller_end overlay_active={} state={}",
                    controller->IsOverlayActive(),
                    static_cast<int>(controller->State()));
                if (controller->IsOverlayActive()) {
                    SuppressVanillaLoadScreenArt("controller_opened");
                    if (HideVanillaLoadingSpinner()) {
                        SuppressLoadWaitSpinner("controller_opened");
                    }
                } else {
                    RestoreLoadWaitSpinner("controller_open_failed");
                    RestoreVanillaLoadScreenArt("controller_open_failed");
                }
            } else {
                loadingMenuOpen_.store(false, std::memory_order_release);
                Log::diagnostic("loading_menu_close_controller_begin");
                RestoreLoadWaitSpinner("loading_menu_closing");
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
