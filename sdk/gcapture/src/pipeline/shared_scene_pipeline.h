#pragma once

#include "gcapture.h"

#include <d3d11_1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl.h>

class SharedScenePipeline
{
public:
    SharedScenePipeline() = default;
    ~SharedScenePipeline() = default;

    bool initialize(ID3D11Device *d3d,
                    ID3D11DeviceContext *ctx,
                    ID2D1DeviceContext *d2d_ctx,
                    IDWriteFactory *dwrite = nullptr,
                    ID2D1SolidColorBrush *white = nullptr,
                    ID2D1SolidColorBrush *black = nullptr);
    void shutdown();

    bool configurePreview(const gcap_preview_desc_t &desc);
    void release_preview_swapchain();

    bool create_shaders_and_states();
    bool ensure_rt_and_pipeline(int w, int h);
    uint64_t last_ensure_rt_ns() const { return lastEnsureRtNs_; }
    uint64_t total_ensure_rt_ns() const { return totalEnsureRtNs_; }
    uint64_t ensure_rt_calls() const { return ensureRtCalls_; }
    bool last_ensure_rt_rebuilt() const { return lastEnsureRtRebuilt_; }
    const char *last_ensure_rt_rebuild_reason() const { return lastEnsureRtReason_; }
    bool ensure_preview_swapchain(int w, int h);
    bool preview_swapchain_10bit() const { return preview_swapchain_10bit_; }
    bool present_preview(int src_w, int src_h);
    DXGI_FORMAT preview_backbuffer_format() const;
    DXGI_FORMAT scene_texture_format() const;
    DXGI_FORMAT linear_fp16_texture_format() const;
    bool gpu_overlay_text(const wchar_t *text, int frame_w, int frame_h);
    bool composite_overlay_to_scene_fp16(int frame_w, int frame_h);
    bool blit_fp16_to_rgba8(int frame_w, int frame_h);
    bool upload_argb_frame(const void *data, int frame_w, int frame_h, int src_stride);
    bool render_uploaded_argb_to_fp16(int frame_w, int frame_h);
    bool upload_nv12_frame(const uint8_t *y, int stride_y, const uint8_t *uv, int stride_uv, int frame_w, int frame_h);
    bool upload_p010_frame(const uint8_t *y, int stride_y, const uint8_t *uv, int stride_uv, int frame_w, int frame_h);
    bool upload_yuy2_frame(const uint8_t *data, int src_stride, int frame_w, int frame_h);
    bool upload_y210_frame(const uint8_t *data, int src_stride, int frame_w, int frame_h);
    bool render_uploaded_yuv_to_fp16(gcap_pixfmt_t fmt, int frame_w, int frame_h);
    bool copy_fp16_to_scene();
    bool readback_to_frame(int frame_w, int frame_h, uint64_t pts_ns, uint64_t frame_id,
                           gcap_frame_t *out);
    bool export_scene_rgb10(const wchar_t *base_path, bool export_raw, bool export_tiff, bool export_stats);

    ID3D11Device *d3d_ = nullptr;
    ID3D11DeviceContext *ctx_ = nullptr;
    ID2D1DeviceContext *d2d_ctx_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> rt_fp16_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_fp16_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_fp16_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> rt_scene_fp16_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_scene_fp16_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_scene_fp16_;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_rgba_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_rgba8_to_preview_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> rt_rgba_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> upload_nv12_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> upload_yuy2_packed_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> upload_y210_packed_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_rgba_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> rt_stage_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> rt_scene_stage_fp16_;

    Microsoft::WRL::ComPtr<ID3D11Buffer> cs_params_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> rt_uav_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_nv12_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_p010_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_yuy2_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_y210_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_fp16_to_rgba8_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_fp16_to_preview_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_composite_overlay_fp16_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> il_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samp_;

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> d2d_bitmap_rt_;

    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> d2d_white_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> d2d_black_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> overlay_rgba_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> overlay_rtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> overlay_srv_;

    void *preview_hwnd_ = nullptr;
    bool preview_enabled_ = false;
    bool preview_use_fp16_ = false;
    bool preview_swapchain_10bit_ = false;
    int preview_w_ = 0;
    int preview_h_ = 0;


    int rt_w_ = 0;
    int rt_h_ = 0;
    uint64_t lastEnsureRtNs_ = 0;
    uint64_t totalEnsureRtNs_ = 0;
    uint64_t ensureRtCalls_ = 0;
    bool lastEnsureRtRebuilt_ = false;
    const char *lastEnsureRtReason_ = "never";

    Microsoft::WRL::ComPtr<IDXGISwapChain1> preview_swapchain_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> preview_backbuf_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> preview_rtv_;
};
