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
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = UINT(sizeof(Vertex) * 6 * 128 * 3); bd.Usage = D3D11_USAGE_DYNAMIC;
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
    pipelineReady = true; renderError.clear(); return true;
#else
    return false;
#endif
}

bool GpuRenderer::renderColoredRects(int width, int height, const QVector<QRectF> &rects,
                                     const QVector<QColor> &colors, QImage *output, const QImage *background)
{
#ifdef Q_OS_WIN
    QElapsedTimer totalTimer; totalTimer.start();
    renderError.clear();
    if (!output || width < 1 || height < 1 || rects.isEmpty() || rects.size() != colors.size()) {
        ++failedFrames;
        renderError = QStringLiteral("Invalid render dimensions or rectangle/color arrays."); return false;
    }
    if (!initialize() || !createPipeline()) { ++failedFrames; return false; }
    if (rects.size() > 128 * 3) { ++failedFrames; renderError = QStringLiteral("Too many rectangles for current vertex buffer."); return false; }
    // Phase 3: retain the render target, view and staging texture across frames.
    // Rebuild only when the preview size changes. No cross-frame GPU readback is queued.
    QElapsedTimer resourceTimer; resourceTimer.start();
    if (!cachedTexture || !cachedRtv || !cachedStaging || cachedWidth != width || cachedHeight != height) {
        cachedRtv.Reset(); cachedStaging.Reset(); cachedTexture.Reset();
        cachedWidth = cachedHeight = 0;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(width); td.Height = UINT(height); td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
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
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        createHr = device->CreateTexture2D(&sd, nullptr, cachedStaging.GetAddressOf());
        if (FAILED(createHr)) {
            cachedRtv.Reset(); cachedTexture.Reset(); ++failedFrames;
            renderError = QStringLiteral("Create staging texture failed: %1").arg(hexResult(createHr));
            return false;
        }
        cachedWidth = width; cachedHeight = height;
        ++resourceRebuilds;
    } else {
        ++resourceReuses;
    }
    lastResourceMs = resourceTimer.nsecsElapsed() / 1000000.0;
    HRESULT hr = S_OK;

    QVector<Vertex> vertices; vertices.reserve(rects.size() * 6);
    auto addVertex = [&](float x, float y, const QColor &c) {
        vertices.push_back({2.0f * x / float(width) - 1.0f, 1.0f - 2.0f * y / float(height),
                            float(c.redF()), float(c.greenF()), float(c.blueF()), float(c.alphaF())});
    };
    for (int i = 0; i < rects.size(); ++i) {
        const QRectF r = rects[i].normalized(); const QColor c = colors[i];
        const float x1 = float(r.left()), y1 = float(r.top()), x2 = float(r.right()), y2 = float(r.bottom());
        addVertex(x1,y1,c); addVertex(x2,y1,c); addVertex(x1,y2,c);
        addVertex(x1,y2,c); addVertex(x2,y1,c); addVertex(x2,y2,c);
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) { ++failedFrames; renderError = QStringLiteral("Map vertex buffer failed: %1").arg(hexResult(hr)); return false; }
    std::memcpy(mapped.pData, vertices.constData(), size_t(vertices.size()) * sizeof(Vertex));
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
    const float blendFactor[4] = {0,0,0,0}; context->OMSetBlendState(blendState.Get(), blendFactor, 0xffffffff);
    UINT stride = sizeof(Vertex), offset = 0; ID3D11Buffer *vb = vertexBuffer.Get();
    context->IASetInputLayout(inputLayout.Get()); context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertexShader.Get(), nullptr, 0); context->PSSetShader(pixelShader.Get(), nullptr, 0);
    context->Draw(UINT(vertices.size()), 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    lastDrawSubmitMs = drawTimer.nsecsElapsed() / 1000000.0;
    QElapsedTimer readbackTimer; readbackTimer.start();
    context->CopyResource(cachedStaging.Get(), cachedTexture.Get());
    D3D11_MAPPED_SUBRESOURCE read{};
    hr = context->Map(cachedStaging.Get(), 0, D3D11_MAP_READ, 0, &read);
    if (FAILED(hr)) { ++failedFrames; renderError = QStringLiteral("Readback map failed: %1").arg(hexResult(hr)); return false; }
    lastCopyMapMs = readbackTimer.nsecsElapsed() / 1000000.0;
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < height; ++y)
        std::memcpy(image.scanLine(y), static_cast<const char *>(read.pData) + size_t(y) * read.RowPitch, size_t(width) * 4);
    context->Unmap(cachedStaging.Get(), 0);
    lastReadbackMs = readbackTimer.nsecsElapsed() / 1000000.0;
    lastRenderMs = totalTimer.nsecsElapsed() / 1000000.0;
    ++successfulFrames;
    if (background && !background->isNull()) ++compositedFrames;
    averageRenderMs += (lastRenderMs - averageRenderMs) / double(successfulFrames);
    *output = std::move(image);
    return true;
#else
    Q_UNUSED(width); Q_UNUSED(height); Q_UNUSED(rects); Q_UNUSED(colors); Q_UNUSED(output); Q_UNUSED(background);
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
                          "Timing note: draw submission is CPU-side; it is NOT GPU execution time.\n"
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
        .arg(renderError.isEmpty() ? QStringLiteral("none") : renderError);
}

QString GpuRenderer::diagnostics() const
{
    if (!attempted) return QStringLiteral("Direct3D 11 has not been initialized.");
    if (!available) return QStringLiteral("Direct3D 11 unavailable: %1\nCurrent CPU preview remains active.").arg(errorMessage);
    return QStringLiteral("Direct3D 11 device: READY\nAdapter: %1\nFeature level: %2\nDriver: %3\nGPU Spectrum bar renderer: %4\n\n"
                          "Phase 4A hybrid: video is uploaded to a cached D3D11 render target and Spectrum is composited on GPU. Final readback to Qt still remains; this is NOT zero-copy presentation.")
        .arg(adapterName, featureLevelName, hardware ? QStringLiteral("Hardware GPU") : QStringLiteral("WARP software fallback"),
             pipelineReady ? QStringLiteral("READY") : QStringLiteral("not ready: %1").arg(renderError))
        + QStringLiteral("\n\nRuntime rendering statistics:\n") + runtimeStats();
}
