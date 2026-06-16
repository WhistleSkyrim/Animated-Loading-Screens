#include "Render/D3D11OverlayRenderer.h"

#include "Logging/Log.h"

#include <D3Dcompiler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace
{
    using Microsoft::WRL::ComPtr;

    using D3DCompileFn = HRESULT(WINAPI*)(
        LPCVOID,
        SIZE_T,
        LPCSTR,
        const D3D_SHADER_MACRO*,
        ID3DInclude*,
        LPCSTR,
        LPCSTR,
        UINT,
        UINT,
        ID3DBlob**,
        ID3DBlob**);

    struct Vertex
    {
        float position[3];
        float uv[2];
        float color[4];
    };

    constexpr std::size_t kMaxQuadCount = 4;
    constexpr std::size_t kVerticesPerQuad = 4;
    constexpr std::size_t kMaxVertexCount = kMaxQuadCount * kVerticesPerQuad;
    constexpr bool kEnableBackBufferReadbackDiagnostics = false;

    std::atomic_bool g_loggedPipelineInit{ false };
    std::atomic_bool g_loggedBackBuffer{ false };
    std::atomic_bool g_loggedFirstTextureUpload{ false };
    std::atomic_bool g_loggedFirstDraw{ false };
    std::atomic_uint g_noDrawCommandLogs{ 0 };
    std::atomic_uint g_textureMapFailureLogs{ 0 };
    std::atomic_uint g_vertexMapFailureLogs{ 0 };
    std::atomic_uint g_backBufferSampleLogs{ 0 };
    std::atomic_uint g_frameSampleLogs{ 0 };
    std::atomic_uint g_currentTargetLogs{ 0 };
    std::atomic_uint g_currentTargetFailureLogs{ 0 };

    [[nodiscard]] bool ShouldLog(std::atomic_uint& counter, unsigned limit)
    {
        return counter.fetch_add(1, std::memory_order_relaxed) < limit;
    }

    void LogBackBufferSample(
        IDXGISwapChain* swapChain,
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        UINT x,
        UINT y,
        std::string_view label)
    {
        if (!swapChain || !device || !context) {
            ALS::Log::diagnostic("backbuffer_sample label={} skipped=missing_device_context", label);
            return;
        }

        ComPtr<ID3D11Texture2D> backBuffer;
        if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backBuffer.GetAddressOf())))) {
            ALS::Log::diagnostic("backbuffer_sample label={} skipped=get_buffer_failed", label);
            return;
        }

        D3D11_TEXTURE2D_DESC desc{};
        backBuffer->GetDesc(&desc);
        if (desc.Width == 0 || desc.Height == 0) {
            ALS::Log::diagnostic("backbuffer_sample label={} skipped=empty_backbuffer", label);
            return;
        }
        if (desc.SampleDesc.Count != 1) {
            ALS::Log::diagnostic(
                "backbuffer_sample label={} skipped=multisampled sample_count={} format={}",
                label,
                desc.SampleDesc.Count,
                static_cast<unsigned>(desc.Format));
            return;
        }

        x = std::min(x, desc.Width - 1);
        y = std::min(y, desc.Height - 1);

        D3D11_TEXTURE2D_DESC stagingDesc{};
        stagingDesc.Width = 1;
        stagingDesc.Height = 1;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = desc.Format;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, staging.GetAddressOf()))) {
            ALS::Log::diagnostic(
                "backbuffer_sample label={} skipped=create_staging_failed format={}",
                label,
                static_cast<unsigned>(desc.Format));
            return;
        }

        const D3D11_BOX sourceBox{ x, y, 0, x + 1, y + 1, 1 };
        context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, backBuffer.Get(), 0, &sourceBox);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            ALS::Log::diagnostic("backbuffer_sample label={} skipped=map_failed", label);
            return;
        }

        const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData);
        ALS::Log::diagnostic(
            "backbuffer_sample label={} x={} y={} format={} row_pitch={} bytes={:02X},{:02X},{:02X},{:02X}",
            label,
            x,
            y,
            static_cast<unsigned>(desc.Format),
            mapped.RowPitch,
            static_cast<unsigned>(bytes[0]),
            static_cast<unsigned>(bytes[1]),
            static_cast<unsigned>(bytes[2]),
            static_cast<unsigned>(bytes[3]));
        context->Unmap(staging.Get(), 0);
    }

    constexpr char kVertexShader[] = R"(
struct VSIn
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

VSOut main(VSIn input)
{
    VSOut output;
    output.position = float4(input.position, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
)";

    constexpr char kPixelShader[] = R"(
Texture2D overlayTexture : register(t0);
SamplerState overlaySampler : register(s0);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0, float4 color : COLOR0) : SV_Target
{
    return overlayTexture.Sample(overlaySampler, uv) * color;
}
)";

    [[nodiscard]] D3DCompileFn ResolveD3DCompile()
    {
        struct CompilerModule
        {
            HMODULE module{ nullptr };
            D3DCompileFn compile{ nullptr };
        };

        static const auto compiler = [] {
            CompilerModule result{};
            for (const auto* dllName : { L"D3DCompiler_47.dll", L"D3DCompiler_43.dll" }) {
                result.module = LoadLibraryExW(dllName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!result.module) {
                    continue;
                }

                result.compile = reinterpret_cast<D3DCompileFn>(GetProcAddress(result.module, "D3DCompile"));
                if (result.compile) {
                    return result;
                }

                FreeLibrary(result.module);
                result.module = nullptr;
            }
            return result;
        }();

        return compiler.compile;
    }

    [[nodiscard]] bool CompileShader(
        const char* source,
        const char* entryPoint,
        const char* target,
        ComPtr<ID3DBlob>& blob)
    {
        ComPtr<ID3DBlob> errors;
        const auto flags = D3DCOMPILE_ENABLE_STRICTNESS;
        const auto d3dCompile = ResolveD3DCompile();
        if (!d3dCompile) {
            ALS::Log::error("Unable to load D3DCompiler_47.dll or D3DCompiler_43.dll. Overlay shaders cannot be compiled.");
            return false;
        }

        const auto hr = d3dCompile(
            source,
            std::strlen(source),
            nullptr,
            nullptr,
            nullptr,
            entryPoint,
            target,
            flags,
            0,
            blob.GetAddressOf(),
            errors.GetAddressOf());

        if (FAILED(hr)) {
            if (errors) {
                ALS::Log::error("D3D shader compile failed: {}", static_cast<const char*>(errors->GetBufferPointer()));
            } else {
                ALS::Log::error("D3D shader compile failed with HRESULT 0x{:08X}", static_cast<unsigned>(hr));
            }
            return false;
        }
        return true;
    }

    [[nodiscard]] float ClampAlpha(float value)
    {
        if (value < 0.0F) {
            return 0.0F;
        }
        if (value > 1.0F) {
            return 1.0F;
        }
        return value;
    }

    class D3D11StateGuard
    {
    public:
        explicit D3D11StateGuard(ID3D11DeviceContext* context) :
            context_(context)
        {
            if (!context_) {
                return;
            }

            viewportCount_ = static_cast<UINT>(viewports_.size());
            context_->RSGetViewports(&viewportCount_, viewports_.data());
            context_->RSGetState(&rasterizerState_);

            context_->OMGetBlendState(&blendState_, blendFactor_, &sampleMask_);
            context_->OMGetDepthStencilState(&depthStencilState_, &stencilRef_);

            context_->IAGetInputLayout(&inputLayout_);
            context_->IAGetVertexBuffers(0, 1, &vertexBuffer_, &vertexStride_, &vertexOffset_);
            context_->IAGetIndexBuffer(&indexBuffer_, &indexFormat_, &indexOffset_);
            context_->IAGetPrimitiveTopology(&topology_);
            context_->GetPredication(&predicate_, &predicateValue_);

            scissorRectCount_ = static_cast<UINT>(scissorRects_.size());
            context_->RSGetScissorRects(&scissorRectCount_, scissorRects_.data());

            vsClassInstanceCount_ = static_cast<UINT>(vertexShaderInstances_.size());
            context_->VSGetShader(&vertexShader_, vertexShaderInstances_.data(), &vsClassInstanceCount_);

            hsClassInstanceCount_ = static_cast<UINT>(hullShaderInstances_.size());
            context_->HSGetShader(&hullShader_, hullShaderInstances_.data(), &hsClassInstanceCount_);

            dsClassInstanceCount_ = static_cast<UINT>(domainShaderInstances_.size());
            context_->DSGetShader(&domainShader_, domainShaderInstances_.data(), &dsClassInstanceCount_);

            gsClassInstanceCount_ = static_cast<UINT>(geometryShaderInstances_.size());
            context_->GSGetShader(&geometryShader_, geometryShaderInstances_.data(), &gsClassInstanceCount_);

            psClassInstanceCount_ = static_cast<UINT>(pixelShaderInstances_.size());
            context_->PSGetShader(&pixelShader_, pixelShaderInstances_.data(), &psClassInstanceCount_);

            csClassInstanceCount_ = static_cast<UINT>(computeShaderInstances_.size());
            context_->CSGetShader(&computeShader_, computeShaderInstances_.data(), &csClassInstanceCount_);

            context_->VSGetShaderResources(0, static_cast<UINT>(vertexShaderResources_.size()), vertexShaderResources_.data());
            context_->HSGetShaderResources(0, static_cast<UINT>(hullShaderResources_.size()), hullShaderResources_.data());
            context_->DSGetShaderResources(0, static_cast<UINT>(domainShaderResources_.size()), domainShaderResources_.data());
            context_->GSGetShaderResources(0, static_cast<UINT>(geometryShaderResources_.size()), geometryShaderResources_.data());
            context_->PSGetShaderResources(0, static_cast<UINT>(pixelShaderResources_.size()), pixelShaderResources_.data());
            context_->CSGetShaderResources(0, static_cast<UINT>(computeShaderResources_.size()), computeShaderResources_.data());
            context_->VSGetSamplers(0, static_cast<UINT>(vertexSamplers_.size()), vertexSamplers_.data());
            context_->HSGetSamplers(0, static_cast<UINT>(hullSamplers_.size()), hullSamplers_.data());
            context_->DSGetSamplers(0, static_cast<UINT>(domainSamplers_.size()), domainSamplers_.data());
            context_->GSGetSamplers(0, static_cast<UINT>(geometrySamplers_.size()), geometrySamplers_.data());
            context_->PSGetSamplers(0, static_cast<UINT>(pixelSamplers_.size()), pixelSamplers_.data());
            context_->CSGetSamplers(0, static_cast<UINT>(computeSamplers_.size()), computeSamplers_.data());
            context_->VSGetConstantBuffers(0, static_cast<UINT>(vertexConstantBuffers_.size()), vertexConstantBuffers_.data());
            context_->HSGetConstantBuffers(0, static_cast<UINT>(hullConstantBuffers_.size()), hullConstantBuffers_.data());
            context_->DSGetConstantBuffers(0, static_cast<UINT>(domainConstantBuffers_.size()), domainConstantBuffers_.data());
            context_->GSGetConstantBuffers(0, static_cast<UINT>(geometryConstantBuffers_.size()), geometryConstantBuffers_.data());
            context_->PSGetConstantBuffers(0, static_cast<UINT>(pixelConstantBuffers_.size()), pixelConstantBuffers_.data());
            context_->CSGetConstantBuffers(0, static_cast<UINT>(computeConstantBuffers_.size()), computeConstantBuffers_.data());
            context_->CSGetUnorderedAccessViews(0, static_cast<UINT>(computeUnorderedAccessViews_.size()), computeUnorderedAccessViews_.data());

            context_->OMGetRenderTargets(
                static_cast<UINT>(renderTargetViews_.size()),
                renderTargetViews_.data(),
                &depthStencilView_);
        }

        D3D11StateGuard(const D3D11StateGuard&) = delete;
        D3D11StateGuard& operator=(const D3D11StateGuard&) = delete;

        ~D3D11StateGuard()
        {
            if (!context_) {
                return;
            }

            context_->OMSetRenderTargets(
                static_cast<UINT>(renderTargetViews_.size()),
                renderTargetViews_.data(),
                depthStencilView_);

            context_->RSSetViewports(viewportCount_, viewportCount_ > 0 ? viewports_.data() : nullptr);
            context_->RSSetScissorRects(scissorRectCount_, scissorRectCount_ > 0 ? scissorRects_.data() : nullptr);
            context_->RSSetState(rasterizerState_);
            context_->OMSetBlendState(blendState_, blendFactor_, sampleMask_);
            context_->OMSetDepthStencilState(depthStencilState_, stencilRef_);

            context_->IASetInputLayout(inputLayout_);
            ID3D11Buffer* vertexBuffer = vertexBuffer_;
            context_->IASetVertexBuffers(0, 1, &vertexBuffer, &vertexStride_, &vertexOffset_);
            context_->IASetIndexBuffer(indexBuffer_, indexFormat_, indexOffset_);
            context_->IASetPrimitiveTopology(topology_);
            context_->SetPredication(predicate_, predicateValue_);

            context_->VSSetShader(
                vertexShader_,
                vsClassInstanceCount_ > 0 ? vertexShaderInstances_.data() : nullptr,
                vsClassInstanceCount_);
            context_->HSSetShader(
                hullShader_,
                hsClassInstanceCount_ > 0 ? hullShaderInstances_.data() : nullptr,
                hsClassInstanceCount_);
            context_->DSSetShader(
                domainShader_,
                dsClassInstanceCount_ > 0 ? domainShaderInstances_.data() : nullptr,
                dsClassInstanceCount_);
            context_->GSSetShader(
                geometryShader_,
                gsClassInstanceCount_ > 0 ? geometryShaderInstances_.data() : nullptr,
                gsClassInstanceCount_);
            context_->PSSetShader(
                pixelShader_,
                psClassInstanceCount_ > 0 ? pixelShaderInstances_.data() : nullptr,
                psClassInstanceCount_);
            context_->CSSetShader(
                computeShader_,
                csClassInstanceCount_ > 0 ? computeShaderInstances_.data() : nullptr,
                csClassInstanceCount_);

            context_->VSSetShaderResources(0, static_cast<UINT>(vertexShaderResources_.size()), vertexShaderResources_.data());
            context_->HSSetShaderResources(0, static_cast<UINT>(hullShaderResources_.size()), hullShaderResources_.data());
            context_->DSSetShaderResources(0, static_cast<UINT>(domainShaderResources_.size()), domainShaderResources_.data());
            context_->GSSetShaderResources(0, static_cast<UINT>(geometryShaderResources_.size()), geometryShaderResources_.data());
            context_->PSSetShaderResources(0, static_cast<UINT>(pixelShaderResources_.size()), pixelShaderResources_.data());
            context_->CSSetShaderResources(0, static_cast<UINT>(computeShaderResources_.size()), computeShaderResources_.data());
            context_->VSSetSamplers(0, static_cast<UINT>(vertexSamplers_.size()), vertexSamplers_.data());
            context_->HSSetSamplers(0, static_cast<UINT>(hullSamplers_.size()), hullSamplers_.data());
            context_->DSSetSamplers(0, static_cast<UINT>(domainSamplers_.size()), domainSamplers_.data());
            context_->GSSetSamplers(0, static_cast<UINT>(geometrySamplers_.size()), geometrySamplers_.data());
            context_->PSSetSamplers(0, static_cast<UINT>(pixelSamplers_.size()), pixelSamplers_.data());
            context_->CSSetSamplers(0, static_cast<UINT>(computeSamplers_.size()), computeSamplers_.data());
            context_->VSSetConstantBuffers(0, static_cast<UINT>(vertexConstantBuffers_.size()), vertexConstantBuffers_.data());
            context_->HSSetConstantBuffers(0, static_cast<UINT>(hullConstantBuffers_.size()), hullConstantBuffers_.data());
            context_->DSSetConstantBuffers(0, static_cast<UINT>(domainConstantBuffers_.size()), domainConstantBuffers_.data());
            context_->GSSetConstantBuffers(0, static_cast<UINT>(geometryConstantBuffers_.size()), geometryConstantBuffers_.data());
            context_->PSSetConstantBuffers(0, static_cast<UINT>(pixelConstantBuffers_.size()), pixelConstantBuffers_.data());
            context_->CSSetConstantBuffers(0, static_cast<UINT>(computeConstantBuffers_.size()), computeConstantBuffers_.data());
            uavInitialCounts_.fill(D3D11_KEEP_UNORDERED_ACCESS_VIEWS);
            context_->CSSetUnorderedAccessViews(
                0,
                static_cast<UINT>(computeUnorderedAccessViews_.size()),
                computeUnorderedAccessViews_.data(),
                uavInitialCounts_.data());

            ReleaseCapturedReferences();
        }

    private:
        static constexpr UINT kMaxClassInstances = 256;
        static constexpr UINT kShaderResourceSlotCount = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
        static constexpr UINT kSamplerSlotCount = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
        static constexpr UINT kConstantBufferSlotCount = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
        static constexpr UINT kOmUavSlotCount = D3D11_PS_CS_UAV_REGISTER_COUNT;

        ID3D11DeviceContext* context_{ nullptr };
        std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> renderTargetViews_{};
        ID3D11DepthStencilView* depthStencilView_{ nullptr };
        std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports_{};
        UINT viewportCount_{ 0 };
        ID3D11RasterizerState* rasterizerState_{ nullptr };
        ID3D11BlendState* blendState_{ nullptr };
        ID3D11DepthStencilState* depthStencilState_{ nullptr };
        FLOAT blendFactor_[4]{};
        UINT sampleMask_{ 0xFFFFFFFF };
        UINT stencilRef_{ 0 };
        std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissorRects_{};
        UINT scissorRectCount_{ 0 };
        ID3D11InputLayout* inputLayout_{ nullptr };
        ID3D11Buffer* vertexBuffer_{ nullptr };
        ID3D11Buffer* indexBuffer_{ nullptr };
        UINT vertexStride_{ 0 };
        UINT vertexOffset_{ 0 };
        DXGI_FORMAT indexFormat_{ DXGI_FORMAT_UNKNOWN };
        UINT indexOffset_{ 0 };
        D3D11_PRIMITIVE_TOPOLOGY topology_{ D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED };
        ID3D11Predicate* predicate_{ nullptr };
        BOOL predicateValue_{ FALSE };
        ID3D11VertexShader* vertexShader_{ nullptr };
        std::array<ID3D11ClassInstance*, kMaxClassInstances> vertexShaderInstances_{};
        UINT vsClassInstanceCount_{ 0 };
        ID3D11HullShader* hullShader_{ nullptr };
        std::array<ID3D11ClassInstance*, kMaxClassInstances> hullShaderInstances_{};
        UINT hsClassInstanceCount_{ 0 };
        ID3D11DomainShader* domainShader_{ nullptr };
        std::array<ID3D11ClassInstance*, kMaxClassInstances> domainShaderInstances_{};
        UINT dsClassInstanceCount_{ 0 };
        ID3D11GeometryShader* geometryShader_{ nullptr };
        std::array<ID3D11ClassInstance*, kMaxClassInstances> geometryShaderInstances_{};
        UINT gsClassInstanceCount_{ 0 };
        ID3D11PixelShader* pixelShader_{ nullptr };
        std::array<ID3D11ClassInstance*, kMaxClassInstances> pixelShaderInstances_{};
        UINT psClassInstanceCount_{ 0 };
        ID3D11ComputeShader* computeShader_{ nullptr };
        std::array<ID3D11ClassInstance*, kMaxClassInstances> computeShaderInstances_{};
        UINT csClassInstanceCount_{ 0 };
        std::array<ID3D11ShaderResourceView*, kShaderResourceSlotCount> vertexShaderResources_{};
        std::array<ID3D11ShaderResourceView*, kShaderResourceSlotCount> hullShaderResources_{};
        std::array<ID3D11ShaderResourceView*, kShaderResourceSlotCount> domainShaderResources_{};
        std::array<ID3D11ShaderResourceView*, kShaderResourceSlotCount> geometryShaderResources_{};
        std::array<ID3D11ShaderResourceView*, kShaderResourceSlotCount> pixelShaderResources_{};
        std::array<ID3D11ShaderResourceView*, kShaderResourceSlotCount> computeShaderResources_{};
        std::array<ID3D11SamplerState*, kSamplerSlotCount> vertexSamplers_{};
        std::array<ID3D11SamplerState*, kSamplerSlotCount> hullSamplers_{};
        std::array<ID3D11SamplerState*, kSamplerSlotCount> domainSamplers_{};
        std::array<ID3D11SamplerState*, kSamplerSlotCount> geometrySamplers_{};
        std::array<ID3D11SamplerState*, kSamplerSlotCount> pixelSamplers_{};
        std::array<ID3D11SamplerState*, kSamplerSlotCount> computeSamplers_{};
        std::array<ID3D11Buffer*, kConstantBufferSlotCount> vertexConstantBuffers_{};
        std::array<ID3D11Buffer*, kConstantBufferSlotCount> hullConstantBuffers_{};
        std::array<ID3D11Buffer*, kConstantBufferSlotCount> domainConstantBuffers_{};
        std::array<ID3D11Buffer*, kConstantBufferSlotCount> geometryConstantBuffers_{};
        std::array<ID3D11Buffer*, kConstantBufferSlotCount> pixelConstantBuffers_{};
        std::array<ID3D11Buffer*, kConstantBufferSlotCount> computeConstantBuffers_{};
        std::array<ID3D11UnorderedAccessView*, kOmUavSlotCount> computeUnorderedAccessViews_{};
        std::array<UINT, kOmUavSlotCount> uavInitialCounts_{};

        template <class T>
        static void ReleaseIfHeld(T*& value) noexcept
        {
            if (value) {
                value->Release();
                value = nullptr;
            }
        }

        template <class T, std::size_t Size>
        static void ReleaseArray(std::array<T*, Size>& values) noexcept
        {
            for (auto*& value : values) {
                ReleaseIfHeld(value);
            }
        }

        void ReleaseCapturedReferences() noexcept
        {
            for (auto*& renderTargetView : renderTargetViews_) {
                ReleaseIfHeld(renderTargetView);
            }
            ReleaseIfHeld(depthStencilView_);
            ReleaseIfHeld(rasterizerState_);
            ReleaseIfHeld(blendState_);
            ReleaseIfHeld(depthStencilState_);
            ReleaseIfHeld(inputLayout_);
            ReleaseIfHeld(vertexBuffer_);
            ReleaseIfHeld(indexBuffer_);
            ReleaseIfHeld(predicate_);
            ReleaseIfHeld(vertexShader_);
            for (UINT i = 0; i < vsClassInstanceCount_; ++i) {
                ReleaseIfHeld(vertexShaderInstances_[i]);
            }
            ReleaseIfHeld(hullShader_);
            for (UINT i = 0; i < hsClassInstanceCount_; ++i) {
                ReleaseIfHeld(hullShaderInstances_[i]);
            }
            ReleaseIfHeld(domainShader_);
            for (UINT i = 0; i < dsClassInstanceCount_; ++i) {
                ReleaseIfHeld(domainShaderInstances_[i]);
            }
            ReleaseIfHeld(geometryShader_);
            for (UINT i = 0; i < gsClassInstanceCount_; ++i) {
                ReleaseIfHeld(geometryShaderInstances_[i]);
            }
            ReleaseIfHeld(pixelShader_);
            for (UINT i = 0; i < psClassInstanceCount_; ++i) {
                ReleaseIfHeld(pixelShaderInstances_[i]);
            }
            ReleaseIfHeld(computeShader_);
            for (UINT i = 0; i < csClassInstanceCount_; ++i) {
                ReleaseIfHeld(computeShaderInstances_[i]);
            }
            ReleaseArray(vertexShaderResources_);
            ReleaseArray(hullShaderResources_);
            ReleaseArray(domainShaderResources_);
            ReleaseArray(geometryShaderResources_);
            ReleaseArray(pixelShaderResources_);
            ReleaseArray(computeShaderResources_);
            ReleaseArray(vertexSamplers_);
            ReleaseArray(hullSamplers_);
            ReleaseArray(domainSamplers_);
            ReleaseArray(geometrySamplers_);
            ReleaseArray(pixelSamplers_);
            ReleaseArray(computeSamplers_);
            ReleaseArray(vertexConstantBuffers_);
            ReleaseArray(hullConstantBuffers_);
            ReleaseArray(domainConstantBuffers_);
            ReleaseArray(geometryConstantBuffers_);
            ReleaseArray(pixelConstantBuffers_);
            ReleaseArray(computeConstantBuffers_);
            ReleaseArray(computeUnorderedAccessViews_);
        }
    };
}

namespace ALS
{
    bool D3D11OverlayRenderer::Render(IDXGISwapChain* swapChain, const OverlayRenderData& data)
    {
        if (!swapChain || !data.shouldRender) {
            return false;
        }
        if (!EnsureInitialized(swapChain) || !EnsureBackBuffer(swapChain)) {
            return false;
        }

        if (backBufferWidth_ == 0 || backBufferHeight_ == 0) {
            return false;
        }

        const auto viewportWidth = static_cast<float>(backBufferWidth_);
        const auto viewportHeight = static_cast<float>(backBufferHeight_);

        const D3D11StateGuard stateGuard(context_.Get());

        D3D11_VIEWPORT viewport{};
        viewport.Width = viewportWidth;
        viewport.Height = viewportHeight;
        viewport.MaxDepth = 1.0F;
        context_->RSSetViewports(1, &viewport);
        context_->RSSetState(rasterizerState_.Get());
        ID3D11RenderTargetView* renderTargetView = renderTargetView_.Get();
        context_->OMSetRenderTargets(1, &renderTargetView, nullptr);
        context_->OMSetBlendState(blendState_.Get(), nullptr, 0xFFFFFFFF);
        context_->OMSetDepthStencilState(depthStencilState_.Get(), 0);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->HSSetShader(nullptr, nullptr, 0);
        context_->DSSetShader(nullptr, nullptr, 0);
        context_->GSSetShader(nullptr, nullptr, 0);
        context_->IASetInputLayout(inputLayout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());

        std::array<QuadCommand, kMaxQuadCount> commands{};
        std::size_t commandCount = 0;
        const auto addCommand = [&](const RectF& destination, const RectF& uv, ID3D11ShaderResourceView* srv, Color color, float alpha) {
            if (!srv || alpha <= 0.0F || commandCount >= commands.size()) {
                return;
            }
            commands[commandCount++] = QuadCommand{ destination, uv, srv, color, alpha };
        };

        if (data.drawBackground && whiteTexture_.srv) {
            addCommand(
                RectF{ 0.0F, 0.0F, viewportWidth, viewportHeight },
                RectF{ 0.0F, 0.0F, 1.0F, 1.0F },
                whiteTexture_.srv.Get(),
                data.backgroundColor,
                data.backgroundAlpha);
        }

        const auto currentAlpha = ClampAlpha(data.current.alpha * data.opacity);
        if (data.current.frame && currentAlpha > 0.0F && UpdateTexture(currentTexture_, data.current.frame)) {
            const auto mapping = CalculateAspectMapping(
                data.fitMode,
                viewportWidth,
                viewportHeight,
                static_cast<float>(data.current.frame->width),
                static_cast<float>(data.current.frame->height));
            addCommand(
                mapping.destination,
                mapping.uv,
                currentTexture_.srv.Get(),
                Color{ 1.0F, 1.0F, 1.0F, 1.0F },
                currentAlpha);
        }

        const auto nextAlpha = ClampAlpha(data.next.alpha * data.opacity);
        if (data.next.frame && nextAlpha > 0.0F && UpdateTexture(nextTexture_, data.next.frame)) {
            const auto mapping = CalculateAspectMapping(
                data.fitMode,
                viewportWidth,
                viewportHeight,
                static_cast<float>(data.next.frame->width),
                static_cast<float>(data.next.frame->height));
            addCommand(
                mapping.destination,
                mapping.uv,
                nextTexture_.srv.Get(),
                Color{ 1.0F, 1.0F, 1.0F, 1.0F },
                nextAlpha);
        }

        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        const auto overlayQuads = DrawTexturedQuads(commands.data(), commandCount, viewportWidth, viewportHeight);
        const auto drawnQuads = overlayQuads;
        if (drawnQuads == 0) {
            if (ShouldLog(g_noDrawCommandLogs, 10)) {
                Log::warn(
                    "Overlay render produced no draw commands. requestedCommands={}, currentFrame={}, nextFrame={}, drawBackground={}.",
                    commandCount,
                    static_cast<bool>(data.current.frame),
                    static_cast<bool>(data.next.frame),
                    data.drawBackground);
            }
            return false;
        }
        if (kEnableBackBufferReadbackDiagnostics && ShouldLog(g_backBufferSampleLogs, 8)) {
            ALS::Log::diagnostic(
                "overlay_render_drawn quads={} overlay_quads={} requested_commands={} viewport={}x{} draw_background={} current_frame={} current_alpha={} next_frame={} next_alpha={} opacity={}",
                drawnQuads,
                overlayQuads,
                commandCount,
                backBufferWidth_,
                backBufferHeight_,
                data.drawBackground,
                static_cast<bool>(data.current.frame),
                currentAlpha,
                static_cast<bool>(data.next.frame),
                nextAlpha,
                data.opacity);
            LogBackBufferSample(
                swapChain,
                device_.Get(),
                context_.Get(),
                backBufferWidth_ / 2,
                backBufferHeight_ / 2,
                "center_after_overlay_draw");
            LogBackBufferSample(
                swapChain,
                device_.Get(),
                context_.Get(),
                8,
                8,
                "corner_after_overlay_draw");
        }
        if (!g_loggedFirstDraw.exchange(true, std::memory_order_acq_rel)) {
            Log::info("Overlay submitted {} quad(s) to D3D11.", drawnQuads);
        }
        return true;
    }

    bool D3D11OverlayRenderer::RenderCurrentTarget(ID3D11DeviceContext* context, const OverlayRenderData& data)
    {
        if (!context || !data.shouldRender) {
            return false;
        }

        ComPtr<ID3D11Device> device;
        context->GetDevice(device.GetAddressOf());
        if (!device || !EnsureInitialized(device.Get(), context)) {
            return false;
        }

        ComPtr<ID3D11RenderTargetView> activeRenderTargetView;
        context_->OMGetRenderTargets(1, activeRenderTargetView.GetAddressOf(), nullptr);
        if (!activeRenderTargetView) {
            if (ShouldLog(g_currentTargetFailureLogs, 12)) {
                Log::diagnostic("overlay_render_current_target result=skipped reason=no_active_rtv");
            }
            return false;
        }

        ComPtr<ID3D11Resource> activeResource;
        activeRenderTargetView->GetResource(activeResource.GetAddressOf());
        ComPtr<ID3D11Texture2D> activeTexture;
        if (!activeResource || FAILED(activeResource.As(&activeTexture))) {
            if (ShouldLog(g_currentTargetFailureLogs, 12)) {
                Log::diagnostic("overlay_render_current_target result=skipped reason=active_target_not_texture2d");
            }
            return false;
        }

        D3D11_TEXTURE2D_DESC targetDesc{};
        activeTexture->GetDesc(&targetDesc);
        if (targetDesc.Width == 0 || targetDesc.Height == 0) {
            return false;
        }

        std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
        UINT viewportCount = static_cast<UINT>(viewports.size());
        context_->RSGetViewports(&viewportCount, viewports.data());
        D3D11_VIEWPORT viewport{};
        if (viewportCount > 0 && viewports[0].Width > 0.0F && viewports[0].Height > 0.0F) {
            viewport = viewports[0];
        } else {
            viewport.Width = static_cast<float>(targetDesc.Width);
            viewport.Height = static_cast<float>(targetDesc.Height);
            viewport.MaxDepth = 1.0F;
        }
        viewport.MinDepth = 0.0F;
        viewport.MaxDepth = 1.0F;

        const auto viewportWidth = viewport.Width;
        const auto viewportHeight = viewport.Height;
        if (viewportWidth <= 0.0F || viewportHeight <= 0.0F) {
            return false;
        }

        const D3D11StateGuard stateGuard(context_.Get());

        context_->RSSetViewports(1, &viewport);
        context_->RSSetState(rasterizerState_.Get());
        ID3D11RenderTargetView* renderTargetView = activeRenderTargetView.Get();
        context_->OMSetRenderTargets(1, &renderTargetView, nullptr);
        context_->OMSetBlendState(blendState_.Get(), nullptr, 0xFFFFFFFF);
        context_->OMSetDepthStencilState(depthStencilState_.Get(), 0);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->HSSetShader(nullptr, nullptr, 0);
        context_->DSSetShader(nullptr, nullptr, 0);
        context_->GSSetShader(nullptr, nullptr, 0);
        context_->IASetInputLayout(inputLayout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());

        std::array<QuadCommand, kMaxQuadCount> commands{};
        std::size_t commandCount = 0;
        const auto addCommand = [&](const RectF& destination, const RectF& uv, ID3D11ShaderResourceView* srv, Color color, float alpha) {
            if (!srv || alpha <= 0.0F || commandCount >= commands.size()) {
                return;
            }
            commands[commandCount++] = QuadCommand{ destination, uv, srv, color, alpha };
        };

        if (data.drawBackground && whiteTexture_.srv) {
            addCommand(
                RectF{ 0.0F, 0.0F, viewportWidth, viewportHeight },
                RectF{ 0.0F, 0.0F, 1.0F, 1.0F },
                whiteTexture_.srv.Get(),
                data.backgroundColor,
                data.backgroundAlpha);
        }

        const auto currentAlpha = ClampAlpha(data.current.alpha * data.opacity);
        if (data.current.frame && currentAlpha > 0.0F && UpdateTexture(currentTexture_, data.current.frame)) {
            const auto mapping = CalculateAspectMapping(
                data.fitMode,
                viewportWidth,
                viewportHeight,
                static_cast<float>(data.current.frame->width),
                static_cast<float>(data.current.frame->height));
            addCommand(
                mapping.destination,
                mapping.uv,
                currentTexture_.srv.Get(),
                Color{ 1.0F, 1.0F, 1.0F, 1.0F },
                currentAlpha);
        }

        const auto nextAlpha = ClampAlpha(data.next.alpha * data.opacity);
        if (data.next.frame && nextAlpha > 0.0F && UpdateTexture(nextTexture_, data.next.frame)) {
            const auto mapping = CalculateAspectMapping(
                data.fitMode,
                viewportWidth,
                viewportHeight,
                static_cast<float>(data.next.frame->width),
                static_cast<float>(data.next.frame->height));
            addCommand(
                mapping.destination,
                mapping.uv,
                nextTexture_.srv.Get(),
                Color{ 1.0F, 1.0F, 1.0F, 1.0F },
                nextAlpha);
        }

        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        const auto overlayQuads = DrawTexturedQuads(commands.data(), commandCount, viewportWidth, viewportHeight);
        const auto drawnQuads = overlayQuads;
        if (drawnQuads == 0) {
            if (ShouldLog(g_noDrawCommandLogs, 10)) {
                Log::warn(
                    "Current target overlay render produced no draw commands. requestedCommands={}, currentFrame={}, nextFrame={}, drawBackground={}.",
                    commandCount,
                    static_cast<bool>(data.current.frame),
                    static_cast<bool>(data.next.frame),
                    data.drawBackground);
            }
            return false;
        }

        if (ShouldLog(g_currentTargetLogs, 16)) {
            Log::diagnostic(
                "overlay_render_current_target result=drawn quads={} overlay_quads={} requested_commands={} viewport={}x{} target={}x{} format={} draw_background={} current_frame={} current_alpha={} next_frame={} next_alpha={} opacity={}",
                drawnQuads,
                overlayQuads,
                commandCount,
                viewportWidth,
                viewportHeight,
                targetDesc.Width,
                targetDesc.Height,
                static_cast<unsigned>(targetDesc.Format),
                data.drawBackground,
                static_cast<bool>(data.current.frame),
                currentAlpha,
                static_cast<bool>(data.next.frame),
                nextAlpha,
                data.opacity);
        }
        if (!g_loggedFirstDraw.exchange(true, std::memory_order_acq_rel)) {
            Log::info("Overlay submitted {} quad(s) to the active UI render target.", drawnQuads);
        }
        return true;
    }

    void D3D11OverlayRenderer::Invalidate()
    {
        renderTargetView_.Reset();
        backBufferWidth_ = 0;
        backBufferHeight_ = 0;
    }

    void D3D11OverlayRenderer::PrepareForResize()
    {
        if (context_ && renderTargetView_) {
            ID3D11RenderTargetView* renderTargetView = nullptr;
            context_->OMSetRenderTargets(1, &renderTargetView, nullptr);
        }
        Invalidate();
    }

    void D3D11OverlayRenderer::Shutdown()
    {
        ResetDeviceResources();
    }

    bool D3D11OverlayRenderer::EnsureInitialized(IDXGISwapChain* swapChain)
    {
        if (swapChain_ == swapChain && HasPipelineResources()) {
            return true;
        }

        ComPtr<ID3D11Device> swapChainDevice;
        if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(swapChainDevice.GetAddressOf())))) {
            Log::error("Unable to get D3D11 device from swapchain.");
            return false;
        }

        if ((device_ && device_.Get() != swapChainDevice.Get()) || (swapChain_ && swapChain_ != swapChain)) {
            Log::warn("Swapchain changed. Recreating overlay resources.");
            ResetDeviceResources();
        } else if (device_ || context_) {
            Log::warn("Overlay D3D resources were incomplete. Recreating them before rendering.");
            ResetDeviceResources();
        }

        if (HasPipelineResources()) {
            return true;
        }

        device_ = swapChainDevice;
        swapChain_ = swapChain;

        device_->GetImmediateContext(context_.GetAddressOf());
        if (!context_) {
            Log::error("Unable to get D3D11 immediate context.");
            return false;
        }

        if (!CreatePipelineResources() || !CreateWhiteTexture()) {
            ResetDeviceResources();
            return false;
        }

        if (!g_loggedPipelineInit.exchange(true, std::memory_order_acq_rel)) {
            Log::info(
                "Overlay D3D11 pipeline initialized. device={}, context={}.",
                static_cast<const void*>(device_.Get()),
                static_cast<const void*>(context_.Get()));
        }
        return HasPipelineResources();
    }

    bool D3D11OverlayRenderer::EnsureInitialized(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        if (!device || !context) {
            return false;
        }

        if (device_ && context_ && device_.Get() == device && context_.Get() == context && !swapChain_ && HasPipelineResources()) {
            return true;
        }

        if ((device_ && device_.Get() != device) || (context_ && context_.Get() != context) || swapChain_) {
            Log::warn("Overlay D3D resources changed for current-target rendering. Recreating overlay resources.");
            ResetDeviceResources();
        } else if (device_ || context_) {
            Log::warn("Overlay D3D resources were incomplete. Recreating them before current-target rendering.");
            ResetDeviceResources();
        }

        if (HasPipelineResources()) {
            return true;
        }

        device_ = device;
        context_ = context;
        swapChain_ = nullptr;

        if (!CreatePipelineResources() || !CreateWhiteTexture()) {
            ResetDeviceResources();
            return false;
        }

        if (!g_loggedPipelineInit.exchange(true, std::memory_order_acq_rel)) {
            Log::info(
                "Overlay D3D11 pipeline initialized from active render context. device={}, context={}.",
                static_cast<const void*>(device_.Get()),
                static_cast<const void*>(context_.Get()));
        }
        return HasPipelineResources();
    }

    bool D3D11OverlayRenderer::EnsureBackBuffer(IDXGISwapChain* swapChain)
    {
        if (renderTargetView_) {
            return true;
        }

        if (!device_) {
            return false;
        }

        ComPtr<ID3D11Texture2D> backBuffer;
        if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backBuffer.GetAddressOf())))) {
            Log::error("Unable to access swapchain back buffer.");
            return false;
        }

        D3D11_TEXTURE2D_DESC backBufferDesc{};
        backBuffer->GetDesc(&backBufferDesc);
        if (backBufferDesc.Width == 0 || backBufferDesc.Height == 0) {
            return false;
        }

        if (FAILED(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, renderTargetView_.GetAddressOf()))) {
            Log::error("Unable to create overlay render target view.");
            return false;
        }
        backBufferWidth_ = backBufferDesc.Width;
        backBufferHeight_ = backBufferDesc.Height;
        if (!g_loggedBackBuffer.exchange(true, std::memory_order_acq_rel)) {
            Log::info("Overlay render target acquired: {}x{}.", backBufferWidth_, backBufferHeight_);
        }
        return true;
    }

    bool D3D11OverlayRenderer::HasPipelineResources() const noexcept
    {
        return device_ &&
            context_ &&
            vertexShader_ &&
            pixelShader_ &&
            inputLayout_ &&
            vertexBuffer_ &&
            sampler_ &&
            blendState_ &&
            depthStencilState_ &&
            rasterizerState_ &&
            whiteTexture_.srv;
    }

    bool D3D11OverlayRenderer::CreatePipelineResources()
    {
        ComPtr<ID3DBlob> vsBlob;
        ComPtr<ID3DBlob> psBlob;
        if (!CompileShader(kVertexShader, "main", "vs_4_0", vsBlob) ||
            !CompileShader(kPixelShader, "main", "ps_4_0", psBlob)) {
            return false;
        }

        if (FAILED(device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, vertexShader_.GetAddressOf()))) {
            Log::error("Unable to create overlay vertex shader.");
            return false;
        }
        if (FAILED(device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, pixelShader_.GetAddressOf()))) {
            Log::error("Unable to create overlay pixel shader.");
            return false;
        }

        const D3D11_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(device_->CreateInputLayout(layout, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), inputLayout_.GetAddressOf()))) {
            Log::error("Unable to create overlay input layout.");
            return false;
        }

        D3D11_BUFFER_DESC vertexDesc{};
        vertexDesc.ByteWidth = static_cast<UINT>(sizeof(Vertex) * kMaxVertexCount);
        vertexDesc.Usage = D3D11_USAGE_DYNAMIC;
        vertexDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vertexDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateBuffer(&vertexDesc, nullptr, vertexBuffer_.GetAddressOf()))) {
            Log::error("Unable to create overlay vertex buffer.");
            return false;
        }

        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device_->CreateSamplerState(&samplerDesc, sampler_.GetAddressOf()))) {
            Log::error("Unable to create overlay sampler.");
            return false;
        }

        D3D11_BLEND_DESC blendDesc{};
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device_->CreateBlendState(&blendDesc, blendState_.GetAddressOf()))) {
            Log::error("Unable to create overlay blend state.");
            return false;
        }

        D3D11_DEPTH_STENCIL_DESC depthDesc{};
        depthDesc.DepthEnable = FALSE;
        depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        depthDesc.StencilEnable = FALSE;
        if (FAILED(device_->CreateDepthStencilState(&depthDesc, depthStencilState_.GetAddressOf()))) {
            Log::error("Unable to create overlay depth-stencil state.");
            return false;
        }

        D3D11_RASTERIZER_DESC rasterizerDesc{};
        rasterizerDesc.FillMode = D3D11_FILL_SOLID;
        rasterizerDesc.CullMode = D3D11_CULL_NONE;
        rasterizerDesc.DepthClipEnable = FALSE;
        rasterizerDesc.ScissorEnable = FALSE;
        if (FAILED(device_->CreateRasterizerState(&rasterizerDesc, rasterizerState_.GetAddressOf()))) {
            Log::error("Unable to create overlay rasterizer state.");
            return false;
        }

        return true;
    }

    bool D3D11OverlayRenderer::CreateWhiteTexture()
    {
        const std::uint32_t white = 0xFFFFFFFF;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 1;
        desc.Height = 1;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = &white;
        data.SysMemPitch = sizeof(white);

        if (FAILED(device_->CreateTexture2D(&desc, &data, whiteTexture_.texture.GetAddressOf()))) {
            return false;
        }
        if (FAILED(device_->CreateShaderResourceView(whiteTexture_.texture.Get(), nullptr, whiteTexture_.srv.GetAddressOf()))) {
            return false;
        }
        whiteTexture_.width = 1;
        whiteTexture_.height = 1;
        return true;
    }

    void D3D11OverlayRenderer::ResetDeviceResources()
    {
        renderTargetView_.Reset();
        vertexShader_.Reset();
        pixelShader_.Reset();
        inputLayout_.Reset();
        vertexBuffer_.Reset();
        sampler_.Reset();
        blendState_.Reset();
        depthStencilState_.Reset();
        rasterizerState_.Reset();
        currentTexture_ = {};
        nextTexture_ = {};
        whiteTexture_ = {};
        backBufferWidth_ = 0;
        backBufferHeight_ = 0;
        swapChain_ = nullptr;
        context_.Reset();
        device_.Reset();
    }

    bool D3D11OverlayRenderer::UpdateTexture(TextureSlot& slot, const VideoFramePtr& framePtr)
    {
        if (!framePtr) {
            return false;
        }

        const auto& frame = *framePtr;
        if (frame.width <= 0 || frame.height <= 0 || frame.bgra.empty()) {
            return false;
        }
        if (slot.texture && slot.srv && slot.uploadedFrame.get() == framePtr.get() &&
            slot.width == frame.width && slot.height == frame.height) {
            return true;
        }
        if (static_cast<std::size_t>(frame.width) >
            (std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(frame.height) / 4U)) {
            Log::warn("Skipping oversized video frame upload: {}x{}.", frame.width, frame.height);
            return false;
        }
        const auto expectedBytes = static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 4U;
        if (expectedBytes == 0 || frame.bgra.size() < expectedBytes) {
            Log::warn("Skipping malformed video frame upload: {}x{}, {} bytes.", frame.width, frame.height, frame.bgra.size());
            return false;
        }
        if (ShouldLog(g_frameSampleLogs, 8)) {
            const auto centerOffset =
                ((static_cast<std::size_t>(frame.height) / 2U) * static_cast<std::size_t>(frame.width) +
                    (static_cast<std::size_t>(frame.width) / 2U)) *
                4U;
            ALS::Log::diagnostic(
                "video_frame_sample size={}x{} pts={} bytes={} first_bgra={:02X},{:02X},{:02X},{:02X} center_bgra={:02X},{:02X},{:02X},{:02X}",
                frame.width,
                frame.height,
                frame.ptsSeconds,
                frame.bgra.size(),
                static_cast<unsigned>(frame.bgra[0]),
                static_cast<unsigned>(frame.bgra[1]),
                static_cast<unsigned>(frame.bgra[2]),
                static_cast<unsigned>(frame.bgra[3]),
                static_cast<unsigned>(frame.bgra[centerOffset + 0]),
                static_cast<unsigned>(frame.bgra[centerOffset + 1]),
                static_cast<unsigned>(frame.bgra[centerOffset + 2]),
                static_cast<unsigned>(frame.bgra[centerOffset + 3]));
        }

        if (!slot.texture || slot.width != frame.width || slot.height != frame.height) {
            slot = {};

            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = static_cast<UINT>(frame.width);
            desc.Height = static_cast<UINT>(frame.height);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            if (FAILED(device_->CreateTexture2D(&desc, nullptr, slot.texture.GetAddressOf()))) {
                Log::error("Unable to create overlay video texture.");
                return false;
            }
            if (FAILED(device_->CreateShaderResourceView(slot.texture.Get(), nullptr, slot.srv.GetAddressOf()))) {
                Log::error("Unable to create overlay video SRV.");
                slot.texture.Reset();
                return false;
            }
            slot.width = frame.width;
            slot.height = frame.height;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(slot.texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            if (ShouldLog(g_textureMapFailureLogs, 10)) {
                Log::warn("Unable to map overlay video texture for upload.");
            }
            return false;
        }

        const auto srcPitch = static_cast<std::size_t>(frame.width) * 4U;
        auto* dst = static_cast<std::uint8_t*>(mapped.pData);
        const auto* src = frame.bgra.data();
        if (mapped.RowPitch == srcPitch) {
            std::memcpy(dst, src, expectedBytes);
        } else {
            for (int row = 0; row < frame.height; ++row) {
                std::memcpy(
                    dst + static_cast<std::size_t>(row) * mapped.RowPitch,
                    src + static_cast<std::size_t>(row) * srcPitch,
                    srcPitch);
            }
        }
        context_->Unmap(slot.texture.Get(), 0);
        slot.uploadedFrame = framePtr;
        if (!g_loggedFirstTextureUpload.exchange(true, std::memory_order_acq_rel)) {
            Log::info("First overlay video frame uploaded to GPU texture: {}x{}.", frame.width, frame.height);
        }
        return true;
    }

    std::size_t D3D11OverlayRenderer::DrawTexturedQuads(
        const QuadCommand* commands,
        std::size_t commandCount,
        float viewportWidth,
        float viewportHeight)
    {
        if (!commands || commandCount == 0 || viewportWidth <= 0.0F || viewportHeight <= 0.0F) {
            return 0;
        }

        commandCount = std::min(commandCount, kMaxQuadCount);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(vertexBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            if (ShouldLog(g_vertexMapFailureLogs, 10)) {
                Log::warn("Unable to map overlay vertex buffer.");
            }
            return 0;
        }

        auto* vertices = static_cast<Vertex*>(mapped.pData);
        std::array<ID3D11ShaderResourceView*, kMaxQuadCount> srvs{};
        UINT quadCount = 0;

        for (std::size_t i = 0; i < commandCount; ++i) {
            const auto& command = commands[i];
            if (!command.srv || command.alpha <= 0.0F) {
                continue;
            }

            const auto& destination = command.destination;
            const auto& uv = command.uv;
            const auto left = (destination.x / viewportWidth) * 2.0F - 1.0F;
            const auto right = ((destination.x + destination.width) / viewportWidth) * 2.0F - 1.0F;
            const auto top = 1.0F - (destination.y / viewportHeight) * 2.0F;
            const auto bottom = 1.0F - ((destination.y + destination.height) / viewportHeight) * 2.0F;

            const auto u0 = uv.x;
            const auto u1 = uv.x + uv.width;
            const auto v0 = uv.y;
            const auto v1 = uv.y + uv.height;

            const float color[4]{
                command.color.r,
                command.color.g,
                command.color.b,
                ClampAlpha(command.alpha * command.color.a)
            };

            const auto vertexOffset = static_cast<std::size_t>(quadCount) * kVerticesPerQuad;
            vertices[vertexOffset + 0] = Vertex{ { left, top, 0.0F }, { u0, v0 }, { color[0], color[1], color[2], color[3] } };
            vertices[vertexOffset + 1] = Vertex{ { right, top, 0.0F }, { u1, v0 }, { color[0], color[1], color[2], color[3] } };
            vertices[vertexOffset + 2] = Vertex{ { left, bottom, 0.0F }, { u0, v1 }, { color[0], color[1], color[2], color[3] } };
            vertices[vertexOffset + 3] = Vertex{ { right, bottom, 0.0F }, { u1, v1 }, { color[0], color[1], color[2], color[3] } };
            srvs[quadCount] = command.srv;
            ++quadCount;
        }
        context_->Unmap(vertexBuffer_.Get(), 0);

        if (quadCount == 0) {
            return 0;
        }

        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        context_->IASetVertexBuffers(0, 1, vertexBuffer_.GetAddressOf(), &stride, &offset);
        for (UINT i = 0; i < quadCount; ++i) {
            auto* srv = srvs[i];
            context_->PSSetShaderResources(0, 1, &srv);
            context_->Draw(static_cast<UINT>(kVerticesPerQuad), i * static_cast<UINT>(kVerticesPerQuad));
        }
        return quadCount;
    }
}
