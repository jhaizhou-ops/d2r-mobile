// Real PC clone held state; doubled dispatcher, production getter and cancellation.
#define DM_PC_ITEMS_CONNECTED 0
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD];int g_nmod;
static int desktop=1,ingame=1,busy,world=1,checks,failures;
int dm_ui_pc_desktop(void){return desktop;}int dm_uiscreen_ingame(void){return ingame;}int dm_padmode_busy(void){return busy;}int dm_touch_host_world_active(void){return world;}
uint64_t dm_guest_frame(void){return 123;}
void dm_world_pc_skill_cancel(void){}
#define CHECK(c,t) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
#define Q(p,o,v) (*(uint64_t*)((p)+(o))=(uint64_t)(v))
static int press(const void *arg,void *table,int force){const pc_binding_record *r=arg;CHECK(force==1,"DOWN conserva force de partida");((pc_binding_entry*)table)[r->action].latched=1;return 1;}
static int release(const void *arg,void *table,int force){const pc_binding_record*r=arg;(void)force;((pc_binding_entry*)table)[r->action].latched=0;return 1;}
#ifdef DM_TEST_BINDING_HELD_BASELINE
#define HELD(a) 0
#else
#define HELD(a) dm_ui_pc_binding_held(a)
#endif
static void release_consumed(const void *arg,void *table,int force){(void)arg;(void)table;(void)force;}
static int release_noop(const void *arg,void *table,int force){release_consumed(arg,table,force);return 0;}
static uint64_t mutate_at;static int mutation,reads;
static int reader(uint64_t at,void*out,size_t size){reads++;if(mutation&&at==mutate_at){int k=mutation;mutation=0;if(k==1)atomic_fetch_add(&g_ui_gen,1);if(k==2)atomic_fetch_add(&g_pc_tabbar_epoch,1);if(k==3)Q(g_mod[0].base,0x2f05458,0);if(k==4)busy=1;if(k==5)world=0;if(k==6)atomic_store(&g_load_up,1);if(k==7)atomic_store(&g_pc_binding_cancel,1);if(k==8)atomic_store(&g_pc_key_reset,1);}return pc_tooltip_read(at,out,size);}
int main(void){
 module_t *m=&g_mod[0];strcpy(m->name,"main");g_nmod=1;m->size=0x2f05460;m->base=calloc(1,m->size);if(!m->base)return 2;
 FILE*f=fopen(getenv("DM_TEST_MAIN")?getenv("DM_TEST_MAIN"):"build/main.orig.bin","rb");if(!f||fread(m->base,1,m->size,f)!=m->size)return 2;fclose(f);
 _Alignas(8) uint8_t root[0x200]={0};Q(m->base,0x2f05458,root);root[0xd9]=root[0x50]=root[0x51]=1;
 atomic_store(&g_ui_gen,6);atomic_store(&g_pc_tabbar_epoch,2);atomic_store(&g_load_up,0);atomic_store(&g_pc_binding_cancel,0);atomic_store(&g_pc_key_reset,0);
 g_pc_binding_table_valid=1;g_pc_binding_base=m->base;memset(g_pc_binding_table,0,sizeof g_pc_binding_table);
 CHECK(dm_ui_pc_binding(36,1),"DOWN36 se encola por API real");pc_binding_flush(m,g_pc_binding_table,press,release);
 *(uint32_t*)(m->base+0x2d20e5c)=0;
 CHECK(HELD(36),"Held del clon persiste aunque Update borre el pulso nativo sin UP");
 CHECK(dm_ui_pc_binding(36,0),"UP36 se encola por API real");pc_binding_flush(m,g_pc_binding_table,press,release);CHECK(!HELD(36),"UP real termina held");

 // All action IDs use the same clone latch and never import the original pulse.
 for(int action=0;action<69;action++){CHECK(dm_ui_pc_binding(action,1),"DOWN válido se encola");pc_binding_flush(m,g_pc_binding_table,press,release);CHECK(HELD(action),"El flanco real del clon publica held por acción");dm_ui_pc_binding(action,0);pc_binding_flush(m,g_pc_binding_table,press,release);CHECK(!HELD(action),"UP invalida held de cada acción");}
 dm_ui_pc_binding(36,1);pc_binding_flush(m,g_pc_binding_table,press,release);
 uint8_t *image=malloc(m->size);if(!image)return 2;memcpy(image,m->base,m->size);pc_binding_entry clone_before[69];pc_binding_held_context context_before[69];memcpy(clone_before,g_pc_binding_table,sizeof clone_before);memcpy(context_before,g_pc_binding_held,sizeof context_before);uint8_t root_before[sizeof root];memcpy(root_before,root,sizeof root);
 for(int i=0;i<1000;i++)if(!HELD(36)){CHECK(0,"Mil consultas conservan held");break;}
 CHECK(!memcmp(image,m->base,m->size)&&!memcmp(root_before,root,sizeof root)&&!memcmp(clone_before,g_pc_binding_table,sizeof clone_before)&&!memcmp(context_before,g_pc_binding_held,sizeof context_before),"Getter no cambia imagen guest, raíz, tabla clonada ni metadatos host");free(image);
 CHECK(!HELD(-1)&&!HELD(69),"Acciones fuera de rango devuelven cero");
 desktop=0;reads=0;CHECK(!pc_binding_held_read(m,36,desktop,reader)&&!reads,"Legacy no consulta memoria guest");desktop=1;
 CHECK(!pc_binding_held_read(NULL,36,1,reader),"Módulo ausente falla cerrado");
 g_pc_binding_table_valid=0;CHECK(!HELD(36),"Tabla no inicializada no expresa held");g_pc_binding_table_valid=1;g_pc_binding_base++;CHECK(!HELD(36),"Otra imagen no reutiliza latch");g_pc_binding_base--;
 m->base[0x3700e0]^=1;CHECK(!HELD(36),"Dispatcher ABI distinto rechaza held");m->base[0x3700e0]^=1;
 world=0;CHECK(!HELD(36),"Panel o mundo no activo oculta held");world=1;busy=1;CHECK(!HELD(36),"Reconstrucción no expresa held");busy=0;ingame=0;CHECK(!HELD(36),"Frontend no expresa held");ingame=1;atomic_store(&g_load_up,1);CHECK(!HELD(36),"Carga no expresa held");atomic_store(&g_load_up,0);
 for(unsigned i=0;i<2;i++){root[0x50+i]=0;CHECK(!HELD(36),"Raíz oculta o inactiva rechaza held");root[0x50+i]=1;}root[0xd9]=0;CHECK(!HELD(36),"Raíz fuera de ToGame rechaza held");root[0xd9]=1;
 Q(m->base,0x2f05458,0);CHECK(!HELD(36),"Raíz destruida no reutiliza held");Q(m->base,0x2f05458,root);
 atomic_fetch_add(&g_ui_gen,1);CHECK(!HELD(36),"Cambio de generación invalida held antiguo");atomic_store(&g_ui_gen,6);atomic_fetch_add(&g_pc_tabbar_epoch,1);CHECK(!HELD(36),"Cambio de epoch invalida held antiguo");atomic_store(&g_pc_tabbar_epoch,2);
 atomic_store(&g_pc_key_reset,1);CHECK(!HELD(36),"Reset pendiente no expresa held");atomic_store(&g_pc_key_reset,0);
 _Alignas(8) uint8_t modal[0x100]={0},leaf[0x100]={0};char modal_name[]="ConfirmationModal";uint64_t children[]={(uint64_t)modal},leaves[]={(uint64_t)leaf};Q(modal,8,modal_name);modal[0x50]=modal[0x51]=leaf[0x50]=leaf[0x51]=1;Q(modal,0x58,leaves);Q(modal,0x60,1);Q(root,0x58,children);Q(root,0x60,1);CHECK(!HELD(36),"Modal visible conserva prioridad sobre held");Q(root,0x58,0);Q(root,0x60,0);
 mutate_at=(uint64_t)root+0x58;for(int k=1;k<=8;k++){mutation=k;CHECK(!pc_binding_held_read(m,36,1,reader),"Contexto que cambia durante consulta no se publica");atomic_store(&g_ui_gen,6);atomic_store(&g_pc_tabbar_epoch,2);Q(m->base,0x2f05458,root);busy=0;world=1;atomic_store(&g_load_up,0);atomic_store(&g_pc_binding_cancel,0);atomic_store(&g_pc_key_reset,0);}
 dm_ui_pc_bindings_cancel();CHECK(!HELD(36)&&g_pc_binding_table[36].latched,"Cancelación invalida al instante sin mutar latch antes de release guest");
 pc_binding_release_all(release_noop);atomic_store(&g_pc_binding_cancel,0);atomic_store(&g_pc_key_reset,0);CHECK(!HELD(36),"Release consumido no resucita el latch antiguo al recuperar mundo");
 g_pc_binding_table[36].latched=0;dm_ui_pc_binding(36,1);pc_binding_flush(m,g_pc_binding_table,press,release);CHECK(HELD(36),"Un nuevo flanco después de cancelar vuelve a publicar held");
 dm_ui_pc_binding(36,0);pc_binding_flush(m,g_pc_binding_table,press,release_noop);CHECK(!HELD(36)&&g_pc_binding_table[36].latched,"UP consumido invalida observación host aunque el latch nativo siga puesto");
 g_pc_binding_table[36].latched=0;dm_ui_pc_binding(36,1);pc_binding_flush(m,g_pc_binding_table,press,release);uint32_t old_epoch=g_pc_binding_held[36].epoch;atomic_fetch_add(&g_pc_tabbar_epoch,1);g_pc_binding_desired[36]=0;dm_ui_pc_binding(36,1);pc_binding_flush(m,g_pc_binding_table,press,release);CHECK(!HELD(36)&&g_pc_binding_held[36].epoch==old_epoch,"Repetir DOWN con latch antiguo no renueva artificialmente su contexto");
 free(m->base);printf("PC BINDING HELD %d comprobaciones, %d fallos\n",checks,failures);return!!failures;
}
