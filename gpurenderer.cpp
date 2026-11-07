#include "gpurenderer.h"
#include <QElapsedTimer>

#ifdef Q_OS_WIN
#include <dxgi.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <algorithm>
#include <cstring>

namespace {
QString featureName(D3D_FEATURE_LEVEL level)
{
    switch (level) {
    case D3D_FEATURE_LEVEL_11_1: return QStringLiteral("11.1");
    case D3D_FEATURE_LEVEL_11_0: return QStringLiteral("11.0");
    case D3D_FEATURE_LEVEL_10_1: return QStringLiteral("10.1");
    case D3D_FEATURE_LEVEL_10_0: return QStringLiteral("10.0");
    default: return QStringLiteral("Unknown");
    }
}
QString hexResult(HRESULT hr)
{
    return QStringLiteral("0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0')).toUpper();
}
const char *shaderSource = R"HLSL(
struct VSIn { float2 pos : POSITION; float4 color : COLOR; };
struct VSOut { float4 pos : SV_POSITION; float4 color : COLOR; };
VSOut VSMain(VSIn input) {
    VSOut o; o.pos = float4(input.pos, 0.0, 1.0); o.color = input.color; return o;
}
float4 PSMain(VSOut input) : SV_TARGET { return input.color; }
)HLSL";
const char *bokehShaderSource = R"HLSL(
struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 color : COLOR; };
VSOut VSMain(VSIn input) {
    VSOut o; o.pos = float4(input.pos, 0.0, 1.0); o.uv = input.uv; o.color = input.color; return o;
}
cbuffer BokehParams : register(b0) { float softness; float3 padding; };
float4 PSMain(VSOut input) : SV_TARGET {
    float2 p = input.uv * 2.0 - 1.0;
    float d = length(p);
    float edge = saturate(1.0 - d);
    float power = lerp(4.0, 0.72, saturate(softness));
    float falloff = pow(edge, power);
    float core = exp(-d * d * lerp(2.0, 12.0, saturate(softness)));
    float alpha = saturate(falloff * 0.82 + core * 0.18) * input.color.a;
    return float4(input.color.rgb, alpha);
}
)HLSL";
const char *glowShaderSource = R"HLSL(
struct VSOut { float4 pos : SV_POSITION; };
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 p = id == 0 ? float2(-1.0, -1.0) : (id == 1 ? float2(-1.0, 3.0) : float2(3.0, -1.0));
    o.pos = float4(p, 0.0, 1.0); return o;
}
cbuffer GlowParams : register(b0) {
    float4 colors[4]; float4 stops;
    float4 params0; // strength, hole radius, outer radius, phase
    float4 params1; // width, height, padding
};
float4 PSMain(VSOut input) : SV_TARGET {
    float2 center = params1.xy * 0.5;
    float minHalf = max(min(params1.x, params1.y) * 0.5, 1.0);
    float2 radial = (input.pos.xy - center) / minHalf;
    float radius = length(radial);
    float alpha = smoothstep(params0.y, max(params0.z, params0.y + 0.0001), radius) * params0.x;
    if (alpha <= 0.0005) discard;
    float angle = frac(atan2(radial.y, radial.x) * 0.15915494309 + params0.w);
    float4 color = colors[0];
    if (angle >= stops.x && angle < stops.y) color = lerp(colors[0], colors[1], saturate((angle-stops.x)/max(stops.y-stops.x,0.0001)));
    else if (angle >= stops.y && angle < stops.z) color = lerp(colors[1], colors[2], saturate((angle-stops.y)/max(stops.z-stops.y,0.0001)));
    else if (angle >= stops.z) color = lerp(colors[2], colors[3], saturate((angle-stops.z)/max(1.0-stops.z,0.0001)));
    else color = lerp(colors[3], colors[0], saturate(angle/max(stops.x,0.0001)));
    return float4(color.rgb * alpha, alpha);
}
)HLSL";
const char *nativeCopyShaderSource = R"HLSL(
struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(VSIn i) { VSOut o; o.pos=float4(i.pos,0.0,1.0); o.uv=i.uv; return o; }
Texture2D sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);
float4 PSMain(VSOut i) : SV_TARGET { return sourceTexture.Sample(sourceSampler,i.uv); }
)HLSL";
struct NativeCopyVertex { float x, y, u, v; };
}
#endif

GpuRenderer &GpuRenderer::instance()
{
    static GpuRenderer renderer;
    return renderer;
}

bool GpuRenderer::initialize()
{
    if (attempted) return available;
    attempted = true;
#ifdef Q_OS_WIN
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                         D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL selected = D3D_FEATURE_LEVEL_10_0;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, device.GetAddressOf(), &selected, context.GetAddressOf());
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            levels + 1, ARRAYSIZE(levels) - 1, D3D11_SDK_VERSION,
            device.GetAddressOf(), &selected, context.GetAddressOf());
    }
    if (SUCCEEDED(hr)) hardware = true;
    else {
        const HRESULT hardwareError = hr;
        device.Reset(); context.Reset();
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, device.GetAddressOf(), &selected, context.GetAddressOf());
        if (hr == E_INVALIDARG) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                levels + 1, ARRAYSIZE(levels) - 1, D3D11_SDK_VERSION,
                device.GetAddressOf(), &selected, context.GetAddressOf());
        }
        if (FAILED(hr)) {
            errorMessage = QStringLiteral("Hardware device: %1; WARP device: %2").arg(hexResult(hardwareError), hexResult(hr));
            return false;
        }
    }
    available = true;
    featureLevelName = featureName(selected);
    adapterName = hardware ? QStringLiteral("Hardware adapter") : QStringLiteral("Microsoft WARP (software)");
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (SUCCEEDED(device.As(&dxgiDevice))) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc))) adapterName = QString::fromWCharArray(desc.Description);
        }
    }
    createPipeline();
    return true;
#else
    errorMessage = QStringLiteral("Direct3D 11 is only available on Windows.");
    return false;
#endif
}

bool GpuRenderer::createPipeline()
{
#ifdef Q_OS_WIN
    if (!available || pipelineReady) return pipelineReady;
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errors;
    HRESULT hr = D3DCompile(shaderSource, std::strlen(shaderSource), nullptr, nullptr, nullptr,
                            "VSMain", "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                            vsBlob.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr)) { renderError = QStringLiteral("Vertex shader compile failed: %1").arg(hexResult(hr)); return false; }
    hr = D3DCompile(shaderSource, std::strlen(shaderSource), nullptr, nullptr, nullptr,
                    "PSMain", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
                    psBlob.GetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { renderError = QStringLiteral("Pixel shader compile failed: %1").arg(hexResult(hr)); return false; }
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, vertexShader.GetAddressOf());
    if (FAILED(hr)) { renderError = QStringLiteral("CreateVertexShader failed: %1").arg(hexResult(hr)); return false; }
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, pixelShader.GetAddressOf());
    if (FAILED(hr)) { renderError = QStringLiteral("CreatePixelShader failed: %1").arg(hexResult(hr)); return false; }
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };
    hr = device->CreateInputLayout(layout, ARRAYSIZE(layout), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), inputLayout.GetAddressOf());
    if (FAILED(hr)) { renderError = QStringLiteral("CreateInputLayout failed: %1").arg(hexResult(hr)); return false; }
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = UINT(sizeof(Vertex) * 6 * 128 * 6); bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&bd, nullptr, vertexBuffer.GetAddressOf());
    if (FAILED(hr)) { renderError = QStringLiteral("CreateBuffer failed: %1").arg(hexResult(hr)); return false; }
    D3D11_BLEND_DESC blend{}; blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&blend, blendState.GetAddressOf());
    if (FAILED(hr)) { renderError = QStringLiteral("CreateBlendState failed: %1").arg(hexResult(hr)); return false; }
    D3D11_BLEND_DESC glowBlend{};
    glowBlend.RenderTarget[0].BlendEnable = TRUE;
    glowBlend.RenderTarget[0].SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
    glowBlend.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    glowBlend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    glowBlend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    glowBlend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    glowBlend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    glowBlend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&glowBlend, glowBlendState.GetAddressOf());
    if (FAILED(hr)) { renderError = QStringLiteral("Glow blend state creation failed: %1").arg(hexResult(hr)); return false; }
    pipelineReady = true; renderError.clear(); return true;
#else
    return false;
#endif
}


bool GpuRenderer::configureNativePresentation(void *nativeWindow, int width, int height)
{
#ifdef Q_OS_WIN
    if (!initialize() || !nativeWindow || width < 1 || height < 1) {
        nativePresentError = QStringLiteral("Native preview window or D3D11 device is unavailable.");
        return false;
    }
    HWND hwnd = static_cast<HWND>(nativeWindow);
    if (nativePreviewReady && nativePreviewHwnd == hwnd &&
        nativePreviewWidth == width && nativePreviewHeight == height) return true;
    if (nativeSwapChain && nativePreviewHwnd == hwnd && nativePreviewReady) {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        nativeBackBufferRtv.Reset();
        HRESULT hr = nativeSwapChain->ResizeBuffers(0, UINT(width), UINT(height), DXGI_FORMAT_UNKNOWN, 0);
        if (SUCCEEDED(hr)) {
            nativePreviewWidth = width; nativePreviewHeight = height;
            if (createNativeBackBuffer()) { nativePresentError.clear(); return true; }
        } else nativePresentError = QStringLiteral("ResizeBuffers failed: %1").arg(hexResult(hr));
        disableNativePresentation(); return false;
    }
    disableNativePresentation();
    nativePreviewHwnd = hwnd; nativePreviewWidth = width; nativePreviewHeight = height;
    if (!createNativeSwapChain(width, height)) { disableNativePresentation(); return false; }
    nativePreviewReady = true; nativePresentError.clear(); return true;
#else
    Q_UNUSED(nativeWindow); Q_UNUSED(width); Q_UNUSED(height);
    nativePresentError = QStringLiteral("Native D3D11 presentation is Windows only."); return false;
#endif
}

void GpuRenderer::disableNativePresentation()
{
#ifdef Q_OS_WIN
    if (context) context->OMSetRenderTargets(0, nullptr, nullptr);
    nativeBackBufferRtv.Reset(); nativeSwapChain.Reset(); nativeCopySampler.Reset();
    nativeCopyVertexBuffer.Reset(); nativeCopyInputLayout.Reset();
    nativeCopyPixelShader.Reset(); nativeCopyVertexShader.Reset();
#endif
    nativePreviewHwnd = nullptr; nativePreviewWidth = nativePreviewHeight = 0;
    nativePreviewReady = false; nativeFramePresented = false;
}
bool GpuRenderer::nativePresentationActive() const { return nativeFramePresented; }
QString GpuRenderer::nativePresentationError() const { return nativePresentError; }

#ifdef Q_OS_WIN
bool GpuRenderer::createNativeSwapChain(int width, int height)
{
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory> factory;
    HRESULT hr = device.As(&dxgiDevice);
    if (FAILED(hr) || FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf())) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())))) {
        nativePresentError = QStringLiteral("Could not obtain DXGI factory from renderer device."); return false;
    }
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width=UINT(width); desc.BufferDesc.Height=UINT(height);
    desc.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator=0; desc.BufferDesc.RefreshRate.Denominator=1;
    desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount=2; desc.OutputWindow=nativePreviewHwnd;
    desc.Windowed=TRUE; desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    hr=factory->CreateSwapChain(device.Get(),&desc,nativeSwapChain.GetAddressOf());
    if(FAILED(hr)){nativePresentError=QStringLiteral("CreateSwapChain failed: %1").arg(hexResult(hr));return false;}
    factory->MakeWindowAssociation(nativePreviewHwnd,DXGI_MWA_NO_ALT_ENTER);

    Microsoft::WRL::ComPtr<ID3DBlob> vs,ps,errors;
    hr=D3DCompile(nativeCopyShaderSource,std::strlen(nativeCopyShaderSource),nullptr,nullptr,nullptr,
                  "VSMain","vs_4_0",D3DCOMPILE_ENABLE_STRICTNESS,0,vs.GetAddressOf(),errors.GetAddressOf());
    if(FAILED(hr)){nativePresentError=QStringLiteral("Native VS compile failed: %1").arg(hexResult(hr));return false;}
    hr=D3DCompile(nativeCopyShaderSource,std::strlen(nativeCopyShaderSource),nullptr,nullptr,nullptr,
                  "PSMain","ps_4_0",D3DCOMPILE_ENABLE_STRICTNESS,0,ps.GetAddressOf(),errors.ReleaseAndGetAddressOf());
    if(FAILED(hr)){nativePresentError=QStringLiteral("Native PS compile failed: %1").arg(hexResult(hr));return false;}
    hr=device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,nativeCopyVertexShader.GetAddressOf());
    if(FAILED(hr)){nativePresentError=QStringLiteral("Create native VS failed: %1").arg(hexResult(hr));return false;}
    hr=device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,nativeCopyPixelShader.GetAddressOf());
    if(FAILED(hr)){nativePresentError=QStringLiteral("Create native PS failed: %1").arg(hexResult(hr));return false;}
    const D3D11_INPUT_ELEMENT_DESC layout[]={
        {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0}};
    hr=device->CreateInputLayout(layout,2,vs->GetBufferPointer(),vs->GetBufferSize(),nativeCopyInputLayout.GetAddressOf());
    if(FAILED(hr)){nativePresentError=QStringLiteral("Create native layout failed: %1").arg(hexResult(hr));return false;}
    D3D11_BUFFER_DESC vb{};vb.ByteWidth=sizeof(NativeCopyVertex)*6;vb.Usage=D3D11_USAGE_DYNAMIC;
    vb.BindFlags=D3D11_BIND_VERTEX_BUFFER;vb.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    hr=device->CreateBuffer(&vb,nullptr,nativeCopyVertexBuffer.GetAddressOf());
    if(FAILED(hr)){nativePresentError=QStringLiteral("Create native VB failed: %1").arg(hexResult(hr));return false;}
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    hr=device->CreateSamplerState(&sd,nativeCopySampler.GetAddressOf());
    if(FAILED(hr)){nativePresentError=QStringLiteral("Create native sampler failed: %1").arg(hexResult(hr));return false;}
    return createNativeBackBuffer();
}
bool GpuRenderer::createNativeBackBuffer()
{
    if(!nativeSwapChain)return false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
    HRESULT hr=nativeSwapChain->GetBuffer(0,IID_PPV_ARGS(back.GetAddressOf()));
    if(FAILED(hr)){nativePresentError=QStringLiteral("GetBuffer failed: %1").arg(hexResult(hr));return false;}
    hr=device->CreateRenderTargetView(back.Get(),nullptr,nativeBackBufferRtv.GetAddressOf());
    if(FAILED(hr)){nativePresentError=QStringLiteral("Back-buffer RTV failed: %1").arg(hexResult(hr));return false;}
    return true;
}
bool GpuRenderer::presentNativeTexture()
{
    if(!nativePreviewReady||!nativeSwapChain||!nativeBackBufferRtv||!cachedTextureSrv)return false;
    ++nativePresentAttempts;
    QElapsedTimer timer;timer.start();
    ID3D11RenderTargetView *rtv=nativeBackBufferRtv.Get();context->OMSetRenderTargets(1,&rtv,nullptr);
    D3D11_VIEWPORT vp{};vp.Width=float(nativePreviewWidth);vp.Height=float(nativePreviewHeight);
    vp.MinDepth=0;vp.MaxDepth=1;context->RSSetViewports(1,&vp);
    const float clear[4]={0.063f,0.067f,0.078f,1.0f};context->ClearRenderTargetView(rtv,clear);
    const NativeCopyVertex quad[6]={{-1,-1,0,1},{-1,1,0,0},{1,1,1,0},
                                     {-1,-1,0,1},{1,1,1,0},{1,-1,1,1}};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr=context->Map(nativeCopyVertexBuffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
    if(FAILED(hr)){
        nativePresentCpuMs=timer.nsecsElapsed()/1000000.0;
        nativePresentCpuMaxMs=qMax(nativePresentCpuMaxMs,nativePresentCpuMs);
        const quint64 completedAttempts = nativePresentAttempts - nativePresentFailures;
        if (completedAttempts > 0)
            nativePresentCpuAverageMs += (nativePresentCpuMs-nativePresentCpuAverageMs)
                / double(completedAttempts);
        ++nativePresentFailures;
        nativePresentError=QStringLiteral("Native vertex Map failed: %1").arg(hexResult(hr));
        context->OMSetRenderTargets(0,nullptr,nullptr);return false;
    }
    std::memcpy(mapped.pData,quad,sizeof(quad));context->Unmap(nativeCopyVertexBuffer.Get(),0);
    UINT stride=sizeof(NativeCopyVertex),offset=0;ID3D11Buffer *vb=nativeCopyVertexBuffer.Get();
    context->IASetInputLayout(nativeCopyInputLayout.Get());context->IASetVertexBuffers(0,1,&vb,&stride,&offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(nativeCopyVertexShader.Get(),nullptr,0);
    context->PSSetShader(nativeCopyPixelShader.Get(),nullptr,0);
    ID3D11ShaderResourceView *srv=cachedTextureSrv.Get();ID3D11SamplerState *sampler=nativeCopySampler.Get();
    context->PSSetShaderResources(0,1,&srv);context->PSSetSamplers(0,1,&sampler);context->Draw(6,0);
    ID3D11ShaderResourceView *nullSrv=nullptr;context->PSSetShaderResources(0,1,&nullSrv);
    context->OMSetRenderTargets(0,nullptr,nullptr);
    hr=nativeSwapChain->Present(0,0);
    nativePresentCpuMs=timer.nsecsElapsed()/1000000.0;
    nativePresentCpuMaxMs=qMax(nativePresentCpuMaxMs,nativePresentCpuMs);
    const quint64 completedAttempts = nativePresentAttempts - nativePresentFailures;
    if (completedAttempts > 0)
        nativePresentCpuAverageMs += (nativePresentCpuMs-nativePresentCpuAverageMs)
            / double(completedAttempts);
    if(FAILED(hr)){
        ++nativePresentFailures;
        nativePresentError=QStringLiteral("Present failed: %1").arg(hexResult(hr));return false;
    }
    ++nativePresentFrames;nativePresentError.clear();return true;
}
#endif

bool GpuRenderer::renderColoredRects(int width, int height, const QVector<QRectF> &rects,
                                     const QVector<QColor> &colors, QImage *output, const QImage *background,
                                     const QVector<QColor> *topColors, const GpuGlowParams *glow)
{
#ifdef Q_OS_WIN
    nativeFramePresented = false;
    QElapsedTimer totalTimer; totalTimer.start();
    renderError.clear();
    if (!output || width < 1 || height < 1 || rects.isEmpty() || rects.size() != colors.size()) {
        ++failedFrames;
        renderError = QStringLiteral("Invalid render dimensions or rectangle/color arrays."); return false;
    }
    if (!initialize() || !createPipeline()) { ++failedFrames; return false; }
    if (topColors && topColors->size() != rects.size()) { ++failedFrames; renderError = QStringLiteral("Gradient color count mismatch."); return false; }
    if (rects.size() > 128 * 6) { ++failedFrames; renderError = QStringLiteral("Too many rectangles for current vertex buffer (maximum 6 per spectrum column)."); return false; }
    // Phase 6D: keep the render target and a three-slot staging ring.
    // Completed frames are mapped only after their event query signals readiness.
    QElapsedTimer resourceTimer; resourceTimer.start();
    if (!cachedTexture || !cachedRtv || cachedWidth != width || cachedHeight != height) {
        cachedRtv.Reset(); cachedTextureSrv.Reset(); cachedTexture.Reset();
        for (auto &slot : phase3ReadbackSlots) {
            slot.staging.Reset(); slot.completion.Reset(); slot.pending = false;
        }
        phase3AsyncPending = 0;
        nextPhase3ReadbackSlot = 0;
        cachedPhase3Image = QImage();
        cachedWidth = cachedHeight = 0;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(width); td.Height = UINT(height); td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT createHr = device->CreateTexture2D(&td, nullptr, cachedTexture.GetAddressOf());
        if (FAILED(createHr)) {
            ++failedFrames;
            renderError = QStringLiteral("CreateTexture2D failed: %1").arg(hexResult(createHr));
            return false;
        }
        createHr = device->CreateRenderTargetView(cachedTexture.Get(), nullptr, cachedRtv.GetAddressOf());
        if (FAILED(createHr)) {
            cachedTexture.Reset(); ++failedFrames;
            renderError = QStringLiteral("CreateRenderTargetView failed: %1").arg(hexResult(createHr));
            return false;
        }
        createHr = device->CreateShaderResourceView(cachedTexture.Get(), nullptr, cachedTextureSrv.GetAddressOf());
        if (FAILED(createHr)) { ++failedFrames; renderError = QStringLiteral("Create render-target SRV failed: %1").arg(hexResult(createHr)); return false; }
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (int i = 0; i < 3; ++i) {
            auto &slot = phase3ReadbackSlots[i];
            createHr = device->CreateTexture2D(&sd, nullptr, slot.staging.GetAddressOf());
            if (FAILED(createHr)) {
                renderError = QStringLiteral("Phase 3 staging texture %1 creation failed: %2").arg(i).arg(hexResult(createHr));
                ++failedFrames; return false;
            }
            D3D11_QUERY_DESC qd{}; qd.Query = D3D11_QUERY_EVENT;
            createHr = device->CreateQuery(&qd, slot.completion.GetAddressOf());
            if (FAILED(createHr)) {
                renderError = QStringLiteral("Phase 3 completion query %1 creation failed: %2").arg(i).arg(hexResult(createHr));
                ++failedFrames; return false;
            }
        }
        cachedWidth = width; cachedHeight = height;
        ++resourceRebuilds;
    } else {
        ++resourceReuses;
    }
    lastResourceMs = resourceTimer.nsecsElapsed() / 1000000.0;
    HRESULT hr = S_OK;

    // Phase 4B.3: retain vector capacity between paints. Rectangle count
    // changes with glow/column settings, so clear and grow only when needed.
    cachedVertices.clear();
    const int requiredVertices = rects.size() * 6;
    if (cachedVertices.capacity() < requiredVertices)
        cachedVertices.reserve(requiredVertices);
    auto addVertex = [&](float x, float y, const QColor &c) {
        cachedVertices.push_back({2.0f * x / float(width) - 1.0f, 1.0f - 2.0f * y / float(height),
                                  float(c.redF()), float(c.greenF()), float(c.blueF()), float(c.alphaF())});
    };
    for (int i = 0; i < rects.size(); ++i) {
        const QRectF r = rects[i].normalized(); const QColor c = colors[i];
        const QColor top = topColors ? (*topColors)[i] : c;
        const float x1 = float(r.left()), y1 = float(r.top()), x2 = float(r.right()), y2 = float(r.bottom());
        addVertex(x1,y1,top); addVertex(x2,y1,top); addVertex(x1,y2,c);
        addVertex(x1,y2,c); addVertex(x2,y1,top); addVertex(x2,y2,c);
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) { ++failedFrames; renderError = QStringLiteral("Map vertex buffer failed: %1").arg(hexResult(hr)); return false; }
    std::memcpy(mapped.pData, cachedVertices.constData(), size_t(cachedVertices.size()) * sizeof(Vertex));
    context->Unmap(vertexBuffer.Get(), 0);

    // Phase 4A: upload an opaque, CPU-prepared video background into the GPU
    // render target, then draw Spectrum bars on that SAME GPU surface.
    // This is GPU compositing, but NOT zero-copy presentation: Qt video conversion,
    // upload and final readback still exist. See README for Phase 4B.
    lastUploadMs = 0.0;
    if (background && !background->isNull()) {
        QElapsedTimer uploadTimer; uploadTimer.start();
        QImage bg = background->convertToFormat(QImage::Format_ARGB32);
        if (bg.size() != QSize(width, height)) {
            ++failedFrames;
            renderError = QStringLiteral("Background size does not match render target.");
            return false;
        }
        context->UpdateSubresource(cachedTexture.Get(), 0, nullptr,
                                   bg.constBits(), UINT(bg.bytesPerLine()), 0);
        lastUploadMs = uploadTimer.nsecsElapsed() / 1000000.0;
    }
    QElapsedTimer drawTimer; drawTimer.start();
    const float clear[4] = {0,0,0,0};
    if (!background || background->isNull()) context->ClearRenderTargetView(cachedRtv.Get(), clear);
    D3D11_VIEWPORT vp{}; vp.Width = float(width); vp.Height = float(height); vp.MinDepth = 0; vp.MaxDepth = 1;
    context->RSSetViewports(1, &vp); ID3D11RenderTargetView *rtvRaw = cachedRtv.Get(); context->OMSetRenderTargets(1, &rtvRaw, nullptr);
    const float blendFactor[4] = {0,0,0,0};
    if (glow && glow->strength > 0.001f) {
        QElapsedTimer submitTimer; submitTimer.start();
        if (!glowVertexShader || !glowPixelShader || !glowConstantBuffer) {
            Microsoft::WRL::ComPtr<ID3DBlob> vs, ps, shaderErrors;
            HRESULT glowHr = D3DCompile(glowShaderSource, std::strlen(glowShaderSource), nullptr, nullptr, nullptr, "VSMain", "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, vs.GetAddressOf(), shaderErrors.GetAddressOf());
            if (FAILED(glowHr)) { renderError = QStringLiteral("Glow VS compile failed: %1").arg(hexResult(glowHr)); return false; }
            glowHr = D3DCompile(glowShaderSource, std::strlen(glowShaderSource), nullptr, nullptr, nullptr, "PSMain", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, ps.GetAddressOf(), shaderErrors.ReleaseAndGetAddressOf());
            if (FAILED(glowHr)) { renderError = QStringLiteral("Glow PS compile failed: %1").arg(hexResult(glowHr)); return false; }
            glowHr = device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, glowVertexShader.GetAddressOf());
            if (FAILED(glowHr)) { renderError = QStringLiteral("Glow VS creation failed: %1").arg(hexResult(glowHr)); return false; }
            glowHr = device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, glowPixelShader.GetAddressOf());
            if (FAILED(glowHr)) { renderError = QStringLiteral("Glow PS creation failed: %1").arg(hexResult(glowHr)); return false; }
            D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = sizeof(GpuGlowParams); cbd.Usage = D3D11_USAGE_DYNAMIC; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            glowHr = device->CreateBuffer(&cbd, nullptr, glowConstantBuffer.GetAddressOf());
            if (FAILED(glowHr)) { renderError = QStringLiteral("Glow constant buffer creation failed: %1").arg(hexResult(glowHr)); return false; }
        }
        if (!glowGpuTimingQueriesReady) {
            bool created = true;
            for (auto &slot : glowGpuTimingSlots) {
                D3D11_QUERY_DESC qd{};
                if (!slot.disjoint) { qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; if (FAILED(device->CreateQuery(&qd, slot.disjoint.GetAddressOf()))) { created = false; break; } }
                if (!slot.start) { qd.Query = D3D11_QUERY_TIMESTAMP; if (FAILED(device->CreateQuery(&qd, slot.start.GetAddressOf()))) { created = false; break; } }
                if (!slot.end) { qd.Query = D3D11_QUERY_TIMESTAMP; if (FAILED(device->CreateQuery(&qd, slot.end.GetAddressOf()))) { created = false; break; } }
            }
            glowGpuTimingQueriesReady = created;
            glowGpuTimingStatus = created ? QStringLiteral("active (non-blocking)") : QStringLiteral("query creation failed");
        }
        for (auto &slot : glowGpuTimingSlots) {
            if (!slot.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
            HRESULT ready = context->GetData(slot.disjoint.Get(), &data, sizeof(data), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (ready == S_OK) {
                UINT64 t0 = 0, t1 = 0;
                HRESULT r0 = context->GetData(slot.start.Get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                HRESULT r1 = context->GetData(slot.end.Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (r0 == S_OK && r1 == S_OK && !data.Disjoint && data.Frequency && t1 >= t0) {
                    glowGpuLastMs = double(t1-t0)*1000.0/double(data.Frequency);
                    ++glowGpuSamples; glowGpuAverageMs += (glowGpuLastMs-glowGpuAverageMs)/double(glowGpuSamples);
                    glowGpuMaxMs = qMax(glowGpuMaxMs, glowGpuLastMs);
                }
                slot.pending = false;
            } else if (ready != S_FALSE) slot.pending = false;
        }
        GlowGpuTimingSlot *timing = nullptr;
        if (glowGpuTimingQueriesReady) for (int n=0;n<4;++n) {
            int idx=(nextGlowGpuTimingSlot+n)%4;
            if (!glowGpuTimingSlots[idx].pending) { timing=&glowGpuTimingSlots[idx]; nextGlowGpuTimingSlot=(idx+1)%4; break; }
        }
        if (timing) context->Begin(timing->disjoint.Get());
        if (timing) context->End(timing->start.Get());
        D3D11_MAPPED_SUBRESOURCE mappedGlow{};
        HRESULT mapHr=context->Map(glowConstantBuffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mappedGlow);
        if (FAILED(mapHr)) { renderError=QStringLiteral("Glow constant buffer map failed: %1").arg(hexResult(mapHr)); return false; }
        std::memcpy(mappedGlow.pData,glow,sizeof(GpuGlowParams)); context->Unmap(glowConstantBuffer.Get(),0);
        context->OMSetBlendState(glowBlendState.Get(),blendFactor,0xffffffff);
        context->IASetInputLayout(nullptr); context->IASetVertexBuffers(0,0,nullptr,nullptr,nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(glowVertexShader.Get(),nullptr,0); context->PSSetShader(glowPixelShader.Get(),nullptr,0);
        ID3D11Buffer *cb=glowConstantBuffer.Get(); context->PSSetConstantBuffers(0,1,&cb);
        context->Draw(3,0);
        if (timing) context->End(timing->end.Get());
        if (timing) { context->End(timing->disjoint.Get()); timing->pending=true; }
        glowCpuSubmitLastMs=submitTimer.nsecsElapsed()/1000000.0; ++glowCpuSubmitSamples;
        glowCpuSubmitAverageMs+=(glowCpuSubmitLastMs-glowCpuSubmitAverageMs)/double(glowCpuSubmitSamples);
        ++glowDraws;
    }
    context->OMSetBlendState(blendState.Get(), blendFactor, 0xffffffff);
    UINT stride = sizeof(Vertex), offset = 0; ID3D11Buffer *vb = vertexBuffer.Get();
    context->IASetInputLayout(inputLayout.Get()); context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertexShader.Get(), nullptr, 0); context->PSSetShader(pixelShader.Get(), nullptr, 0);
    context->Draw(UINT(cachedVertices.size()), 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    lastDrawSubmitMs = drawTimer.nsecsElapsed() / 1000000.0;
    // Phase 6E.1: same-device swap-chain presentation bypasses staging readback.
    if (nativePreviewReady && presentNativeTexture()) {
        lastCopyMapMs = 0.0; phase3AsyncPollMs = 0.0; phase3CpuCopyMs = 0.0;
        phase3AsyncFlushMs = 0.0; lastReadbackMs = 0.0;
        nativeFramePresented = true;
        lastRenderMs = totalTimer.nsecsElapsed() / 1000000.0;
        ++successfulFrames; if (background && !background->isNull()) ++compositedFrames;
        averageRenderMs += (lastRenderMs - averageRenderMs) / double(successfulFrames);
        return true;
    }
    if (nativePreviewReady) { nativePreviewReady = false; if (nativePresentError.isEmpty()) nativePresentError = QStringLiteral("Native present failed; Phase 6D fallback active."); }
    // Poll completed Phase 3 readbacks without flushing or waiting for the GPU.
    QElapsedTimer asyncPollTimer; asyncPollTimer.start();
    phase3CpuCopyMs = 0.0;
    for (auto &slot : phase3ReadbackSlots) {
        if (!slot.pending) continue;
        const HRESULT ready = context->GetData(slot.completion.Get(), nullptr, 0,
                                               D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (ready == S_FALSE) continue;
        if (FAILED(ready)) {
            slot.pending = false;
            if (phase3AsyncPending > 0) --phase3AsyncPending;
            renderError = QStringLiteral("Phase 3 async completion query failed: %1").arg(hexResult(ready));
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE read{};
        const HRESULT mapHr = context->Map(slot.staging.Get(), 0, D3D11_MAP_READ,
                                           D3D11_MAP_FLAG_DO_NOT_WAIT, &read);
        if (mapHr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        if (FAILED(mapHr)) {
            slot.pending = false;
            if (phase3AsyncPending > 0) --phase3AsyncPending;
            renderError = QStringLiteral("Phase 3 async readback Map failed: %1").arg(hexResult(mapHr));
            continue;
        }
        QElapsedTimer copyTimer; copyTimer.start();
        QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < height; ++y)
            std::memcpy(image.scanLine(y),
                        static_cast<const char *>(read.pData) + size_t(y) * read.RowPitch,
                        size_t(width) * 4);
        context->Unmap(slot.staging.Get(), 0);
        phase3CpuCopyMs = copyTimer.nsecsElapsed() / 1000000.0;
        cachedPhase3Image = std::move(image);
        slot.pending = false;
        if (phase3AsyncPending > 0) --phase3AsyncPending;
        ++phase3AsyncCompleted;
    }
    phase3AsyncPollMs = asyncPollTimer.nsecsElapsed() / 1000000.0;

    Phase3ReadbackSlot *freeSlot = nullptr;
    for (int offset = 0; offset < 3; ++offset) {
        const int index = (nextPhase3ReadbackSlot + offset) % 3;
        if (!phase3ReadbackSlots[index].pending) {
            freeSlot = &phase3ReadbackSlots[index];
            nextPhase3ReadbackSlot = (index + 1) % 3;
            break;
        }
    }
    if (freeSlot) {
        context->CopyResource(freeSlot->staging.Get(), cachedTexture.Get());
        context->End(freeSlot->completion.Get());
        // An immediate-context event query is not guaranteed to progress while
        // commands remain buffered. Flush submits the copy/query without waiting
        // for GPU completion; GetData below remains non-blocking.
        QElapsedTimer flushTimer; flushTimer.start();
        context->Flush();
        phase3AsyncFlushMs = flushTimer.nsecsElapsed() / 1000000.0;
        freeSlot->pending = true;
        ++phase3AsyncPending;
        ++phase3AsyncQueued;
    } else {
        ++phase3AsyncQueueFull;
        phase3AsyncFlushMs = 0.0;
    }
    lastCopyMapMs = 0.0; // Phase 6D never blocks on the staging texture.
    lastReadbackMs = phase3AsyncPollMs + phase3CpuCopyMs;
    lastRenderMs = totalTimer.nsecsElapsed() / 1000000.0;
    ++successfulFrames;
    if (background && !background->isNull()) ++compositedFrames;
    averageRenderMs += (lastRenderMs - averageRenderMs) / double(successfulFrames);
    if (cachedPhase3Image.isNull()) return false; // Qt fallback until first GPU frame completes.
    *output = cachedPhase3Image;
    return true;
#else
    Q_UNUSED(width); Q_UNUSED(height); Q_UNUSED(rects); Q_UNUSED(colors); Q_UNUSED(output); Q_UNUSED(background);
    renderError = QStringLiteral("Direct3D 11 unavailable on this platform."); return false;
#endif
}

bool GpuRenderer::renderBokehCircles(int width, int height, const QVector<QRectF> &bounds,
                                     const QVector<QColor> &colors, int softness, QImage *output)
{
#ifdef Q_OS_WIN
    QElapsedTimer bokehPathTimer; bokehPathTimer.start();
    renderError.clear();
    if (!output || width < 1 || height < 1 || bounds.isEmpty() || bounds.size() != colors.size() || bounds.size() > 100) {
        renderError = QStringLiteral("Invalid Bokeh dimensions or particle arrays."); return false;
    }
    if (!initialize() || !createPipeline()) return false;

    // Phase 6B: collect completed timestamp samples without blocking the CPU.
    // A four-slot ring keeps query retrieval off the critical readback path.
    if (!bokehGpuTimingQueriesReady) {
        bool allCreated = true;
        for (auto &slot : bokehGpuTimingSlots) {
            D3D11_QUERY_DESC qd{};
            HRESULT qhr = S_OK;
            if (!slot.disjoint) {
                qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
                qhr = device->CreateQuery(&qd, slot.disjoint.GetAddressOf());
                if (FAILED(qhr)) { allCreated = false; break; }
            }
            if (!slot.start) {
                qd.Query = D3D11_QUERY_TIMESTAMP;
                qhr = device->CreateQuery(&qd, slot.start.GetAddressOf());
                if (FAILED(qhr)) { allCreated = false; break; }
            }
            if (!slot.end) {
                qd.Query = D3D11_QUERY_TIMESTAMP;
                qhr = device->CreateQuery(&qd, slot.end.GetAddressOf());
                if (FAILED(qhr)) { allCreated = false; break; }
            }
        }
        bokehGpuTimingQueriesReady = allCreated;
        bokehGpuTimingStatus = allCreated ? QStringLiteral("active (non-blocking)") :
                                             QStringLiteral("query creation failed");
    }
    if (bokehGpuTimingQueriesReady) {
        for (auto &slot : bokehGpuTimingSlots) {
            if (!slot.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
            const HRESULT ready = context->GetData(slot.disjoint.Get(), &disjointData,
                                                    sizeof(disjointData), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (ready == S_OK) {
                UINT64 startTick = 0, endTick = 0;
                const HRESULT startReady = context->GetData(slot.start.Get(), &startTick, sizeof(startTick),
                                                            D3D11_ASYNC_GETDATA_DONOTFLUSH);
                const HRESULT endReady = context->GetData(slot.end.Get(), &endTick, sizeof(endTick),
                                                          D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (startReady == S_OK && endReady == S_OK && !disjointData.Disjoint &&
                    disjointData.Frequency > 0 && endTick >= startTick) {
                    bokehLastGpuMs = double(endTick - startTick) * 1000.0 /
                                     double(disjointData.Frequency);
                    ++bokehGpuSamples;
                    bokehAverageGpuMs += (bokehLastGpuMs - bokehAverageGpuMs) /
                                         double(bokehGpuSamples);
                }
                slot.pending = false;
            } else if (ready != S_FALSE) {
                slot.pending = false;
            }
        }
    }

    if (!bokehVertexShader || !bokehPixelShader || !bokehInputLayout || !bokehVertexBuffer || !bokehConstantBuffer) {
        Microsoft::WRL::ComPtr<ID3DBlob> vs, ps, errors;
        HRESULT hr = D3DCompile(bokehShaderSource, std::strlen(bokehShaderSource), nullptr, nullptr, nullptr,
                                "VSMain", "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, vs.GetAddressOf(), errors.GetAddressOf());
        if (FAILED(hr)) { renderError = QStringLiteral("Bokeh VS compile failed: %1").arg(hexResult(hr)); return false; }
        hr = D3DCompile(bokehShaderSource, std::strlen(bokehShaderSource), nullptr, nullptr, nullptr,
                        "PSMain", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, ps.GetAddressOf(), errors.ReleaseAndGetAddressOf());
        if (FAILED(hr)) { renderError = QStringLiteral("Bokeh PS compile failed: %1").arg(hexResult(hr)); return false; }
        hr = device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, bokehVertexShader.GetAddressOf());
        if (FAILED(hr)) { renderError = QStringLiteral("Bokeh vertex shader creation failed: %1").arg(hexResult(hr)); return false; }
        hr = device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, bokehPixelShader.GetAddressOf());
        if (FAILED(hr)) { renderError = QStringLiteral("Bokeh pixel shader creation failed: %1").arg(hexResult(hr)); return false; }
        const D3D11_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}
        };
        hr = device->CreateInputLayout(layout, ARRAYSIZE(layout), vs->GetBufferPointer(), vs->GetBufferSize(), bokehInputLayout.GetAddressOf());
        if (FAILED(hr)) { renderError = QStringLiteral("Bokeh input layout creation failed: %1").arg(hexResult(hr)); return false; }
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = UINT(sizeof(BokehVertex) * 6 * 100); bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(&bd, nullptr, bokehVertexBuffer.GetAddressOf());
        if (FAILED(hr)) { renderError = QStringLiteral("Bokeh vertex buffer creation failed: %1").arg(hexResult(hr)); return false; }
        D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = 16; cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(&cbd, nullptr, bokehConstantBuffer.GetAddressOf());
        if (FAILED(hr)) { renderError = QStringLiteral("Bokeh constant buffer creation failed: %1").arg(hexResult(hr)); return false; }
    }
    if (!bokehTexture || !bokehRtv || bokehWidth != width || bokehHeight != height) {
        bokehRtv.Reset(); bokehTexture.Reset();
        for (auto &slot : bokehReadbackSlots) {
            slot.staging.Reset();
            slot.completion.Reset();
            slot.pending = false;
        }
        cachedBokehImage = QImage();
        bokehAsyncPending = 0;
        nextBokehReadbackSlot = 0;
        bokehWidth = bokehHeight = 0;
        D3D11_TEXTURE2D_DESC td{}; td.Width = UINT(width); td.Height = UINT(height); td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT hr = device->CreateTexture2D(&td, nullptr, bokehTexture.GetAddressOf());
        if (FAILED(hr)) { renderError = QStringLiteral("Bokeh texture creation failed: %1").arg(hexResult(hr)); return false; }
        hr = device->CreateRenderTargetView(bokehTexture.Get(), nullptr, bokehRtv.GetAddressOf());
        if (FAILED(hr)) { renderError = QStringLiteral("Bokeh RTV creation failed: %1").arg(hexResult(hr)); return false; }
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (int i = 0; i < 3; ++i) {
            hr = device->CreateTexture2D(&td, nullptr, bokehReadbackSlots[i].staging.GetAddressOf());
            if (FAILED(hr)) { renderError = QStringLiteral("Bokeh async staging texture %1 creation failed: %2").arg(i).arg(hexResult(hr)); return false; }
            D3D11_QUERY_DESC qd{}; qd.Query = D3D11_QUERY_EVENT;
            hr = device->CreateQuery(&qd, bokehReadbackSlots[i].completion.GetAddressOf());
            if (FAILED(hr)) { renderError = QStringLiteral("Bokeh async event query %1 creation failed: %2").arg(i).arg(hexResult(hr)); return false; }
        }
        bokehWidth = width; bokehHeight = height;
    }
    cachedBokehVertices.clear();
    if (cachedBokehVertices.capacity() < bounds.size() * 6) cachedBokehVertices.reserve(bounds.size() * 6);
    auto vertex = [&](float x, float y, float u, float v, const QColor &col) {
        cachedBokehVertices.push_back({2.0f*x/float(width)-1.0f, 1.0f-2.0f*y/float(height), u, v,
                                       float(col.redF()), float(col.greenF()), float(col.blueF()), float(col.alphaF())});
    };
    for (int i=0; i<bounds.size(); ++i) {
        const QRectF r = bounds[i].normalized(); const QColor col = colors[i];
        const float x1=float(r.left()), y1=float(r.top()), x2=float(r.right()), y2=float(r.bottom());
        vertex(x1,y1,0,0,col); vertex(x2,y1,1,0,col); vertex(x1,y2,0,1,col);
        vertex(x1,y2,0,1,col); vertex(x2,y1,1,0,col); vertex(x2,y2,1,1,col);
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context->Map(bokehVertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) { renderError = QStringLiteral("Bokeh vertex buffer map failed: %1").arg(hexResult(hr)); return false; }
    std::memcpy(mapped.pData, cachedBokehVertices.constData(), size_t(cachedBokehVertices.size()) * sizeof(BokehVertex));
    context->Unmap(bokehVertexBuffer.Get(), 0);
    const float clear[4] = {0,0,0,0}; context->ClearRenderTargetView(bokehRtv.Get(), clear);
    D3D11_VIEWPORT vp{}; vp.Width=float(width); vp.Height=float(height); vp.MinDepth=0; vp.MaxDepth=1;
    context->RSSetViewports(1, &vp); ID3D11RenderTargetView *rtv = bokehRtv.Get(); context->OMSetRenderTargets(1, &rtv, nullptr);
    const float blendFactor[4] = {0,0,0,0}; context->OMSetBlendState(blendState.Get(), blendFactor, 0xffffffff);
    UINT stride=sizeof(BokehVertex), offset=0; ID3D11Buffer *vb=bokehVertexBuffer.Get();
    context->IASetInputLayout(bokehInputLayout.Get()); context->IASetVertexBuffers(0,1,&vb,&stride,&offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(bokehVertexShader.Get(), nullptr, 0); context->PSSetShader(bokehPixelShader.Get(), nullptr, 0);
    D3D11_MAPPED_SUBRESOURCE params{};
    HRESULT paramHr = context->Map(bokehConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &params);
    if (FAILED(paramHr)) { renderError = QStringLiteral("Bokeh parameter map failed: %1").arg(hexResult(paramHr)); return false; }
    struct BokehParams { float softness; float padding[3]; } paramsData{};
    paramsData.softness = qBound(0, softness, 100) / 100.0f;
    std::memcpy(params.pData, &paramsData, sizeof(paramsData));
    context->Unmap(bokehConstantBuffer.Get(), 0);
    ID3D11Buffer *cb = bokehConstantBuffer.Get(); context->PSSetConstantBuffers(0, 1, &cb);
    // Timestamp queries bracket only the GPU draw. Results are retrieved
    // asynchronously on later frames, without waiting in this frame.
    BokehGpuTimingSlot *timingSlot = nullptr;
    if (bokehGpuTimingQueriesReady) {
        for (int offset = 0; offset < 4; ++offset) {
            const int index = (nextBokehGpuTimingSlot + offset) % 4;
            if (!bokehGpuTimingSlots[index].pending) {
                timingSlot = &bokehGpuTimingSlots[index];
                nextBokehGpuTimingSlot = (index + 1) % 4;
                break;
            }
        }
    }
    if (timingSlot) context->Begin(timingSlot->disjoint.Get());
    if (timingSlot) context->End(timingSlot->start.Get());
    QElapsedTimer bokehDrawTimer; bokehDrawTimer.start();
    context->Draw(UINT(cachedBokehVertices.size()),0);
    if (timingSlot) context->End(timingSlot->end.Get());
    context->OMSetRenderTargets(0,nullptr,nullptr);
    if (timingSlot) {
        context->End(timingSlot->disjoint.Get());
        timingSlot->pending = true;
    }
    bokehLastDrawSubmitMs = bokehDrawTimer.nsecsElapsed() / 1000000.0;
    // Phase 6C: poll completed staging copies without waiting, then enqueue
    // this frame into a free slot. The previous completed image remains visible
    // while newer GPU copies are still in flight.
    QElapsedTimer asyncPollTimer; asyncPollTimer.start();
    bokehLastCpuCopyMs = 0.0;
    for (auto &slot : bokehReadbackSlots) {
        if (!slot.pending) continue;
        const HRESULT ready = context->GetData(slot.completion.Get(), nullptr, 0,
                                               D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (ready == S_FALSE) continue;
        if (FAILED(ready)) {
            slot.pending = false;
            if (bokehAsyncPending > 0) --bokehAsyncPending;
            renderError = QStringLiteral("Bokeh async completion query failed: %1").arg(hexResult(ready));
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE read{};
        const HRESULT mapHr = context->Map(slot.staging.Get(), 0, D3D11_MAP_READ,
                                           D3D11_MAP_FLAG_DO_NOT_WAIT, &read);
        if (mapHr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        if (FAILED(mapHr)) {
            slot.pending = false;
            if (bokehAsyncPending > 0) --bokehAsyncPending;
            renderError = QStringLiteral("Bokeh async readback map failed: %1").arg(hexResult(mapHr));
            continue;
        }
        QElapsedTimer cpuCopyTimer; cpuCopyTimer.start();
        QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < height; ++y)
            std::memcpy(image.scanLine(y),
                        static_cast<const char *>(read.pData) + size_t(y) * read.RowPitch,
                        size_t(width) * 4);
        context->Unmap(slot.staging.Get(), 0);
        bokehLastCpuCopyMs = cpuCopyTimer.nsecsElapsed() / 1000000.0;
        cachedBokehImage = std::move(image);
        slot.pending = false;
        if (bokehAsyncPending > 0) --bokehAsyncPending;
        ++bokehAsyncCompleted;
    }
    bokehAsyncPollMs = asyncPollTimer.nsecsElapsed() / 1000000.0;

    BokehReadbackSlot *freeSlot = nullptr;
    for (int offset = 0; offset < 3; ++offset) {
        const int index = (nextBokehReadbackSlot + offset) % 3;
        if (!bokehReadbackSlots[index].pending) {
            freeSlot = &bokehReadbackSlots[index];
            nextBokehReadbackSlot = (index + 1) % 3;
            break;
        }
    }
    if (freeSlot) {
        context->CopyResource(freeSlot->staging.Get(), bokehTexture.Get());
        context->End(freeSlot->completion.Get());
        // Submit the copy and event query so DONOTFLUSH polling can observe
        // completion. Flush is asynchronous and does not wait for the GPU.
        QElapsedTimer flushTimer; flushTimer.start();
        context->Flush();
        bokehAsyncFlushMs = flushTimer.nsecsElapsed() / 1000000.0;
        freeSlot->pending = true;
        ++bokehAsyncPending;
        ++bokehAsyncQueued;
    } else {
        ++bokehAsyncQueueFull;
        bokehAsyncFlushMs = 0.0;
    }
    bokehLastCopyMapWaitMs = 0.0; // No blocking Map wait in the asynchronous path.
    bokehLastReadbackMs = bokehAsyncPollMs + bokehLastCpuCopyMs;
    bokehLastPathMs = bokehPathTimer.nsecsElapsed() / 1000000.0;
    bokehAveragePathMs += (bokehLastPathMs - bokehAveragePathMs) / double(bokehFrames + 1);
    ++successfulFrames; ++bokehFrames;
    if (cachedBokehImage.isNull()) return false; // Keep Qt fallback until first GPU result arrives.
    *output = cachedBokehImage;
    return true;
#else
    Q_UNUSED(width); Q_UNUSED(height); Q_UNUSED(bounds); Q_UNUSED(colors); Q_UNUSED(softness); Q_UNUSED(output);
    renderError = QStringLiteral("Direct3D 11 unavailable on this platform."); return false;
#endif
}
bool GpuRenderer::isAvailable() const { return available; }
bool GpuRenderer::isHardwareAccelerated() const { return available && hardware; }
QString GpuRenderer::lastRenderError() const { return renderError; }
QString GpuRenderer::runtimeStats() const
{
    return QStringLiteral("D3D11 frame path: %1 | GPU frames: %2 | fallback/errors: %3\n"
                          "Last GPU path: %4 ms | readback incl. copy: %5 ms | average path: %6 ms\n"
                          "Phase 3 resources: rebuilt %7 | reused %8 | cache: %9 x %10\n"
                          "Last resource lookup/create: %11 ms | draw submission: %12 ms | copy+map wait: %13 ms\n"
                          "Phase 4A composite frames: %14 | last video upload: %15 ms\n"
                          "Phase 6E.1 native presentation: %41 | frames %42 | CPU present %43 ms | %44\n"
                          "Phase 5 D3D11 Bokeh radial-shader frames: %17\n"
                          "Phase 6A Bokeh CPU path: last %18 ms | average %19 ms | draw submit %20 ms | readback %21 ms\n"
                          "Phase 6B GPU draw: last %22 ms | average %23 ms | samples %24 | %25\n"
                          "Phase 6C readback: blocking wait %26 ms | CPU memcpy last %27 ms\n"
                          "Phase 6C async readback: queued %28 | completed %29 | pending %30 | queue-full %31 | poll %32 ms | flush %39 ms\n"
                          "Phase 6D Spectrum async readback: queued %33 | completed %34 | pending %35 | queue-full %36 | poll %37 ms | CPU memcpy %38 ms | flush %40 ms\n"
                          "Timing note: Phase 6C Bokeh and Phase 6D Spectrum use independent 3-slot staging rings; both may display a few frames behind.\n"
                          "Last error: %16")
        .arg(successfulFrames > 0 ? QStringLiteral("ACTIVE") : QStringLiteral("not yet successful"))
        .arg(successfulFrames).arg(failedFrames)
        .arg(lastRenderMs, 0, 'f', 2).arg(lastReadbackMs, 0, 'f', 2)
        .arg(averageRenderMs, 0, 'f', 2)
        .arg(resourceRebuilds).arg(resourceReuses)
        .arg(cachedWidth).arg(cachedHeight)
        .arg(lastResourceMs, 0, 'f', 3)
        .arg(lastDrawSubmitMs, 0, 'f', 3)
        .arg(lastCopyMapMs, 0, 'f', 3)
        .arg(compositedFrames).arg(lastUploadMs, 0, 'f', 3)
        .arg(renderError.isEmpty() ? QStringLiteral("none") : renderError)
        .arg(bokehFrames)
        .arg(bokehLastPathMs, 0, 'f', 2)
        .arg(bokehAveragePathMs, 0, 'f', 2)
        .arg(bokehLastDrawSubmitMs, 0, 'f', 3)
        .arg(bokehLastReadbackMs, 0, 'f', 2)
        .arg(bokehLastGpuMs, 0, 'f', 3)
        .arg(bokehAverageGpuMs, 0, 'f', 3)
        .arg(bokehGpuSamples)
        .arg(bokehGpuTimingStatus)
        .arg(bokehLastCopyMapWaitMs, 0, 'f', 3)
        .arg(bokehLastCpuCopyMs, 0, 'f', 3)
        .arg(bokehAsyncQueued)
        .arg(bokehAsyncCompleted)
        .arg(bokehAsyncPending)
        .arg(bokehAsyncQueueFull)
        .arg(bokehAsyncPollMs, 0, 'f', 3)
        .arg(phase3AsyncQueued)
        .arg(phase3AsyncCompleted)
        .arg(phase3AsyncPending)
        .arg(phase3AsyncQueueFull)
        .arg(phase3AsyncPollMs, 0, 'f', 3)
        .arg(phase3CpuCopyMs, 0, 'f', 3)
        .arg(bokehAsyncFlushMs, 0, 'f', 3)
        .arg(phase3AsyncFlushMs, 0, 'f', 3)
        .arg(nativeFramePresented ? QStringLiteral("ACTIVE") : QStringLiteral("fallback/off"))
        .arg(nativePresentFrames)
        .arg(nativePresentCpuMs, 0, 'f', 3)
        .arg(nativePresentError.isEmpty() ? QStringLiteral("none") : nativePresentError)
        + QStringLiteral("\nPhase 6F.10 native present calls: attempts %1 | success %2 | failures %3 | CPU last/avg/max %4 / %5 / %6 ms (render submission + DXGI Present)")
            .arg(nativePresentAttempts).arg(nativePresentFrames).arg(nativePresentFailures)
            .arg(nativePresentCpuMs, 0, 'f', 3)
            .arg(nativePresentCpuAverageMs, 0, 'f', 3)
            .arg(nativePresentCpuMaxMs, 0, 'f', 3)
        + QStringLiteral("\nGlow D3D11 shader: draws %1 | GPU last/avg/max %2 / %3 / %4 ms (%5 samples; %6) | CPU submit last/avg %7 / %8 ms (%9 samples)")
            .arg(glowDraws).arg(glowGpuLastMs,0,'f',4).arg(glowGpuAverageMs,0,'f',4)
            .arg(glowGpuMaxMs,0,'f',4).arg(glowGpuSamples).arg(glowGpuTimingStatus)
            .arg(glowCpuSubmitLastMs,0,'f',4).arg(glowCpuSubmitAverageMs,0,'f',4).arg(glowCpuSubmitSamples);
}

QString GpuRenderer::diagnostics() const
{
    if (!attempted) return QStringLiteral("Direct3D 11 has not been initialized.");
    if (!available) return QStringLiteral("Direct3D 11 unavailable: %1\nCurrent CPU preview remains active.").arg(errorMessage);
    return QStringLiteral("Direct3D 11 device: READY\nAdapter: %1\nFeature level: %2\nDriver: %3\nGPU Spectrum bar renderer: %4\n\n"
                          "Phase 4A uploads CPU-prepared video to a cached D3D11 render target. Phase 6E.1 can present the composited target through a same-device DXGI swap chain without staging readback; Qt video conversion and upload still remain.")
        .arg(adapterName, featureLevelName, hardware ? QStringLiteral("Hardware GPU") : QStringLiteral("WARP software fallback"),
             pipelineReady ? QStringLiteral("READY") : QStringLiteral("not ready: %1").arg(renderError))
        + QStringLiteral("\n\nRuntime rendering statistics:\n") + runtimeStats();
}
