/* Release builds load code from HAP-signed ELF segments, never copied anonymous pages.
 * Library images are generated from exactly the same offline output as the runtime pack.
 * Data/GOT/key pages remain writable; executable segments are never modified here.
 */
#include "signed_guest_image.h"
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(dm_signed_guest_descriptor) == 144, "guest descriptor ABI");
static const char *const names[] = {"main", "sdk"};
static struct {
    void *handle, *image;
    const dm_signed_guest_descriptor *descriptor;
} guests[2];
static int ready;

static int matches_file(const char *path, const unsigned char expected[32]) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    unsigned char buf[65536], actual[32];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) != 0) CC_SHA256_Update(&ctx, buf, (CC_LONG)n);
    int good = !ferror(f);
    fclose(f);
    CC_SHA256_Final(actual, &ctx);
    return good && !memcmp(actual, expected, sizeof actual);
}

int dm_signed_guest_prepare(const char *dir, char *error, size_t error_size) {
    if (ready) return 1;
    for (unsigned i = 0; i < 2; ++i) {
        char library[80], path[1200];
        snprintf(library, sizeof library, "libdm_guest_%s.so", names[i]);
        if (!guests[i].handle) guests[i].handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
        if (!guests[i].handle) {
            const char *why = dlerror();
            snprintf(error, error_size, "%s: %s", library, why ? why : "dlopen failed");
            return 0;
        }
        guests[i].image = dlsym(guests[i].handle, "dm_guest_image");
        guests[i].descriptor = dlsym(guests[i].handle, "dm_guest_descriptor");
        const dm_signed_guest_descriptor *d = guests[i].descriptor;
        if (!guests[i].image || !d || d->magic != UINT64_C(0x444d5349474e3031) || d->version != 1 ||
            !d->raw_size || d->raw_size > d->mapped_size || d->mapped_size - d->raw_size >= 0x4000 ||
            ((uintptr_t)guests[i].image & 0x3fff) || (d->mapped_size & 0x3fff) ||
            !d->text_end || d->text_end > d->key_page_off ||
            d->key_page_size != 0x4000 || (d->key_page_off & 0x3fff) ||
            d->key_page_off > d->patch_off || d->patch_off - d->key_page_off < d->key_page_size ||
            (d->patch_off & 0x3fff) || d->patch_off >= d->raw_size) {
            snprintf(error, error_size, "%s: invalid signed image descriptor", library);
            return 0;
        }
        int n = snprintf(path, sizeof path, "%s/%s.patched.bin", dir, names[i]);
        if (n < 0 || (size_t)n >= sizeof path || !matches_file(path, d->image_sha256)) {
            snprintf(error, error_size, "%s: runtime image does not match the signed library", names[i]);
            return 0;
        }
        n = snprintf(path, sizeof path, "%s/%s.fixups.tsv", dir, names[i]);
        if (n < 0 || (size_t)n >= sizeof path || !matches_file(path, d->fixups_sha256)) {
            snprintf(error, error_size, "%s: runtime layout does not match the signed library", names[i]);
            return 0;
        }
    }
    ready = 1;
    return 1;
}

void *dm_signed_guest_image(const char *name, const dm_signed_guest_descriptor **descriptor) {
    if (!ready) return NULL;
    for (unsigned i = 0; i < 2; ++i) if (!strcmp(name, names[i])) {
        *descriptor = guests[i].descriptor;
        return guests[i].image;
    }
    return NULL;
}
