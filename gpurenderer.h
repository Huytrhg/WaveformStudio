#ifndef GPURENDERER_H
#define GPURENDERER_H

#include <QString>
#include <QImage>
#include <QRectF>
#include <QColor>
#include <QVector>

#ifdef Q_OS_WIN
#include <d3d11.h>
#include <wrl/client.h>
#endif

// Phase 2: D3D11 off-screen renderer for colored Spectrum bar rectangles.
// QPainter remains the fallback and continues to render waveform/circular modes.
class GpuRenderer final
{
public:
    static GpuRenderer &instance();
    bool initialize();
    bool isAvailable() const;
    bool isHardwareAccelerated() const;
    QString diagnostics() const;
    bool renderColoredRects(int width, int height,
                            const QVector<QRectF> &rects,
                            const QVector<QColor> &colors,
                            QImage *output, const QImage *background = nullptr,
                            const QVector<QColor> *topColors = nullptr);
    bool renderBokehCircles(int width, int height,
                            const QVector<QRectF> &bounds,
                            const QVector<QColor> &colors,
                            int softness, QImage *output);
    QString lastRenderError() const;
    QString runtimeStats() const;

private:
    GpuRenderer() = default;
    GpuRenderer(const GpuRenderer &) = delete;
    GpuRenderer &operator=(const GpuRenderer &) = delete;
    bool createPipeline();

    bool attempted = false;
    bool available = false;
    bool hardware = false;
    bool pipelineReady = false;
    QString adapterName;
    QString errorMessage;
    QString featureLevelName;
    QString renderError;
    quint64 successfulFrames = 0;
    quint64 failedFrames = 0;
    double lastRenderMs = 0.0;
    double lastReadbackMs = 0.0;
    double averageRenderMs = 0.0;
    double lastResourceMs = 0.0;
    double lastDrawSubmitMs = 0.0;
    double lastCopyMapMs = 0.0;
    quint64 resourceRebuilds = 0;
    quint64 resourceReuses = 0;
    quint64 compositedFrames = 0;
    double lastUploadMs = 0.0;
    int cachedWidth = 0;
    int cachedHeight = 0;

#ifdef Q_OS_WIN
    struct Vertex { float x, y, r, g, b, a; };
    // Reuse CPU-side vertex storage across frames to avoid repeated allocations.
    QVector<Vertex> cachedVertices;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cachedTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> cachedRtv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cachedStaging;
    struct BokehVertex { float x, y, u, v, r, g, b, a; };
    Microsoft::WRL::ComPtr<ID3D11VertexShader> bokehVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> bokehPixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> bokehInputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> bokehVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> bokehTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> bokehRtv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> bokehStaging;
    int bokehWidth = 0, bokehHeight = 0;
    QVector<BokehVertex> cachedBokehVertices;
#endif
};

#endif // GPURENDERER_H
