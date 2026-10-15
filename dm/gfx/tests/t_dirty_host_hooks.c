/* Linux-only test adapter for the dirty tracker's three host interfaces.
 * Real mmap/mprotect/SIGSEGV and the production Android tracker are used;
 * Android crash routing and GPU execution remain device-test boundaries. */
#include "../../host/dm.h"
#include <signal.h>
#include <time.h>
#include <unistd.h>

static int (*fault_hook)(void *,int,void *);
static void (*prepare_hook)(void *,size_t);

void dm_fault_hook_register(int (*fn)(void *,int,void *)) {fault_hook=fn;}
void dm_guest_write_prep_register(void (*fn)(void *,size_t)) {prepare_hook=fn;}
void dm_guest_write_prepare(void *address,size_t bytes) {if(prepare_hook)prepare_hook(address,bytes);}
uint64_t dm_android_nvn_clock_ns(void) {
    struct timespec value;clock_gettime(CLOCK_MONOTONIC,&value);
    return (uint64_t)value.tv_sec*1000000000u+(uint64_t)value.tv_nsec;
}
static void fault(int signal,siginfo_t *info,void *context) {
    if(signal==SIGSEGV&&fault_hook&&fault_hook(info->si_addr,1,context))return;
    static const char message[]="Unhandled offline dirty-tracker fault\n";
    ssize_t written=write(STDERR_FILENO,message,sizeof(message)-1);(void)written;_Exit(139);
}
void crash_install(void) {
    struct sigaction action={0};action.sa_sigaction=fault;action.sa_flags=SA_SIGINFO;
    sigemptyset(&action.sa_mask);if(sigaction(SIGSEGV,&action,NULL))abort();
}
