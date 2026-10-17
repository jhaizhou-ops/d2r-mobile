/* Kernel-mediated, read-only snapshots: never dereference untrusted guest
 * pointers in an observer. Short reads/failure are absence of evidence. */
static int xblr_guest_read(void *out,uint64_t address,size_t bytes) {
    if(!out||!bytes||bytes>4096||!address||address>UINT64_MAX-bytes)return 0;
    struct iovec local={out,bytes},remote={(void*)(uintptr_t)address,bytes};
    long n;
    do { n=syscall(SYS_process_vm_readv,getpid(),&local,1,&remote,1,0); } while(n<0&&errno==EINTR);
    if(n==(long)bytes)return 1;
    memset(out,0,bytes);return 0;
}
static int xblr_guest_string(char *out,size_t capacity,uint64_t address) {
    if(!out||capacity<1||capacity>4096)return 0;
    out[0]=0;
    for(size_t i=0;i<capacity;++i) {
        char c;
        if(address>UINT64_MAX-i||!xblr_guest_read(&c,address+i,1)){out[0]=0;return 0;}
        out[i]=c;if(!c)return 1;
    }
    out[0]=0;return 0;
}
