// AppKit request, native Chronicle close, then editor; no app or preferences.
#define DM_PC_ITEMS_CONNECTED 0
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD];int g_nmod;
static int desktop=1,ingame=1,checks,failures;static uint64_t frame;
int dm_ui_pc_desktop(void){return desktop;}int dm_uiscreen_ingame(void){return ingame;}
uint64_t dm_guest_frame(void){return frame;}
#define CHECK(c,t) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
static pc_controls_context context;
static uint64_t token(void){return (uint64_t)context.generation<<32|context.epoch;}
static void reset(void){memset(&g_pc_controls_open,0,sizeof g_pc_controls_open);context=(pc_controls_context){.root=0x1234,.frame=10,.epoch=atomic_load(&g_pc_tabbar_epoch),.generation=atomic_load(&g_pc_controls_context),.ready=1,.pause_ready=1};}
static int advance(void){context.frame++;return pc_controls_transition(0,&context);}
static int sends;static void send_close(const char *category,const char *action,const char *name){CHECK(!strcmp(category,"PanelManager")&&!strcmp(action,"ClosePanel")&&!strcmp(name,"ChroniclePanel"),"Cierre usa exactamente el mensaje original del botón");sends++;}
int main(void){
 reset();context.chronicle=1;context.pages=1;
 CHECK(pc_controls_transition(token(),&context)==3,"Crónica se cierra antes de abrir Settings");
 CHECK(!advance()&&!advance(),"No repite cierre mientras OnClose está pendiente");
 context.chronicle=0;CHECK(!advance(),"Padre cerrado no basta con páginas todavía visibles");context.pages=0;context.pause_ready=0;CHECK(!advance(),"Espera restauración nativa de Pausa");context.pause_ready=1;
 CHECK(advance()==1,"Sólo padre e hijos cerrados y Pausa restaurada permiten Settings");
 CHECK(!advance(),"No repite OpenPanel mientras Settings se construye");context.settings=1;
 CHECK(advance()==2,"CheckChanges se conserva tras Settings visible");CHECK(!advance(),"No repite CheckChanges ni aplica ajustes");context.menu=1;CHECK(!advance()&&!g_pc_controls_open.phase,"Editor visible completa transición");
 reset();CHECK(pc_controls_transition(token(),&context)==1,"Mundo conserva apertura directa de Settings");
 reset();context.settings=1;CHECK(pc_controls_transition(token(),&context)==2,"Settings existente conserva CheckChanges y sus confirmaciones");
 reset();context.menu=1;CHECK(!pc_controls_transition(token(),&context)&&!g_pc_controls_open.phase,"Editor ya visible no se reconstruye");
 reset();context.pages=1;CHECK(!pc_controls_transition(token(),&context),"Página huérfana no permite Settings ni cierre de padre inexistente");context.frame+=181;CHECK(!advance()&&!g_pc_controls_open.phase,"Huérfano sin cleanup caduca sin abrir debajo");
 for(int stage=0;stage<3;stage++)for(int reason=0;reason<7;reason++){
  reset();context.chronicle=stage==0;context.pages=stage==0;
  CHECK(pc_controls_transition(token(),&context)==(stage==0?3:1),"Prepara cada fase real de transición");
  if(stage==2){context.settings=1;CHECK(advance()==2,"Prepara fase de confirmación/editor");}
  if(reason==0)context.epoch++;if(reason==1)context.generation++;if(reason==2)context.root++;
  if(reason==3)context.ready=0;if(reason==4)context.blocked=1;if(reason==5)context.frame+=181;if(reason==6)context.root=0;
  CHECK(!advance()&&!g_pc_controls_open.phase,"Blur/contexto/raíz/carga/modal/deadline cancela en cada fase");
  context.chronicle=context.pages=context.blocked=0;context.ready=context.pause_ready=1;context.settings=0;context.root=0x1234;
  CHECK(!advance(),"Retirar bloqueo no revive una solicitud cancelada");
 }
 reset();uint64_t before=token();dm_ui_pc_bindings_cancel();context.epoch=atomic_load(&g_pc_tabbar_epoch);
 CHECK(!pc_controls_transition(before,&context),"Blur real antes de consumir request invalida token");
 CHECK(pc_controls_transition(token(),&context)==1,"Nuevo comando tras blur funciona sin barrera adicional");
 CHECK(pc_controls_release_input(),"Liberación propia usa CAS y no absorbe cancelaciones concurrentes");context.epoch=atomic_load(&g_pc_tabbar_epoch);
 CHECK(!advance(),"Reset interno de apertura no se confunde con blur posterior");context.settings=1;CHECK(advance()==2,"Reset propio no impide completar CheckChanges");
 reset();CHECK(pc_controls_transition(token(),&context)==1,"Prepara acción aún no ejecutada");dm_ui_pc_bindings_cancel();CHECK(!pc_controls_release_input()&&!g_pc_controls_open.phase,"Blur entre decisión y envío no se absorbe como reset propio");
 reset();CHECK(pc_controls_transition(token(),&context)==1,"Prepara decisión antes de notificación asíncrona");dm_ui_gen_bump(DM_UIGEN_LANG);CHECK(!pc_controls_release_input()&&!g_pc_controls_open.phase,"Cambio de contexto entre decisión y envío no pasa por CAS de blur");
 reset();CHECK(pc_controls_transition(token(),&context)==1,"Prepara Settings en construcción");context.chronicle=1;CHECK(!advance()&&!g_pc_controls_open.phase,"Nueva Crónica durante construcción cancela antes de CheckChanges");
 reset();before=atomic_load(&g_pc_controls_context);dm_ui_gen_bump(DM_UIGEN_PANEL_GONE);CHECK(atomic_load(&g_pc_controls_context)==before,"Descarga esperada de Crónica no cancela el contexto");
 for(int why=DM_UIGEN_OPMODE;why<=DM_UIGEN_ROOT;why++)if(why!=DM_UIGEN_PANEL_GONE){before=atomic_load(&g_pc_controls_context);dm_ui_gen_bump(why);CHECK(atomic_load(&g_pc_controls_context)==(uint32_t)before+1,"Modo/rendimiento/idioma/raíz cambian epoch de contexto");}
 for(int why=DM_UIGEN_OPMODE;why<=DM_UIGEN_ROOT;why++){
  reset();context.chronicle=1;CHECK(pc_controls_transition(token(),&context)==3,"Prepara cierre antes de notificación real de UI");
  dm_ui_gen_bump(why);context.generation=atomic_load(&g_pc_controls_context);
  CHECK(!advance(),"Notificación no abre Settings antes de cleanup");
  if(why==DM_UIGEN_PANEL_GONE){CHECK(g_pc_controls_open.phase==5,"Unload nativo mantiene espera de cleanup");context.chronicle=0;CHECK(advance()==1,"Unload esperado permite avanzar tras cierre real");}
  else CHECK(!g_pc_controls_open.phase,"Notificación real de modo/idioma/raíz cancela espera");
 }
 reset();atomic_store(&g_pc_profile_ready,1);CHECK(dm_ui_pc_controls(),"Productor AppKit encola sólo un token atómico");CHECK(atomic_exchange(&g_pc_controls_request,0)==token(),"Token preserva epoch de blur y de contexto de publicación");
 module_t module={.size=0x2f05460};module.base=calloc(1,module.size);if(!module.base)return 2;
 _Alignas(8)uint8_t root[0x200]={0},chron[0x200]={0},page[0x100]={0},pause[0x100]={0},button[0x100]={0},modal[0x100]={0};
 uint64_t children[]={(uint64_t)chron,(uint64_t)page,(uint64_t)pause,(uint64_t)modal},pending[]={(uint64_t)chron},buttons[]={(uint64_t)button};
 *(uint64_t*)(root+0x58)=(uint64_t)children;*(uint64_t*)(root+0x60)=4;
 *(const char**)(chron+8)="ChroniclePanel";*(uint64_t*)chron=(uint64_t)module.base+0x20ba740;chron[0x50]=chron[0x51]=1;
 *(const char**)(page+8)="ChronicleUniquesListPanel";page[0x50]=page[0x51]=1;
 *(const char**)(pause+8)="PauseLayoutGarden";*(uint64_t*)pause=(uint64_t)module.base+0x20ba830;pause[0x50]=pause[0x51]=1;
 *(uint64_t*)(pause+0x58)=(uint64_t)buttons;*(uint64_t*)(pause+0x60)=1;button[0x50]=button[0x51]=1;
 CHECK(pc_controls_current_root(&module,(uint64_t)root)==0,"Root no publicado se rechaza antes de leer widgets");
 *(uint64_t*)(module.base+0x2f05458)=(uint64_t)root;
 CHECK(pc_controls_current_root(&module,(uint64_t)root)==(uint64_t)root&&!pc_controls_current_root(&module,1),"Snapshot exige raíz actual sin desreferenciar raíz anterior");
 module.base[0x2d5ff51]=1;int c,p,pr;pc_controls_chronicle_state(&module,(uint64_t)root,&c,&p,&pr);
 CHECK(c&&p&&pr,"Snapshot fresco reconoce padre, hijo y ambas variantes de Pausa");
 button[0x51]=0;pc_controls_chronicle_state(&module,(uint64_t)root,&c,&p,&pr);CHECK(!pr,"Pausa todavía con hijos ocultos no está restaurada");
 *(uint64_t*)(root+0x58)=(uint64_t)(children+1);*(uint64_t*)(root+0x60)=3;*(uint64_t*)(root+0x88)=(uint64_t)pending;*(uint64_t*)(root+0x90)=1;
 pc_controls_chronicle_state(&module,(uint64_t)root,&c,&p,&pr);CHECK(c&&p,"Registro88 evita apertura bajo OnOpen previo al attach58");
 *(uint64_t*)(root+0x90)=0;*(uint64_t*)(root+0xa0)=(uint64_t)pending;*(uint64_t*)(root+0xa8)=1;
 pc_controls_chronicle_state(&module,(uint64_t)root,&c,&p,&pr);CHECK(!c&&p,"Registro retiradoA0 no se usa como propiedad viva");
 *(uint64_t*)(root+0x58)=(uint64_t)children;*(uint64_t*)(root+0x60)=4;*(uint64_t*)(root+0xa8)=0;
 *(uint64_t*)(module.base+0x2f05458)=(uint64_t)root;
 const uint32_t prefix[]={0xd10203ff,0xa9026ffc,0xa90367fa,0xa9045ff8};memcpy(module.base+0xae390c,prefix,sizeof prefix);
 CHECK(pc_controls_close_chronicle(&module,(uint64_t)root,send_close)&&sends==1,"Cierre nativo se envía con clase y ABI comprobadas");
 *(uint64_t*)chron+=8;CHECK(!pc_controls_close_chronicle(&module,(uint64_t)root,send_close)&&sends==1,"Clase homónima distinta no se cierra");*(uint64_t*)chron-=8;
 *(const char**)(modal+8)="ConfirmationModal";modal[0x50]=modal[0x51]=1;*(uint64_t*)(modal+0x58)=(uint64_t)buttons;*(uint64_t*)(modal+0x60)=1;button[0x51]=1;
 CHECK(!pc_controls_close_chronicle(&module,(uint64_t)root,send_close)&&sends==1,"Confirmación nativa conserva su propio cierre y cambios");modal[0x50]=0;
 modal[0x50]=1;*(uint64_t*)(root+0x60)=3;uint64_t pending_modal[]={(uint64_t)modal};*(uint64_t*)(root+0x88)=(uint64_t)pending_modal;*(uint64_t*)(root+0x90)=1;
 CHECK(pc_controls_modal((uint64_t)root)&&!pc_controls_close_chronicle(&module,(uint64_t)root,send_close),"Modal pendiente88 bloquea sin esperar attach ni hoja visible");
 *(const char**)(modal+8)="YouDiedModal";CHECK(pc_controls_modal((uint64_t)root),"Muerte pendiente cancela también sin contenido construido");
 modal[0x50]=0;*(uint64_t*)(root+0x90)=0;*(uint64_t*)(root+0x60)=4;
 module.base[0xae390c]^=1;CHECK(!pc_controls_close_chronicle(&module,(uint64_t)root,send_close),"ABI diferente cancela antes de enviar mensaje");module.base[0xae390c]^=1;
 CHECK(!pc_controls_close_chronicle(&module,(uint64_t)root+8,send_close),"Raíz anterior no se desreferencia");
 strcpy(g_chr_child[0],"TouchKillBoard");*(const char**)(page+8)="TouchKillBoard";pc_controls_chronicle_state(&module,(uint64_t)root,&c,&p,&pr);CHECK(p,"Páginas añadidas por el port también bloquean hasta cerrar");
 *(uint64_t*)(root+0x90)=257;pc_controls_chronicle_state(&module,(uint64_t)root,&c,&p,&pr);CHECK(p,"Registro malformado impide avanzar sin adivinar estado");
 uint32_t prior_epoch=atomic_load(&g_pc_tabbar_epoch);
 pc_items_pointer(DM_UP_END,0,0);CHECK(atomic_load(&g_pc_tabbar_epoch)==prior_epoch+1,"Pointer leave advances the epoch after cancelling the optional item module");
 prior_epoch=atomic_load(&g_pc_tabbar_epoch);pc_items_pointer(UI_POINTER_CANCEL,0,0);CHECK(atomic_load(&g_pc_tabbar_epoch)==prior_epoch+1,"Queue cancellation permits only future physical events to re-arm");
 desktop=0;prior_epoch=atomic_load(&g_pc_tabbar_epoch);pc_items_pointer(DM_UP_END,0,0);CHECK(atomic_load(&g_pc_tabbar_epoch)==prior_epoch,"Legacy pointer leave preserves the PC epoch");
 free(module.base);printf("PC CONTROLS TRANSITION %d comprobaciones, %d fallos\n",checks,failures);return !!failures;
}
