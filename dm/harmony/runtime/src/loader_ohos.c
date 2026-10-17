// 复用 sw/host/loader.c 的 MOD0、符号哈希、跨模块优先序和四类AArch64重定位语义。
// 平台无关边界解析移入本目录；不包含Mach-O/Apple线程槽、不运行原指令、不生成返回0桩。
#include "xblr_loader.h"
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t name; uint8_t info, other; uint16_t section; uint64_t value, size; } sym64;
typedef struct { int64_t tag; uint64_t value; } dyn64;
typedef struct { uint64_t offset, info; int64_t addend; } rela64;
_Static_assert(sizeof(sym64)==24 && sizeof(dyn64)==16 && sizeof(rela64)==24,"ELF64 layout");
struct xblr_module {
    xblr_image_desc image;
    const sym64 *symbols;
    const char *strings;
    uint64_t string_bytes;
    uint32_t symbol_count, hash_mask, *hash;
    const rela64 *rela[2]; uint64_t rela_count[2];
};
static int fits(uint64_t offset,uint64_t bytes,uint64_t size) { return offset<=size && bytes<=size-offset; }
static uint32_t u32(const void *p) { uint32_t v; memcpy(&v,p,4); return v; }
static uint32_t symhash(const char *s) { uint32_t h=2166136261u; while(*s) h=(h^(uint8_t)*s++)*16777619u; return h; }
static const char *symbol_name(const xblr_module *m,const sym64 *s) { return m->strings+s->name; }
static int symbol_value(const xblr_module *m,const sym64 *s,uint64_t *out) {
    if (!s->section || (s->info&15)==6) return XBLR_LOAD_FORMAT; // STT_TLS needs its own ABI, never guess.
    if (s->section==0xfff1) { *out=s->value; return 0; } // SHN_ABS
    if (!fits(s->value,s->size,m->image.size)) return XBLR_LOAD_FORMAT;
    *out=(uintptr_t)m->image.base+s->value; return 0;
}
void xblr_release_module(xblr_module *m) { if(m) { free(m->hash); free(m); } }
int xblr_bind_image(const xblr_image_desc *image,xblr_module **out,xblr_module_info *info) {
    if(!image||!out||!info||!image->base) return XBLR_LOAD_ARGUMENT;
    *out=NULL; *info=(xblr_module_info){0};
    if(image->source_kind==XBLR_IMAGE_APPLE_PATCHED) return XBLR_LOAD_APPLE_ABI;
    if((image->source_kind!=XBLR_IMAGE_ORIGINAL_UNREWRITTEN && image->source_kind!=XBLR_IMAGE_HARMONY_REWRITTEN) || image->size>256u*1024u*1024u || image->size<32 ||
       (uintptr_t)image->base%8 || !memchr(image->name,0,sizeof image->name) || !image->name[0] ||
       !fits(0,image->text_bytes,image->size) || image->text_bytes>image->ro_offset ||
       !fits(image->ro_offset,image->ro_bytes,image->size) || image->ro_offset+image->ro_bytes>image->data_offset ||
       !fits(image->data_offset,image->data_bytes,image->size) ||
       !fits(image->data_offset+image->data_bytes,image->bss_bytes,image->size)) return XBLR_LOAD_FORMAT;
    uint64_t mod0=u32(image->base+4);
    if(!fits(mod0,28,image->size)||memcmp(image->base+mod0,"MOD0",4)) return XBLR_LOAD_FORMAT;
    int32_t delta; memcpy(&delta,image->base+mod0+4,4);
    if(delta<0 && (uint64_t)(-(int64_t)delta)>mod0) return XBLR_LOAD_FORMAT;
    uint64_t dynoff=mod0+delta;
    if(dynoff%8||!fits(dynoff,16,image->size)) return XBLR_LOAD_FORMAT;
    uint64_t syms=0,strings=0,strsize=0,hashoff=0,syment=0,roff[2]={0},rsize[2]={0},rent=0,plt_kind=0;
    uint64_t init=0,initarr=0,initsz=0; int terminated=0;
    for(uint64_t i=0;i<4096 && fits(dynoff+i*16,16,image->size);++i) {
        dyn64 d; memcpy(&d,image->base+dynoff+i*16,16);
        if(!d.tag) { terminated=1; break; }
        switch(d.tag) {
            case 4: hashoff=d.value;break; case 6: syms=d.value;break; case 11:syment=d.value;break;
            case 5:strings=d.value;break;case 10:strsize=d.value;break;
            case 7:roff[0]=d.value;break;case 8:rsize[0]=d.value;break;case 9:rent=d.value;break;
            case 23:roff[1]=d.value;break;case 2:rsize[1]=d.value;break;case 20:plt_kind=d.value;break;
            case 12:init=d.value;break;case 25:initarr=d.value;break;case 27:initsz=d.value;break;
        }
    }
    if(!terminated||syment!=24||rent!=24||plt_kind!=7||!fits(hashoff,8,image->size)||hashoff%4||syms%8||
       !strsize||!fits(strings,strsize,image->size)||!fits(init,4,image->text_bytes)||
       initsz%8||!fits(initarr,initsz,image->size)) return XBLR_LOAD_FORMAT;
    uint32_t buckets=u32(image->base+hashoff),nsym=u32(image->base+hashoff+4);
    if(!nsym||nsym>1000000||!fits(hashoff,8+4ull*(buckets+(uint64_t)nsym),image->size)||
       !fits(syms,24ull*nsym,image->size)) return XBLR_LOAD_FORMAT;
    xblr_module *m=calloc(1,sizeof *m); if(!m) return XBLR_LOAD_MEMORY;
    m->image=*image;m->symbols=(const sym64*)(image->base+syms);m->strings=(const char*)image->base+strings;
    m->string_bytes=strsize;m->symbol_count=nsym;
    uint32_t cap=16;while(cap<nsym*2)cap<<=1;
    m->hash=calloc(cap,sizeof *m->hash);m->hash_mask=cap-1;
    if(!m->hash) { xblr_release_module(m);return XBLR_LOAD_MEMORY; }
    int error=XBLR_LOAD_FORMAT;
    for(unsigned t=0;t<2;++t) {
        if(roff[t]%8||rsize[t]%24||!fits(roff[t],rsize[t],image->size))goto fail;
        m->rela[t]=(const rela64*)(image->base+roff[t]);m->rela_count[t]=rsize[t]/24;
    }
    for(uint32_t i=0;i<nsym;++i) {
        const sym64 *s=&m->symbols[i];
        if(s->name>=strsize||!memchr(m->strings+s->name,0,(size_t)(strsize-s->name)))goto fail;
        if(!i)continue;
        if(!s->section) { info->undefined_symbols++;continue; }
        uint64_t unused;
        if(symbol_value(m,s,&unused))goto fail;
        info->defined_symbols++;
        const char *name=symbol_name(m,s);
        for(uint32_t j=symhash(name)&m->hash_mask;;j=(j+1)&m->hash_mask) {
            if(!m->hash[j]) { m->hash[j]=i;break; }
            if(!strcmp(symbol_name(m,&m->symbols[m->hash[j]]),name))break;
        }
    }
    info->image_bytes=image->size;info->symbol_count=nsym;info->relocation_count=m->rela_count[0]+m->rela_count[1];
    info->mod0_offset=mod0;info->dynamic_offset=dynoff;info->init_offset=init;info->init_array_offset=initarr;info->init_array_bytes=initsz;
    info->source_kind=image->source_kind;info->execution_allowed=image->source_kind==XBLR_IMAGE_HARMONY_REWRITTEN;*out=m;return 0;
fail: xblr_release_module(m);*info=(xblr_module_info){0};return error;
}
int xblr_module_symbol(const xblr_module *m,const char *name,uint64_t *address) {
    if(!m||!name||!address)return XBLR_LOAD_ARGUMENT;
    *address=0;
    for(uint32_t j=symhash(name)&m->hash_mask;m->hash[j];j=(j+1)&m->hash_mask) {
        const sym64 *s=&m->symbols[m->hash[j]];
        if(!strcmp(symbol_name(m,s),name))return symbol_value(m,s,address);
    }
    return XBLR_LOAD_MISSING_IMPORT;
}
int xblr_module_execution_status(const xblr_module *m) { return !m?XBLR_LOAD_ARGUMENT:m->image.source_kind==XBLR_IMAGE_HARMONY_REWRITTEN?0:XBLR_LOAD_NEEDS_HARMONY_REWRITE; }
typedef struct { uint8_t *target; uint64_t value; } patch;
int xblr_link_modules(xblr_module *const *mods,size_t n,const xblr_hle_binding *hle,size_t nh,int apply,
    xblr_missing_import_fn missing,void *user,xblr_link_report *report) {
    if(!mods||!n||n>8||(!hle&&nh)||nh>65536||!report||(apply!=0&&apply!=1))return XBLR_LOAD_ARGUMENT;
    *report=(xblr_link_report){0}; uint64_t total=0;
    for(size_t i=0;i<n;++i) { if(!mods[i])return XBLR_LOAD_ARGUMENT;total+=mods[i]->rela_count[0]+mods[i]->rela_count[1]; }
    for(size_t i=0;i<nh;++i) {
        if(!hle[i].name||!hle[i].name[0]||!hle[i].address||hle[i].force>1)return XBLR_LOAD_ARGUMENT;
        for(size_t j=0;j<i;++j)if(!strcmp(hle[i].name,hle[j].name))return XBLR_LOAD_ARGUMENT;
    }
    if(total>2000000)return XBLR_LOAD_FORMAT;
    patch *patches=calloc((size_t)(total?total:1),sizeof *patches);if(!patches)return XBLR_LOAD_MEMORY;
    uint64_t np=0;int status=0;
    for(size_t k=0;k<n;++k) {
        xblr_module *m=mods[k];
        for(unsigned t=0;t<2;++t)for(uint64_t i=0;i<m->rela_count[t];++i) {
            const rela64 *r=&m->rela[t][i];uint32_t type=(uint32_t)r->info;uint64_t si=r->info>>32,v=0;
            report->relocations++;
            // The real main contains 175 valid packed (4-byte-aligned) slots.
            // memcpy below intentionally avoids imposing uint64_t alignment on them.
            if(r->offset<m->image.data_offset || !fits(r->offset,8,m->image.size)) {
                report->invalid_targets++;status=XBLR_LOAD_FORMAT;continue;
            }
            if(type==1027) { // RELATIVE
                if(si || r->addend<0 || !fits((uint64_t)r->addend,0,m->image.size)) { report->invalid_targets++;status=XBLR_LOAD_FORMAT;continue; }
                v=(uintptr_t)m->image.base+(uint64_t)r->addend;report->relative++;
            } else {
                if(type!=257&&type!=1025&&type!=1026) { report->unsupported++;status=XBLR_LOAD_UNSUPPORTED_RELOCATION;continue; }
                if(si>=m->symbol_count) { report->invalid_targets++;status=XBLR_LOAD_FORMAT;continue; }
                if(type==257)report->absolute++;else if(type==1025)report->global_data++;else report->jump_slot++;
                const sym64 *s=&m->symbols[si];const char *name=symbol_name(m,s);int found=0;
                for(size_t h=0;h<nh;++h)if(hle[h].force&&!strcmp(name,hle[h].name)) { v=hle[h].address;found=1;report->hle_bindings++;break; }
                if(!found && s->section) { if(symbol_value(m,s,&v)) { report->unsupported++;status=XBLR_LOAD_FORMAT;continue; }found=1;report->module_bindings++; }
                if(!found)for(size_t j=0;j<n;++j)if(!xblr_module_symbol(mods[j],name,&v)) { found=1;report->module_bindings++;break; }
                if(!found)for(size_t h=0;h<nh;++h)if(!hle[h].force&&!strcmp(name,hle[h].name)) { v=hle[h].address;found=1;report->hle_bindings++;break; }
                if(!found && (s->info>>4)==2) { found=1;v=0;report->weak_zero++; }
                if(!found) { report->missing_imports++;if(missing)missing(user,m->image.name,name,r->offset,type);continue; }
                if(type!=1026)v+=(uint64_t)r->addend;
            }
            patches[np++]=(patch){(uint8_t*)m->image.base+r->offset,v};
        }
    }
    if(!status&&report->missing_imports)status=XBLR_LOAD_MISSING_IMPORT;
    if(!status&&apply)for(size_t i=0;i<n;++i)if(mods[i]->image.readonly) { status=XBLR_LOAD_READONLY;break; }
    if(!status&&apply) { for(uint64_t i=0;i<np;++i)memcpy(patches[i].target,&patches[i].value,8);report->writes_applied=np; }
    free(patches);return status;
}
