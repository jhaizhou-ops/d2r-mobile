/* Ejecuta el protocolo del juego con un WebM original y una referencia NV12 de
   su primer fotograma. No necesita ventana, audio ni parches del ejecutable. */
#include "../hle_movie.c"
#include <assert.h>
#include <CommonCrypto/CommonDigest.h>
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
    assert(movie_output(dec, index, &buffer) == ST_OK);
    uint64_t hash = fingerprint(pixels, count);
    assert(movie_release_output(dec, index) == ST_OK);
    mv_MediaDataD(&meta);
    return hash;
}
int main(int argc, char **argv) {
    assert(argc == 3);
    void *vt[5] = {NULL, NULL, reader_open, reader_read, reader_close}; void *reader[3] = {vt, NULL, NULL};
    void **ext = calloc(2, sizeof(void *)), **dec = calloc(2, sizeof(void *));
    mv_SetAllocator(NULL, free_test, NULL, NULL);
    mv_Extractor(ext, 0); assert(movie_source(ext, reader, argv[1]) == ST_OK);
    int count = 0; assert(movie_tracks(ext, &count) == ST_OK && count == 1);
    void *meta = NULL; mv_MediaData(&meta); assert(movie_track(ext, 0, &meta) == ST_OK);
    const char *mime = NULL; int64_t duration = 0;
    assert(mv_FindString(&meta, "mime", &mime) && !strcmp(mime, "video/x-vnd.on2.vp8"));
    assert(mv_FindInt64(&meta, "durationUs", &duration) && duration > 0);
    mv_Decoder(dec, 2, 4, 1, 16); assert(movie_configure(dec, &meta) == ST_OK); mv_MediaDataD(&meta);
    void *events[2] = {&opens, &closes}; movie_events(dec, events); assert(movie_start(dec) == ST_OK);
    size_t bytes = (size_t)movie(dec)->info.width * movie(dec)->info.height * 3 / 2;
    uint8_t *pixels = malloc(bytes), *reference = malloc(bytes);
    FILE *ref = fopen(argv[2], "rb"); assert(ref && fread(reference, 1, bytes, ref) == bytes); fclose(ref);
    int frames = 0, eof = 0, varied = 0; int64_t previous = -1; uint64_t first = 0;
    CC_SHA256_CTX digest; CC_SHA256_Init(&digest);
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
                CC_SHA256_Update(&digest, pixels, (CC_LONG)bytes);
                assert(output_pts >= previous); previous = output_pts;
                if (!frames) { assert(!memcmp(pixels, reference, bytes)); first = h; }
                else varied += h != first;
                frames++;
            }
        }
    }
    assert(frames > 30 && varied > 10 && previous < duration && signals > frames);
    uint8_t sha[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(sha, &digest);
    printf("NV12_SHA256="); for (size_t i = 0; i < sizeof sha; i++) printf("%02x", sha[i]); putchar('\n');
    printf("Vídeo real: %d fotogramas, PTS final %lld us, %d señales; referencia NV12 idéntica.\n", frames, (long long)previous, signals);
    /* Salto de bucle: el juego envía un paquete vacío tras SeekTo, sin EOS. */
    assert(movie_seek(ext, 0) == ST_OK); assert(movie_send(dec, 0, 0, 0, 0, 0) == ST_OK);
    mvbuf buf; movie_input(dec, 0, &buf); assert(movie_read_packet(ext, &buf) == ST_OK);
    assert(movie_send(dec, 0, 0, buf.size, 0, 0) == ST_OK);
    int64_t pts; int eos; assert(consume(dec, pixels, bytes, &eos, &pts) == first && !eos && pts == 0);
    /* Backpressure: no reutilizar un fotograma adquirido y aún no liberado. */
    for (int i = 0; i < MV_SLOTS; i++) {
        assert(movie_read_packet(ext, &buf) == ST_OK); assert(movie_send(dec, 0, 0, buf.size, 0, 0) == ST_OK);
    }
    int index, remaining; movie_input_index(dec, &index, &remaining); assert(index == -1);
    movie_stop(dec); movie_input_index(dec, &index, &remaining); assert(index == -1);
    movie_delete(dec); movie_delete(ext); assert(opens == 1 && closes == 1 && frees == 2);
    free(pixels); free(reference);
    puts("Bucle, paquete vacío, contrapresión, salto y cierre: correctos.");
    return 0;
}
