/* FFmpeg 9 custom-reader demuxing and VP8 decoding to packed NV12.
   Guest movie:: events and buffer ownership are handled by hle_movie.c. */
#ifdef DM_MOVIE_FFMPEG
#include "dm_movie_decode.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

struct dm_movie_decode {
    dm_movie_read read;
    void *opaque;
    int64_t pos;
    AVFormatContext *format;
    AVIOContext *io;
    AVCodecContext *codec;
    AVPacket *packet;
    AVFrame *frame;
    struct SwsContext *scale;
    int stream, width, height;
    uint8_t *nv12;
    size_t nv12_size;
};

static int read_bytes(void *opaque, uint8_t *dst, int count) {
    dm_movie_decode *d = opaque;
    int64_t got = d->read(d->opaque, d->pos, dst, (size_t)count);
    if (got < 0 || got > count) return AVERROR(EIO);
    if (!got) return AVERROR_EOF;
    d->pos += got;
    return (int)got;
}

static int64_t seek_bytes(void *opaque, int64_t off, int mode) {
    dm_movie_decode *d = opaque;
    mode &= ~AVSEEK_FORCE;
    if (mode == AVSEEK_SIZE || mode == SEEK_END) return AVERROR(ENOSYS);
    int64_t base = mode == SEEK_CUR ? d->pos : 0;
    if ((mode != SEEK_SET && mode != SEEK_CUR) || off < -base || off > INT64_MAX - base)
        return AVERROR(EINVAL);
    d->pos = base + off;
    return d->pos;
}

void dm_movie_decode_close(dm_movie_decode *d) {
    if (!d) return;
    av_packet_free(&d->packet);
    av_frame_free(&d->frame);
    avcodec_free_context(&d->codec);
    avformat_close_input(&d->format);
    if (d->io) av_freep(&d->io->buffer);
    avio_context_free(&d->io);
    sws_freeContext(d->scale);
    av_free(d->nv12);
    free(d);
}

dm_movie_decode *dm_movie_decode_open(dm_movie_read read, void *opaque, dm_movie_info *info, char *error, size_t error_cap) {
    int result = AVERROR(ENOMEM);
    dm_movie_decode *d = calloc(1, sizeof *d);
    if (!d) goto fail;
    d->read = read; d->opaque = opaque;
    d->format = avformat_alloc_context();
    uint8_t *buffer = av_malloc(65536);
    if (!d->format || !buffer) { av_free(buffer); goto fail; }
    d->io = avio_alloc_context(buffer, 65536, 0, d, read_bytes, NULL, seek_bytes);
    if (!d->io) { av_free(buffer); goto fail; }
    d->format->pb = d->io;
    d->format->flags |= AVFMT_FLAG_CUSTOM_IO;
    result = avformat_open_input(&d->format, NULL, av_find_input_format("matroska,webm"), NULL);
    if (result < 0) goto fail;
    result = avformat_find_stream_info(d->format, NULL);
    if (result < 0) goto fail;
    d->stream = av_find_best_stream(d->format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (d->stream < 0) { result = d->stream; goto fail; }
    AVCodecParameters *p = d->format->streams[d->stream]->codecpar;
    if (p->codec_id != AV_CODEC_ID_VP8 || p->width <= 0 || p->height <= 0 ||
        (p->width & 1) || (p->height & 1) || p->width > 4096 || p->height > 4096) {
        result = AVERROR_INVALIDDATA; goto fail;
    }
    d->width = p->width; d->height = p->height;
    d->codec = avcodec_alloc_context3(avcodec_find_decoder(p->codec_id));
    if (!d->codec) { result = AVERROR(ENOMEM); goto fail; }
    result = avcodec_parameters_to_context(d->codec, p);
    if (result < 0) goto fail;
    /* Keep SDK input and event delivery on the guest thread. */
    d->codec->thread_count = 1;
    result = avcodec_open2(d->codec, avcodec_find_decoder(p->codec_id), NULL);
    if (result < 0) goto fail;
    d->packet = av_packet_alloc(); d->frame = av_frame_alloc();
    d->nv12_size = (size_t)d->width * d->height * 3 / 2;
    d->nv12 = av_malloc(d->nv12_size);
    if (!d->packet || !d->frame || !d->nv12) { result = AVERROR(ENOMEM); goto fail; }
    info->width = d->width; info->height = d->height;
    info->duration_us = d->format->duration;
    if (info->duration_us <= 0 || info->duration_us == AV_NOPTS_VALUE) { result = AVERROR_INVALIDDATA; goto fail; }
    return d;
fail:
    if (error_cap) av_strerror(result, error, error_cap);
    dm_movie_decode_close(d);
    return NULL;
}

int dm_movie_decode_read(dm_movie_decode *d, const uint8_t **data, int *size, int64_t *pts) {
    for (;;) {
        av_packet_unref(d->packet);
        int r = av_read_frame(d->format, d->packet);
        if (r == AVERROR_EOF) return 0;
        if (r < 0) return r;
        if (d->packet->stream_index != d->stream) continue;
        if (d->packet->pts == AV_NOPTS_VALUE) return AVERROR_INVALIDDATA;
        *data = d->packet->data; *size = d->packet->size;
        *pts = av_rescale_q(d->packet->pts, d->format->streams[d->stream]->time_base, AV_TIME_BASE_Q);
        return 1;
    }
}

int dm_movie_decode_send(dm_movie_decode *d, int eos, dm_movie_frame deliver, void *opaque) {
    int r = avcodec_send_packet(d->codec, eos ? NULL : d->packet);
    if (r < 0 && r != AVERROR_EOF) return r;
    while ((r = avcodec_receive_frame(d->codec, d->frame)) >= 0) {
        AVFrame *f = d->frame;
        if (f->width != d->width || f->height != d->height) return AVERROR_INVALIDDATA;
        d->scale = sws_getCachedContext(d->scale, f->width, f->height, f->format,
            d->width, d->height, AV_PIX_FMT_NV12, SWS_POINT, NULL, NULL, NULL);
        if (!d->scale) return AVERROR(ENOMEM);
        uint8_t *planes[4] = {d->nv12, d->nv12 + (size_t)d->width * d->height, NULL, NULL};
        int strides[4] = {d->width, d->width, 0, 0};
        if (sws_scale(d->scale, (const uint8_t *const *)f->data, f->linesize, 0, d->height, planes, strides) != d->height)
            return AVERROR_INVALIDDATA;
        int64_t pts = av_rescale_q(f->best_effort_timestamp, d->format->streams[d->stream]->time_base, AV_TIME_BASE_Q);
        int delivered = deliver(opaque, d->nv12, d->nv12_size, pts);
        av_frame_unref(f);
        if (delivered < 0) return delivered;
    }
    return r == AVERROR(EAGAIN) || r == AVERROR_EOF ? 0 : r;
}

int dm_movie_decode_seek(dm_movie_decode *d, int64_t pts) {
    int64_t at = av_rescale_q(pts, AV_TIME_BASE_Q, d->format->streams[d->stream]->time_base);
    int r = av_seek_frame(d->format, d->stream, at, AVSEEK_FLAG_BACKWARD);
    if (r < 0) return r;
    avcodec_flush_buffers(d->codec);
    av_packet_unref(d->packet);
    return 0;
}
#endif
