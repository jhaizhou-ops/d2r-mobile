/* Rebase readonly raw uint32 storage-buffer indices through specialization.
 * Unknown interfaces, root pointer aliases and array-length queries retain
 * the original module and binding path. This never truncates a guest range. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct {uint32_t binding,eligible;} SBResource;
typedef struct {uint64_t mask;uint32_t spec_id[64];} SBInfo;
static uint8_t *dm_storage_bias_apply(uint8_t *bytes,size_t *size,const SBResource *resources,unsigned count,SBInfo *info) {
    memset(info,0,sizeof(*info));if(count>64||*size<20||*size%4)return NULL;
    uint32_t *w=(uint32_t *)bytes,bound=w[3];if(!bound||bound>1000000)return NULL;
    uint32_t *def=calloc(bound,4),*rtype=calloc(bound,4),*integer=calloc(bound,4);
    if(!def||!rtype||!integer){free(def);free(rtype);free(integer);return bytes;}
    uint32_t variable[64]={0},uint_type[64]={0},spec_const[64]={0};
    uint32_t next_spec=0;int spec_exhausted=0;size_t total_accesses=0;
    for(size_t o=5;o<*size/4;){unsigned n=w[o]>>16,op=w[o]&65535;if(!n||n>*size/4-o)goto malformed;
        if(op>=19&&op<=39&&n>=2&&w[o+1]<bound)def[w[o+1]]=(uint32_t)o;
        if(op==21&&n==4&&w[o+1]<bound&&w[o+2]==32)integer[w[o+1]]=1;
        if((op==43||op==50||op==59)&&n>=3&&w[o+2]<bound)def[w[o+2]]=(uint32_t)o;
        if(op==71&&n==4&&w[o+2]==1){if(w[o+3]==UINT32_MAX)spec_exhausted=1;else if(w[o+3]>=next_spec)next_spec=w[o+3]+1;}
        o+=n;
    }
    if(spec_exhausted||next_spec>UINT32_MAX-count)goto unchanged;
    for(size_t o=5;o<*size/4;o+=w[o]>>16){unsigned n=w[o]>>16,op=w[o]&65535;
        if(n>=3&&(op==12||op>=40)&&(op<71||op>75)&&w[o+1]<bound&&integer[w[o+1]]&&w[o+2]<bound)rtype[w[o+2]]=w[o+1];
    }
    for(unsigned i=0;i<count;i++)if(resources[i].eligible){
        uint32_t var=0;
        for(size_t o=5;o<*size/4;o+=w[o]>>16)if((w[o]&65535)==71&&w[o]>>16==4&&w[o+2]==33&&w[o+3]==resources[i].binding)var=w[o+1];
        if(!var||var>=bound||!def[var])continue;uint32_t v=def[var];
        if((w[v]&65535)!=59||w[v]>>16<4||w[v+1]>=bound||!def[w[v+1]])continue;uint32_t p=def[w[v+1]];
        if((w[p]&65535)!=32||w[p]>>16!=4||(w[p+2]!=2&&w[p+2]!=12)||w[p+3]>=bound||!def[w[p+3]])continue;
        uint32_t structure_id=w[p+3],structure=def[structure_id];
        if((w[structure]&65535)!=30||w[structure]>>16!=3||w[structure+2]>=bound||!def[w[structure+2]])continue;
        uint32_t array_id=w[structure+2],array=def[array_id];
        if((w[array]&65535)!=29||w[array]>>16!=3||w[array+2]>=bound||!def[w[array+2]])continue;
        uint32_t elem_id=w[array+2],elem=def[elem_id];
        if((w[elem]&65535)!=21||w[elem]>>16!=4||w[elem+2]!=32||w[elem+3]!=0)continue;
        int block=0,stride=0,member_offset=0;
        for(size_t o=5;o<*size/4;o+=w[o]>>16){unsigned n=w[o]>>16,op=w[o]&65535;
            if(op==71&&n==3&&w[o+1]==structure_id&&(w[o+2]==2||w[o+2]==3))block=1;
            if(op==71&&n==4&&w[o+1]==array_id&&w[o+2]==6&&w[o+3]==4)stride=1;
            if(op==72&&n==5&&w[o+1]==structure_id&&w[o+2]==0&&w[o+3]==35&&w[o+4]==0)member_offset=1;
        }
        if(!block||!stride||!member_offset)continue;
        int valid=1;unsigned hits=0;
        for(size_t o=5;o<*size/4&&valid;o+=w[o]>>16){unsigned n=w[o]>>16,op=w[o]&65535;
            /* Types cannot use a variable/access-chain result as a type ID;
             * scalar constants carry literal bits, including widths/IDs. */
            if(op==5||op==6||op==15||op==71||op==72||(op>=19&&op<=39)||op==43||op==50||(op==59&&n>=3&&w[o+2]==var))continue;
            /* OpExtInst's instruction number is a literal, not an ID.
             * The game's Log2 instruction (30) equals an SSBO variable ID. */
            int used=0;for(unsigned j=1;j<n;j++)if(op!=12||j!=4)used|=w[o+j]==var;if(!used)continue;
            if((op!=65&&op!=66)||n!=6||w[o+3]!=var){valid=0;break;}
            uint32_t member=w[o+4],index=w[o+5];
            if(member>=bound||!def[member]||index>=bound||!rtype[index]){valid=0;break;}
            uint32_t c=def[member];
            if((w[c]&65535)!=43||w[c]>>16!=4||w[c+3]!=0||w[c+1]>=bound||!integer[w[c+1]]){valid=0;break;}
            uint32_t pointer=w[o+2];
            for(size_t d=5;d<*size/4&&valid;d+=w[d]>>16){unsigned dn=w[d]>>16,dop=w[d]&65535;
                /* Decorations contain literal enum values (NonWritable=24),
                 * which must not be mistaken for a derived pointer ID. */
                if(d==o||dop==5||dop==6||dop==71||dop==72||(dop>=19&&dop<=39)||dop==43||dop==50)continue;
                int pointer_used=0;for(unsigned j=1;j<dn;j++)if(dop!=12||j!=4)pointer_used|=w[d+j]==pointer;
                if(pointer_used&&(dop!=61||dn<4||w[d+3]!=pointer))valid=0;
            }
            hits++;
        }
        if(valid&&hits){variable[i]=var;uint_type[i]=elem_id;total_accesses+=hits;info->mask|=1ull<<i;info->spec_id[i]=next_spec++;}
    }
    if(!info->mask)goto unchanged;
    /* Each access adds at most one bitcast and one addition; each binding
     * adds a SpecId decoration and a scalar specialization constant. */
    size_t capacity=*size/4+total_accesses*9+count*8;uint32_t *out=malloc(capacity*4);
    if(!out)goto unchanged;memcpy(out,w,20);size_t at=5;uint32_t next=bound;int decorated=0,declared=0;
    for(unsigned i=0;i<count;i++)if(variable[i])spec_const[i]=next++;
    for(size_t o=5;o<*size/4;o+=w[o]>>16){unsigned n=w[o]>>16,op=w[o]&65535;
        if(!decorated&&op>=19&&op<=39){
            for(unsigned i=0;i<count;i++)if(variable[i]){uint32_t d[]={4u<<16|71,spec_const[i],1,info->spec_id[i]};memcpy(out+at,d,sizeof d);at+=4;}
            decorated=1;
        }
        if(!declared&&op==54){
            for(unsigned i=0;i<count;i++)if(variable[i]){uint32_t c[]={4u<<16|50,uint_type[i],spec_const[i],0};memcpy(out+at,c,sizeof c);at+=4;}
            declared=1;
        }
        int binding=-1;if((op==65||op==66)&&n==6)for(unsigned i=0;i<count;i++)if(variable[i]&&w[o+3]==variable[i]){binding=(int)i;break;}
        if(binding>=0){
            uint32_t index=w[o+5],type=rtype[index],bias=spec_const[binding];
            if(type!=uint_type[binding]){uint32_t cast[]={4u<<16|124,type,next++,bias};bias=cast[2];memcpy(out+at,cast,sizeof cast);at+=4;}
            uint32_t add[]={5u<<16|128,type,next++,index,bias};memcpy(out+at,add,sizeof add);at+=5;
            memcpy(out+at,w+o,n*4);out[at+5]=add[2];at+=n;
        }else {memcpy(out+at,w+o,n*4);at+=n;}
    }
    if(!decorated||!declared){free(out);goto unchanged;}
    out[3]=next;free(bytes);free(def);free(rtype);free(integer);*size=at*4;return (uint8_t *)out;
unchanged:
    memset(info,0,sizeof(*info));free(def);free(rtype);free(integer);return bytes;
malformed:
    free(def);free(rtype);free(integer);return NULL;
}
