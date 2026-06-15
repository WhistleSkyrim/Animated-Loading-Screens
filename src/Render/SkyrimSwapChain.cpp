#include "Render/SkyrimSwapChain.h"

#include "Logging/Log.h"

#include <RE/R/Renderer.h>

namespace ALS::SkyrimSwapChain
{
    void* Get()
    {
        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer) {
            Log::error("BSGraphics::Renderer is unavailable. D3D11 hooks were not installed.");
            return nullptr;
        }

        auto* swapChain = renderer->GetRuntimeData().renderWindows[0].swapChain;
        if (!swapChain) {
            Log::error("Skyrim swapchain is unavailable. D3D11 hooks were not installed.");
            return nullptr;
        }

        return swapChain;
    }
}
