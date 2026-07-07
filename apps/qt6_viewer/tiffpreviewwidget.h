#ifndef TIFFPREVIEWWIDGET_H
#define TIFFPREVIEWWIDGET_H

#include <QWidget>
#include <QImage>
#include <QMutex>
#include <QByteArray>
#include <QRect>
#include <QString>

class QHideEvent;
class QMouseEvent;
class QWheelEvent;

#ifdef _WIN32
#include <dxgi1_2.h>
#include <d3d11.h>
#include <wrl/client.h>
#endif

class TiffPreviewWidget : public QWidget
{
    Q_OBJECT
public:
    explicit TiffPreviewWidget(QWidget *parent = nullptr);
    ~TiffPreviewWidget() override;

    WId nativePreviewId() const { return winId(); }
    void setFrame(const QImage &img);
    void setFrameRgba64(int width, int height, const QByteArray &rgba64, int strideBytes);
    void setSourceFormatInfo(const QString &photometric, int samplesPerPixel, int bitsPerSample);
    void clearFrame();

    QString rendererName() const;
    QString internalTextureFormatName() const;
    QString outputSurfaceFormatName() const;
    bool isTenBitOutputSurface() const;
    QString diagnosticsText() const;

signals:
    void diagnosticsChanged();
    void pixelHoverTextChanged(const QString &text);

protected:
    QPaintEngine *paintEngine() const override { return nullptr; }
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;
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
    QString pixelTextAt(const QPoint &widgetPos) const;
    QRect imageDisplayRect() const;
    void resetZoom();
    void clampZoomOffset();

    mutable QMutex frameMtx_;
    PendingKind pendingKind_ = PendingKind::None;
    QImage frame8_;
    QByteArray frameRgba64_;
    int frameWidth_ = 0;
    int frameHeight_ = 0;
    int frameStrideBytes_ = 0;
    bool frameDirty_ = false;
    QString sourcePhotometric_;
    int sourceSamplesPerPixel_ = 0;
    int sourceBitsPerSample_ = 0;

    QString rendererName_;
    QString internalTextureFormatName_ = QStringLiteral("None");
    QString outputSurfaceFormatName_ = QStringLiteral("None");
    bool outputSurface10Bit_ = false;
    bool swapChainFallbackTo8Bit_ = false;
    float zoomScale_ = 1.0f;
    float uvOffsetX_ = 0.0f;
    float uvOffsetY_ = 0.0f;
    bool middleButtonPanning_ = false;
    QPoint lastPanPos_;

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

#endif // TIFFPREVIEWWIDGET_H
