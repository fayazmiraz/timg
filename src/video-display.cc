// -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
// (c) 2020 Henner Zeller <h.zeller@acm.org>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation version 2.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://gnu.org/licenses/gpl-2.0.txt>

// TODO; help needed.
// * sound output ((platform independently ?)

#include "video-display.h"

#include "image-display.h"
#include "timg-time.h"

#include <mutex>

// libav: "U NO extern C in header ?"
extern "C" {
#  include <libavcodec/avcodec.h>
#  include <libavformat/avformat.h>
#  include <libavutil/imgutils.h>
#  include <libswscale/swscale.h>
}

namespace timg {
// Convert deprecated color formats to new and manually set the color range.
// YUV has funny ranges (16-235), while the YUVJ are 0-255. SWS prefers to
// deal with the YUV range, but then requires to set the output range.
// https://libav.org/documentation/doxygen/master/pixfmt_8h.html#a9a8e335cf3be472042bc9f0cf80cd4c5
static SwsContext *CreateSWSContext(const AVCodecContext *codec_ctx,
                                    int display_width, int display_height) {
    AVPixelFormat src_pix_fmt;
    bool src_range_extended_yuvj = true;
    // Remap deprecated to new pixel format.
    switch (codec_ctx->pix_fmt) {
    case AV_PIX_FMT_YUVJ420P: src_pix_fmt = AV_PIX_FMT_YUV420P; break;
    case AV_PIX_FMT_YUVJ422P: src_pix_fmt = AV_PIX_FMT_YUV422P; break;
    case AV_PIX_FMT_YUVJ444P: src_pix_fmt = AV_PIX_FMT_YUV444P; break;
    case AV_PIX_FMT_YUVJ440P: src_pix_fmt = AV_PIX_FMT_YUV440P; break;
    default:
        src_range_extended_yuvj = false;
        src_pix_fmt = codec_ctx->pix_fmt;
    }
    SwsContext *swsCtx = sws_getContext(codec_ctx->width, codec_ctx->height,
                                        src_pix_fmt,
                                        display_width, display_height,
                                        AV_PIX_FMT_RGB24,
                                        SWS_BILINEAR, NULL, NULL, NULL);
    if (src_range_extended_yuvj) {
        // Manually set the source range to be extended. Read modify write.
        int dontcare[4];
        int src_range, dst_range;
        int brightness, contrast, saturation;
        sws_getColorspaceDetails(swsCtx, (int**)&dontcare, &src_range,
                                 (int**)&dontcare, &dst_range, &brightness,
                                 &contrast, &saturation);
        const int* coefs = sws_getCoefficients(SWS_CS_DEFAULT);
        src_range = 1;  // New src range.
        sws_setColorspaceDetails(swsCtx, coefs, src_range, coefs, dst_range,
                                 brightness, contrast, saturation);
    }
    return swsCtx;
}

static void OnceInitialize() {
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    av_register_all();
#endif
    avformat_network_init();
}

VideoLoader::VideoLoader() {
    static std::once_flag init;
    std::call_once(init, OnceInitialize);
}

VideoLoader::~VideoLoader() {
    avcodec_close(codec_context_);
    sws_freeContext(sws_context_);
    av_frame_free(&output_frame_);
    avformat_close_input(&format_context_);
    delete terminal_fb_;
}

const char *VideoLoader::VersionInfo() {
    return "libav " AV_STRINGIFY(LIBAVFORMAT_VERSION);
}

bool VideoLoader::LoadAndScale(const char *filename,
                               int screen_width, int screen_height,
                               const DisplayOptions &display_options) {
    if (strcmp(filename, "-") == 0) {
        filename = "/dev/stdin";
    }
    format_context_ = avformat_alloc_context();
    int ret;
    if ((ret = avformat_open_input(&format_context_, filename, NULL, NULL)) != 0) {
        char msg[100];
        av_strerror(ret, msg, sizeof(msg));
        fprintf(stderr, "%s: %s\n", filename, msg);
        return false;
    }

    if (avformat_find_stream_info(format_context_, NULL) < 0) {
        fprintf(stderr, "Couldn't find stream information\n");
        return false;
    }

    // Find the first video stream
    AVCodecParameters *codec_parameters = nullptr;
    AVCodec *av_codec = nullptr;
    for (int i = 0; i < (int)format_context_->nb_streams; ++i) {
        codec_parameters = format_context_->streams[i]->codecpar;
        av_codec = avcodec_find_decoder(codec_parameters->codec_id);
        if (!av_codec) continue;
        if (codec_parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_ = i;
            break;
        }
    }
    if (video_stream_index_ == -1)
        return false;

    auto stream = format_context_->streams[video_stream_index_];
    AVRational rate = av_guess_frame_rate(format_context_, stream, nullptr);
    frame_duration_ = Duration::Nanos(1e9 * rate.den / rate.num);

    codec_context_ = avcodec_alloc_context3(av_codec);
    if (avcodec_parameters_to_context(codec_context_, codec_parameters) < 0)
        return false;
    if (avcodec_open2(codec_context_, av_codec, NULL) < 0)
        return false;

    /*
     * Prepare frame to hold the scaled target frame to be send to matrix.
     */
    int target_width = 0;
    int target_height = 0;

    // Make display fit within canvas using the timg scaling utility.
    DisplayOptions opts(display_options);
    // Make sure we don't confuse users. Some image URLs actually end up here,
    // so make sure that it is clear certain options won't work.
    // TODO: this is a crude work-around. And while we tell the user what to
    // do, it would be better if we'd dealt with it already.
    if (opts.crop_border != 0 || opts.auto_trim_image) {
        const bool is_url = (strncmp(filename, "http://", 7) == 0 ||
                             strncmp(filename, "https://", 8) == 0);
        fprintf(stderr, "%s%s is handled by video subsystem. "
                "Unfortunately, no -T trimming feature is implemented there.\n",
                is_url ? "URL " : "", filename);
        if (is_url) {
            fprintf(stderr, "use:\n\twget -qO- %s | timg -T%d -\n... instead "
                    "for this to work\n", filename, opts.crop_border);
        }
    }
    opts.fill_height = false;  // This only makes sense for horizontal scroll.
    ScaleToFit(codec_context_->width, codec_context_->height,
               screen_width, screen_height, opts,
               &target_width, &target_height);

    if (display_options.center_horizontally) {
        center_indentation_ = (screen_width - target_width)/2;
    }
    // initialize SWS context for software scaling
    sws_context_ = CreateSWSContext(codec_context_,
                                    target_width, target_height);
    if (!sws_context_) {
        fprintf(stderr, "Trouble doing scaling to %dx%d :(\n",
                screen_width, screen_height);
        return false;
    }

    // The output_frame_ will receive the scaled result.
    output_frame_ = av_frame_alloc();
    if (av_image_alloc(output_frame_->data, output_frame_->linesize,
                       target_width, target_height, AV_PIX_FMT_RGB24,
                       64) < 0) {
        return false;
    }

    // Framebuffer to interface with the timg TerminalCanvas
    terminal_fb_ = new timg::Framebuffer(target_width, target_height);
    return true;
}

bool VideoLoader::DecodePacket(AVPacket *packet, AVFrame *output_frame) {
    if (avcodec_send_packet(codec_context_, packet) < 0)
        return false;

    // TODO: the API looks like we could possibly call receive frame multiple
    // times, possibly for some queued multiple frames ? In that case we
    // should do something for each frame we get.
    return avcodec_receive_frame(codec_context_, output_frame) == 0;
}

void VideoLoader::CopyToFramebuffer(const AVFrame *av_frame) {
    struct Pixel { uint8_t r, g, b; } __attribute__((packed));
    for (int y = 0; y < terminal_fb_->height(); ++y) {
        Pixel *pix = (Pixel*) (av_frame->data[0] + y*av_frame->linesize[0]);
        for (int x = 0; x < terminal_fb_->width(); ++x, ++pix) {
            terminal_fb_->SetPixel(x, y, pix->r, pix->g, pix->b);
        }
    }
}

void VideoLoader::Play(Duration duration,
                       const volatile sig_atomic_t &interrupt_received,
                       timg::TerminalCanvas *canvas) {
    AVPacket *packet = av_packet_alloc();
    bool is_first = true;
    const Time end_time = Time::Now() + duration;
    AVFrame *decode_frame = av_frame_alloc();  // Decode video into this
    timg::Time end_next_frame;
    while (Time::Now() < end_time && !interrupt_received
           && av_read_frame(format_context_, packet) >= 0) {
        if (packet->stream_index == video_stream_index_) {
            // Determine absolute end of this frame now so that we don't include
            // decoding overhead.
            // TODO: skip frames if getting too much behind ?
            end_next_frame.Add(frame_duration_);

            // Decode video frame
            if (DecodePacket(packet, decode_frame)) {
                sws_scale(sws_context_,
                          decode_frame->data, decode_frame->linesize,
                          0, codec_context_->height,
                          output_frame_->data, output_frame_->linesize);
                CopyToFramebuffer(output_frame_);
                if (!is_first) canvas->JumpUpPixels(terminal_fb_->height());
                canvas->Send(*terminal_fb_, center_indentation_);
                is_first = false;
            }
            end_next_frame.WaitUntil();
        }
        av_packet_unref(packet);  // was allocated by av_read_frame
    }
    av_frame_free(&decode_frame);
    av_packet_free(&packet);
}

}  // namespace timg
