#include "nativegpupresenter.h"

#include <QElapsedTimer>
#include <QPainter>
#include <algorithm>

#ifdef Q_OS_WIN
namespace {
const char *kShader = R"(
struct VIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };
struct VOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };
VOut VSMain(VIn i) { VOut o; o.pos=float4(i.pos,0,1); o.uv=i.uv; o.color=i.color; return o; }
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);
float4 PSTexture(VOut i) : SV_TARGET { return videoTexture.Sample(videoSampler,i.uv); }
float4 PSColor(VOut i) : SV_TARGET { return i.color; }
)";
bool compileShader(const char *source, const char *entry, const char *target,
                   Microsoft::WRL::ComPtr<ID3DBlob> &blob, QString &error)
{
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, entry, target,
                            D3DCOMPILE_ENABLE_STRICTNESS, 0, blob.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr)) {
        error = errors ? QString::fromLocal8Bit(static_cast<const char *>(errors->GetBufferPointer()),
                                                  int(errors->GetBufferSize()))
                       : QStringLiteral("D3DCompile failed.");
        return false;
    }
    return true;
}
}
#endif

NativeGpuPresenter::~NativeGpuPresenter() { shutdown(); }

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
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                         D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL selected = D3D_FEATURE_LEVEL_10_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        m_device.GetAddressOf(), &selected, m_context.GetAddressOf());
    if (hr == E_INVALIDARG) {
        m_device.Reset(); m_context.Reset();
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, ARRAYSIZE(levels) - 1,
            D3D11_SDK_VERSION, m_device.GetAddressOf(), &selected, m_context.GetAddressOf());
    }
    if (FAILED(hr)) {
        m_error = QStringLiteral("D3D11CreateDevice failed (0x%1).").arg(quint32(hr),8,16,QChar('0'));
        shutdown(); return false;
    }
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory> factory;
    if (SUCCEEDED(m_device.As(&dxgiDevice))) dxgiDevice->GetAdapter(adapter.GetAddressOf());
    if (!adapter || FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())))) {
        m_error = QStringLiteral("Could not obtain DXGI adapter/factory."); shutdown(); return false;
    }
    DXGI_ADAPTER_DESC adapterDesc{};
    if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) m_adapterName = QString::fromWCharArray(adapterDesc.Description);
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = UINT(width); desc.BufferDesc.Height = UINT(height);
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 0; desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    desc.OutputWindow = static_cast<HWND>(nativeWindow); desc.Windowed = TRUE;
    hr = factory->CreateSwapChain(m_device.Get(), &desc, m_swapChain.GetAddressOf());
    if (FAILED(hr)) {
        m_error = QStringLiteral("CreateSwapChain failed (0x%1).").arg(quint32(hr),8,16,QChar('0'));
        shutdown(); return false;
    }
    factory->MakeWindowAssociation(static_cast<HWND>(nativeWindow), DXGI_MWA_NO_ALT_ENTER);
    if (!createPipeline() || !createBackBuffer()) { shutdown(); return false; }
    m_ready = true; m_error.clear(); return true;
#else
    Q_UNUSED(nativeWindow); Q_UNUSED(width); Q_UNUSED(height);
    m_error = QStringLiteral("Native Direct3D 11 presentation is only available on Windows.");
    return false;
#endif
}

#ifdef Q_OS_WIN
bool NativeGpuPresenter::createPipeline()
{
    Microsoft::WRL::ComPtr<ID3DBlob> vs, psTexture, psColor;
    if (!compileShader(kShader,"VSMain","vs_4_0",vs,m_error) ||
        !compileShader(kShader,"PSTexture","ps_4_0",psTexture,m_error) ||
        !compileShader(kShader,"PSColor","ps_4_0",psColor,m_error)) return false;
    HRESULT hr=m_device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,m_vertexShader.GetAddressOf());
    if(FAILED(hr)){m_error=QStringLiteral("CreateVertexShader failed.");return false;}
    hr=m_device->CreatePixelShader(psTexture->GetBufferPointer(),psTexture->GetBufferSize(),nullptr,m_textureShader.GetAddressOf());
    if(FAILED(hr)){m_error=QStringLiteral("Create texture pixel shader failed.");return false;}
    hr=m_device->CreatePixelShader(psColor->GetBufferPointer(),psColor->GetBufferSize(),nullptr,m_colorShader.GetAddressOf());
    if(FAILED(hr)){m_error=QStringLiteral("Create color pixel shader failed.");return false;}
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0}
    };
    hr=m_device->CreateInputLayout(layout,3,vs->GetBufferPointer(),vs->GetBufferSize(),m_inputLayout.GetAddressOf());
    if(FAILED(hr)){m_error=QStringLiteral("CreateInputLayout failed.");return false;}
    D3D11_BUFFER_DESC vb{}; vb.ByteWidth=sizeof(Vertex)*6*260; vb.Usage=D3D11_USAGE_DYNAMIC;
    vb.BindFlags=D3D11_BIND_VERTEX_BUFFER; vb.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    hr=m_device->CreateBuffer(&vb,nullptr,m_vertexBuffer.GetAddressOf());
    if(FAILED(hr)){m_error=QStringLiteral("Create vertex buffer failed.");return false;}
    D3D11_SAMPLER_DESC sd{}; sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD=D3D11_FLOAT32_MAX;
    hr=m_device->CreateSamplerState(&sd,m_sampler.GetAddressOf());
    if(FAILED(hr)){m_error=QStringLiteral("Create sampler failed.");return false;}
    D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].BlendEnable=TRUE;
    bd.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC_ALPHA; bd.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD; bd.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    hr=m_device->CreateBlendState(&bd,m_blendState.GetAddressOf());
    if(FAILED(hr)){m_error=QStringLiteral("Create blend state failed.");return false;}
    return true;
}

bool NativeGpuPresenter::createBackBuffer()
{
    m_renderTarget.Reset(); m_videoView.Reset(); m_uploadTexture.Reset();
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr=m_swapChain->GetBuffer(0,IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if(FAILED(hr)){m_error=QStringLiteral("Swap-chain GetBuffer failed.");return false;}
    hr=m_device->CreateRenderTargetView(backBuffer.Get(),nullptr,m_renderTarget.GetAddressOf());
    if(FAILED(hr)){m_error=QStringLiteral("CreateRenderTargetView failed.");return false;}
    D3D11_TEXTURE2D_DESC td{}; td.Width=UINT(m_size.width()); td.Height=UINT(m_size.height());
    td.MipLevels=1; td.ArraySize=1; td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count=1; td.Usage=D3D11_USAGE_DEFAULT; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    hr=m_device->CreateTexture2D(&td,nullptr,m_uploadTexture.GetAddressOf());
    if(FAILED(hr)){m_error=QStringLiteral("Create video upload texture failed.");return false;}
    hr=m_device->CreateShaderResourceView(m_uploadTexture.Get(),nullptr,m_videoView.GetAddressOf());
    if(FAILED(hr)){m_error=QStringLiteral("Create video texture view failed.");return false;}
    return true;
}
#endif

bool NativeGpuPresenter::resize(int width,int height)
{
#ifdef Q_OS_WIN
    if(!m_ready||!m_swapChain||width<1||height<1)return false;
    if(m_size==QSize(width,height))return true;
    m_context->OMSetRenderTargets(0,nullptr,nullptr); m_renderTarget.Reset();m_videoView.Reset();m_uploadTexture.Reset();
    HRESULT hr=m_swapChain->ResizeBuffers(0,UINT(width),UINT(height),DXGI_FORMAT_UNKNOWN,0);
    if(FAILED(hr)){m_error=QStringLiteral("ResizeBuffers failed (0x%1).").arg(quint32(hr),8,16,QChar('0'));return false;}
    m_size=QSize(width,height);return createBackBuffer();
#else
    Q_UNUSED(width);Q_UNUSED(height);return false;
#endif
}

bool NativeGpuPresenter::presentImage(const QImage &frame)
{
    return presentFrame(frame,{}, {},1.0);
}

bool NativeGpuPresenter::presentFrame(const QImage &frame,const QVector<QRectF> &rects,
                                      const QVector<QColor> &colors,qreal opacity)
{
#ifdef Q_OS_WIN
    if(!m_ready||frame.isNull()){m_error=QStringLiteral("Presenter not ready or frame empty.");return false;}
    if(m_size!=QSize(width(),height())) {}
    if(frame.size()!=m_size && !resize(frame.width(),frame.height()))return false;
    QElapsedTimer timer;timer.start();
    const QImage bgra=frame.convertToFormat(QImage::Format_ARGB32);
    m_context->UpdateSubresource(m_uploadTexture.Get(),0,nullptr,bgra.constBits(),UINT(bgra.bytesPerLine()),0);
    m_context->OMSetRenderTargets(1,m_renderTarget.GetAddressOf(),nullptr);
    D3D11_VIEWPORT vp{};vp.Width=float(m_size.width());vp.Height=float(m_size.height());vp.MinDepth=0;vp.MaxDepth=1;
    m_context->RSSetViewports(1,&vp);
    const float clear[4]={0.063f,0.067f,0.078f,1.0f};m_context->ClearRenderTargetView(m_renderTarget.Get(),clear);
    m_context->IASetInputLayout(m_inputLayout.Get());UINT stride=sizeof(Vertex),offset=0;
    m_context->IASetVertexBuffers(0,1,m_vertexBuffer.GetAddressOf(),&stride,&offset);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vertexShader.Get(),nullptr,0);
    m_context->OMSetBlendState(nullptr,nullptr,0xffffffff);
    Vertex quad[6]={{-1,-1,0,1,1,1,1,1},{-1,1,0,0,1,1,1,1},{1,1,1,0,1,1,1,1},
                    {-1,-1,0,1,1,1,1,1},{1,1,1,0,1,1,1,1},{1,-1,1,1,1,1,1,1}};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr=m_context->Map(m_vertexBuffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
    if(FAILED(hr)){m_error=QStringLiteral("Map vertex buffer failed.");return false;}
    memcpy(mapped.pData,quad,sizeof(quad));m_context->Unmap(m_vertexBuffer.Get(),0);
    m_context->PSSetShader(m_textureShader.Get(),nullptr,0);
    m_context->PSSetShaderResources(0,1,m_videoView.GetAddressOf());
    m_context->PSSetSamplers(0,1,m_sampler.GetAddressOf());m_context->Draw(6,0);
    m_context->PSSetShaderResources(0,0,nullptr);
    QVector<Vertex> vertices;vertices.reserve(qMin(rects.size(),256)*6);
    const int n=qMin(rects.size(),colors.size());
    for(int i=0;i<n&&vertices.size()+6<=1560;++i){
        const QRectF r=rects[i].normalized().intersected(QRectF(0,0,m_size.width(),m_size.height()));
        if(r.isEmpty())continue;
        const float l=float(2*r.left()/m_size.width()-1), rr=float(2*r.right()/m_size.width()-1);
        const float top=float(1-2*r.top()/m_size.height()), bot=float(1-2*r.bottom()/m_size.height());
        QColor c=colors[i];c.setAlphaF(qBound<qreal>(0,c.alphaF()*opacity,1));
        const float cr=c.redF(),cg=c.greenF(),cb=c.blueF(),ca=c.alphaF();
        const Vertex v[6]={{l,bot,0,0,cr,cg,cb,ca},{l,top,0,0,cr,cg,cb,ca},{rr,top,0,0,cr,cg,cb,ca},
                           {l,bot,0,0,cr,cg,cb,ca},{rr,top,0,0,cr,cg,cb,ca},{rr,bot,0,0,cr,cg,cb,ca}};
        for(const auto &item:v)vertices.push_back(item);
    }
    if(!vertices.isEmpty()){
        hr=m_context->Map(m_vertexBuffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
        if(FAILED(hr)){m_error=QStringLiteral("Map spectrum vertex buffer failed.");return false;}
        memcpy(mapped.pData,vertices.constData(),size_t(vertices.size())*sizeof(Vertex));
        m_context->Unmap(m_vertexBuffer.Get(),0);
        m_context->OMSetBlendState(m_blendState.Get(),nullptr,0xffffffff);
        m_context->PSSetShader(m_colorShader.Get(),nullptr,0);
        m_context->Draw(UINT(vertices.size()),0);
        m_context->OMSetBlendState(nullptr,nullptr,0xffffffff);
    }
    hr=m_swapChain->Present(0,0);m_lastPresentCpuMs=timer.nsecsElapsed()/1000000.0;
    if(FAILED(hr)){m_error=QStringLiteral("Present failed (0x%1).").arg(quint32(hr),8,16,QChar('0'));return false;}
    ++m_presentedFrames;m_error.clear();return true;
#else
    Q_UNUSED(frame);Q_UNUSED(rects);Q_UNUSED(colors);Q_UNUSED(opacity);return false;
#endif
}

void NativeGpuPresenter::shutdown()
{
#ifdef Q_OS_WIN
    if(m_context)m_context->OMSetRenderTargets(0,nullptr,nullptr);
    m_blendState.Reset();m_sampler.Reset();m_vertexBuffer.Reset();m_inputLayout.Reset();
    m_textureShader.Reset();m_colorShader.Reset();m_vertexShader.Reset();m_videoView.Reset();
    m_renderTarget.Reset();m_uploadTexture.Reset();m_swapChain.Reset();m_context.Reset();m_device.Reset();
#endif
    m_ready=false;m_nativeWindow=nullptr;m_size=QSize();m_adapterName.clear();
}
bool NativeGpuPresenter::isReady()const{return m_ready;}
QString NativeGpuPresenter::diagnostics()const
{
#ifdef Q_OS_WIN
 return QStringLiteral("Native D3D11 compositing: %1 | Adapter: %2 | Frames: %3 | CPU present: %4 ms | GPU readback: none")
 .arg(m_ready?QStringLiteral("READY"):QStringLiteral("NOT READY"),
      m_adapterName.isEmpty()?QStringLiteral("unknown"):m_adapterName).arg(m_presentedFrames).arg(m_lastPresentCpuMs,0,'f',3);
#else
 return QStringLiteral("Native D3D11 presentation is Windows only.");
#endif
}
QString NativeGpuPresenter::lastError()const{return m_error;}
