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
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <windows.h>
#endif

// Phase 4B.1: native D3D11 presenter. It can composite a video texture and
// colored spectrum rectangles into the swap-chain back buffer without readback.
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
    bool presentFrame(const QImage &frame, const QVector<QRectF> &rects,
                      const QVector<QColor> &colors, qreal opacity = 1.0);
    void shutdown();

    bool isReady() const;
    QString diagnostics() const;
    QString lastError() const;
    quint64 presentedFrames() const { return m_presentedFrames; }
    double lastPresentCpuMs() const { return m_lastPresentCpuMs; }

private:
#ifdef Q_OS_WIN
    bool createBackBuffer();
    bool createPipeline();
    struct Vertex { float x, y, u, v, r, g, b, a; };
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTarget;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_uploadTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_videoView;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_textureShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_colorShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendState;
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
