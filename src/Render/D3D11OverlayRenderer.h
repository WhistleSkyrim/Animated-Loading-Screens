#pragma once

#include "Controller/AnimatedLoadingScreenController.h"
#include "Render/AspectRatio.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstddef>
#include <memory>
#include <wrl/client.h>

namespace ALS
{
    class D3D11OverlayRenderer
    {
    public:
        bool Render(IDXGISwapChain* swapChain, const OverlayRenderData& data);
        bool RenderCurrentTarget(ID3D11DeviceContext* context, const OverlayRenderData& data);
        void PrepareForResize();
        void Invalidate();
        void Shutdown();

    private:
        struct QuadCommand
        {
            RectF destination{};
            RectF uv{};
            ID3D11ShaderResourceView* srv{ nullptr };
            Color color{};
            float alpha{ 0.0F };
        };

        struct TextureSlot
        {
            int width{ 0 };
            int height{ 0 };
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture{};
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv{};
            VideoFramePtr uploadedFrame{};
        };

        Microsoft::WRL::ComPtr<ID3D11Device> device_{};
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_{};
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTargetView_{};
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_{};
        Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_{};
        Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout_{};
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer_{};
        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_{};
        Microsoft::WRL::ComPtr<ID3D11BlendState> blendState_{};
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencilState_{};
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState_{};
        TextureSlot currentTexture_{};
        TextureSlot nextTexture_{};
        TextureSlot whiteTexture_{};
        UINT backBufferWidth_{ 0 };
        UINT backBufferHeight_{ 0 };
        IDXGISwapChain* swapChain_{ nullptr };

        bool EnsureInitialized(IDXGISwapChain* swapChain);
        bool EnsureInitialized(ID3D11Device* device, ID3D11DeviceContext* context);
        bool EnsureBackBuffer(IDXGISwapChain* swapChain);
        [[nodiscard]] bool HasPipelineResources() const noexcept;
        bool CreatePipelineResources();
        bool CreateWhiteTexture();
        void ResetDeviceResources();
        bool UpdateTexture(TextureSlot& slot, const VideoFramePtr& frame);
        std::size_t DrawTexturedQuads(
            const QuadCommand* commands,
            std::size_t commandCount,
            float viewportWidth,
            float viewportHeight);
    };
}
