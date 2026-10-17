#pragma once
// Minimal crash recorder: on SIGSEGV/SIGBUS/SIGILL/SIGABRT/SIGFPE writes signal, fault address, pc/lr and the
// containing module + offset to files/crash-last.txt (async-signal-safe writes only, dladdr best effort), then
// re-raises through the previous handler so the system crash report is unchanged. faultlogger is not readable
// over hdc on release devices; this file is.
#include <signal.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <string>
namespace crash_recorder {
inline char path[512];
inline struct sigaction previous[32];
inline void put(int fd,const char *s){(void)!write(fd,s,strlen(s));}
inline void hex(int fd,uint64_t v){char b[19]={'0','x'};for(int i=0;i<16;i++){int d=int(v>>(60-4*i))&15;b[2+i]=char(d<10?'0'+d:'a'+d-10);}b[18]=0;put(fd,b);}
inline void where(int fd,uint64_t a){Dl_info info{};if(a&&dladdr(reinterpret_cast<void*>(a),&info)&&info.dli_fname){put(fd," ");put(fd,info.dli_fname);put(fd,"+");hex(fd,a-uint64_t(uintptr_t(info.dli_fbase)));if(info.dli_sname){put(fd," ");put(fd,info.dli_sname);}}}
inline void handler(int sig,siginfo_t *si,void *uc){
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(fd>=0){
        put(fd,"signal ");char s[4]={char('0'+sig/10),char('0'+sig%10),'\n',0};put(fd,s);put(fd,"fault ");hex(fd,uint64_t(uintptr_t(si?si->si_addr:nullptr)));put(fd,"\n");
#if defined(__aarch64__)
        if(uc){auto *c=static_cast<ucontext_t*>(uc);put(fd,"pc ");hex(fd,c->uc_mcontext.pc);where(fd,c->uc_mcontext.pc);put(fd,"\nlr ");hex(fd,c->uc_mcontext.regs[30]);where(fd,c->uc_mcontext.regs[30]);put(fd,"\nsp ");hex(fd,c->uc_mcontext.sp);put(fd,"\ntid ");hex(fd,uint64_t(gettid()));put(fd,"\n");
            uint64_t *fp=reinterpret_cast<uint64_t*>(c->uc_mcontext.regs[29]);for(int i=0;i<16&&fp&&(uintptr_t(fp)&7)==0;i++){uint64_t ret=fp[1];put(fd,"  #");hex(fd,ret);where(fd,ret);put(fd,"\n");fp=reinterpret_cast<uint64_t*>(fp[0]);}}
#endif
        close(fd);
    }
    struct sigaction &p=previous[sig&31];
    if(p.sa_flags&SA_SIGINFO){if(p.sa_sigaction){p.sa_sigaction(sig,si,uc);return;}}
    else if(p.sa_handler!=SIG_DFL&&p.sa_handler!=SIG_IGN&&p.sa_handler){p.sa_handler(sig);return;}
    signal(sig,SIG_DFL);raise(sig);
}
inline void install(const std::string &files){
    static bool done=false;if(done)return;done=true;
    std::string p=files+"/crash-last.txt";strncpy(path,p.c_str(),sizeof(path)-1);
    for(int sig:{SIGSEGV,SIGBUS,SIGILL,SIGABRT,SIGFPE}){struct sigaction sa{};sa.sa_sigaction=handler;sa.sa_flags=SA_SIGINFO|SA_ONSTACK;sigemptyset(&sa.sa_mask);sigaction(sig,&sa,&previous[sig&31]);}
}
}
