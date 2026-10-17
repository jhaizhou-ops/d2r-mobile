/* Bounded targeted manager slot write. The aligned 8-byte slot stays within a
 * page; failure is explicit. No general arbitrary guest-write interface. */
static int world_write_target_slot(uint64_t address,uint64_t value){
 if(!address||(address&7)||address>UINT64_MAX-8)return 0;
 struct iovec local={&value,8},remote={(void*)(uintptr_t)address,8};long n;
 do {n=syscall(SYS_process_vm_writev,getpid(),&local,1,&remote,1,0);}while(n<0&&errno==EINTR);
 return n==8;
}
