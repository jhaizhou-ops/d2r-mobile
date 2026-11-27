#include <cassert>
#include "h11_measured_types.h" // extracted verbatim from xbg_game.cpp by evidence harness
static uint64_t now=0,reads=0;
static uint64_t clock_read(){++reads;return now;}
int main(){
 static H11Repeat r;std::array<uint64_t,4> k{1,2,3,4};
 r.observe(k,{10,0,64,2},1);r.observe(k,{10,0,64,2},1);
 r.observe(k,{10,16,64,2},2);r.observe(k,{10,16,128,2},2);
 assert(r.total==4 && r.same==1 && r.offset_only==1 && r.unique_draw==2);
 r.reset();r.observe(k,{10,16,128,2},1);assert(r.same==0 && r.unique_draw==1);
 // A bucket collision must not become an equal binding.
 auto c=k;c[0]+=8192;r.observe(c,{10,16,128,2},1);assert(r.collisions==1 && r.same==0);
 XbgCpuTiming cpu(clock_read);H11DescriptorTiming detail;
 {XbgCpuTiming::Scope root(&cpu,XbgCpuTiming::Descriptor);
  {H11DescriptorTiming::Scope a(&detail,&root,H11DescriptorTiming::MemoKeyAndCheck);
   now=10;{H11DescriptorTiming::Scope b(&detail,&root,H11DescriptorTiming::MemoRefresh);
    now=20;{XbgCpuTiming::Scope child(&cpu,XbgCpuTiming::Snapshot);now=50;}now=70;
   }now=100;
  }
 }
 assert(!detail.invalid && detail.sum()==70 && cpu.counters[XbgCpuTiming::Descriptor].exclusive_ns==70);
 assert(detail.counters[H11DescriptorTiming::MemoRefresh].ns==30);
 assert(detail.counters[H11DescriptorTiming::MemoKeyAndCheck].ns==40);
 auto before=reads;{H11DescriptorTiming::Scope off(nullptr,nullptr,H11DescriptorTiming::Audit);}assert(reads==before);
 // Changed sampler pool/format must not count as equal image input.
 static H11Repeat image;image.observe(k,{10,0,64,2,100,3,128,5},1);image.observe(k,{10,0,64,2,101,3,128,5},2);assert(!image.same && !image.offset_only);
 // Simultaneous detail recorders must exclude each other's main CPU child scopes.
 now=0;XbgCpuTiming both(clock_read);H11DrawTiming draw;
 {XbgCpuTiming::Scope root(&both,XbgCpuTiming::Draw);
  H11DrawTiming::Scope phase(&draw,&root,H11DrawTiming::StateBuild);
  now=10;{XbgCpuTiming::Scope child(&both,XbgCpuTiming::Descriptor);H11DescriptorTiming d;
    {H11DescriptorTiming::Scope part(&d,&child,H11DescriptorTiming::Setup);now=40;}assert(d.sum()==30);
  }now=70;phase.finish();phase.finish();
  {H11DrawTiming::Scope record(&draw,&root,H11DrawTiming::Record);now=90;}
 }
 assert(draw.finish(both.counters[XbgCpuTiming::Draw].exclusive_ns)==60 && !draw.flags);
 assert(draw.counters[H11DrawTiming::StateBuild].ns==40 && draw.counters[H11DrawTiming::Record].ns==20);
 WindowTest::test();
 return 0;
}
