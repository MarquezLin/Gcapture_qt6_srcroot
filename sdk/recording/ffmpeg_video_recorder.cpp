#include "ffmpeg_video_recorder.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <filesystem>
#include <chrono>
#include <cstdio>
#include <deque>
#include <limits>

#ifdef CAPTURE_ENABLE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace
{
#ifdef CAPTURE_ENABLE_FFMPEG
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

static const char *fmtName(FfmpegVideoPixelFormat fmt)
{
    switch (fmt)
    {
    case FfmpegVideoPixelFormat::Nv12: return "NV12";
    case FfmpegVideoPixelFormat::Yuy2: return "YUY2";
    case FfmpegVideoPixelFormat::Bgra32: return "BGRA32";
    case FfmpegVideoPixelFormat::P010: return "P010";
    case FfmpegVideoPixelFormat::Y210: return "Y210";
    case FfmpegVideoPixelFormat::V210: return "V210";
    case FfmpegVideoPixelFormat::R210: return "R210";
    default: return "UNKNOWN";
    }
}

static AVPixelFormat avInputFormat(FfmpegVideoPixelFormat fmt)
{
    switch (fmt)
    {
    case FfmpegVideoPixelFormat::Nv12: return AV_PIX_FMT_NV12;
    case FfmpegVideoPixelFormat::Yuy2: return AV_PIX_FMT_YUYV422;
    case FfmpegVideoPixelFormat::Bgra32:
        // DirectShow RGB32/ARGB32 samples arrive in little-endian BGRA byte order.
        return AV_PIX_FMT_BGRA;
    case FfmpegVideoPixelFormat::P010: return AV_PIX_FMT_P010LE;
    case FfmpegVideoPixelFormat::Y210: return AV_PIX_FMT_Y210LE;
    default: return AV_PIX_FMT_NONE;
    }
}

static bool supportedInput(FfmpegVideoPixelFormat fmt)
{
    return avInputFormat(fmt) != AV_PIX_FMT_NONE;
}

static bool wantsHevcMain10(FfmpegVideoPixelFormat fmt, bool force)
{
    return force || fmt == FfmpegVideoPixelFormat::P010 ||
           fmt == FfmpegVideoPixelFormat::Y210;
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
        "h264_qsv",
        "libopenh264",
        nullptr,
    };
    static const char *const hevcEncoders[] = {
        "hevc_qsv",
        "libkvazaar",
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

static AVSampleFormat pcmSampleFormat(int bits)
{
    switch (bits)
    {
    case 8: return AV_SAMPLE_FMT_U8;
    case 16: return AV_SAMPLE_FMT_S16;
    case 32: return AV_SAMPLE_FMT_S32;
    default: return AV_SAMPLE_FMT_NONE;
    }
}
#endif
}

struct FfmpegVideoRecorder::Impl
{
#ifdef CAPTURE_ENABLE_FFMPEG
    AVFormatContext *fmt = nullptr;
    AVCodecContext *codec = nullptr;
    AVStream *stream = nullptr;
    AVPacket *pkt = nullptr;
    SwsContext *sws = nullptr;
    AVCodecContext *audioCodec = nullptr;
    AVStream *audioStream = nullptr;
    AVPacket *audioPkt = nullptr;
    SwrContext *swr = nullptr;
    AVAudioFifo *audioFifo = nullptr;
    int64_t audioPts = 0;
    bool audioTimelineInitialized = false;
    std::chrono::steady_clock::time_point storageCheck{};
    bool headerWritten = false;
    // Stable codec time bases; container time bases change at each rollover.
    std::deque<AVPacket *> pendingVideo;
    std::deque<AVPacket *> pendingAudio;
    uint64_t pendingBytes = 0;
#endif
};

int64_t ffmpegVideoPtsFromNanoseconds(uint64_t elapsedNs, int fpsNum, int fpsDen)
{
#ifdef CAPTURE_ENABLE_FFMPEG
    if (fpsNum <= 0 || fpsDen <= 0 ||
        elapsedNs > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
        return -1;
    return av_rescale_q(static_cast<int64_t>(elapsedNs),
                        AVRational{1, 1'000'000'000}, AVRational{fpsDen, fpsNum});
#else
    (void)elapsedNs; (void)fpsNum; (void)fpsDen;
    return -1;
#endif
}

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
#ifndef CAPTURE_ENABLE_FFMPEG
    (void)cfg;
    if (error)
        *error = "FFmpeg support is not enabled. Set FFMPEG_ROOT and rebuild capture_ffmpeg_recorder.";
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
    segment_index_ = 1;
    segment_payload_bytes_ = 0;
    segment_origin_ns_ = 0;
    impl_->storageCheck = {};
    if ((cfg.segment_bytes || cfg.segment_duration_ns) && !checkStorage(error))
        return false;
    const bool useHevc = !cfg.force_h264 && wantsHevcMain10(cfg.input_format, cfg.force_hevc_main10);

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
                   ? "HEVC Main10 encoder not found. Install an LGPL shared FFmpeg build with hevc_qsv or libkvazaar."
                   : "H.264 encoder not found. Install an LGPL shared FFmpeg build with h264_qsv or libopenh264.");
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

    if (cfg.audio_enabled)
    {
        const AVSampleFormat inputFormat = pcmSampleFormat(cfg.audio_bits_per_sample);
        if (cfg.audio_sample_rate <= 0 || cfg.audio_channels <= 0 || inputFormat == AV_SAMPLE_FMT_NONE)
        {
            setErr(error, "invalid PCM audio recorder config");
            close();
            return false;
        }
        const AVCodec *audioEncoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!audioEncoder)
        {
            setErr(error, "AAC encoder not found in FFmpeg build");
            close();
            return false;
        }
        impl_->audioStream = avformat_new_stream(impl_->fmt, nullptr);
        impl_->audioCodec = avcodec_alloc_context3(audioEncoder);
        if (!impl_->audioStream || !impl_->audioCodec)
        {
            setErr(error, "failed to allocate FFmpeg AAC stream");
            close();
            return false;
        }
        impl_->audioCodec->codec_type = AVMEDIA_TYPE_AUDIO;
        impl_->audioCodec->codec_id = AV_CODEC_ID_AAC;
        impl_->audioCodec->sample_rate = cfg.audio_sample_rate;
        impl_->audioCodec->time_base = AVRational{1, cfg.audio_sample_rate};
        impl_->audioCodec->bit_rate = static_cast<int64_t>(std::max(64, cfg.audio_bitrate_kbps)) * 1000;
        impl_->audioCodec->sample_fmt = audioEncoder->sample_fmts
                                           ? audioEncoder->sample_fmts[0]
                                           : AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&impl_->audioCodec->ch_layout, cfg.audio_channels);
        if (impl_->fmt->oformat->flags & AVFMT_GLOBALHEADER)
            impl_->audioCodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        ret = avcodec_open2(impl_->audioCodec, audioEncoder, nullptr);
        if (ret < 0)
        {
            setErr(error, "avcodec_open2 failed for AAC: " + ffErr(ret));
            close();
            return false;
        }
        ret = avcodec_parameters_from_context(impl_->audioStream->codecpar, impl_->audioCodec);
        if (ret < 0)
        {
            setErr(error, "AAC parameters failed: " + ffErr(ret));
            close();
            return false;
        }
        impl_->audioStream->time_base = impl_->audioCodec->time_base;

        AVChannelLayout inputLayout{};
        av_channel_layout_default(&inputLayout, cfg.audio_channels);
        ret = swr_alloc_set_opts2(&impl_->swr,
                                  &impl_->audioCodec->ch_layout, impl_->audioCodec->sample_fmt,
                                  impl_->audioCodec->sample_rate,
                                  &inputLayout, inputFormat, cfg.audio_sample_rate,
                                  0, nullptr);
        av_channel_layout_uninit(&inputLayout);
        if (ret < 0 || !impl_->swr || (ret = swr_init(impl_->swr)) < 0)
        {
            setErr(error, "PCM to AAC resampler initialization failed: " + ffErr(ret));
            close();
            return false;
        }
        impl_->audioFifo = av_audio_fifo_alloc(impl_->audioCodec->sample_fmt,
                                               cfg.audio_channels,
                                               std::max(impl_->audioCodec->frame_size, 1024) * 4);
        impl_->audioPkt = av_packet_alloc();
        if (!impl_->audioFifo || !impl_->audioPkt)
        {
            setErr(error, "failed to allocate AAC audio buffers");
            close();
            return false;
        }
    }

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

    impl_->headerWritten = true;
    opened_ = true;
    return true;
#endif
}

bool FfmpegVideoRecorder::writeAudio(const uint8_t *data, size_t bytes,
                                     int64_t timelinePtsNs, std::string *error)
{
#ifndef CAPTURE_ENABLE_FFMPEG
    (void)data;
    (void)bytes;
    (void)timelinePtsNs;
    if (error)
        *error = "FFmpeg support is not enabled";
    return false;
#else
    if (!opened_ || !cfg_.audio_enabled || !impl_->audioCodec || !impl_->audioFifo || !impl_->swr)
    {
        setErr(error, "FFmpeg audio recorder is not open");
        return false;
    }
    const int bytesPerSample = cfg_.audio_bits_per_sample / 8;
    const int blockAlign = bytesPerSample * cfg_.audio_channels;
    if (!data || blockAlign <= 0 || bytes == 0 || (bytes % static_cast<size_t>(blockAlign)) != 0)
    {
        setErr(error, "invalid PCM packet for FFmpeg audio recorder");
        return false;
    }

    int inputSamples = static_cast<int>(bytes / static_cast<size_t>(blockAlign));
    if (timelinePtsNs >= 0)
    {
        const int sampleRate = impl_->audioCodec->sample_rate;
        const int64_t desiredPts = av_rescale_q(
            timelinePtsNs, AVRational{1, 1'000'000'000}, AVRational{1, sampleRate});
        if (!impl_->audioTimelineInitialized)
        {
            impl_->audioPts = desiredPts;
            impl_->audioTimelineInitialized = true;
        }
        else
        {
            const int64_t queuedSamples = av_audio_fifo_size(impl_->audioFifo);
            const int64_t resamplerDelay = swr_get_delay(impl_->swr, sampleRate);
            const int64_t predictedPts = impl_->audioPts + queuedSamples + resamplerDelay;
            const int64_t driftSamples = desiredPts - predictedPts;
            const int64_t correctionLimit = std::max<int64_t>(1, sampleRate / 200); // 5 ms
            const int64_t discontinuityLimit = sampleRate / 4; // 250 ms
            if (driftSamples > discontinuityLimit)
            {
                // Preserve missing time (e.g. dropped PCM packets) as silence,
                // rather than compressing the remainder of the audio timeline.
                if (driftSamples > static_cast<int64_t>(sampleRate) * 10 ||
                    swr_set_compensation(impl_->swr, 0, 0) < 0 ||
                    swr_inject_silence(impl_->swr, static_cast<int>(driftSamples)) < 0)
                {
                    setErr(error, "Audio timestamp gap exceeds the recording recovery limit");
                    return false;
                }
            }
            else if (driftSamples < -discontinuityLimit)
            {
                // Discard overlapping input, keeping already queued PCM and
                // its original PTS. Never relabel old samples as new content.
                if (swr_set_compensation(impl_->swr, 0, 0) < 0)
                {
                    setErr(error, "Cannot reset audio timestamp compensation");
                    return false;
                }
                const int skipped = static_cast<int>((std::min<int64_t>)(inputSamples, -driftSamples));
                data += static_cast<size_t>(skipped) * blockAlign;
                inputSamples -= skipped;
                if (!inputSamples) return true;
            }
            else if (driftSamples > correctionLimit || driftSamples < -correctionLimit)
            {
                const int correction = static_cast<int>(
                    std::clamp<int64_t>(driftSamples, -correctionLimit, correctionLimit));
                if (swr_set_compensation(impl_->swr, correction, sampleRate) < 0)
                {
                    setErr(error, "failed to apply audio timestamp drift compensation");
                    return false;
                }
            }
        }
    }
    const int outputCapacity = swr_get_out_samples(impl_->swr, inputSamples);
    uint8_t **converted = nullptr;
    int convertedLinesize = 0;
    int ret = av_samples_alloc_array_and_samples(&converted, &convertedLinesize,
                                                 cfg_.audio_channels, outputCapacity,
                                                 impl_->audioCodec->sample_fmt, 0);
    if (ret < 0)
    {
        setErr(error, "AAC conversion buffer allocation failed: " + ffErr(ret));
        return false;
    }
    const uint8_t *input[] = {data};
    const int convertedSamples = swr_convert(impl_->swr, converted, outputCapacity,
                                             input, inputSamples);
    if (convertedSamples < 0 ||
        av_audio_fifo_realloc(impl_->audioFifo,
                              av_audio_fifo_size(impl_->audioFifo) + std::max(0, convertedSamples)) < 0 ||
        (convertedSamples > 0 &&
         av_audio_fifo_write(impl_->audioFifo, reinterpret_cast<void **>(converted), convertedSamples) != convertedSamples))
    {
        setErr(error, "PCM to AAC conversion failed");
        av_freep(&converted[0]);
        av_freep(&converted);
        return false;
    }
    av_freep(&converted[0]);
    av_freep(&converted);

    const int frameSamples = impl_->audioCodec->frame_size > 0 ? impl_->audioCodec->frame_size : 1024;
    while (av_audio_fifo_size(impl_->audioFifo) >= frameSamples)
    {
        AVFrame *frame = av_frame_alloc();
        if (!frame)
        {
            setErr(error, "AAC frame allocation failed");
            return false;
        }
        frame->nb_samples = frameSamples;
        frame->format = impl_->audioCodec->sample_fmt;
        frame->sample_rate = impl_->audioCodec->sample_rate;
        av_channel_layout_copy(&frame->ch_layout, &impl_->audioCodec->ch_layout);
        frame->pts = impl_->audioPts;
        impl_->audioPts += frameSamples;
        ret = av_frame_get_buffer(frame, 0);
        if (ret >= 0)
            ret = av_audio_fifo_read(impl_->audioFifo,
                                     reinterpret_cast<void **>(frame->extended_data), frameSamples);
        if (ret >= 0)
            ret = avcodec_send_frame(impl_->audioCodec, frame);
        av_frame_free(&frame);
        if (ret < 0)
        {
            setErr(error, "AAC frame submission failed: " + ffErr(ret));
            return false;
        }
        while ((ret = avcodec_receive_packet(impl_->audioCodec, impl_->audioPkt)) >= 0)
        {
            av_packet_rescale_ts(impl_->audioPkt, impl_->audioCodec->time_base,
                                 impl_->audioStream->time_base);
            impl_->audioPkt->stream_index = impl_->audioStream->index;
            const bool written = writePacket(true, error);
            av_packet_unref(impl_->audioPkt);
            if (!written)
            {
                return false;
            }
        }
        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        {
            setErr(error, "AAC packet encoding failed: " + ffErr(ret));
            return false;
        }
    }
    return true;
#endif
}

bool FfmpegVideoRecorder::writeFrame(const FfmpegVideoFrameView &view, std::string *error)
{
#ifndef CAPTURE_ENABLE_FFMPEG
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
        const bool written = writePacket(false, error);
        av_packet_unref(impl_->pkt);
        if (!written)
        {
            return false;
        }
    }
    return true;
#endif
}

bool FfmpegVideoRecorder::checkStorage(std::string *error)
{
#ifdef CAPTURE_ENABLE_FFMPEG
    const auto now = std::chrono::steady_clock::now();
    if (impl_->storageCheck.time_since_epoch().count() != 0 &&
        now - impl_->storageCheck < std::chrono::seconds(1))
        return true;
    impl_->storageCheck = now;
    std::error_code ec;
    auto directory = std::filesystem::u8path(cfg_.path).parent_path();
    if (directory.empty()) directory = std::filesystem::current_path(ec);
    const auto space = std::filesystem::space(directory, ec);
    if (ec || space.available <= cfg_.reserve_free_bytes)
    {
        setErr(error, "Recording stopped before disk exhaustion (reserved space), or unable to query storage: " +
                      cfg_.path + (ec ? " | " + ec.message() : ""));
        return false;
    }
    return true;
#else
    (void)error;
    return false;
#endif
}

bool FfmpegVideoRecorder::nextSegment(int64_t originNs, std::string *error)
{
#ifdef CAPTURE_ENABLE_FFMPEG
    const auto original = std::filesystem::u8path(cfg_.path);
    char suffix[32]{};
    std::snprintf(suffix, sizeof(suffix), "_%03u", segment_index_ + 1);
    const auto path = original.parent_path() /
        std::filesystem::u8path(original.stem().u8string() + suffix + original.extension().u8string());
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (exists || ec)
    {
        setErr(error, "Cannot create recording segment (existing file or path error): " + path.u8string());
        return false;
    }
    // Finish only the old container; keep video/AAC encoders and queued audio alive.
    int ret = av_write_trailer(impl_->fmt);
    impl_->headerWritten = false;
    if (ret < 0)
    {
        setErr(error, "Recording segment trailer failed: " + ffErr(ret));
        return false;
    }
    ret = avio_closep(&impl_->fmt->pb);
    if (ret < 0)
    {
        setErr(error, "Recording segment close failed: " + ffErr(ret));
        return false;
    }
    avformat_free_context(impl_->fmt);
    impl_->fmt = nullptr;
    impl_->stream = nullptr;
    impl_->audioStream = nullptr;
    ret = avformat_alloc_output_context2(&impl_->fmt, nullptr, nullptr, path.u8string().c_str());
    if (ret < 0 || !impl_->fmt)
    {
        setErr(error, "Cannot allocate recording segment: " + path.u8string());
        return false;
    }
    impl_->stream = avformat_new_stream(impl_->fmt, nullptr);
    if (!impl_->stream || avcodec_parameters_from_context(impl_->stream->codecpar, impl_->codec) < 0)
    {
        setErr(error, "Cannot create video stream for recording segment");
        return false;
    }
    impl_->stream->time_base = impl_->codec->time_base;
    if (impl_->codec->codec_id == AV_CODEC_ID_HEVC)
        impl_->stream->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
    if (impl_->audioCodec)
    {
        impl_->audioStream = avformat_new_stream(impl_->fmt, nullptr);
        if (!impl_->audioStream || avcodec_parameters_from_context(impl_->audioStream->codecpar, impl_->audioCodec) < 0)
        {
            setErr(error, "Cannot create audio stream for recording segment");
            return false;
        }
        impl_->audioStream->time_base = impl_->audioCodec->time_base;
    }
    ret = avio_open(&impl_->fmt->pb, path.u8string().c_str(), AVIO_FLAG_WRITE);
    if (ret >= 0) ret = avformat_write_header(impl_->fmt, nullptr);
    if (ret < 0)
    {
        setErr(error, "Cannot open recording segment " + path.u8string() + ": " + ffErr(ret));
        return false;
    }
    segment_origin_ns_ = originNs;
    impl_->headerWritten = true;
    segment_payload_bytes_ = 0;
    ++segment_index_;
    return true;
#else
    (void)originNs; (void)error;
    return false;
#endif
}

bool FfmpegVideoRecorder::writePacket(bool audio, std::string *error)
{
#ifdef CAPTURE_ENABLE_FFMPEG
    AVPacket *packet = audio ? impl_->audioPkt : impl_->pkt;
    if (!impl_->audioCodec || (!cfg_.segment_bytes && !cfg_.segment_duration_ns))
        return writeMuxPacket(audio, packet, error);

    // Wait for both streams before committing packets. In particular, audio
    // older than a rollover keyframe must still go into the old container.
    AVPacket *copy = av_packet_clone(packet);
    if (!copy)
    {
        setErr(error, "Cannot queue recording packet");
        return false;
    }
    const AVRational sourceBase = audio ? impl_->audioStream->time_base : impl_->stream->time_base;
    const AVRational codecBase = audio ? impl_->audioCodec->time_base : impl_->codec->time_base;
    av_packet_rescale_ts(copy, sourceBase, codecBase);
    auto &queue = audio ? impl_->pendingAudio : impl_->pendingVideo;
    queue.push_back(copy);
    impl_->pendingBytes += static_cast<uint64_t>(copy->size);
    if (!drainPackets(false, error)) return false;
    if (impl_->pendingBytes > 64ULL * 1024 * 1024 ||
        impl_->pendingVideo.size() + impl_->pendingAudio.size() > 4096)
    {
        setErr(error, "Recording stopped safely: audio/video packet interleave backlog exceeded its limit");
        return false;
    }
    return true;
#else
    (void)audio; (void)error;
    return false;
#endif
}

bool FfmpegVideoRecorder::drainPackets(bool force, std::string *error)
{
#ifdef CAPTURE_ENABLE_FFMPEG
    while ((!impl_->pendingVideo.empty() && !impl_->pendingAudio.empty()) ||
           (force && (!impl_->pendingVideo.empty() || !impl_->pendingAudio.empty())))
    {
        bool audio = impl_->pendingVideo.empty();
        if (!audio && !impl_->pendingAudio.empty())
        {
            const auto timestamp = [](const AVPacket *p) {
                return p->dts != AV_NOPTS_VALUE ? p->dts : p->pts;
            };
            audio = av_compare_ts(timestamp(impl_->pendingAudio.front()), impl_->audioCodec->time_base,
                                  timestamp(impl_->pendingVideo.front()), impl_->codec->time_base) < 0;
        }
        auto &queue = audio ? impl_->pendingAudio : impl_->pendingVideo;
        AVPacket *packet = queue.front();
        queue.pop_front();
        impl_->pendingBytes -= static_cast<uint64_t>(packet->size);
        const AVRational codecBase = audio ? impl_->audioCodec->time_base : impl_->codec->time_base;
        const AVRational muxBase = audio ? impl_->audioStream->time_base : impl_->stream->time_base;
        av_packet_rescale_ts(packet, codecBase, muxBase);
        const bool success = writeMuxPacket(audio, packet, error);
        av_packet_free(&packet);
        if (!success) return false;
    }
    return true;
#else
    (void)force; (void)error;
    return false;
#endif
}

bool FfmpegVideoRecorder::writeMuxPacket(bool audio, AVPacket *packet, std::string *error)
{
#ifdef CAPTURE_ENABLE_FFMPEG
    AVStream *stream = audio ? impl_->audioStream : impl_->stream;
    const AVRational packetTimeBase = stream->time_base;
    if ((cfg_.segment_bytes || cfg_.segment_duration_ns) && !closing_)
    {
        if (!checkStorage(error)) return false;
        const int64_t position = avio_tell(impl_->fmt->pb);
        const uint64_t used = (std::max)(segment_payload_bytes_,
            static_cast<uint64_t>((std::max)(int64_t{0}, position)));
        const int64_t packetTimestamp = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
        const int64_t packetTimestampNs = packetTimestamp == AV_NOPTS_VALUE
                                              ? AV_NOPTS_VALUE
                                              : av_rescale_q(packetTimestamp, packetTimeBase,
                                                             AVRational{1, 1'000'000'000});
        const bool durationReached = cfg_.segment_duration_ns && packetTimestampNs != AV_NOPTS_VALUE &&
                                     packetTimestampNs >= segment_origin_ns_ &&
                                     static_cast<uint64_t>(packetTimestampNs - segment_origin_ns_) >=
                                         cfg_.segment_duration_ns;
        // Leave 64 MiB for a GOP, interleaving, and the final MP4 index.
        const uint64_t margin = cfg_.segment_bytes
                                    ? (std::min<uint64_t>)(64ULL * 1024 * 1024, cfg_.segment_bytes / 4)
                                    : 0;
        const bool sizeReached = cfg_.segment_bytes && used >= cfg_.segment_bytes - margin;
        if (!audio && (packet->flags & AV_PKT_FLAG_KEY) && (durationReached || sizeReached))
        {
            if (packetTimestampNs == AV_NOPTS_VALUE || !nextSegment(packetTimestampNs, error))
                return false;
            stream = impl_->stream;
            av_packet_rescale_ts(packet, packetTimeBase, stream->time_base);
        }
        else if (cfg_.segment_bytes &&
                 used + static_cast<uint64_t>(packet->size) >= cfg_.segment_bytes - margin / 2)
        {
            setErr(error, "Recording stopped safely: segment limit reached without a video keyframe | " + cfg_.path);
            return false;
        }
    }
    const int64_t offset = av_rescale_q(segment_origin_ns_, AVRational{1, 1'000'000'000}, stream->time_base);
    if (packet->pts != AV_NOPTS_VALUE) packet->pts -= offset;
    if (packet->dts != AV_NOPTS_VALUE) packet->dts -= offset;
    packet->stream_index = stream->index;
    segment_payload_bytes_ += static_cast<uint64_t>(packet->size);
    const int ret = av_interleaved_write_frame(impl_->fmt, packet);
    if (ret < 0 || (impl_->fmt->pb && impl_->fmt->pb->error < 0))
    {
        setErr(error, "Recording write failed: " + ffErr(ret < 0 ? ret : impl_->fmt->pb->error) +
                      " | " + cfg_.path + " | segment " + std::to_string(segment_index_));
        return false;
    }
    return true;
#else
    (void)audio; (void)packet; (void)error;
    return false;
#endif
}

void FfmpegVideoRecorder::close()
{
    close(nullptr);
}

bool FfmpegVideoRecorder::close(std::string *error)
{
    bool success = true;
    closing_ = true;
#ifdef CAPTURE_ENABLE_FFMPEG
    if (impl_ && impl_->codec && impl_->fmt && opened_ && impl_->headerWritten)
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
            if (!writePacket(false, error)) success = false;
            av_packet_unref(impl_->pkt);
        }
        if (impl_->audioCodec)
        {
            // Do not discard the final partial AAC frame when stopping.
            const int remaining = av_audio_fifo_size(impl_->audioFifo);
            if (remaining > 0)
            {
                AVFrame *tail = av_frame_alloc();
                int tailRet = AVERROR(ENOMEM);
                if (tail)
                {
                    tail->nb_samples = remaining;
                    tail->format = impl_->audioCodec->sample_fmt;
                    tail->sample_rate = impl_->audioCodec->sample_rate;
                    tail->pts = impl_->audioPts;
                    av_channel_layout_copy(&tail->ch_layout, &impl_->audioCodec->ch_layout);
                    tailRet = av_frame_get_buffer(tail, 0);
                    if (tailRet >= 0)
                        tailRet = av_audio_fifo_read(impl_->audioFifo,
                            reinterpret_cast<void **>(tail->extended_data), remaining);
                    if (tailRet >= 0) tailRet = avcodec_send_frame(impl_->audioCodec, tail);
                    av_frame_free(&tail);
                }
                if (tailRet < 0)
                {
                    setErr(error, "AAC final frame failed: " + ffErr(tailRet));
                    success = false;
                }
                while (avcodec_receive_packet(impl_->audioCodec, impl_->audioPkt) >= 0)
                {
                    av_packet_rescale_ts(impl_->audioPkt, impl_->audioCodec->time_base,
                                         impl_->audioStream->time_base);
                    if (!writePacket(true, error)) success = false;
                    av_packet_unref(impl_->audioPkt);
                }
            }
            avcodec_send_frame(impl_->audioCodec, nullptr);
            while (avcodec_receive_packet(impl_->audioCodec, impl_->audioPkt) >= 0)
            {
                av_packet_rescale_ts(impl_->audioPkt, impl_->audioCodec->time_base,
                                     impl_->audioStream->time_base);
                impl_->audioPkt->stream_index = impl_->audioStream->index;
                if (!writePacket(true, error)) success = false;
                av_packet_unref(impl_->audioPkt);
            }
        }
        if (!drainPackets(true, error)) success = false;
        const int trailerRet = av_write_trailer(impl_->fmt);
        if (trailerRet < 0)
        {
            setErr(error, "Recording trailer failed: " + ffErr(trailerRet) + " | " + cfg_.path);
            success = false;
        }
    }
    if (impl_)
    {
        for (auto &queue : {&impl_->pendingVideo, &impl_->pendingAudio})
        {
            for (AVPacket *packet : *queue) av_packet_free(&packet);
            queue->clear();
        }
        impl_->pendingBytes = 0;
        if (impl_->sws)
        {
            sws_freeContext(impl_->sws);
            impl_->sws = nullptr;
        }
        if (impl_->pkt)
            av_packet_free(&impl_->pkt);
        if (impl_->audioPkt)
            av_packet_free(&impl_->audioPkt);
        if (impl_->audioFifo)
        {
            av_audio_fifo_free(impl_->audioFifo);
            impl_->audioFifo = nullptr;
        }
        if (impl_->swr)
            swr_free(&impl_->swr);
        if (impl_->audioCodec)
            avcodec_free_context(&impl_->audioCodec);
        if (impl_->codec)
            avcodec_free_context(&impl_->codec);
        if (impl_->fmt)
        {
            if (!(impl_->fmt->oformat->flags & AVFMT_NOFILE) && impl_->fmt->pb)
            {
                const int closeRet = avio_closep(&impl_->fmt->pb);
                if (closeRet < 0)
                {
                    setErr(error, "Recording file close failed: " + ffErr(closeRet) + " | " + cfg_.path);
                    success = false;
                }
            }
            avformat_free_context(impl_->fmt);
            impl_->fmt = nullptr;
        }
        impl_->stream = nullptr;
        impl_->audioStream = nullptr;
        impl_->audioPts = 0;
        impl_->audioTimelineInitialized = false;
        impl_->headerWritten = false;
    }
#endif
    opened_ = false;
    closing_ = false;
    return success;
}
