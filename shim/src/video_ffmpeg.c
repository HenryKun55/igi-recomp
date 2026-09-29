/* libavcodec-backed video decoder (built only when FFmpeg is available). */
#include "video.h"

#if defined(IGI_HAVE_FFMPEG)

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <stdlib.h>
#include <string.h>

struct VideoDecoder {
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
    int width, height;
    int have_frame;
};

static enum AVCodecID codec_for(uint32_t fourcc)
{
    char f[5] = { (char)(fourcc & 0xFF), (char)((fourcc >> 8) & 0xFF), (char)((fourcc >> 16) & 0xFF),
                  (char)(fourcc >> 24), 0 };
    for (int i = 0; i < 4; i++)
        if (f[i] >= 'a' && f[i] <= 'z')
            f[i] = (char)(f[i] - 32);
    if (strcmp(f, "IV50") == 0)
        return AV_CODEC_ID_INDEO5;
    if (strcmp(f, "IV41") == 0)
        return AV_CODEC_ID_INDEO4;
    if (strcmp(f, "IV32") == 0 || strcmp(f, "IV31") == 0)
        return AV_CODEC_ID_INDEO3;
    return AV_CODEC_ID_NONE;
}

VideoDecoder *video_decoder_open(uint32_t fourcc, int width, int height)
{
    enum AVCodecID id = codec_for(fourcc);
    if (id == AV_CODEC_ID_NONE)
        return NULL;
    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec)
        return NULL;
    VideoDecoder *d = calloc(1, sizeof(*d));
    d->ctx = avcodec_alloc_context3(codec);
    d->ctx->width = width;
    d->ctx->height = height;
    d->ctx->codec_tag = fourcc;
    if (avcodec_open2(d->ctx, codec, NULL) < 0) {
        avcodec_free_context(&d->ctx);
        free(d);
        return NULL;
    }
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    d->width = width;
    d->height = height;
    return d;
}

static uint8_t clamp8(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

int video_decode(VideoDecoder *d, const uint8_t *data, size_t size)
{
    d->pkt->data = (uint8_t *)data;
    d->pkt->size = (int)size;
    if (avcodec_send_packet(d->ctx, d->pkt) < 0)
        return -1;
    if (avcodec_receive_frame(d->ctx, d->frame) < 0)
        return -1;
    d->have_frame = 1;
    return 0;
}

void video_frame_rgba(VideoDecoder *d, uint32_t *rgba)
{
    AVFrame *f = d->frame;
    if (!d->have_frame)
        return;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get((enum AVPixelFormat)f->format);
    if (!desc || !(desc->flags & AV_PIX_FMT_FLAG_PLANAR) || desc->nb_components < 3)
        return;
    int sx = desc->log2_chroma_w, sy = desc->log2_chroma_h;
    int w = f->width < d->width ? f->width : d->width;
    int h = f->height < d->height ? f->height : d->height;
    for (int y = 0; y < h; y++) {
        const uint8_t *py = f->data[0] + y * f->linesize[0];
        const uint8_t *pu = f->data[1] + (y >> sy) * f->linesize[1];
        const uint8_t *pv = f->data[2] + (y >> sy) * f->linesize[2];
        uint32_t *out = rgba + (size_t)y * (size_t)d->width;
        for (int x = 0; x < w; x++) {
            int yy = (py[x] - 16) * 298;
            int u = pu[x >> sx] - 128, v = pv[x >> sx] - 128;
            uint8_t r = clamp8((yy + 409 * v + 128) >> 8);
            uint8_t g = clamp8((yy - 100 * u - 208 * v + 128) >> 8);
            uint8_t b = clamp8((yy + 516 * u + 128) >> 8);
            out[x] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xFF000000u;
        }
    }
}

void video_decoder_close(VideoDecoder *d)
{
    if (!d)
        return;
    av_frame_free(&d->frame);
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->ctx);
    free(d);
}

#else

VideoDecoder *video_decoder_open(uint32_t fourcc, int width, int height)
{
    (void)fourcc;
    (void)width;
    (void)height;
    return NULL;
}

int video_decode(VideoDecoder *d, const uint8_t *data, size_t size)
{
    (void)d;
    (void)data;
    (void)size;
    return -1;
}

void video_frame_rgba(VideoDecoder *d, uint32_t *rgba)
{
    (void)d;
    (void)rgba;
}

void video_decoder_close(VideoDecoder *d) { (void)d; }

#endif
