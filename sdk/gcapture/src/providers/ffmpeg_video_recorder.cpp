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
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
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

static AVPixelFormat avInputFormat(gcap_pixfmt_t fmt)
{
    switch (fmt)
    {
    case GCAP_FMT_NV12: return AV_PIX_FMT_NV12;
    case GCAP_FMT_YUY2: return AV_PIX_FMT_YUYV422;
    case GCAP_FMT_ARGB:
        // DirectShow RGB32/ARGB32 samples arrive in little-endian BGRA byte order.
        return AV_PIX_FMT_BGRA;
    case GCAP_FMT_P010: return AV_PIX_FMT_P010LE;
    case GCAP_FMT_Y210: return AV_PIX_FMT_Y210LE;
    default: return AV_PIX_FMT_NONE;
    }
}

static bool supportedInput(gcap_pixfmt_t fmt)
{
    return avInputFormat(fmt) != AV_PIX_FMT_NONE;
}

static bool wantsHevcMain10(gcap_pixfmt_t fmt, bool force)
{
    return force || fmt == GCAP_FMT_P010 || fmt == GCAP_FMT_Y210;
}

static const AVCodec *findEncoderByNameList(const char *const *names)
{
    for (const char *const *name = names; *name; ++name)
    {
        if (const AVCodec *codec = avcodec_find_encoder_by_name(*name))
            return codec;
    }
    return nullptr;
}

static const AVCodec *findReleaseEncoder(bool useHevc)
{
    static const char *const h264Encoders[] = {
        "h264_mf",
        nullptr,
    };
    static const char *const hevcEncoders[] = {
        "hevc_mf",
        nullptr,
    };
    return findEncoderByNameList(useHevc ? hevcEncoders : h264Encoders);
}

static bool codecSupportsPixFmt(const AVCodec *codec, AVPixelFormat fmt)
{
    if (!codec || !codec->pix_fmts)
        return true;
    for (const AVPixelFormat *p = codec->pix_fmts; *p != AV_PIX_FMT_NONE; ++p)
    {
        if (*p == fmt)
            return true;
    }
    return false;
}

static bool codecNameEquals(const AVCodec *codec, const char *name)
{
    return codec && codec->name && std::strcmp(codec->name, name) == 0;
}

static const char *pixFmtName(AVPixelFormat fmt)
{
    const char *name = av_get_pix_fmt_name(fmt);
    return name ? name : "unknown";
}

static AVPixelFormat chooseOutputFormat(const AVCodec *codec, bool useHevc)
{
    if (codecNameEquals(codec, "h264_mf") || codecNameEquals(codec, "hevc_mf"))
    {
        if (codecSupportsPixFmt(codec, AV_PIX_FMT_NV12))
            return AV_PIX_FMT_NV12;
        if (codecSupportsPixFmt(codec, AV_PIX_FMT_YUV420P))
            return AV_PIX_FMT_YUV420P;
    }

    static const AVPixelFormat hevcPrefs[] = {
        AV_PIX_FMT_YUV420P10LE,
        AV_PIX_FMT_P010LE,
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_NONE,
    };
    static const AVPixelFormat h264Prefs[] = {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_NONE,
    };
    const AVPixelFormat *prefs = useHevc ? hevcPrefs : h264Prefs;
    for (const AVPixelFormat *p = prefs; *p != AV_PIX_FMT_NONE; ++p)
    {
        if (codecSupportsPixFmt(codec, *p))
            return *p;
    }
    return codec && codec->pix_fmts ? codec->pix_fmts[0] : (useHevc ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P);
}

static bool validateFrameView(const FfmpegVideoFrameView &view, AVPixelFormat srcFmt, std::string *error)
{
    if (view.width <= 0 || view.height <= 0 || (view.width & 1) || (view.height & 1))
    {
        setErr(error, "FFmpeg recorder requires even video size");
        return false;
    }

    if (srcFmt == AV_PIX_FMT_NV12 || srcFmt == AV_PIX_FMT_P010LE)
    {
        if (!view.data[0] || !view.data[1] || view.stride[0] <= 0 || view.stride[1] <= 0)
        {
            setErr(error, std::string("invalid ") + fmtName(view.format) + " frame");
            return false;
        }
        return true;
    }

    if (!view.data[0] || view.stride[0] <= 0)
    {
        setErr(error, std::string("invalid ") + fmtName(view.format) + " frame");
        return false;
    }
    return true;
}

static void configureColorSpace(SwsContext *sws, AVPixelFormat srcFmt)
{
    if (!sws)
        return;

    const bool srcIsRgb = srcFmt == AV_PIX_FMT_BGRA;
    const int srcRange = srcIsRgb ? 1 : 0;
    const int dstRange = 0;
    const int *coeff = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(sws, coeff, srcRange, coeff, dstRange,
                             0, 1 << 16, 1 << 16);
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
    SwsContext *sws = nullptr;
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
    if ((cfg.width & 1) || (cfg.height & 1))
    {
        setErr(error, "FFmpeg recorder requires even video size");
        return false;
    }
    if (!supportedInput(cfg.input_format))
    {
        setErr(error, std::string("unsupported FFmpeg recorder input format: ") + fmtName(cfg.input_format));
        return false;
    }

    cfg_ = cfg;
    const bool useHevc = wantsHevcMain10(cfg.input_format, cfg.force_hevc_main10);

    int ret = avformat_alloc_output_context2(&impl_->fmt, nullptr, nullptr, cfg.path.c_str());
    if (ret < 0 || !impl_->fmt)
    {
        setErr(error, "avformat_alloc_output_context2 failed: " + ffErr(ret));
        close();
        return false;
    }

    const AVCodec *codec = findReleaseEncoder(useHevc);
    if (!codec)
    {
        setErr(error,
               useHevc
                   ? "HEVC encoder not found. Use an LGPL shared FFmpeg build with hevc_mf."
                   : "H.264 encoder not found. Use an LGPL shared FFmpeg build with h264_mf.");
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
    impl_->codec->pix_fmt = chooseOutputFormat(codec, useHevc);
    impl_->codec->time_base = AVRational{cfg.fps_den, cfg.fps_num};
    impl_->codec->framerate = AVRational{cfg.fps_num, cfg.fps_den};
    impl_->codec->bit_rate = static_cast<int64_t>(cfg.bitrate_kbps) * 1000;
    impl_->codec->gop_size = std::max(1, cfg.fps_num / std::max(1, cfg.fps_den));
    impl_->codec->max_b_frames = 0;

    if (impl_->fmt->oformat->flags & AVFMT_GLOBALHEADER)
        impl_->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (codecNameEquals(codec, "h264_mf") || codecNameEquals(codec, "hevc_mf"))
    {
        av_opt_set_int(impl_->codec->priv_data, "rate_control", 0, 0); // cbr
        av_opt_set_int(impl_->codec->priv_data, "scenario", 5, 0);     // camera_record
    }
    else
    {
        av_opt_set(impl_->codec->priv_data, "preset", "veryfast", 0);
        if (useHevc)
        {
            av_opt_set(impl_->codec->priv_data, "profile", "main10", 0);
            av_opt_set(impl_->codec->priv_data, "x265-params", "repeat-headers=1", 0);
        }
        else
        {
            av_opt_set(impl_->codec->priv_data, "tune", "zerolatency", 0);
        }
    }

    ret = avcodec_open2(impl_->codec, codec, nullptr);
    if (ret < 0)
    {
        setErr(error,
               std::string("avcodec_open2 failed for ") +
                   (codec->name ? codec->name : "unknown encoder") +
                   " output=" + pixFmtName(impl_->codec->pix_fmt) +
                   " size=" + std::to_string(cfg.width) + "x" + std::to_string(cfg.height) +
                   " fps=" + std::to_string(cfg.fps_num) + "/" + std::to_string(cfg.fps_den) +
                   ": " + ffErr(ret));
        close();
        return false;
    }

    ret = avcodec_parameters_from_context(impl_->stream->codecpar, impl_->codec);
    if (ret >= 0 && useHevc && impl_->stream && impl_->stream->codecpar)
    {
        // Prefer hvc1 sample entry for better compatibility with common MP4 players.
        impl_->stream->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
    }
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

    const AVPixelFormat srcFmt = avInputFormat(view.format);
    const AVPixelFormat dstFmt = impl_->codec->pix_fmt;
    if (!validateFrameView(view, srcFmt, error))
        return false;

    AVFrame *frame = av_frame_alloc();
    if (!frame)
    {
        setErr(error, "av_frame_alloc failed");
        return false;
    }
    frame->format = dstFmt;
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

    impl_->sws = sws_getCachedContext(impl_->sws,
                                      view.width, view.height, srcFmt,
                                      frame->width, frame->height, dstFmt,
                                      SWS_BILINEAR | SWS_ACCURATE_RND,
                                      nullptr, nullptr, nullptr);
    if (!impl_->sws)
    {
        setErr(error, std::string("sws_getCachedContext failed for ") + fmtName(view.format));
        av_frame_free(&frame);
        return false;
    }
    configureColorSpace(impl_->sws, srcFmt);

    const uint8_t *srcData[4] = {view.data[0], view.data[1], view.data[2], view.data[3]};
    int srcStride[4] = {view.stride[0], view.stride[1], view.stride[2], view.stride[3]};
    ret = sws_scale(impl_->sws, srcData, srcStride, 0, view.height, frame->data, frame->linesize);
    if (ret != view.height)
    {
        setErr(error, "sws_scale failed");
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
        if (impl_->sws)
        {
            sws_freeContext(impl_->sws);
            impl_->sws = nullptr;
        }
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
