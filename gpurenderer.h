#ifndef GPURENDERER_H
#define GPURENDERER_H

#include <QString>
#include <QImage>
#include <QRectF>
#include <QColor>
#include <QVector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#endif

// GPU Glow parameters are sampled from UI settings once per preview frame.
struct GpuGlowParams {
    float colors[4][4] = {};
    float stops[4] = {0.0f, 0.33f, 0.66f, 1.0f};
    float strength = 0.0f, holeRadius = 0.0f, outerRadius = 0.0f, phase = 0.0f;
    float width = 0.0f, height = 0.0f, padding[2] = {};
};

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
                            const QVector<QColor> *topColors = nullptr,
                            const GpuGlowParams *glow = nullptr);
    bool renderBokehCircles(int width, int height,
                            const QVector<QRectF> &bounds,
                            const QVector<QColor> &colors,
                            int softness, QImage *output);
    QString lastRenderError() const;
    QString runtimeStats() const;
    bool configureNativePresentation(void *nativeWindow, int width, int height);
    void disableNativePresentation();
    bool nativePresentationActive() const;
    QString nativePresentationError() const;

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
    quint64 bokehFrames = 0;
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
    double lastUploadConvertMs = 0.0, lastUploadSubmitMs = 0.0;
    QVector<double> uploadConvertTimingWindow, uploadSubmitTimingWindow;
    QVector<double> drawSubmitTimingWindow, nativeCopySubmitTimingWindow, nativePresentTimingWindow;
    QVector<double> glowCpuSubmitTimingWindow, spectrumCpuSubmitTimingWindow;
    int uploadConvertTimingWriteIndex = 0, uploadSubmitTimingWriteIndex = 0;
    int drawSubmitTimingWriteIndex = 0, nativeCopySubmitTimingWriteIndex = 0;
    int nativePresentTimingWriteIndex = 0;
    int glowCpuSubmitTimingWriteIndex = 0, spectrumCpuSubmitTimingWriteIndex = 0;
    // Phase 6D: asynchronous Phase 3 preview readback diagnostics.
    double phase3AsyncPollMs = 0.0;
    double phase3AsyncFlushMs = 0.0;
    double phase3CpuCopyMs = 0.0;
    quint64 phase3AsyncQueued = 0;
    quint64 phase3AsyncCompleted = 0;
    quint64 phase3AsyncQueueFull = 0;
    int phase3AsyncPending = 0;
    // Phase 6A: Bokeh CPU-side submission and synchronous readback diagnostics.
    double bokehLastPathMs = 0.0;
    double bokehAveragePathMs = 0.0;
    double bokehLastDrawSubmitMs = 0.0;
    double bokehLastReadbackMs = 0.0;
    double bokehLastCopyMapWaitMs = 0.0;
    double bokehLastCpuCopyMs = 0.0;
    double bokehAsyncPollMs = 0.0;
    double bokehAsyncFlushMs = 0.0;
    quint64 bokehAsyncQueued = 0;
    quint64 bokehAsyncCompleted = 0;
    quint64 bokehAsyncQueueFull = 0;
    int bokehAsyncPending = 0;
    double bokehLastGpuMs = 0.0;
    double bokehAverageGpuMs = 0.0;
    quint64 bokehGpuSamples = 0;
    QString bokehGpuTimingStatus = QStringLiteral("initializing");
    int cachedWidth = 0;
    int cachedHeight = 0;
    quint64 nativePresentFrames = 0;
    quint64 nativePresentAttempts = 0;
    quint64 nativePresentFailures = 0;
    double nativePresentCpuMs = 0.0;
    double nativePresentCpuAverageMs = 0.0;
    double nativePresentCpuMaxMs = 0.0;
    double nativeCopySubmitMs = 0.0, nativeDxgiPresentMs = 0.0;
    QString nativePresentError;

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
    Microsoft::WRL::ComPtr<ID3D11BlendState> glowBlendState;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> glowVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> glowPixelShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> glowConstantBuffer;
    double glowGpuLastMs = 0.0, glowGpuAverageMs = 0.0, glowGpuMaxMs = 0.0;
    double glowCpuSubmitLastMs = 0.0, glowCpuSubmitAverageMs = 0.0, glowCpuSubmitMaxMs = 0.0;
    quint64 glowGpuSamples = 0, glowCpuSubmitSamples = 0, glowDraws = 0;
    double spectrumCpuSubmitLastMs = 0.0, spectrumCpuSubmitAverageMs = 0.0, spectrumCpuSubmitMaxMs = 0.0;
    quint64 spectrumCpuSubmitSamples = 0;
    quint64 glowCpuSubmitOver1Ms = 0, glowCpuSubmitOver5Ms = 0;
    quint64 spectrumCpuSubmitOver1Ms = 0, spectrumCpuSubmitOver5Ms = 0;
    QString glowGpuTimingStatus = QStringLiteral("not initialized");
    struct GlowGpuTimingSlot {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint, start, end;
        bool pending = false;
    };
    GlowGpuTimingSlot glowGpuTimingSlots[4];
    int nextGlowGpuTimingSlot = 0;
    bool glowGpuTimingQueriesReady = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cachedTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> cachedRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cachedTextureSrv;
    Microsoft::WRL::ComPtr<IDXGISwapChain> nativeSwapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> nativeBackBufferRtv;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> nativeCopyVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> nativeCopyPixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> nativeCopyInputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> nativeCopyVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> nativeCopySampler;
    HWND nativePreviewHwnd = nullptr;
    int nativePreviewWidth = 0, nativePreviewHeight = 0;
    bool nativePreviewReady = false;
    bool nativeFramePresented = false;
    bool createNativeSwapChain(int width, int height);
    bool createNativeBackBuffer();
    bool presentNativeTexture();
    struct Phase3ReadbackSlot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        Microsoft::WRL::ComPtr<ID3D11Query> completion;
        bool pending = false;
    };
    Phase3ReadbackSlot phase3ReadbackSlots[3];
    int nextPhase3ReadbackSlot = 0;
    QImage cachedPhase3Image;
    struct BokehVertex { float x, y, u, v, r, g, b, a; };
    Microsoft::WRL::ComPtr<ID3D11VertexShader> bokehVertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> bokehPixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> bokehInputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> bokehVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> bokehConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> bokehTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> bokehRtv;
    struct BokehReadbackSlot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        Microsoft::WRL::ComPtr<ID3D11Query> completion;
        bool pending = false;
    };
    BokehReadbackSlot bokehReadbackSlots[3];
    int nextBokehReadbackSlot = 0;
    QImage cachedBokehImage;
    int bokehWidth = 0, bokehHeight = 0;
    QVector<BokehVertex> cachedBokehVertices;
    struct BokehGpuTimingSlot {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        Microsoft::WRL::ComPtr<ID3D11Query> start;
        Microsoft::WRL::ComPtr<ID3D11Query> end;
        bool pending = false;
    };
    BokehGpuTimingSlot bokehGpuTimingSlots[4];
    int nextBokehGpuTimingSlot = 0;
    bool bokehGpuTimingQueriesReady = false;
#endif
};

#endif // GPURENDERER_H
