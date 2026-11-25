/* Training package only. Dump before force-stop without changing the control
 * flow of profiled functions between the generate and use builds. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

extern void __llvm_profile_set_filename(const char *);
extern int __llvm_profile_write_file(void);

static void *publish_profile(void *unused) {
    (void)unused;
    char completed[160], temporary[192];
    snprintf(completed, sizeof completed, "/data/storage/el2/base/haps/entry/files/dm/h12b-%d.profraw", getpid());
    /* The shell cannot create a request inside this sandbox. The collector can
     * read the last completed cumulative snapshot at any time, even after stop. */
    for (unsigned sequence = 0; ; ++sequence) {
        sleep(sequence ? 30 : 5);
        snprintf(temporary, sizeof temporary, "%s-%u.partial", completed, sequence);
        __llvm_profile_set_filename(temporary);
        if (__llvm_profile_write_file()) fprintf(stderr, "H12 PGO write failed sequence=%u\n", sequence);
        else if (rename(temporary, completed)) fprintf(stderr, "H12 PGO rename errno=%d\n", errno);
    }
    return NULL;
}

__attribute__((constructor))
static void start_profile_drain(void) {
    pthread_t t;
    if (!pthread_create(&t, NULL, publish_profile, NULL)) pthread_detach(t);
}
