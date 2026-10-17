/* Reuses the original r32 OBS_TICK semantics, with a transparent observation
 * after SP allocation. Publish identity only; read tick on the main thread. */
static _Atomic uint64_t world_tick_game;
void xblr_world_tick_observe(uint64_t game){atomic_store_explicit(&world_tick_game,game,memory_order_release);}
static int world_tick_read(module_t *m,uint64_t *identity,uint32_t *out){
 uint64_t game=atomic_load_explicit(&world_tick_game,memory_order_acquire);if(!m||!game||!identity||!out||m->size<0x2d685d8)return 0;
 int live=0;uint64_t slots[128];
 for(unsigned block=0;block<8;block++){
  if(!xblr_guest_read(slots,(uintptr_t)m->base+0x2d665d8+block*sizeof slots,sizeof slots))return 0;
  for(unsigned i=0;i<128;i++)if(slots[i]+1>=2)live=1;
 }
 if(!live){atomic_compare_exchange_strong(&world_tick_game,&game,0);return 0;}
 uint32_t tick;if(!game||game>UINT64_MAX-0x170||!xblr_guest_read(&tick,game+0x170,4))return 0;
 if(atomic_load_explicit(&world_tick_game,memory_order_acquire)!=game)return 0;
 *identity=game;*out=tick;return 1;
}
