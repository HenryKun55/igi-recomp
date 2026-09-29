/* Video frame decoding for the AVI cutscenes (Indeo Video 5). */
#ifndef IGI_VIDEO_H
#define IGI_VIDEO_H

#include <stddef.h>
#include <stdint.h>

typedef struct VideoDecoder VideoDecoder;

/* Returns NULL when no decoder is available for the codec. */
VideoDecoder *video_decoder_open(uint32_t fourcc, int width, int height);
/* Decode one compressed frame (kept inside the decoder). Returns 0 on success. */
int video_decode(VideoDecoder *d, const uint8_t *data, size_t size);
/* Convert the last decoded frame to width*height RGBA8 (R in the low byte, top row first). */
void video_frame_rgba(VideoDecoder *d, uint32_t *rgba);
void video_decoder_close(VideoDecoder *d);

#endif
