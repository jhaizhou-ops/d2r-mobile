// 接静态嵌入的真实原模块，不从Apple patched.bin取指，不调用客体入口。
#include "xblr_loader.h"
#include <string.h>
extern const xblr_image_desc xblr_original_images[2];
extern const xblr_hle_binding __start_xblr_hle[],__stop_xblr_hle[];
static void record_first(void *user,const char *module,const char *symbol,uint64_t offset,uint32_t type) {
    (void)module;(void)offset;(void)type;
    xblr_originals_report *out=user;
    if(out->first_missing_symbol[0])return;
    size_t n=strlen(symbol);if(n>=sizeof out->first_missing_symbol)n=sizeof out->first_missing_symbol-1;
    memcpy(out->first_missing_symbol,symbol,n);out->first_missing_symbol[n]=0;
}
int xblr_prepare_originals(xblr_originals_report *out,size_t bytes) {
    if(!out||bytes!=sizeof *out)return XBLR_LOAD_ARGUMENT;
    *out=(xblr_originals_report){.abi_version=1,.struct_bytes=sizeof *out,
        .execution_status=XBLR_LOAD_NEEDS_HARMONY_REWRITE};
    xblr_module *modules[2]={NULL,NULL};int status=0;
    for(unsigned i=0;i<2;++i) {
        status=xblr_bind_image(&xblr_original_images[i],&modules[i],&out->modules[i]);
        if(status) { out->bind_status=(uint32_t)status;goto done; }
    }
    uint64_t entry=0;
    status=xblr_module_symbol(modules[0],"nnMain",&entry);
    if(status) { out->bind_status=(uint32_t)status;goto done; }
    out->nnmain_offset=entry-(uintptr_t)xblr_original_images[0].base;
    size_t hle_count=(size_t)(__stop_xblr_hle-__start_xblr_hle);
    status=xblr_link_modules(modules,2,__start_xblr_hle,hle_count,0,record_first,out,&out->link);
    out->link_status=(uint32_t)status;
done:
    for(unsigned i=0;i<2;++i)xblr_release_module(modules[i]);
    return status;
}
