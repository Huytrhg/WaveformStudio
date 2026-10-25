#include "nativegpupresenter.h"

#include <QElapsedTimer>
#include <QDebug>\n#include <QPainter>

#ifdef Q_OS_WIN
#include <algorithm>
#endif

NativeGpuPresenter::~NativeGpuPresenter()
{
    shutdown();
}

bool NativeGpuPresenter::initialize(void *nativeWindow, int width, int height)
{
    shutdown();
#ifdef Q_OS_WIN
    if (!nativeWindow || width < 1 || height < 1) {
        m_error = QStringLiteral("Native preview needs a valid HWND and positive dimensions.");
        return false;
    }

    m_nativeWindow = nativeWindow;
    m_size = QSize(width, height);

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL selected = D3D_FEATURE_LEVEL_10_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION, m_device.GetAddressOf(), &selected, m_context.GetAddressOf());

    // Older Windows runtimes can reject the 11.1 feature-level enum.
    if (hr == E_INVALIDARG) {
        m_device.Reset();
        m_context.Reset();
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, ARRAYSIZE(levels) - 1,
            D3D11_SDK_VERSION, m_device.GetAddressOf(), &selected, m_context.GetAddressOf());
    }
    if (FAILED(hr)) {
        m_error = QStringLiteral("D3D11CreateDevice failed (0x%1).")
            .arg(quint32(hr), 8, 16, QLatin1Char('0'));
        shutdown();
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(m_device.As(&dxgiDevice)) &&
        SUCCEEDED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))) {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc)))
            m_adapterName = QString::fromWCharArray(desc.Description);
    }

    Microsoft::WRL::ComPtr<IDXGIFactory> factory;
    if (!adapter || FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())))) {
        m_error = QStringLiteral("Could not obtain DXGI adapter/factory.");
        shutdown();
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = UINT(width);
    desc.BufferDesc.Height = UINT(height);
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 0;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    // DISCARD is broadly compatible with HWND-based Qt child windows.
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    desc.OutputWindow = static_cast<HWND>(nativeWindow);
    desc.Windowed = TRUE;
    desc.Flags = 0;

    hr = factory->CreateSwapChain(m_device.Get(), &desc, m_swapChain.GetAddressOf());
    if (FAILED(hr)) {
        m_error = QStringLiteral("CreateSwapChain failed (0x%1).")
            .arg(quint32(hr), 8, 16, QLatin1Char('0'));
        shutdown();
        return false;
    }
    factory->MakeWindowAssociation(static_cast<HWND>(nativeWindow), DXGI_MWA_NO_ALT_ENTER);

    if (!createBackBuffer()) {
        shutdown();
        return false;
    }
    m_ready = true;
    m_error.clear();
    return true;
#else
    Q_UNUSED(nativeWindow);
    Q_UNUSED(width);
    Q_UNUSED(height);
    m_error = QStringLiteral("Native Direct3D 11 presentation is only available on Windows.");
    return false;
#endif
}

#ifdef Q_OS_WIN
bool NativeGpuPresenter::createBackBuffer()
{
    m_renderTarget.Reset();
    m_uploadTexture.Reset();

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if (FAILED(hr)) {
        m_error = QStringLiteral("Swap-chain GetBuffer failed (0x%1).")
            .arg(quint32(hr), 8, 16, QLatin1Char('0'));
        return false;
    }
    hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, m_renderTarget.GetAddressOf());
    if (FAILED(hr)) {
        m_error = QStringLiteral("CreateRenderTargetView failed (0x%1).")
            .arg(quint32(hr), 8, 16, QLatin1Char('0'));
        return false;
    }

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = UINT(m_size.width());
    textureDesc.Height = UINT(m_size.height());
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = m_device->CreateTexture2D(&textureDesc, nullptr, m_uploadTexture.GetAddressOf());
    if (FAILED(hr)) {
        m_error = QStringLiteral("Create upload texture failed (0x%1).")
            .arg(quint32(hr), 8, 16, QLatin1Char('0'));
        return false;
    }
    return true;
}
#endif

bool NativeGpuPresenter::resize(int width, int height)
{
#ifdef Q_OS_WIN
    if (!m_ready || !m_swapChain || width < 1 || height < 1)
        return false;
    if (m_size == QSize(width, height))
        return true;

    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_renderTarget.Reset();
    m_uploadTexture.Reset();
    HRESULT hr = m_swapChain->ResizeBuffers(0, UINT(width), UINT(height),
                                             DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        m_error = QStringLiteral("ResizeBuffers failed (0x%1).")
            .arg(quint32(hr), 8, 16, QLatin1Char('0'));
        return false;
    }
    m_size = QSize(width, height);
    if (!createBackBuffer())
        return false;
    m_error.clear();
    return true;
#else
    Q_UNUSED(width);
    Q_UNUSED(height);
    return false;
#endif
}

bool NativeGpuPresenter::presentImage(const QImage &frame)
{
#ifdef Q_OS_WIN
    if (!m_ready || frame.isNull()) {
        m_error = QStringLiteral("Native presenter is not initialized or frame is empty.");
        return false;
    }
    // The swap chain follows the native preview widget's client size, not
    // the decoded video's source size. CopyResource requires matching texture
    // dimensions, so fit the frame into a widget-sized BGRA image before upload.
    // This keeps the HWND/swap-chain size stable when source video dimensions
    // differ from the preview panel.
    QElapsedTimer timer;
    timer.start();
    QImage fitted(m_size, QImage::Format_ARGB32);
    fitted.fill(Qt::black);
    const QImage source = frame.convertToFormat(QImage::Format_ARGB32);
    const QSize drawSize = source.size().scaled(m_size, Qt::KeepAspectRatio);
    const QRect drawRect((m_size.width() - drawSize.width()) / 2,
                         (m_size.height() - drawSize.height()) / 2,
                         drawSize.width(), drawSize.height());
    {
        QPainter painter(&fitted);
        painter.drawImage(drawRect, source);
    }
    // The only CPU->GPU transfer is this upload. There is deliberately no
    // staging texture, Map(READ), or GPU->CPU readback in this presentation path.
    m_context->UpdateSubresource(m_uploadTexture.Get(), 0, nullptr,
        fitted.constBits(), UINT(fitted.bytesPerLine()), 0);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if (FAILED(hr)) {
        m_error = QStringLiteral("Swap-chain GetBuffer failed during present (0x%1).")
            .arg(quint32(hr), 8, 16, QLatin1Char('0'));
        return false;
    }
    m_context->CopyResource(backBuffer.Get(), m_uploadTexture.Get());
    hr = m_swapChain->Present(0, 0);
    m_lastPresentCpuMs = timer.nsecsElapsed() / 1000000.0;
    if (FAILED(hr)) {
        m_error = QStringLiteral("Present failed (0x%1).")
            .arg(quint32(hr), 8, 16, QLatin1Char('0'));
        return false;
    }
    ++m_presentedFrames;
    m_error.clear();
    return true;
#else
    Q_UNUSED(frame);
    return false;
#endif
}

void NativeGpuPresenter::shutdown()
{
#ifdef Q_OS_WIN
    if (m_context)
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_renderTarget.Reset();
    m_uploadTexture.Reset();
    m_swapChain.Reset();
    m_context.Reset();
    m_device.Reset();
#endif
    m_ready = false;
    m_nativeWindow = nullptr;
    m_size = QSize();
}

bool NativeGpuPresenter::isReady() const { return m_ready; }

QString NativeGpuPresenter::diagnostics() const
{
#ifdef Q_OS_WIN
    return QStringLiteral("Native D3D11 Swap Chain: %1 | Adapter: %2 | Frames presented: %3 | Last CPU present path: %4 ms | Readback: none")
        .arg(m_ready ? QStringLiteral("READY") : QStringLiteral("NOT READY"),
             m_adapterName.isEmpty() ? QStringLiteral("unknown") : m_adapterName)
        .arg(m_presentedFrames)
        .arg(m_lastPresentCpuMs, 0, 'f', 3);
#else
    return QStringLiteral("Native D3D11 Swap Chain: Windows only.");
#endif
}

QString NativeGpuPresenter::lastError() const { return m_error; }
