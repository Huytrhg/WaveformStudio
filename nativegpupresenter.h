#ifndef NATIVEGPUPRESENTER_H
#define NATIVEGPUPRESENTER_H

#include <QImage>
#include <QString>
#include <QSize>
#include <QVector>
#include <QRectF>
#include <QColor>

#ifdef Q_OS_WIN
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <windows.h>
#endif

// Phase 4B: Direct3D 11 swap-chain presenter.
// This class is deliberately separate from GpuRenderer's Phase 4A off-screen
// path so native presentation can be enabled/fallback-tested independently.
class NativeGpuPresenter final
{
public:
    NativeGpuPresenter() = default;
    ~NativeGpuPresenter();

    NativeGpuPresenter(const NativeGpuPresenter &) = delete;
    NativeGpuPresenter &operator=(const NativeGpuPresenter &) = delete;

    bool initialize(void *nativeWindow, int width, int height);
    bool resize(int width, int height);
    bool presentImage(const QImage &frame);
    void shutdown();

    bool isReady() const;
    QString diagnostics() const;
    QString lastError() const;
    quint64 presentedFrames() const { return m_presentedFrames; }
    double lastPresentCpuMs() const { return m_lastPresentCpuMs; }

private:
#ifdef Q_OS_WIN
    bool createBackBuffer();
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTarget;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_uploadTexture;
#endif
    void *m_nativeWindow = nullptr;
    QSize m_size;
    QString m_error;
    QString m_adapterName;
    quint64 m_presentedFrames = 0;
    double m_lastPresentCpuMs = 0.0;
    bool m_ready = false;
};

#endif // NATIVEGPUPRESENTER_H
