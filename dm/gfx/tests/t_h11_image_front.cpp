#include "h11_image_front_types.h"
#include <cassert>
#include <unordered_map>
#include <cstdio>
#include <limits>
int main(){
 H11LookupFront<ImgKey,int,2> f;ImgKey k{1,2,3,4,5,6,7};int a=11,b=22;
 assert(!f.find(0,k));f.put(0,k,&a);assert(f.find(0,k)==&a);assert(!f.find(1,k));
 for(int i=0;i<7;i++){auto q=k;switch(i){case 0:q.h++;break;case 1:q.sh++;break;case 2:q.tp++;break;case 3:q.to++;break;case 4:q.sp++;break;case 5:q.so++;break;case 6:q.kind++;break;}assert(!f.find(0,q));}
 auto q=k;q.h=99;f.put(0,q,&b);assert(!f.find(0,k));assert(f.find(0,q)==&b);
 f.invalidate();assert(!f.find(0,q));f.put(0,k,&a);f.epoch=std::numeric_limits<uint64_t>::max();f.invalidate();assert(!f.find(0,k));
 std::unordered_map<int,int> map;map.emplace(7,77);f.put(1,k,&map.at(7));auto p=f.find(1,k);map.reserve(10000);assert(f.find(1,k)==p && *p==77);
 // Owning-map mutation must invalidate the front before erasing/freeing nodes.
 f.invalidate();map.erase(7);assert(!f.find(1,k));map.emplace(7,88);f.put(1,k,&map.at(7));assert(*f.find(1,k)==88);f.invalidate();map.clear();assert(!f.find(1,k));
 puts("H11 image-front full-key, slot, epoch, replacement, rehash, erase and clear: PASS");
}
