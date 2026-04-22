#ifndef D3DPREVIEWWIDGET_H
#define D3DPREVIEWWIDGET_H

#include <QWidget>
#include <QImage>
#include <QMutex>
#include <QByteArray>
#include <QString>

class QHideEvent;

#ifdef _WIN32
#include <dxgi1_2.h>
#include <d3d11.h>
#include <wrl/client.h>
#endif

class d3dpreviewwidget : public QWidget
{
    Q_OBJECT
public:
    explicit d3dpreviewwidget(QWidget *parent = nullptr);
    ~d3dpreviewwidget() override;

    WId nativePreviewId() const { return winId(); }
    void setFrame(const QImage &img);
    void setFrameRgba64(int width, int height, const QByteArray &rgba64, int strideBytes);
    void clearFrame();
    void setDitheringEnabled(bool enabled);
    bool isDitheringEnabled() const;

    QString rendererName() const;
    QString internalTextureFormatName() const;
    QString outputSurfaceFormatName() const;
    bool isTenBitOutputSurface() const;
    QString diagnosticsText() const;

signals:
    void diagnosticsChanged();

protected:
    QPaintEngine *paintEngine() const override { return nullptr; }
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    enum class PendingKind
    {
        None,
        Bgra8,
        Rgba64
    };

    void ensureDevice();
    void ensureSwapChain();
    void ensurePipeline();
    void ensureTextures();
    void uploadPendingFrame();
    void renderNow();
    void releaseSwapChainResources();
    void releaseAllD3d();
    void updateDiagnosticsLocked();

    mutable QMutex frameMtx_;
    PendingKind pendingKind_ = PendingKind::None;
    QImage frame8_;
    QByteArray frameRgba64_;
    int frameWidth_ = 0;
    int frameHeight_ = 0;
    int frameStrideBytes_ = 0;
    bool frameDirty_ = false;

    QString rendererName_;
    QString internalTextureFormatName_ = QStringLiteral("None");
    QString outputSurfaceFormatName_ = QStringLiteral("None");
    bool outputSurface10Bit_ = false;
    bool swapChainFallbackTo8Bit_ = false;
    bool ditheringEnabled_ = true;

#ifdef _WIN32
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> backbufferRtv_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> psConstants_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> internalTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> internalSrv_;
    DXGI_FORMAT internalTextureFormat_ = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT outputSurfaceFormat_ = DXGI_FORMAT_UNKNOWN;
    D3D_FEATURE_LEVEL featureLevel_ = D3D_FEATURE_LEVEL_11_0;
    WId swapChainWid_ = 0;
#endif
};

#endif // D3DPREVIEWWIDGET_H
