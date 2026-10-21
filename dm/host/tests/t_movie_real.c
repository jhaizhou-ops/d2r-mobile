/* Test the movie:: VP8 protocol with generated WebM and a raw NV12 reference.
   No game files, window, audio device, or executable patches are required. */
#include "../hle_movie.c"
#include <assert.h>
#include <libavutil/sha.h>
#include <libavutil/mem.h>
#include <fcntl.h>
#include <unistd.h>

static int opens, closes, signals, frees;
static void signal_test(void *event) { assert(event); signals++; }
void *guest_sym(const char *name) {
    return !strcmp(name, "_ZN2nn2os11SignalEventEPNS0_9EventTypeE") ? signal_test : NULL;
}
static unsigned reader_open(void *self, const char *path, void **handle) {
    (void)self; int fd = open(path, O_RDONLY); if (fd < 0) return 0;
    *handle = (void *)(intptr_t)(fd + 1); opens++; return 1;
}
static int64_t reader_read(void *self, void *handle, int64_t off, void *data, size_t size) {
    (void)self; return pread((int)(intptr_t)handle - 1, data, size, off);
}
static void reader_close(void *self, void *handle) {
    (void)self; assert(!close((int)(intptr_t)handle - 1)); closes++;
}
static void free_test(void *self, void *user) { (void)user; frees++; free(self); }
static int64_t failed_read(void *self, int64_t off, void *data, size_t size) {
    (void)self; (void)off; (void)data; (void)size; return -1;
}
static uint64_t fingerprint(const uint8_t *data, size_t size) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < size; i++) h = (h ^ data[i]) * 1099511628211ull;
    return h;
}
static uint64_t consume(void *dec, uint8_t *pixels, size_t count, int *eos, int64_t *pts) {
    int index, remaining; unsigned flags;
    assert(movie_output_index(dec, &index, pts, &flags, &remaining) == ST_OK);
    void *meta = NULL; mv_MediaData(&meta);
    int status = movie_format(dec, index, &meta);
    *eos = !!(flags & 4);
    if (*eos) { assert(status == ST_EOS); movie_release_output(dec, index); mv_MediaDataD(&meta); return 0; }
    assert(status == ST_OK);
    int width = 0, height = 0, offset = -1;
    assert(mv_FindInt32(&meta, "width", &width));
    assert(mv_FindInt32(&meta, "height", &height));
    assert(mv_FindInt32(&meta, "nv12-uv-offset", &offset));
    assert(offset == width * height && count == (size_t)width * height * 3 / 2);
    mvbuf buffer; mv_Buffer(&buffer); mv_BufSetData(&buffer, pixels, count); mv_BufSetRange(&buffer, 0, (int)count);
    assert(movie_output(dec, -1, &buffer) == ST_TRY_AGAIN);
    buffer.cap = count - 1;
    assert(movie_output(dec, index, &buffer) == ST_TRY_AGAIN);
    buffer.cap = count;
    assert(movie_output(dec, index, &buffer) == ST_OK);
    uint64_t hash = fingerprint(pixels, count);
    assert(movie_release_output(dec, index) == ST_OK);
    mv_MediaDataD(&meta);
    return hash;
}
int main(int argc, char **argv) {
    assert(argc == 3);
    char error[160] = {0}; dm_movie_info info;
    assert(!dm_movie_decode_open(failed_read, NULL, &info, error, sizeof error) && error[0]);
    dm_movie_decode_close(NULL);
    void *vt[5] = {NULL, NULL, reader_open, reader_read, reader_close}; void *reader[3] = {vt, NULL, NULL};
    void **ext = calloc(2, sizeof(void *)), **dec = calloc(2, sizeof(void *));
    mv_SetAllocator(NULL, free_test, NULL, NULL);
    mv_Extractor(ext, 0); assert(movie_source(ext, reader, argv[1]) == ST_OK);
    int count = 0; assert(movie_tracks(ext, &count) == ST_OK && count == 1);
    void *invalid_meta = NULL;
    assert(movie_track(ext, 1, &invalid_meta) == ST_TRY_AGAIN && !invalid_meta);
    void *meta = NULL; mv_MediaData(&meta); assert(movie_track(ext, 0, &meta) == ST_OK);
    const char *mime = NULL; int64_t duration = 0;
    assert(mv_FindString(&meta, "mime", &mime) && !strcmp(mime, "video/x-vnd.on2.vp8"));
    assert(mv_FindInt64(&meta, "durationUs", &duration) && duration > 0);
    mv_Decoder(dec, 2, 4, 1, 16); assert(movie_configure(dec, &meta) == ST_OK); mv_MediaDataD(&meta);
    void *events[2] = {&opens, &closes}; movie_events(dec, events); assert(movie_start(dec) == ST_OK);
    size_t bytes = (size_t)movie(dec)->info.width * movie(dec)->info.height * 3 / 2;
    uint8_t *pixels = malloc(bytes), *reference = malloc(bytes);
    FILE *ref = fopen(argv[2], "rb"); assert(ref && fread(reference, 1, bytes, ref) == bytes); fclose(ref);
    int frames = 0, eof = 0, varied = 0; int64_t previous = -1, first_pts = -1; uint64_t first = 0;
    struct AVSHA *digest = av_sha_alloc(); assert(digest); av_sha_init(digest, 256);
    while (!eof) {
        int index, remaining; movie_input_index(dec, &index, &remaining); assert(index == 0);
        mvbuf buf; movie_input(dec, index, &buf); mv_BufSetRange(&buf, 0, 0);
        int r = movie_read_packet(ext, &buf); assert(r == ST_OK || r == ST_EOS);
        int64_t pts; movie_time(ext, &pts);
        assert(movie_send(dec, index, buf.off, buf.size, pts, r == ST_EOS ? 4 : 0) == ST_OK);
        movie_advance(ext);
        if (movie(dec)->frames[0].state || r == ST_EOS) {
            int64_t output_pts; uint64_t h = consume(dec, pixels, bytes, &eof, &output_pts);
            if (!eof) {
                av_sha_update(digest, pixels, bytes);
                assert(output_pts >= previous); previous = output_pts;
                if (!frames) { assert(!memcmp(pixels, reference, bytes)); first = h; first_pts = output_pts; }
                else varied += h != first;
                frames++;
            }
        }
    }
    assert(frames > 30 && varied > 10 && previous < duration && signals > frames);
    uint8_t sha[32]; av_sha_final(digest, sha); av_free(digest);
    printf("NV12_SHA256="); for (size_t i = 0; i < sizeof sha; i++) printf("%02x", sha[i]); putchar('\n');
    printf("VP8: %d frames, final PTS %lld us, %d signals; first NV12 frame matches.\n", frames, (long long)previous, signals);
    /* The guest sends an empty packet after loop SeekTo, without EOS. */
    assert(movie_seek(ext, 0) == ST_OK); assert(movie_send(dec, 0, 0, 0, 0, 0) == ST_OK);
    mvbuf buf; movie_input(dec, 0, &buf); assert(movie_read_packet(ext, &buf) == ST_OK);
    assert(movie_send(dec, 0, 0, buf.size, 0, 0) == ST_OK);
    int64_t pts; int eos; assert(consume(dec, pixels, bytes, &eos, &pts) == first && !eos && pts == first_pts);
    /* Backpressure must not reuse an acquired, unreleased frame. */
    for (int i = 0; i < MV_SLOTS; i++) {
        assert(movie_read_packet(ext, &buf) == ST_OK); assert(movie_send(dec, 0, 0, buf.size, 0, 0) == ST_OK);
    }
    int index, remaining; movie_input_index(dec, &index, &remaining); assert(index == -1);
    int acquired; int64_t acquired_pts; unsigned acquired_flags;
    assert(movie_output_index(dec, &acquired, &acquired_pts, &acquired_flags, &remaining) == ST_OK);
    movie_input_index(dec, &index, &remaining); assert(index == -1);
    assert(movie_release_output(dec, acquired) == ST_OK);
    movie_input_index(dec, &index, &remaining); assert(index == 0);
    movie_stop(dec); movie_input_index(dec, &index, &remaining); assert(index == -1);
    assert(movie_send(dec, 0, 0, 0, 0, 0) == ST_TRY_AGAIN);
    movie_delete(dec); assert(closes == 0 && frees == 1);
    movie_delete(ext); assert(opens == 1 && closes == 1 && frees == 2);
    free(pixels); free(reference);
    puts("Loop, empty packet, backpressure, skip, and teardown: passed.");
    return 0;
}
