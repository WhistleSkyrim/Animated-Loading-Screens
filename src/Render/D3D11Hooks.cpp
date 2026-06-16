#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Render/D3D11Hooks.h"

#include "Logging/Log.h"
#include "Render/D3D11OverlayRenderer.h"
#include "Render/SkyrimSwapChain.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <REL/Relocation.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <RE/Offsets_VTABLE.h>

#include <Windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <MinHook.h>

#include <atomic>
#include <mutex>

namespace RE
{
    class LoadingMenu;
}

namespace ALS
{
    void SuppressVanillaLoadingSpinnerForActiveOverlay() noexcept;
}

namespace
{
    using Microsoft::WRL::ComPtr;

    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
    using LoadingMenuPostDisplayFn = void (*)(RE::LoadingMenu*);

    PresentFn g_originalPresent = nullptr;
    Present1Fn g_originalPresent1 = nullptr;
    ResizeBuffersFn g_originalResizeBuffers = nullptr;
    ResizeBuffers1Fn g_originalResizeBuffers1 = nullptr;
    LoadingMenuPostDisplayFn g_originalLoadingMenuPostDisplay = nullptr;
    void* g_presentAddress = nullptr;
    void* g_present1Address = nullptr;
    void* g_resizeBuffersAddress = nullptr;
    void* g_resizeBuffers1Address = nullptr;
    std::uintptr_t g_loadingMenuPostDisplayEntry = 0;
    std::atomic<ALS::AnimatedLoadingScreenController*> g_controller{ nullptr };
    std::atomic<IDXGISwapChain*> g_targetSwapChain{ nullptr };
    std::atomic_bool g_overlayWasActive{ false };
    std::atomic_bool g_skipNextOverlayDraw{ true };
    std::atomic_bool g_postDisplayOverlaySubmitted{ false };
    std::atomic_uint g_activeDetours{ 0 };
    std::atomic_uint g_presentHits{ 0 };
    std::atomic_uint g_present1Hits{ 0 };
    std::atomic_uint g_targetMismatchLogs{ 0 };
    std::atomic_uint g_noTargetLogs{ 0 };
    std::atomic_uint g_inactiveOverlayLogs{ 0 };
    std::atomic_uint g_noRenderDataLogs{ 0 };
    std::atomic_uint g_skipFirstDrawLogs{ 0 };
    std::atomic_uint g_renderFailureLogs{ 0 };
    std::atomic_uint g_postDisplayHits{ 0 };
    std::atomic_uint g_postDisplayNoRenderDataLogs{ 0 };
    std::atomic_uint g_postDisplayRenderFailureLogs{ 0 };
    std::atomic_uint g_presentSkippedAfterPostDisplayLogs{ 0 };
    std::atomic_bool g_loggedFirstPostDisplayRender{ false };
    std::atomic_bool g_loggedFirstSuccessfulRender{ false };
    std::atomic_bool g_uninstalling{ false };
    ALS::D3D11OverlayRenderer g_renderer;
    std::mutex g_rendererMutex;
    bool g_installed = false;
    bool g_loadingMenuPostDisplayHookInstalled = false;

    struct DetourScope
    {
        bool active{ false };

        DetourScope()
        {
            if (g_uninstalling.load(std::memory_order_acquire)) {
                return;
            }
            g_activeDetours.fetch_add(1, std::memory_order_acq_rel);
            if (g_uninstalling.load(std::memory_order_acquire)) {
                g_activeDetours.fetch_sub(1, std::memory_order_acq_rel);
                return;
            }
            active = true;
        }

        ~DetourScope()
        {
            if (active) {
                g_activeDetours.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
    };

    thread_local bool g_insidePresentCall = false;
    thread_local bool g_insideLoadingMenuPostDisplay = false;

    struct PresentCallScope
    {
        bool entered{ false };

        PresentCallScope()
        {
            if (!g_insidePresentCall) {
                g_insidePresentCall = true;
                entered = true;
            }
        }

        ~PresentCallScope()
        {
            if (entered) {
                g_insidePresentCall = false;
            }
        }
    };

    [[nodiscard]] IDXGISwapChain* GetSkyrimSwapChain()
    {
        return reinterpret_cast<IDXGISwapChain*>(ALS::SkyrimSwapChain::Get());
    }

    [[nodiscard]] void** GetInterfaceVTable(IUnknown* object) noexcept
    {
        return object ? *reinterpret_cast<void***>(object) : nullptr;
    }

    [[nodiscard]] bool ShouldLog(std::atomic_uint& counter, unsigned limit)
    {
        return counter.fetch_add(1, std::memory_order_relaxed) < limit;
    }

    [[nodiscard]] bool SameComIdentity(IUnknown* left, IUnknown* right)
    {
        if (!left || !right) {
            return false;
        }
        if (left == right) {
            return true;
        }

        ComPtr<IUnknown> leftIdentity;
        ComPtr<IUnknown> rightIdentity;
        if (FAILED(left->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(leftIdentity.GetAddressOf()))) ||
            FAILED(right->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(rightIdentity.GetAddressOf())))) {
            return false;
        }
        return leftIdentity.Get() == rightIdentity.Get();
    }

    [[nodiscard]] bool IsTargetSwapChain(IDXGISwapChain* candidate, IDXGISwapChain* target)
    {
        if (!candidate || !target) {
            return false;
        }
        if (candidate == target) {
            return true;
        }
        return SameComIdentity(candidate, target);
    }

    void LoadingMenuPostDisplayDetour(RE::LoadingMenu* menu)
    {
        if (g_insideLoadingMenuPostDisplay) {
            if (g_originalLoadingMenuPostDisplay) {
                g_originalLoadingMenuPostDisplay(menu);
            }
            return;
        }

        struct Scope
        {
            Scope() { g_insideLoadingMenuPostDisplay = true; }
            ~Scope() { g_insideLoadingMenuPostDisplay = false; }
        } scope;

        bool renderedBeforeOriginal = false;
        try {
            const auto renderOverlayBeforeOriginal = [&]() -> bool {
                auto* controller = g_controller.load(std::memory_order_acquire);
                if (!controller || !controller->IsOverlayActive()) {
                    return false;
                }

                const auto renderData = controller->TryBuildRenderFrame();
                const auto hitIndex = g_postDisplayHits.fetch_add(1, std::memory_order_relaxed);
                if (hitIndex < 40 || (hitIndex % 120) == 0) {
                    ALS::Log::diagnostic(
                        "loading_menu_postdisplay_render_data index={} menu={} should_render={} draw_background={} current_frame={} current_alpha={} next_frame={} next_alpha={} opacity={}",
                        hitIndex,
                        static_cast<const void*>(menu),
                        renderData.shouldRender,
                        renderData.drawBackground,
                        static_cast<bool>(renderData.current.frame),
                        renderData.current.alpha,
                        static_cast<bool>(renderData.next.frame),
                        renderData.next.alpha,
                        renderData.opacity);
                }
                if (!renderData.shouldRender) {
                    if (ShouldLog(g_postDisplayNoRenderDataLogs, 20)) {
                        ALS::Log::diagnostic("loading_menu_postdisplay_no_render_data index={}", hitIndex);
                    }
                    return false;
                }

                auto* swapChain = g_targetSwapChain.load(std::memory_order_acquire);
                if (!swapChain) {
                    swapChain = GetSkyrimSwapChain();
                }
                if (!swapChain) {
                    if (ShouldLog(g_postDisplayRenderFailureLogs, 10)) {
                        ALS::Log::diagnostic("loading_menu_postdisplay_render_result index={} success=false reason=no_swapchain", hitIndex);
                    }
                    return false;
                }

                ComPtr<ID3D11Device> device;
                if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(device.GetAddressOf()))) || !device) {
                    if (ShouldLog(g_postDisplayRenderFailureLogs, 10)) {
                        ALS::Log::diagnostic("loading_menu_postdisplay_render_result index={} success=false reason=no_device", hitIndex);
                    }
                    return false;
                }

                ComPtr<ID3D11DeviceContext> context;
                device->GetImmediateContext(context.GetAddressOf());
                if (!context) {
                    if (ShouldLog(g_postDisplayRenderFailureLogs, 10)) {
                        ALS::Log::diagnostic("loading_menu_postdisplay_render_result index={} success=false reason=no_context", hitIndex);
                    }
                    return false;
                }

                std::scoped_lock renderLock(g_rendererMutex);
                if (g_renderer.RenderCurrentTarget(context.Get(), renderData)) {
                    if (hitIndex < 40 || (hitIndex % 120) == 0) {
                        ALS::Log::diagnostic("loading_menu_postdisplay_render_result index={} success=true phase=before_original", hitIndex);
                    }
                    if (!g_loggedFirstPostDisplayRender.exchange(true, std::memory_order_acq_rel)) {
                        ALS::Log::info("Overlay render succeeded before LoadingMenu::PostDisplay.");
                    }
                    return true;
                }
                if (ShouldLog(g_postDisplayRenderFailureLogs, 10)) {
                    ALS::Log::diagnostic("loading_menu_postdisplay_render_result index={} success=false reason=renderer_returned_false", hitIndex);
                }
                return false;
            };

            renderedBeforeOriginal = renderOverlayBeforeOriginal();
            if (renderedBeforeOriginal) {
                g_postDisplayOverlaySubmitted.store(true, std::memory_order_release);
            }
        } catch (const std::exception& e) {
            ALS::Log::error("Exception in LoadingMenu::PostDisplay hook: {}", e.what());
            ALS::Log::diagnostic("loading_menu_postdisplay_exception what={}", e.what());
        } catch (...) {
            ALS::Log::error("Unknown exception in LoadingMenu::PostDisplay hook.");
            ALS::Log::diagnostic("loading_menu_postdisplay_exception what=<unknown>");
        }
        if (!renderedBeforeOriginal) {
            g_postDisplayOverlaySubmitted.store(false, std::memory_order_release);
        }

        ALS::SuppressVanillaLoadingSpinnerForActiveOverlay();
        if (g_originalLoadingMenuPostDisplay) {
            g_originalLoadingMenuPostDisplay(menu);
        }
    }

    [[nodiscard]] bool RenderOverlayIfNeeded(IDXGISwapChain* swapChain, UINT flags)
    {
        const DetourScope scope;
        try {
            auto* targetSwapChain = g_targetSwapChain.load(std::memory_order_acquire);
            if (!scope.active) {
                return false;
            }
            if (!targetSwapChain) {
                if (ShouldLog(g_noTargetLogs, 5)) {
                    ALS::Log::info(
                        "D3D Present hook hit before target swapchain was stored. swapchain={}, flags=0x{:X}.",
                        static_cast<const void*>(swapChain),
                        static_cast<unsigned>(flags));
                    ALS::Log::diagnostic(
                        "present_hook_no_target swapchain={} flags=0x{:X}",
                        static_cast<const void*>(swapChain),
                        static_cast<unsigned>(flags));
                }
                return false;
            }
            if (!IsTargetSwapChain(swapChain, targetSwapChain)) {
                if (ShouldLog(g_targetMismatchLogs, 10)) {
                    ALS::Log::info(
                        "D3D Present hook ignored non-target swapchain. candidate={}, target={}, flags=0x{:X}.",
                        static_cast<const void*>(swapChain),
                        static_cast<const void*>(targetSwapChain),
                        static_cast<unsigned>(flags));
                    ALS::Log::diagnostic(
                        "present_hook_target_mismatch candidate={} target={} flags=0x{:X}",
                        static_cast<const void*>(swapChain),
                        static_cast<const void*>(targetSwapChain),
                        static_cast<unsigned>(flags));
                }
                return false;
            }

            auto* controller = g_controller.load(std::memory_order_acquire);
            const auto overlayActive = controller && controller->IsOverlayActive();
            if (!overlayActive) {
                if (ShouldLog(g_inactiveOverlayLogs, 10)) {
                    ALS::Log::info(
                        "D3D Present hook hit target swapchain, but overlay is inactive. controller={}, flags=0x{:X}.",
                        static_cast<const void*>(controller),
                        static_cast<unsigned>(flags));
                    ALS::Log::diagnostic(
                        "present_hook_overlay_inactive controller={} flags=0x{:X}",
                        static_cast<const void*>(controller),
                        static_cast<unsigned>(flags));
                }
                g_overlayWasActive.store(false, std::memory_order_release);
                g_skipNextOverlayDraw.store(true, std::memory_order_release);
                g_postDisplayOverlaySubmitted.store(false, std::memory_order_release);
                return false;
            }
            if (!g_overlayWasActive.exchange(true, std::memory_order_acq_rel)) {
                g_skipNextOverlayDraw.store(true, std::memory_order_release);
                g_postDisplayOverlaySubmitted.store(false, std::memory_order_release);
            }

            if ((flags & DXGI_PRESENT_TEST) == 0) {
                if (g_postDisplayOverlaySubmitted.exchange(false, std::memory_order_acq_rel)) {
                    if (ShouldLog(g_presentSkippedAfterPostDisplayLogs, 20)) {
                        ALS::Log::diagnostic("present_hook_skip_after_postdisplay flags=0x{:X}", static_cast<unsigned>(flags));
                    }
                    return false;
                }

                const auto renderData = controller->TryBuildRenderFrame();
                static std::atomic_uint renderDataDiagnostics{ 0 };
                const auto diagnosticIndex = renderDataDiagnostics.fetch_add(1, std::memory_order_relaxed);
                if (diagnosticIndex < 40 || (diagnosticIndex % 120) == 0) {
                    ALS::Log::diagnostic(
                        "present_hook_render_data index={} should_render={} draw_background={} current_frame={} current_alpha={} next_frame={} next_alpha={} opacity={} flags=0x{:X}",
                        diagnosticIndex,
                        renderData.shouldRender,
                        renderData.drawBackground,
                        static_cast<bool>(renderData.current.frame),
                        renderData.current.alpha,
                        static_cast<bool>(renderData.next.frame),
                        renderData.next.alpha,
                        renderData.opacity,
                        static_cast<unsigned>(flags));
                }
                if (!renderData.shouldRender) {
                    if (ShouldLog(g_noRenderDataLogs, 20)) {
                        ALS::Log::info("Overlay is active, but no renderable frame is available yet.");
                        ALS::Log::diagnostic("present_hook_no_render_data index={}", diagnosticIndex);
                    }
                    return false;
                }
                if (renderData.shouldRender) {
                    if (g_skipNextOverlayDraw.exchange(false, std::memory_order_acq_rel)) {
                        if (ShouldLog(g_skipFirstDrawLogs, 3)) {
                            ALS::Log::info("Skipping first overlay draw after activation to avoid presenting stale D3D state.");
                            ALS::Log::diagnostic("present_hook_skip_first_draw index={}", diagnosticIndex);
                        }
                        return false;
                    }
                    std::scoped_lock renderLock(g_rendererMutex);
                    if (g_renderer.Render(swapChain, renderData)) {
                        if (diagnosticIndex < 40 || (diagnosticIndex % 120) == 0) {
                            ALS::Log::diagnostic("present_hook_render_result index={} success=true", diagnosticIndex);
                        }
                        if (!g_loggedFirstSuccessfulRender.exchange(true, std::memory_order_acq_rel)) {
                            ALS::Log::info("Overlay render succeeded on Skyrim swapchain.");
                        }
                        return true;
                    } else if (ShouldLog(g_renderFailureLogs, 10)) {
                        ALS::Log::warn("Overlay render was requested but D3D renderer returned false.");
                        ALS::Log::diagnostic("present_hook_render_result index={} success=false", diagnosticIndex);
                    }
                }
            }
        } catch (const std::exception& e) {
            ALS::Log::error("Exception in D3D11 Present hook: {}", e.what());
            ALS::Log::diagnostic("present_hook_exception what={}", e.what());
        } catch (...) {
            ALS::Log::error("Unknown exception in D3D11 Present hook.");
            ALS::Log::diagnostic("present_hook_exception what=<unknown>");
        }
        return false;
    }

    void InvalidateRendererIfNeeded(IDXGISwapChain* swapChain)
    {
        const DetourScope scope;
        const auto targetSwapChain = g_targetSwapChain.load(std::memory_order_acquire);
        if (scope.active && targetSwapChain && IsTargetSwapChain(swapChain, targetSwapChain)) {
            std::scoped_lock renderLock(g_rendererMutex);
            g_renderer.PrepareForResize();
        }
    }

    HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
    {
        const PresentCallScope callScope;
        if (callScope.entered) {
            if (ShouldLog(g_presentHits, 10)) {
                ALS::Log::info(
                    "IDXGISwapChain::Present detour hit. swapchain={}, syncInterval={}, flags=0x{:X}.",
                    static_cast<const void*>(swapChain),
                    static_cast<unsigned>(syncInterval),
                    static_cast<unsigned>(flags));
            }
        }
        const auto originalPresent = g_originalPresent;
        if (!originalPresent) {
            return DXGI_ERROR_INVALID_CALL;
        }
        if (callScope.entered) {
            (void)RenderOverlayIfNeeded(swapChain, flags);
        }
        return originalPresent(swapChain, syncInterval, flags);
    }

    HRESULT STDMETHODCALLTYPE Present1Detour(
        IDXGISwapChain1* swapChain,
        UINT syncInterval,
        UINT presentFlags,
        const DXGI_PRESENT_PARAMETERS* presentParameters)
    {
        const PresentCallScope callScope;
        if (callScope.entered) {
            if (ShouldLog(g_present1Hits, 10)) {
                ALS::Log::info(
                    "IDXGISwapChain1::Present1 detour hit. swapchain={}, syncInterval={}, flags=0x{:X}.",
                    static_cast<const void*>(swapChain),
                    static_cast<unsigned>(syncInterval),
                    static_cast<unsigned>(presentFlags));
            }
        }
        const auto originalPresent1 = g_originalPresent1;
        if (!originalPresent1) {
            return DXGI_ERROR_INVALID_CALL;
        }
        if (callScope.entered) {
            (void)RenderOverlayIfNeeded(swapChain, presentFlags);
        }
        return originalPresent1(swapChain, syncInterval, presentFlags, presentParameters);
    }

    HRESULT STDMETHODCALLTYPE ResizeBuffersDetour(
        IDXGISwapChain* swapChain,
        UINT bufferCount,
        UINT width,
        UINT height,
        DXGI_FORMAT newFormat,
        UINT swapChainFlags)
    {
        InvalidateRendererIfNeeded(swapChain);
        const auto originalResizeBuffers = g_originalResizeBuffers;
        return originalResizeBuffers ?
            originalResizeBuffers(swapChain, bufferCount, width, height, newFormat, swapChainFlags) :
            DXGI_ERROR_INVALID_CALL;
    }

    HRESULT STDMETHODCALLTYPE ResizeBuffers1Detour(
        IDXGISwapChain3* swapChain,
        UINT bufferCount,
        UINT width,
        UINT height,
        DXGI_FORMAT newFormat,
        UINT swapChainFlags,
        const UINT* creationNodeMask,
        IUnknown* const* presentQueue)
    {
        InvalidateRendererIfNeeded(swapChain);
        const auto originalResizeBuffers1 = g_originalResizeBuffers1;
        return originalResizeBuffers1 ?
            originalResizeBuffers1(swapChain, bufferCount, width, height, newFormat, swapChainFlags, creationNodeMask, presentQueue) :
            DXGI_ERROR_INVALID_CALL;
    }

    [[nodiscard]] bool MhSucceededOrAlready(MH_STATUS status)
    {
        return status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED;
    }

    template <class Fn>
    [[nodiscard]] bool CreateRequiredHook(void* address, void* detour, Fn& original, const char* name)
    {
        const auto status = MH_CreateHook(address, detour, reinterpret_cast<void**>(&original));
        if (status == MH_OK) {
            return true;
        }
        if (status == MH_ERROR_ALREADY_CREATED && original) {
            ALS::Log::warn("{} hook was already created by this MinHook instance; reusing captured original.", name);
            return true;
        }

        ALS::Log::error("Failed to create {} hook: {}", name, static_cast<int>(status));
        original = nullptr;
        return false;
    }

    template <class Fn>
    void CreateOptionalHook(void* address, void* detour, Fn& original, const char* name)
    {
        if (!address) {
            return;
        }

        const auto status = MH_CreateHook(address, detour, reinterpret_cast<void**>(&original));
        if (status == MH_OK) {
            return;
        }
        if (status == MH_ERROR_ALREADY_CREATED && original) {
            ALS::Log::warn("{} hook was already created by this MinHook instance; reusing captured original.", name);
            return;
        }

        ALS::Log::warn("Failed to create {} hook: {}. Continuing without this notification path.", name, static_cast<int>(status));
        original = nullptr;
    }

    void EnableOptionalHook(void* address, const void* original, const char* name)
    {
        if (!address || !original) {
            return;
        }

        const auto status = MH_EnableHook(address);
        if (status != MH_OK && status != MH_ERROR_ENABLED) {
            ALS::Log::warn("Failed to enable {} hook: {}", name, static_cast<int>(status));
        }
    }

    bool InstallLoadingMenuPostDisplayHook()
    {
        if (g_loadingMenuPostDisplayHookInstalled) {
            return true;
        }

        REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_LoadingMenu[0] };
        if (!vtable.address()) {
            ALS::Log::warn("LoadingMenu vtable is unavailable. UI render hook was not installed.");
            ALS::Log::diagnostic("loading_menu_postdisplay_hook_install result=false reason=no_vtable");
            return false;
        }

        constexpr std::size_t postDisplayIndex = 0x06;
        g_loadingMenuPostDisplayEntry = vtable.address() + (sizeof(void*) * postDisplayIndex);
        g_originalLoadingMenuPostDisplay = reinterpret_cast<LoadingMenuPostDisplayFn>(
            vtable.write_vfunc(postDisplayIndex, reinterpret_cast<std::uintptr_t>(&LoadingMenuPostDisplayDetour)));
        if (!g_originalLoadingMenuPostDisplay) {
            ALS::Log::warn("LoadingMenu::PostDisplay original function was null after vtable patch.");
            ALS::Log::diagnostic("loading_menu_postdisplay_hook_install result=false reason=no_original entry={}", g_loadingMenuPostDisplayEntry);
            return false;
        }

        g_loadingMenuPostDisplayHookInstalled = true;
        ALS::Log::info("LoadingMenu::PostDisplay UI render hook installed.");
        ALS::Log::diagnostic(
            "loading_menu_postdisplay_hook_install result=true entry={} original={} detour={}",
            reinterpret_cast<const void*>(g_loadingMenuPostDisplayEntry),
            reinterpret_cast<const void*>(g_originalLoadingMenuPostDisplay),
            reinterpret_cast<const void*>(&LoadingMenuPostDisplayDetour));
        return true;
    }

    void UninstallLoadingMenuPostDisplayHook()
    {
        if (!g_loadingMenuPostDisplayHookInstalled) {
            return;
        }

        if (g_loadingMenuPostDisplayEntry && g_originalLoadingMenuPostDisplay) {
            REL::safe_write(
                g_loadingMenuPostDisplayEntry,
                reinterpret_cast<std::uintptr_t>(g_originalLoadingMenuPostDisplay));
        }
        ALS::Log::info("LoadingMenu::PostDisplay UI render hook uninstalled.");
        ALS::Log::diagnostic("loading_menu_postdisplay_hook_uninstalled entry={}", reinterpret_cast<const void*>(g_loadingMenuPostDisplayEntry));
        g_loadingMenuPostDisplayHookInstalled = false;
        g_loadingMenuPostDisplayEntry = 0;
        g_originalLoadingMenuPostDisplay = nullptr;
    }

}

namespace ALS::D3D11Hooks
{
    bool Install(AnimatedLoadingScreenController& controller)
    {
        if (g_installed) {
            g_controller.store(&controller, std::memory_order_release);
            (void)InstallLoadingMenuPostDisplayHook();
            return true;
        }

        g_controller.store(&controller, std::memory_order_release);
        const auto postDisplayHookInstalled = InstallLoadingMenuPostDisplayHook();

        auto* swapChain = GetSkyrimSwapChain();
        if (!swapChain) {
            return postDisplayHookInstalled;
        }

        void** vtable = *reinterpret_cast<void***>(swapChain);
        if (!vtable) {
            Log::error("Skyrim swapchain vtable is unavailable. D3D11 hooks were not installed.");
            return false;
        }

        constexpr std::size_t presentIndex = 8;
        constexpr std::size_t resizeBuffersIndex = 13;
        g_presentAddress = vtable[presentIndex];
        g_resizeBuffersAddress = vtable[resizeBuffersIndex];

        ComPtr<IDXGISwapChain1> swapChain1;
        const auto swapChain1Hr = swapChain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(swapChain1.GetAddressOf()));
        if (SUCCEEDED(swapChain1Hr)) {
            constexpr std::size_t present1Index = 22;
            if (auto* swapChain1VTable = GetInterfaceVTable(swapChain1.Get())) {
                g_present1Address = swapChain1VTable[present1Index];
            }
        }

        ComPtr<IDXGISwapChain3> swapChain3;
        const auto swapChain3Hr = swapChain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(swapChain3.GetAddressOf()));
        if (SUCCEEDED(swapChain3Hr)) {
            constexpr std::size_t resizeBuffers1Index = 39;
            if (auto* swapChain3VTable = GetInterfaceVTable(swapChain3.Get())) {
                g_resizeBuffers1Address = swapChain3VTable[resizeBuffers1Index];
            }
        }

        Log::info(
            "Resolved Skyrim swapchain hooks: swapchain={}, QI(IDXGISwapChain1)=0x{:08X}, QI(IDXGISwapChain3)=0x{:08X}, Present={}, Present1={}, ResizeBuffers={}, ResizeBuffers1={}.",
            static_cast<const void*>(swapChain),
            static_cast<unsigned>(swapChain1Hr),
            static_cast<unsigned>(swapChain3Hr),
            g_presentAddress,
            g_present1Address,
            g_resizeBuffersAddress,
            g_resizeBuffers1Address);
        Log::diagnostic(
            "swapchain_hooks_resolved swapchain={} qi_swapchain1=0x{:08X} qi_swapchain3=0x{:08X} present={} present1={} resize_buffers={} resize_buffers1={}",
            static_cast<const void*>(swapChain),
            static_cast<unsigned>(swapChain1Hr),
            static_cast<unsigned>(swapChain3Hr),
            g_presentAddress,
            g_present1Address,
            g_resizeBuffersAddress,
            g_resizeBuffers1Address);

        const auto initStatus = MH_Initialize();
        if (!MhSucceededOrAlready(initStatus)) {
            Log::error("MinHook initialization failed: {}", static_cast<int>(initStatus));
            return postDisplayHookInstalled;
        }

        if (!CreateRequiredHook(g_presentAddress, reinterpret_cast<void*>(&PresentDetour), g_originalPresent, "IDXGISwapChain::Present")) {
            return postDisplayHookInstalled;
        }

        CreateOptionalHook(g_present1Address, reinterpret_cast<void*>(&Present1Detour), g_originalPresent1, "IDXGISwapChain1::Present1");
        CreateOptionalHook(g_resizeBuffersAddress, reinterpret_cast<void*>(&ResizeBuffersDetour), g_originalResizeBuffers, "IDXGISwapChain::ResizeBuffers");
        CreateOptionalHook(g_resizeBuffers1Address, reinterpret_cast<void*>(&ResizeBuffers1Detour), g_originalResizeBuffers1, "IDXGISwapChain3::ResizeBuffers1");

        auto status = MH_EnableHook(g_presentAddress);
        if (status != MH_OK && status != MH_ERROR_ENABLED) {
            Log::error("Failed to enable IDXGISwapChain::Present hook: {}", static_cast<int>(status));
            return postDisplayHookInstalled;
        }

        EnableOptionalHook(g_present1Address, reinterpret_cast<const void*>(g_originalPresent1), "IDXGISwapChain1::Present1");
        EnableOptionalHook(g_resizeBuffersAddress, reinterpret_cast<const void*>(g_originalResizeBuffers), "IDXGISwapChain::ResizeBuffers");
        EnableOptionalHook(g_resizeBuffers1Address, reinterpret_cast<const void*>(g_originalResizeBuffers1), "IDXGISwapChain3::ResizeBuffers1");

        g_uninstalling.store(false, std::memory_order_release);
        g_postDisplayOverlaySubmitted.store(false, std::memory_order_release);
        g_targetSwapChain.store(swapChain, std::memory_order_release);
        g_installed = true;
        Log::info("D3D11 Present hook installed.");
        Log::diagnostic(
            "d3d_hooks_installed target_swapchain={} present={} present1={} resize_buffers={} resize_buffers1={}",
            static_cast<const void*>(swapChain),
            g_presentAddress,
            g_present1Address,
            g_resizeBuffersAddress,
            g_resizeBuffers1Address);
        return true;
    }

    void Uninstall()
    {
        if (!g_installed && !g_loadingMenuPostDisplayHookInstalled) {
            return;
        }
        g_uninstalling.store(true, std::memory_order_release);
        if (g_presentAddress) {
            (void)MH_DisableHook(g_presentAddress);
        }
        if (g_resizeBuffersAddress) {
            (void)MH_DisableHook(g_resizeBuffersAddress);
        }
        if (g_present1Address) {
            (void)MH_DisableHook(g_present1Address);
        }
        if (g_resizeBuffers1Address) {
            (void)MH_DisableHook(g_resizeBuffers1Address);
        }
        while (g_activeDetours.load(std::memory_order_acquire) != 0) {
            Sleep(1);
        }
        if (g_presentAddress) {
            (void)MH_RemoveHook(g_presentAddress);
        }
        if (g_resizeBuffersAddress) {
            (void)MH_RemoveHook(g_resizeBuffersAddress);
        }
        if (g_present1Address) {
            (void)MH_RemoveHook(g_present1Address);
        }
        if (g_resizeBuffers1Address) {
            (void)MH_RemoveHook(g_resizeBuffers1Address);
        }
        UninstallLoadingMenuPostDisplayHook();
        g_controller.store(nullptr, std::memory_order_release);
        g_targetSwapChain.store(nullptr, std::memory_order_release);
        g_overlayWasActive.store(false, std::memory_order_release);
        g_skipNextOverlayDraw.store(true, std::memory_order_release);
        g_postDisplayOverlaySubmitted.store(false, std::memory_order_release);
        g_presentHits.store(0, std::memory_order_release);
        g_present1Hits.store(0, std::memory_order_release);
        g_targetMismatchLogs.store(0, std::memory_order_release);
        g_noTargetLogs.store(0, std::memory_order_release);
        g_inactiveOverlayLogs.store(0, std::memory_order_release);
        g_noRenderDataLogs.store(0, std::memory_order_release);
        g_skipFirstDrawLogs.store(0, std::memory_order_release);
        g_renderFailureLogs.store(0, std::memory_order_release);
        g_postDisplayHits.store(0, std::memory_order_release);
        g_postDisplayNoRenderDataLogs.store(0, std::memory_order_release);
        g_postDisplayRenderFailureLogs.store(0, std::memory_order_release);
        g_presentSkippedAfterPostDisplayLogs.store(0, std::memory_order_release);
        g_loggedFirstPostDisplayRender.store(false, std::memory_order_release);
        g_loggedFirstSuccessfulRender.store(false, std::memory_order_release);
        g_originalPresent = nullptr;
        g_originalPresent1 = nullptr;
        g_originalResizeBuffers = nullptr;
        g_originalResizeBuffers1 = nullptr;
        g_presentAddress = nullptr;
        g_present1Address = nullptr;
        g_resizeBuffersAddress = nullptr;
        g_resizeBuffers1Address = nullptr;
        g_installed = false;
        {
            std::scoped_lock renderLock(g_rendererMutex);
            g_renderer.Shutdown();
        }
        g_uninstalling.store(false, std::memory_order_release);
        Log::info("D3D11 hooks uninstalled.");
    }

    bool IsInstalled()
    {
        return g_installed || g_loadingMenuPostDisplayHookInstalled;
    }
}
