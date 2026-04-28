#include "ffmpeg_video_recorder.h"

#include <algorithm>
#include <cstring>
#include <string>

#ifdef GCAP_ENABLE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
}
#endif

namespace
{
#ifdef GCAP_ENABLE_FFMPEG
static std::string ffErr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

static void setErr(std::string *out, const std::string &msg)
{
    if (out)
        *out = msg;
}

static const char *fmtName(gcap_pixfmt_t fmt)
{
    switch (fmt)
    {
    case GCAP_FMT_NV12: return "NV12";
    case GCAP_FMT_YUY2: return "YUY2";
    case GCAP_FMT_ARGB: return "ARGB32/RGB32";
    case GCAP_FMT_P010: return "P010";
    case GCAP_FMT_Y210: return "Y210";
    case GCAP_FMT_V210: return "V210";
    case GCAP_FMT_R210: return "R210";
    default: return "UNKNOWN";
    }
}

static bool supportedInput(gcap_pixfmt_t fmt)
{
    return fmt == GCAP_FMT_NV12 || fmt == GCAP_FMT_YUY2 || fmt == GCAP_FMT_ARGB ||
           fmt == GCAP_FMT_P010 || fmt == GCAP_FMT_Y210;
}

static uint8_t clampByte(int v)
{
    return static_cast<uint8_t>(std::max(0, std::min(255, v)));
}

static uint8_t word10To8(uint16_t v)
{
    // P010/Y210 store 10-bit components left-aligned in 16-bit words.
    return static_cast<uint8_t>(v >> 8);
}

static void rgbToBt709Limited(uint8_t r, uint8_t g, uint8_t b, uint8_t &y, uint8_t &u, uint8_t &v)
{
    const int yi = 16  + (( 47 * r + 157 * g +  16 * b + 128) >> 8);
    const int ui = 128 + ((-26 * r -  87 * g + 112 * b + 128) >> 8);
    const int vi = 128 + ((112 * r - 102 * g -  10 * b + 128) >> 8);
    y = clampByte(yi);
    u = clampByte(ui);
    v = clampByte(vi);
}

static bool fillYuv420pFromInput(const FfmpegVideoFrameView &view, AVFrame *frame, std::string *error)
{
    const int w = view.width;
    const int h = view.height;
    if (!frame || w <= 0 || h <= 0 || (w & 1) || (h & 1))
    {
        setErr(error, "FFmpeg recorder requires even video size");
        return false;
    }

    uint8_t *dstY = frame->data[0];
    uint8_t *dstU = frame->data[1];
    uint8_t *dstV = frame->data[2];
    const int lsY = frame->linesize[0];
    const int lsU = frame->linesize[1];
    const int lsV = frame->linesize[2];

    switch (view.format)
    {
    case GCAP_FMT_NV12:
    {
        if (!view.data[0] || !view.data[1] || view.stride[0] <= 0 || view.stride[1] <= 0)
        {
            setErr(error, "invalid NV12 frame");
            return false;
        }
        for (int y = 0; y < h; ++y)
            std::memcpy(dstY + y * lsY, view.data[0] + y * view.stride[0], static_cast<size_t>(w));
        for (int y = 0; y < h / 2; ++y)
        {
            const uint8_t *src = view.data[1] + y * view.stride[1];
            uint8_t *du = dstU + y * lsU;
            uint8_t *dv = dstV + y * lsV;
            for (int x = 0; x < w / 2; ++x)
            {
                du[x] = src[x * 2 + 0];
                dv[x] = src[x * 2 + 1];
            }
        }
        return true;
    }
    case GCAP_FMT_P010:
    {
        if (!view.data[0] || !view.data[1] || view.stride[0] <= 0 || view.stride[1] <= 0)
        {
            setErr(error, "invalid P010 frame");
            return false;
        }
        for (int y = 0; y < h; ++y)
        {
            const uint16_t *src = reinterpret_cast<const uint16_t *>(view.data[0] + y * view.stride[0]);
            uint8_t *dy = dstY + y * lsY;
            for (int x = 0; x < w; ++x)
                dy[x] = word10To8(src[x]);
        }
        for (int y = 0; y < h / 2; ++y)
        {
            const uint16_t *src = reinterpret_cast<const uint16_t *>(view.data[1] + y * view.stride[1]);
            uint8_t *du = dstU + y * lsU;
            uint8_t *dv = dstV + y * lsV;
            for (int x = 0; x < w / 2; ++x)
            {
                du[x] = word10To8(src[x * 2 + 0]);
                dv[x] = word10To8(src[x * 2 + 1]);
            }
        }
        return true;
    }
    case GCAP_FMT_YUY2:
    {
        if (!view.data[0] || view.stride[0] <= 0)
        {
            setErr(error, "invalid YUY2 frame");
            return false;
        }
        for (int y = 0; y < h; ++y)
        {
            const uint8_t *src = view.data[0] + y * view.stride[0];
            uint8_t *dy = dstY + y * lsY;
            for (int x = 0; x < w; x += 2)
            {
                dy[x + 0] = src[x * 2 + 0];
                dy[x + 1] = src[x * 2 + 2];
            }
        }
        for (int y = 0; y < h / 2; ++y)
        {
            const uint8_t *row0 = view.data[0] + (y * 2 + 0) * view.stride[0];
            const uint8_t *row1 = view.data[0] + (y * 2 + 1) * view.stride[0];
            uint8_t *du = dstU + y * lsU;
            uint8_t *dv = dstV + y * lsV;
            for (int x = 0; x < w / 2; ++x)
            {
                const int off = x * 4;
                du[x] = static_cast<uint8_t>((int(row0[off + 1]) + int(row1[off + 1]) + 1) / 2);
                dv[x] = static_cast<uint8_t>((int(row0[off + 3]) + int(row1[off + 3]) + 1) / 2);
            }
        }
        return true;
    }
    case GCAP_FMT_Y210:
    {
        if (!view.data[0] || view.stride[0] <= 0)
        {
            setErr(error, "invalid Y210 frame");
            return false;
        }
        for (int y = 0; y < h; ++y)
        {
            const uint16_t *src = reinterpret_cast<const uint16_t *>(view.data[0] + y * view.stride[0]);
            uint8_t *dy = dstY + y * lsY;
            for (int x = 0; x < w; x += 2)
            {
                dy[x + 0] = word10To8(src[x * 2 + 0]);
                dy[x + 1] = word10To8(src[x * 2 + 2]);
            }
        }
        for (int y = 0; y < h / 2; ++y)
        {
            const uint16_t *row0 = reinterpret_cast<const uint16_t *>(view.data[0] + (y * 2 + 0) * view.stride[0]);
            const uint16_t *row1 = reinterpret_cast<const uint16_t *>(view.data[0] + (y * 2 + 1) * view.stride[0]);
            uint8_t *du = dstU + y * lsU;
            uint8_t *dv = dstV + y * lsV;
            for (int x = 0; x < w / 2; ++x)
            {
                const int off = x * 4;
                du[x] = static_cast<uint8_t>((int(word10To8(row0[off + 1])) + int(word10To8(row1[off + 1])) + 1) / 2);
                dv[x] = static_cast<uint8_t>((int(word10To8(row0[off + 3])) + int(word10To8(row1[off + 3])) + 1) / 2);
            }
        }
        return true;
    }
    case GCAP_FMT_ARGB:
    {
        if (!view.data[0] || view.stride[0] <= 0)
        {
            setErr(error, "invalid ARGB/RGB32 frame");
            return false;
        }
        // The app-side ARGB/RGB32 buffer is treated as BGRA byte order, which is the common
        // DIB/DirectShow memory layout for RGB32/ARGB32 on little-endian Windows.
        for (int y = 0; y < h; y += 2)
        {
            const uint8_t *row0 = view.data[0] + y * view.stride[0];
            const uint8_t *row1 = view.data[0] + (y + 1) * view.stride[0];
            uint8_t *dy0 = dstY + y * lsY;
            uint8_t *dy1 = dstY + (y + 1) * lsY;
            uint8_t *du = dstU + (y / 2) * lsU;
            uint8_t *dv = dstV + (y / 2) * lsV;
            for (int x = 0; x < w; x += 2)
            {
                uint8_t yy[4], uu[4], vv[4];
                const uint8_t *p0 = row0 + (x + 0) * 4;
                const uint8_t *p1 = row0 + (x + 1) * 4;
                const uint8_t *p2 = row1 + (x + 0) * 4;
                const uint8_t *p3 = row1 + (x + 1) * 4;
                rgbToBt709Limited(p0[2], p0[1], p0[0], yy[0], uu[0], vv[0]);
                rgbToBt709Limited(p1[2], p1[1], p1[0], yy[1], uu[1], vv[1]);
                rgbToBt709Limited(p2[2], p2[1], p2[0], yy[2], uu[2], vv[2]);
                rgbToBt709Limited(p3[2], p3[1], p3[0], yy[3], uu[3], vv[3]);
                dy0[x + 0] = yy[0];
                dy0[x + 1] = yy[1];
                dy1[x + 0] = yy[2];
                dy1[x + 1] = yy[3];
                du[x / 2] = static_cast<uint8_t>((int(uu[0]) + int(uu[1]) + int(uu[2]) + int(uu[3]) + 2) / 4);
                dv[x / 2] = static_cast<uint8_t>((int(vv[0]) + int(vv[1]) + int(vv[2]) + int(vv[3]) + 2) / 4);
            }
        }
        return true;
    }
    default:
        setErr(error, std::string("unsupported FFmpeg recorder input format: ") + fmtName(view.format));
        return false;
    }
}
#endif
}

struct FfmpegVideoRecorder::Impl
{
#ifdef GCAP_ENABLE_FFMPEG
    AVFormatContext *fmt = nullptr;
    AVCodecContext *codec = nullptr;
    AVStream *stream = nullptr;
    AVPacket *pkt = nullptr;
#endif
};

FfmpegVideoRecorder::FfmpegVideoRecorder()
{
    impl_ = new Impl();
}

FfmpegVideoRecorder::~FfmpegVideoRecorder()
{
    close();
    delete impl_;
    impl_ = nullptr;
}

bool FfmpegVideoRecorder::open(const FfmpegVideoRecordConfig &cfg, std::string *error)
{
#ifndef GCAP_ENABLE_FFMPEG
    (void)cfg;
    if (error)
        *error = "FFmpeg support is not enabled. Set FFMPEG_ROOT or use vcpkg and rebuild with GCAP_ENABLE_FFMPEG.";
    return false;
#else
    close();
    if (cfg.path.empty() || cfg.width <= 0 || cfg.height <= 0 || cfg.fps_num <= 0 || cfg.fps_den <= 0)
    {
        setErr(error, "invalid FFmpeg recorder config");
        return false;
    }
    if (!supportedInput(cfg.input_format))
    {
        setErr(error, std::string("unsupported FFmpeg recorder input format: ") + fmtName(cfg.input_format));
        return false;
    }

    cfg_ = cfg;

    int ret = avformat_alloc_output_context2(&impl_->fmt, nullptr, nullptr, cfg.path.c_str());
    if (ret < 0 || !impl_->fmt)
    {
        setErr(error, "avformat_alloc_output_context2 failed: " + ffErr(ret));
        close();
        return false;
    }

    const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
    if (!codec)
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec)
    {
        setErr(error, "H.264 encoder not found. Install an FFmpeg build with libx264 or H.264 encoder support.");
        close();
        return false;
    }

    impl_->stream = avformat_new_stream(impl_->fmt, nullptr);
    if (!impl_->stream)
    {
        setErr(error, "avformat_new_stream failed");
        close();
        return false;
    }

    impl_->codec = avcodec_alloc_context3(codec);
    if (!impl_->codec)
    {
        setErr(error, "avcodec_alloc_context3 failed");
        close();
        return false;
    }

    impl_->codec->codec_id = codec->id;
    impl_->codec->codec_type = AVMEDIA_TYPE_VIDEO;
    impl_->codec->width = cfg.width;
    impl_->codec->height = cfg.height;
    impl_->codec->pix_fmt = AV_PIX_FMT_YUV420P;
    impl_->codec->time_base = AVRational{cfg.fps_den, cfg.fps_num};
    impl_->codec->framerate = AVRational{cfg.fps_num, cfg.fps_den};
    impl_->codec->bit_rate = static_cast<int64_t>(cfg.bitrate_kbps) * 1000;
    impl_->codec->gop_size = std::max(1, cfg.fps_num / std::max(1, cfg.fps_den));
    impl_->codec->max_b_frames = 0;

    if (impl_->fmt->oformat->flags & AVFMT_GLOBALHEADER)
        impl_->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    av_opt_set(impl_->codec->priv_data, "preset", "veryfast", 0);
    av_opt_set(impl_->codec->priv_data, "tune", "zerolatency", 0);

    ret = avcodec_open2(impl_->codec, codec, nullptr);
    if (ret < 0)
    {
        setErr(error, "avcodec_open2 failed: " + ffErr(ret));
        close();
        return false;
    }

    ret = avcodec_parameters_from_context(impl_->stream->codecpar, impl_->codec);
    if (ret < 0)
    {
        setErr(error, "avcodec_parameters_from_context failed: " + ffErr(ret));
        close();
        return false;
    }
    impl_->stream->time_base = impl_->codec->time_base;

    if (!(impl_->fmt->oformat->flags & AVFMT_NOFILE))
    {
        ret = avio_open(&impl_->fmt->pb, cfg.path.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0)
        {
            setErr(error, "avio_open failed: " + ffErr(ret));
            close();
            return false;
        }
    }

    ret = avformat_write_header(impl_->fmt, nullptr);
    if (ret < 0)
    {
        setErr(error, "avformat_write_header failed: " + ffErr(ret));
        close();
        return false;
    }

    impl_->pkt = av_packet_alloc();
    if (!impl_->pkt)
    {
        setErr(error, "av_packet_alloc failed");
        close();
        return false;
    }

    opened_ = true;
    return true;
#endif
}

bool FfmpegVideoRecorder::writeFrame(const FfmpegVideoFrameView &view, std::string *error)
{
#ifndef GCAP_ENABLE_FFMPEG
    (void)view;
    if (error)
        *error = "FFmpeg support is not enabled";
    return false;
#else
    if (!opened_ || !impl_ || !impl_->codec || !impl_->fmt || !impl_->stream)
    {
        setErr(error, "FFmpeg recorder is not open");
        return false;
    }
    if (view.width != cfg_.width || view.height != cfg_.height || !supportedInput(view.format))
    {
        setErr(error, std::string("invalid frame for FFmpeg recorder: ") + fmtName(view.format));
        return false;
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame)
    {
        setErr(error, "av_frame_alloc failed");
        return false;
    }
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = cfg_.width;
    frame->height = cfg_.height;
    frame->pts = view.pts;

    int ret = av_frame_get_buffer(frame, 32);
    if (ret < 0)
    {
        setErr(error, "av_frame_get_buffer failed: " + ffErr(ret));
        av_frame_free(&frame);
        return false;
    }

    ret = av_frame_make_writable(frame);
    if (ret < 0)
    {
        setErr(error, "av_frame_make_writable failed: " + ffErr(ret));
        av_frame_free(&frame);
        return false;
    }

    if (!fillYuv420pFromInput(view, frame, error))
    {
        av_frame_free(&frame);
        return false;
    }

    ret = avcodec_send_frame(impl_->codec, frame);
    av_frame_free(&frame);
    if (ret < 0)
    {
        setErr(error, "avcodec_send_frame failed: " + ffErr(ret));
        return false;
    }

    while (true)
    {
        ret = avcodec_receive_packet(impl_->codec, impl_->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
        {
            setErr(error, "avcodec_receive_packet failed: " + ffErr(ret));
            return false;
        }

        av_packet_rescale_ts(impl_->pkt, impl_->codec->time_base, impl_->stream->time_base);
        impl_->pkt->stream_index = impl_->stream->index;
        ret = av_interleaved_write_frame(impl_->fmt, impl_->pkt);
        av_packet_unref(impl_->pkt);
        if (ret < 0)
        {
            setErr(error, "av_interleaved_write_frame failed: " + ffErr(ret));
            return false;
        }
    }
    return true;
#endif
}

void FfmpegVideoRecorder::close()
{
#ifdef GCAP_ENABLE_FFMPEG
    if (impl_ && impl_->codec && impl_->fmt && opened_)
    {
        avcodec_send_frame(impl_->codec, nullptr);
        while (true)
        {
            int ret = avcodec_receive_packet(impl_->codec, impl_->pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                break;
            av_packet_rescale_ts(impl_->pkt, impl_->codec->time_base, impl_->stream->time_base);
            impl_->pkt->stream_index = impl_->stream->index;
            av_interleaved_write_frame(impl_->fmt, impl_->pkt);
            av_packet_unref(impl_->pkt);
        }
        av_write_trailer(impl_->fmt);
    }
    if (impl_)
    {
        if (impl_->pkt)
            av_packet_free(&impl_->pkt);
        if (impl_->codec)
            avcodec_free_context(&impl_->codec);
        if (impl_->fmt)
        {
            if (!(impl_->fmt->oformat->flags & AVFMT_NOFILE) && impl_->fmt->pb)
                avio_closep(&impl_->fmt->pb);
            avformat_free_context(impl_->fmt);
            impl_->fmt = nullptr;
        }
        impl_->stream = nullptr;
    }
#endif
    opened_ = false;
}
