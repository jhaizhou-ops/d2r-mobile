/* FFmpeg-backed video API; the reader provides random-access container bytes. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef int64_t (*dm_movie_read)(void *, int64_t, void *, size_t);
typedef struct dm_movie_decode dm_movie_decode;
typedef struct { int width, height; int64_t duration_us; } dm_movie_info;
typedef int (*dm_movie_frame)(void *, const uint8_t *, size_t, int64_t);

dm_movie_decode *dm_movie_decode_open(dm_movie_read, void *, dm_movie_info *, char *, size_t);
void dm_movie_decode_close(dm_movie_decode *);
/* 1 = packet, 0 = end; data remains valid until the next read. */
int dm_movie_decode_read(dm_movie_decode *, const uint8_t **, int *, int64_t *);
int dm_movie_decode_send(dm_movie_decode *, int, dm_movie_frame, void *);
int dm_movie_decode_seek(dm_movie_decode *, int64_t);
