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

// TODO
// * I did this a while ago, in the meantime the AV-API complains about
//   a lot of deprecated functionality. Bring this to the latest state
// * (platform independent?) sound output.
// (help needed)

#include "video-display.h"

#include "image-display.h"
#include "timg-time.h"

// libav: "U NO extern C in header ?"
extern "C" {
#  include <libavcodec/avcodec.h>
#  include <libavformat/avformat.h>
#  include <libswscale/swscale.h>
}

namespace timg {
// Convert deprecated color formats to new and manually set the color range.
// YUV has funny ranges (16-235), while the YUVJ are 0-255. SWS prefers to
// deal with the YUV range, but then requires to set the output range.
// https://libav.org/documentation/doxygen/master/pixfmt_8h.html#a9a8e335cf3be472042bc9f0cf80cd4c5
static SwsContext *CreateSWSContext(const AVCodecContext *codec_ctx,
                                    int display_width, int display_height) {
    AVPixelFormat pix_fmt;
    bool src_range_extended_yuvj = true;
    // Remap deprecated to new pixel format.
    switch (codec_ctx->pix_fmt) {
    case AV_PIX_FMT_YUVJ420P: pix_fmt = AV_PIX_FMT_YUV420P; break;
    case AV_PIX_FMT_YUVJ422P: pix_fmt = AV_PIX_FMT_YUV422P; break;
    case AV_PIX_FMT_YUVJ444P: pix_fmt = AV_PIX_FMT_YUV444P; break;
    case AV_PIX_FMT_YUVJ440P: pix_fmt = AV_PIX_FMT_YUV440P; break;
    default:
        src_range_extended_yuvj = false;
        pix_fmt = codec_ctx->pix_fmt;
    }
    SwsContext *swsCtx = sws_getContext(codec_ctx->width, codec_ctx->height,
                                        pix_fmt,
                                        display_width, display_height,
                                        AV_PIX_FMT_RGB24, SWS_BILINEAR,
                                        NULL, NULL, NULL);
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

void VideoLoader::Init() {
    // Register all formats and codecs
    //av_register_all();  // Deprecated. How is it done now ?
    avformat_network_init();
}

VideoLoader::~VideoLoader() {
    av_free(output_buffer_);
    av_frame_free(&output_frame_);

    avcodec_close(codec_context_);
    avcodec_close(codec_ctx_orig_);

    avformat_close_input(&format_context_);
    delete framebuffer_;
}

bool VideoLoader::LoadAndScale(const char *filename,
                               int screen_width, int screen_height,
                               const ScaleOptions &scale_options) {
    AVCodec   *pCodec = NULL;

    if (avformat_open_input(&format_context_, filename, NULL, NULL) != 0) {
        perror("Issue opening file: ");
        return false;
    }

    if (avformat_find_stream_info(format_context_, NULL) < 0) {
        fprintf(stderr, "Couldn't find stream information\n");
        return false;
    }

    // Find the first video stream
    desiredStream_ = -1;
    for (int i = 0; i < (int)format_context_->nb_streams; ++i) {
        if (format_context_->streams[i]->codec->codec_type
            == AVMEDIA_TYPE_VIDEO) {
            desiredStream_ = i;
            break;
        }
    }
    if (desiredStream_ == -1)
        return false;

    // Get a pointer to the codec context for the video stream
    codec_ctx_orig_ = format_context_->streams[desiredStream_]->codec;
    double fps = av_q2d(format_context_->streams[desiredStream_]->avg_frame_rate);
    if (fps < 0) {
        fps = 1.0 / av_q2d(format_context_
                           ->streams[desiredStream_]->codec->time_base);
    }
    frame_duration_ = Duration::Nanos(1e9 / fps);

    // Find the decoder for the video stream
    pCodec=avcodec_find_decoder(codec_ctx_orig_->codec_id);
    if (pCodec==NULL) {
        fprintf(stderr, "Unsupported codec!\n");
        return false;
    }
    // Copy context
    codec_context_ = avcodec_alloc_context3(pCodec);
    if (avcodec_copy_context(codec_context_, codec_ctx_orig_) != 0) {
        fprintf(stderr, "Couldn't copy codec context");
        return false;
    }

    // Open codec
    if (avcodec_open2(codec_context_, pCodec, NULL)<0)
        return false;

    /*
     * Prepare frame to hold the scaled target frame to be send to matrix.
     */
    output_frame_ = av_frame_alloc();  // Target frame for output
    int target_width = screen_width;
    int target_height = screen_height;
    // Make display fit within canvas.
    ScaleWithOptions(codec_context_->width, codec_context_->height,
                     screen_width, screen_height, scale_options,
                     &target_width, &target_height);

    // Allocate buffer to meet output size requirements
    const size_t output_size = avpicture_get_size(AV_PIX_FMT_RGB24,
                                                  target_width,
                                                  target_height);
    output_buffer_ = (uint8_t *) av_malloc(output_size);

    // Assign appropriate parts of buffer to image planes in output_frame.
    // Note that output_frame is an AVFrame, but AVFrame is a superset
    // of AVPicture
    avpicture_fill((AVPicture *)output_frame_, output_buffer_, AV_PIX_FMT_RGB24,
                   target_width, target_height);

    framebuffer_ = new timg::Framebuffer(target_width, target_height);

    // initialize SWS context for software scaling
    sws_context_ = CreateSWSContext(codec_context_,
                                    target_width, target_height);
    if (!sws_context_) {
        fprintf(stderr, "Trouble doing scaling to %dx%d :(\n",
                screen_width, screen_height);
        return false;
    }
    return true;
}

void VideoLoader::CopyToFramebuffer(const AVFrame *av_frame) {
    struct Pixel { uint8_t r, g, b; };
    for (int y = 0; y < framebuffer_->height(); ++y) {
        Pixel *pix = (Pixel*) (av_frame->data[0] + y*av_frame->linesize[0]);
        for (int x = 0; x < framebuffer_->width(); ++x, ++pix) {
            framebuffer_->SetPixel(x, y, pix->r, pix->g, pix->b);
        }
    }
}

void VideoLoader::Play(Duration duration,
                       const volatile sig_atomic_t &interrupt_received,
                       timg::TerminalCanvas *canvas) {
    AVPacket packet;
    int frameFinished;
    bool is_first = true;
    const Time end_time = Time::Now() + duration;
    AVFrame *decode_frame = av_frame_alloc();  // Decode video into this
    timg::Time end_next_frame;
    while (Time::Now() < end_time && !interrupt_received
           && av_read_frame(format_context_, &packet) >= 0) {
        // Is this a packet from the video stream?
        if (packet.stream_index == desiredStream_) {
            // Determine absolute end of this frame now so that we don't include
            // decoding overhead. TODO: skip frames if getting too slow ?
            end_next_frame.Add(frame_duration_);

            // Decode video frame
            avcodec_decode_video2(codec_context_, decode_frame, &frameFinished, &packet);
            // Did we get a video frame?
            if (frameFinished) {
                // Convert the image from its native format to RGB
                sws_scale(sws_context_,
                          (uint8_t const * const *)decode_frame->data,
                          decode_frame->linesize, 0, codec_context_->height,
                          output_frame_->data, output_frame_->linesize);
                CopyToFramebuffer(output_frame_);
                if (!is_first) canvas->JumpUpPixels(framebuffer_->height());
                canvas->Send(*framebuffer_);
                is_first = false;
            }
            end_next_frame.WaitUntil();
        }
        av_free_packet(&packet);  // was allocated by av_read_frame
    }
    av_frame_free(&decode_frame);
}

}  // namespace timg
