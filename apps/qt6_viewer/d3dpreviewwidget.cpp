#include "d3dpreviewwidget.h"

#include <QMutexLocker>
#include <QResizeEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QPaintEvent>
#include <QByteArray>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3dcompiler.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
using Microsoft::WRL::ComPtr;

namespace
{
struct Vertex
{
    float pos[2];
    float uv[2];
};

static QString dxgiFormatName(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_R10G10B10A2_UNORM: return QStringLiteral("R10G10B10A2_UNORM");
    case DXGI_FORMAT_B8G8R8A8_UNORM: return QStringLiteral("B8G8R8A8_UNORM");
    case DXGI_FORMAT_R16G16B16A16_UNORM: return QStringLiteral("R16G16B16A16_UNORM");
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return QStringLiteral("B8G8R8A8_UNORM_SRGB");
    default: return QStringLiteral("UNKNOWN(%1)").arg(int(fmt));
    }
}

static QByteArray compileShader(const char *source, const char *entry, const char *target)
{
    QByteArray blobData;
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errorBlob;
    const HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr,
                                  entry, target, 0, 0, &blob, &errorBlob);
    if (FAILED(hr) || !blob)
        return blobData;
    blobData = QByteArray(static_cast<const char *>(blob->GetBufferPointer()), int(blob->GetBufferSize()));
    return blobData;
}
}
#endif

d3dpreviewwidget::d3dpreviewwidget(QWidget *parent)
    : QWidget{parent}
{
    setAttribute(Qt::WA_NativeWindow, true);
    setAutoFillBackground(false);
}

d3dpreviewwidget::~d3dpreviewwidget()
{
    releaseAllD3d();
}

void d3dpreviewwidget::setFrame(const QImage &img)
{
    {
        QMutexLocker lock(&frameMtx_);
        pendingKind_ = PendingKind::Bgra8;
        const QImage converted = (img.format() == QImage::Format_ARGB32)
                                     ? img
                                     : img.convertToFormat(QImage::Format_ARGB32);
        frame8_ = converted.copy();
        frameRgba64_.clear();
        frameWidth_ = frame8_.width();
        frameHeight_ = frame8_.height();
        frameStrideBytes_ = frame8_.bytesPerLine();
        frameDirty_ = true;
    }
    renderNow();
}

void d3dpreviewwidget::setFrameRgba64(int width, int height, const QByteArray &rgba64, int strideBytes)
{
    {
        QMutexLocker lock(&frameMtx_);
        pendingKind_ = PendingKind::Rgba64;
        frame8_ = QImage();
        frameRgba64_ = rgba64;
        frameWidth_ = width;
        frameHeight_ = height;
        frameStrideBytes_ = strideBytes;
        frameDirty_ = true;
    }
    renderNow();
}

void d3dpreviewwidget::clearFrame()
{
    {
        QMutexLocker lock(&frameMtx_);
        pendingKind_ = PendingKind::None;
        frame8_ = QImage();
        frameRgba64_.clear();
        frameWidth_ = 0;
        frameHeight_ = 0;
        frameStrideBytes_ = 0;
        frameDirty_ = false;
    }
#ifdef _WIN32
    internalSrv_.Reset();
    internalTexture_.Reset();
    internalTextureFormat_ = DXGI_FORMAT_UNKNOWN;
#endif
    internalTextureFormatName_ = QStringLiteral("None");
    renderNow();
    emit diagnosticsChanged();
}

QString d3dpreviewwidget::rendererName() const { return rendererName_; }
QString d3dpreviewwidget::internalTextureFormatName() const { return internalTextureFormatName_; }
QString d3dpreviewwidget::outputSurfaceFormatName() const { return outputSurfaceFormatName_; }
bool d3dpreviewwidget::isTenBitOutputSurface() const { return outputSurface10Bit_; }
QString d3dpreviewwidget::diagnosticsText() const
{
    return QStringLiteral("Renderer: %1\nInternal Texture: %2\nOutput Surface: %3\n10-bit Output Surface: %4")
        .arg(rendererName_.isEmpty() ? QStringLiteral("Unavailable") : rendererName_)
        .arg(internalTextureFormatName_)
        .arg(outputSurfaceFormatName_)
        .arg(outputSurface10Bit_ ? QStringLiteral("Yes") : QStringLiteral("No"));
}

void d3dpreviewwidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    renderNow();
}

void d3dpreviewwidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    renderNow();
}

void d3dpreviewwidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
#ifdef _WIN32
    releaseSwapChainResources();
    swapChainWid_ = 0;
#endif
    renderNow();
}

void d3dpreviewwidget::hideEvent(QHideEvent *event)
{
#ifdef _WIN32
    releaseSwapChainResources();
    swapChainWid_ = 0;
#endif
    QWidget::hideEvent(event);
}

void d3dpreviewwidget::ensureDevice()
{
#ifndef _WIN32
    rendererName_ = QStringLiteral("D3D11 unavailable");
#else
    if (device_)
        return;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   &device_, &featureLevel_, &context_);
    if (FAILED(hr))
    {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               &device_, &featureLevel_, &context_);
        if (SUCCEEDED(hr))
            rendererName_ = QStringLiteral("D3D11 WARP");
    }

    if (!device_ || !context_)
    {
        rendererName_ = QStringLiteral("D3D11 unavailable");
        return;
    }

    if (rendererName_.isEmpty())
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        if (SUCCEEDED(device_.As(&dxgiDevice)) && dxgiDevice)
        {
            ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter)
            {
                DXGI_ADAPTER_DESC desc = {};
                if (SUCCEEDED(adapter->GetDesc(&desc)))
                    rendererName_ = QStringLiteral("D3D11 Hardware (%1)").arg(QString::fromWCharArray(desc.Description));
            }
        }
        if (rendererName_.isEmpty())
            rendererName_ = QStringLiteral("D3D11 Hardware");
    }
#endif
}

void d3dpreviewwidget::ensureSwapChain()
{
#ifdef _WIN32
    ensureDevice();
    if (!device_ || !context_ || !isVisible())
        return;

    const int w = qMax(1, width());
    const int h = qMax(1, height());
    const WId currentWid = winId();
    if (swapChain_)
    {
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        if (swapChainWid_ == currentWid &&
            SUCCEEDED(swapChain_->GetDesc1(&desc)) &&
            int(desc.Width) == w && int(desc.Height) == h)
            return;
    }

    releaseSwapChainResources();
    swapChainWid_ = 0;

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(device_.As(&dxgiDevice)) || !dxgiDevice ||
        FAILED(dxgiDevice->GetAdapter(&adapter)) || !adapter ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))) || !factory)
        return;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = UINT(w);
    desc.Height = UINT(h);
    desc.BufferCount = 2;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SampleDesc.Count = 1;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;

    HRESULT hr = factory->CreateSwapChainForHwnd(device_.Get(), HWND(currentWid), &desc, nullptr, nullptr, &swapChain_);
    if (FAILED(hr))
    {
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        hr = factory->CreateSwapChainForHwnd(device_.Get(), HWND(currentWid), &desc, nullptr, nullptr, &swapChain_);
    }
    if (FAILED(hr) || !swapChain_)
        return;

    swapChainWid_ = currentWid;
    outputSurfaceFormat_ = desc.Format;
    outputSurfaceFormatName_ = dxgiFormatName(desc.Format);
    outputSurface10Bit_ = (desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);

    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backbuffer))) || !backbuffer)
        return;
    device_->CreateRenderTargetView(backbuffer.Get(), nullptr, &backbufferRtv_);
    emit diagnosticsChanged();
#endif
}

void d3dpreviewwidget::ensurePipeline()
{
#ifdef _WIN32
    ensureDevice();
    if (!device_ || (vs_ && ps_ && sampler_ && vb_ && inputLayout_))
        return;

    static const char *vsSrc =
        "struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };"
        "struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };"
        "PSIn main(VSIn i){ PSIn o; o.pos=float4(i.pos,0,1); o.uv=i.uv; return o; }";

    static const char *psSrc =
        "Texture2D tex0 : register(t0);"
        "SamplerState samp0 : register(s0);"
        "struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };"
        "float4 main(PSIn i) : SV_Target { return tex0.Sample(samp0, i.uv); }";

    const QByteArray vsBlob = compileShader(vsSrc, "main", "vs_4_0");
    const QByteArray psBlob = compileShader(psSrc, "main", "ps_4_0");
    if (vsBlob.isEmpty() || psBlob.isEmpty())
        return;

    device_->CreateVertexShader(vsBlob.constData(), SIZE_T(vsBlob.size()), nullptr, &vs_);
    device_->CreatePixelShader(psBlob.constData(), SIZE_T(psBlob.size()), nullptr, &ps_);

    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device_->CreateInputLayout(il, ARRAYSIZE(il), vsBlob.constData(), SIZE_T(vsBlob.size()), &inputLayout_);

    const Vertex verts[] = {
        {{-1.0f, -1.0f}, {0.0f, 1.0f}},
        {{-1.0f,  1.0f}, {0.0f, 0.0f}},
        {{ 1.0f, -1.0f}, {1.0f, 1.0f}},
        {{ 1.0f,  1.0f}, {1.0f, 0.0f}},
    };
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = UINT(sizeof(verts));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = { verts, 0, 0 };
    device_->CreateBuffer(&bd, &init, &vb_);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&sd, &sampler_);
#endif
}

void d3dpreviewwidget::ensureTextures()
{
#ifdef _WIN32
    QMutexLocker lock(&frameMtx_);
    if (!frameDirty_ || pendingKind_ == PendingKind::None || !device_)
        return;

    DXGI_FORMAT wantedFmt = DXGI_FORMAT_UNKNOWN;
    if (pendingKind_ == PendingKind::Rgba64)
        wantedFmt = DXGI_FORMAT_R16G16B16A16_UNORM;
    else if (pendingKind_ == PendingKind::Bgra8)
        wantedFmt = DXGI_FORMAT_B8G8R8A8_UNORM;

    if (internalTexture_)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        internalTexture_->GetDesc(&desc);
        if (internalTextureFormat_ != wantedFmt || int(desc.Width) != frameWidth_ || int(desc.Height) != frameHeight_)
        {
            internalSrv_.Reset();
            internalTexture_.Reset();
            internalTextureFormat_ = DXGI_FORMAT_UNKNOWN;
        }
    }

    if (!internalTexture_)
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = UINT(qMax(1, frameWidth_));
        td.Height = UINT(qMax(1, frameHeight_));
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = wantedFmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &internalTexture_)) || !internalTexture_)
            return;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
        srvd.Format = td.Format;
        srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels = 1;
        if (FAILED(device_->CreateShaderResourceView(internalTexture_.Get(), &srvd, &internalSrv_)))
            return;

        internalTextureFormat_ = wantedFmt;
        internalTextureFormatName_ = dxgiFormatName(wantedFmt);
    }

    frameDirty_ = false;
    lock.unlock();
    uploadPendingFrame();
    emit diagnosticsChanged();
#endif
}

void d3dpreviewwidget::uploadPendingFrame()
{
#ifdef _WIN32
    QMutexLocker lock(&frameMtx_);
    if (!context_ || !internalTexture_ || pendingKind_ == PendingKind::None)
        return;

    if (pendingKind_ == PendingKind::Rgba64)
    {
        context_->UpdateSubresource(internalTexture_.Get(), 0, nullptr, frameRgba64_.constData(), UINT(frameStrideBytes_), 0);
    }
    else if (pendingKind_ == PendingKind::Bgra8 && !frame8_.isNull())
    {
        context_->UpdateSubresource(internalTexture_.Get(), 0, nullptr, frame8_.constBits(), UINT(frame8_.bytesPerLine()), 0);
    }
#endif
}

void d3dpreviewwidget::renderNow()
{
#ifdef _WIN32
    if (!isVisible())
        return;
    ensureDevice();
    ensureSwapChain();
    ensurePipeline();
    ensureTextures();
    if (!context_ || !swapChain_ || !backbufferRtv_)
        return;

    const float clearColor[4] = {0.f, 0.f, 0.f, 1.f};
    context_->OMSetRenderTargets(1, backbufferRtv_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(backbufferRtv_.Get(), clearColor);

    D3D11_VIEWPORT vp = {};
    vp.Width = float(qMax(1, width()));
    vp.Height = float(qMax(1, height()));
    vp.MinDepth = 0.f;
    vp.MaxDepth = 1.f;
    context_->RSSetViewports(1, &vp);

    if (internalSrv_ && vs_ && ps_ && sampler_ && vb_ && inputLayout_)
    {
        const UINT stride = sizeof(Vertex);
        const UINT offset = 0;
        context_->IASetInputLayout(inputLayout_.Get());
        context_->IASetVertexBuffers(0, 1, vb_.GetAddressOf(), &stride, &offset);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(ps_.Get(), nullptr, 0);
        context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        context_->PSSetShaderResources(0, 1, internalSrv_.GetAddressOf());
        context_->Draw(4, 0);
        ID3D11ShaderResourceView *nullSrv = nullptr;
        context_->PSSetShaderResources(0, 1, &nullSrv);
    }

    swapChain_->Present(1, 0);
#else
    QWidget::update();
#endif
}

void d3dpreviewwidget::releaseSwapChainResources()
{
#ifdef _WIN32
    backbufferRtv_.Reset();
    swapChain_.Reset();
    outputSurfaceFormat_ = DXGI_FORMAT_UNKNOWN;
    outputSurfaceFormatName_ = QStringLiteral("None");
    outputSurface10Bit_ = false;
    swapChainWid_ = 0;
#endif
}

void d3dpreviewwidget::releaseAllD3d()
{
#ifdef _WIN32
    internalSrv_.Reset();
    internalTexture_.Reset();
    vb_.Reset();
    inputLayout_.Reset();
    sampler_.Reset();
    ps_.Reset();
    vs_.Reset();
    releaseSwapChainResources();
    context_.Reset();
    device_.Reset();
    internalTextureFormat_ = DXGI_FORMAT_UNKNOWN;
#endif
}
