#include "../nvn/nvn_dirty_summary.h"
#include "../../host/dm.h"
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define REQUIRE(test) do {if(!(test)){fprintf(stderr,"dirty summary check failed at line %d: %s\n",__LINE__,#test);exit(1);}} while(0)
void crash_install(void);
static uint64_t comparisons;
static uint32_t random_next(uint32_t *seed) {
    *seed^=*seed<<13;*seed^=*seed>>17;*seed^=*seed<<5;return *seed;
}
static int exact(const _Atomic uint64_t *pages,size_t first,size_t last,uint64_t stamp) {
    for(size_t page=first;page<=last;page++)if(atomic_load(&pages[page])>stamp)return 1;
    return 0;
}
static void compare(const NvnDirtySummary *summary,const _Atomic uint64_t *pages,size_t first,size_t last,uint64_t stamp) {
    NvnDirtyQueryStats stats={0};
    REQUIRE(nvn_dirty_summary_since(summary,pages,first,last,stamp,&stats)==exact(pages,first,last,stamp));comparisons++;
    REQUIRE(stats.page_reads+stats.skipped_pages<=last-first+1);
}
static void properties(void) {
    enum { PAGES=8193,BLOCKS=(PAGES+63)/64 };
    _Atomic uint64_t *pages=calloc(PAGES,sizeof(*pages)),*blocks=calloc(BLOCKS,sizeof(*blocks));
    REQUIRE(pages&&blocks);NvnDirtySummary summary={.blocks=blocks,.capacity=BLOCKS};
    const size_t edges[]={0,1,3,4,62,63,64,65,127,128,8191,8192};
    for(unsigned i=0;i<sizeof(edges)/sizeof(*edges);i++)for(unsigned j=i;j<sizeof(edges)/sizeof(*edges);j++)
        compare(&summary,pages,edges[i],edges[j],0);
    for(unsigned i=0;i<sizeof(edges)/sizeof(*edges);i++){
        nvn_dirty_summary_store(&summary,pages,edges[i],i+1);
        for(size_t first=0;first<130;first++)for(size_t size=1;size<9;size++)
            compare(&summary,pages,first,first+size-1,i);
    }
    /* A write elsewhere in the same summary block must not make an exact
     * query dirty; a write in the final partially populated block must not
     * read beyond the generation array. */
    nvn_dirty_summary_store(&summary,pages,8192,100);
    compare(&summary,pages,8191,8192,99);compare(&summary,pages,130,8000,99);
    compare(&summary,pages,0,PAGES-1,UINT64_MAX);
    uint32_t seed=0x34ce81u;uint64_t generation=100;
    for(unsigned step=0;step<100000;step++){
        if(random_next(&seed)%3==0)nvn_dirty_summary_store(&summary,pages,random_next(&seed)%PAGES,++generation);
        size_t first=random_next(&seed)%PAGES,last=first+random_next(&seed)%(PAGES-first);
        uint64_t stamp=random_next(&seed)%(generation+1);
        if(step%7==0)stamp=generation;
        compare(&summary,pages,first,last,stamp);
    }
    /* Emulate a writer interrupted between upper-bound publication and page
     * publication. A bound may be ahead; the original scan decides truth. */
    nvn_dirty_summary_max(&blocks[0],generation+1);nvn_dirty_summary_max(&summary.newest,generation+1);
    compare(&summary,pages,0,63,generation);
    atomic_store(&pages[0],generation+1);compare(&summary,pages,0,63,generation);
    free(blocks);free(pages);
}

typedef struct {
    NvnDirtySummary summary;
    _Atomic uint64_t pages[257],blocks[5];
    pthread_barrier_t ready,done;
} Concurrent;
typedef struct {Concurrent *state;unsigned lane;} Worker;
static void *writer(void *data) {
    Worker *worker=data;Concurrent *state=worker->state;
    for(unsigned round=1;round<=1000;round++){
        /* All four threads update the first block, forcing competing maxima
         * with different generation values and arbitrary publication order. */
        size_t page=round%2?worker->lane:64*worker->lane;
        nvn_dirty_summary_store(&state->summary,state->pages,page,(uint64_t)round*4+worker->lane);
        (void)pthread_barrier_wait(&state->ready);(void)pthread_barrier_wait(&state->done);
    }
    return NULL;
}
static void concurrency(void) {
    Concurrent state={0};state.summary.blocks=state.blocks;state.summary.capacity=5;
    REQUIRE(!pthread_barrier_init(&state.ready,NULL,5)&&!pthread_barrier_init(&state.done,NULL,5));
    pthread_t threads[4];Worker workers[4];
    for(unsigned lane=0;lane<4;lane++){workers[lane]=(Worker){&state,lane};REQUIRE(!pthread_create(&threads[lane],NULL,writer,&workers[lane]));}
    for(unsigned round=1;round<=1000;round++){
        (void)pthread_barrier_wait(&state.ready);
        REQUIRE(atomic_load(&state.summary.newest)==(uint64_t)round*4+3);
        for(unsigned first=0;first<257;first+=17)compare(&state.summary,state.pages,first,256,(uint64_t)round*4-1);
        compare(&state.summary,state.pages,0,256,(uint64_t)round*4+3);
        (void)pthread_barrier_wait(&state.done);
    }
    for(unsigned lane=0;lane<4;lane++)REQUIRE(!pthread_join(threads[lane],NULL));
    REQUIRE(!pthread_barrier_destroy(&state.ready)&&!pthread_barrier_destroy(&state.done));
}

typedef struct {
    uint8_t *memory;
    size_t page_size;
    unsigned lane;
    pthread_barrier_t *ready,*done;
} RealWriter;
static void *real_writer(void *data) {
    RealWriter *worker=data;
    for(unsigned round=0;round<64;round++){
        uint8_t *address=worker->memory+(round*4+worker->lane)*worker->page_size+23;
        if(worker->lane%2)dm_guest_write_prepare(address,1);
        *(volatile uint8_t *)address=(uint8_t)(round+1);
        (void)pthread_barrier_wait(worker->ready);(void)pthread_barrier_wait(worker->done);
    }
    return NULL;
}
static void real_concurrency(uint8_t *memory,size_t page_size,size_t bytes) {
    nvn_dirty_remove_range(memory);nvn_dirty_add_range(memory,bytes);nvn_dirty_protect(memory,bytes);
    uint64_t stamp=nvn_dirty_now();pthread_barrier_t ready,done;
    REQUIRE(!pthread_barrier_init(&ready,NULL,5)&&!pthread_barrier_init(&done,NULL,5));
    pthread_t threads[4];RealWriter workers[4];
    for(unsigned lane=0;lane<4;lane++){
        workers[lane]=(RealWriter){memory,page_size,lane,&ready,&done};
        REQUIRE(!pthread_create(&threads[lane],NULL,real_writer,&workers[lane]));
    }
    for(unsigned round=0;round<64;round++){
        (void)pthread_barrier_wait(&ready);
        REQUIRE(nvn_dirty_now()==stamp+(round+1)*4);
        for(size_t first=0;first<257;first++){
            size_t last=first+(round+first*17u)%(257-first);
            REQUIRE(nvn_dirty_since(memory+first*page_size+11,(last-first+1)*page_size-37,stamp)==(first<(round+1)*4));
            comparisons++;
        }
        (void)pthread_barrier_wait(&done);
    }
    for(unsigned lane=0;lane<4;lane++)REQUIRE(!pthread_join(threads[lane],NULL));
    REQUIRE(!pthread_barrier_destroy(&ready)&&!pthread_barrier_destroy(&done));
}

static void tracker(void) {
    crash_install();size_t page_size=(size_t)getpagesize(),count=257,bytes=count*page_size;
    uint8_t *memory=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);REQUIRE(memory!=MAP_FAILED);
    /* Grow a reused slot from one page/one summary block to 257 pages/five
     * blocks. Old maxima and generations must not survive the reallocation. */
    nvn_dirty_add_range(memory,page_size);nvn_dirty_protect(memory,page_size);
    uint64_t small_stamp=nvn_dirty_now();((volatile uint8_t *)memory)[7]=1;
    REQUIRE(nvn_dirty_since(memory,page_size,small_stamp)==1);
    nvn_dirty_remove_range(memory);nvn_dirty_add_range(memory,bytes);
    REQUIRE(nvn_dirty_since(memory,bytes,0)==0);
    REQUIRE(nvn_dirty_since(memory+bytes-page_size,page_size,0)==0);
    comparisons+=3;
    uint64_t *expected=calloc(count,sizeof(*expected));REQUIRE(expected);
    nvn_dirty_protect(memory,bytes);uint64_t initial=nvn_dirty_now();
    for(unsigned round=0;round<80;round++){
        size_t changed=(round*71u)%count;
        uint64_t before=nvn_dirty_now();
        if(round%2)dm_guest_write_prepare(memory+changed*page_size+17,1);
        ((volatile uint8_t *)memory)[changed*page_size+17]=(uint8_t)round;
        uint64_t after=nvn_dirty_now();REQUIRE(after==before+1);expected[changed]=after;
        for(size_t first=0;first<count;first++){
            size_t last=first+(round+first*13u)%(count-first);
            size_t skip=first%101,len=(last-first+1)*page_size-skip-(round%97);
            uint64_t stamp=round%3?before:initial;int want=0;
            for(size_t page=first;page<=last;page++)if(expected[page]>stamp){want=1;break;}
            REQUIRE(nvn_dirty_since(memory+first*page_size+skip,len,stamp)==want);comparisons++;
        }
        /* Protecting again must preserve the old dirty generation and make
         * the next real write receive a fresh generation in both modes. */
        nvn_dirty_protect(memory+changed*page_size,page_size);
    }
    REQUIRE(nvn_dirty_since(memory+bytes-1,2,initial)==1);
    REQUIRE(nvn_dirty_since(memory,0,initial)==0);
    REQUIRE(nvn_dirty_since(memory,SIZE_MAX,initial)==1);
    REQUIRE(nvn_dirty_since(memory+bytes-1,1,UINT64_MAX)==0);
    REQUIRE(nvn_dirty_since((void *)((uintptr_t)memory|UINT64_C(0xab00000000000000)),bytes,initial)==1);
    size_t cursor=0,offset=0,length=0;unsigned char seen[257]={0};
    while(nvn_dirty_next_range(memory,bytes,initial,&cursor,&offset,&length)){
        REQUIRE(offset%page_size==0&&length%page_size==0);
        for(size_t page=offset/page_size;page<(offset+length)/page_size;page++){REQUIRE(!seen[page]++);REQUIRE(expected[page]>initial);}
    }
    for(size_t page=0;page<count;page++)REQUIRE((seen[page]!=0)==(expected[page]>initial));
    real_concurrency(memory,page_size,bytes);
    NvnDirtyQueryStats stats;int summary,recording;nvn_dirty_query_stats(&stats,&summary,&recording);
    REQUIRE(recording&&stats.calls>20000&&stats.page_reads>0);
    if(summary)REQUIRE(stats.skipped_pages>0&&stats.block_reads>0);else REQUIRE(!stats.skipped_pages&&!stats.block_reads);
    nvn_dirty_remove_range(memory);REQUIRE(!nvn_dirty_tracked(memory,bytes));
    REQUIRE(nvn_dirty_since(memory,bytes,initial)==1);
    /* Reuse the same range slot/address with retained allocation capacity:
     * neither old upper bounds nor old page values may survive. */
    nvn_dirty_add_range(memory,bytes);REQUIRE(nvn_dirty_since(memory,bytes,0)==0);
    nvn_dirty_protect(memory,bytes);initial=nvn_dirty_now();((volatile uint8_t *)memory)[bytes-1]=42;
    REQUIRE(nvn_dirty_since(memory+bytes-page_size,page_size,initial)==1);
    REQUIRE(nvn_dirty_since(memory,bytes-page_size,initial)==0);
    nvn_dirty_remove_range(memory);REQUIRE(!munmap(memory,bytes));free(expected);
    printf("{\"passed\":true,\"summary_enabled\":%s,\"comparisons\":%llu,\"real_fault_and_prepare\":true,\"concurrent_writers\":4,\"concurrent_rounds\":1000,\"real_concurrent_fault_rounds\":64,\"query_calls\":%llu,\"page_reads\":%llu,\"block_reads\":%llu,\"skipped_pages\":%llu,\"gpu_executed\":false}\n",
        summary?"true":"false",(unsigned long long)comparisons,(unsigned long long)stats.calls,(unsigned long long)stats.page_reads,
        (unsigned long long)stats.block_reads,(unsigned long long)stats.skipped_pages);
}

static uint64_t cpu_ns(void) {
    struct timespec value;REQUIRE(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&value));
    return (uint64_t)value.tv_sec*1000000000u+(uint64_t)value.tv_nsec;
}
static int sort_double(const void *a,const void *b) {double x=*(const double *)a,y=*(const double *)b;return x>y?1:x<y?-1:0;}
static void benchmark(void) {
    crash_install();size_t page_size=(size_t)getpagesize(),count=16384,bytes=count*page_size;
    uint8_t *memory=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);REQUIRE(memory!=MAP_FAILED);
    nvn_dirty_add_range(memory,bytes);uint64_t clean=nvn_dirty_now(),dirty_stamp=0;
    double results[4][7];
    for(unsigned scenario=0;scenario<4;scenario++){
        if(scenario==1){nvn_dirty_protect(memory+bytes-page_size,page_size);dirty_stamp=nvn_dirty_now();((volatile uint8_t *)memory)[bytes-1]=17;}
        size_t len=scenario==1?bytes/2:scenario==3?page_size:bytes;
        uint64_t stamp=scenario==0?clean:dirty_stamp;int expected=scenario==2;
        for(unsigned sample=0;sample<7;sample++){
            unsigned sum=0,iterations=scenario==3?1000000:4000;uint64_t started=cpu_ns();
            for(unsigned iteration=0;iteration<iterations;iteration++)sum+=(unsigned)nvn_dirty_since(memory,len,stamp);
            results[scenario][sample]=(double)(cpu_ns()-started)/iterations;
            REQUIRE(sum==(expected?iterations:0));
        }
        qsort(results[scenario],7,sizeof(double),sort_double);
    }
    NvnDirtyQueryStats stats;int summary,recording;nvn_dirty_query_stats(&stats,&summary,&recording);REQUIRE(!recording);
    printf("{\"passed\":true,\"summary_enabled\":%s,\"clock\":\"thread_cpu\",\"pages\":%zu,\"samples_per_case\":7,\"median_ns_per_query\":{\"clean_full_pool\":%.3f,\"unrelated_dirty_half_pool\":%.3f,\"dirty_last_page\":%.3f,\"tiny_clean_range\":%.3f},\"gpu_executed\":false}\n",
        summary?"true":"false",count,results[0][3],results[1][3],results[2][3],results[3][3]);
    nvn_dirty_remove_range(memory);REQUIRE(!munmap(memory,bytes));
}
int main(int argc,char **argv) {
    REQUIRE(argc==3);
    REQUIRE(!setenv("DM_DIRTY_SUMMARY",argv[2],1));
    int bench=!strcmp(argv[1],"benchmark");
    REQUIRE(!setenv("DM_DIRTY_QUERY_STATS",bench?"0":"1",1));
    if(bench){benchmark();return 0;}
    properties();concurrency();
    if(!strcmp(argv[1],"properties")){
        printf("{\"passed\":true,\"comparisons\":%llu,\"concurrent_writers\":4,\"concurrent_rounds\":1000,\"gpu_executed\":false}\n",(unsigned long long)comparisons);return 0;
    }
    tracker();return 0;
}
