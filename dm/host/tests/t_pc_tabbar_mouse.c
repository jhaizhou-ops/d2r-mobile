// Captura de clic PC: ninguna coordenada o tecla de mando participa en la selección.
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD];int g_nmod;
static int checks,failures;
static int ingame=1;
int dm_ui_pc_desktop(void){return 1;}int dm_uiscreen_ingame(void){return ingame;}
void dm_custom_pointer_input(void){}
static int hit_calls;static uint64_t hit_result,hit_xy;
static uint64_t hit(uint64_t w,uint64_t xy){(void)w;hit_calls++;hit_xy=xy;return hit_result;}
#define CHECK(c,t)do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
static int step(pc_tabbar_capture*c,int kind,int after,uint64_t root,uint64_t widget,int index,int current,uint32_t gen){return pc_tabbar_step(c,kind,after,root,widget,index,current,gen);}
static void down(pc_tabbar_capture*c){step(c,DM_UP_LDOWN,0,1,10,2,0,1);step(c,DM_UP_LDOWN,1,1,10,2,0,1);}
static void pending_blur(void){
 pc_tabbar_capture c={0};ui_pointer_event e;int selected=0,downs=0,ups=0,ends=0;
 dm_ui_ptr_at(DM_UP_LDOWN,.4f,.2f,0);dm_ui_ptr_at(DM_UP_LUP,.4f,.2f,0);dm_ui_ptr_at(DM_UP_END,-1,-1,0);dm_ui_pc_bindings_cancel();
 while(ui_pointer_pop(&e)){
  if(atomic_exchange(&g_pc_tabbar_cancel,0))step(&c,DM_UP_END,0,0,0,-1,0,0);
  step(&c,DM_UP_MOVE,0,1,10,2,0,1);
  if(e.kind==DM_UP_END){step(&c,DM_UP_END,0,0,0,-1,0,0);ends++;continue;}
  pc_tabbar_event_enter(&e);
  step(&c,e.kind,0,1,10,2,0,1);downs+=e.kind==DM_UP_LDOWN;ups+=e.kind==DM_UP_LUP;ui_pointer_track(e.kind,e.arg);
  selected+=step(&c,e.kind,1,1,10,2,0,1)>=0;
  pc_tabbar_event_leave();
 }
 CHECK(!selected,"Blur anterior a pump invalida DOWN/UP ya encolados, antes de alcanzar END");CHECK(downs==1&&ups==1&&ends==1&&!g_up_buttons[0],"Cancelar selección conserva los eventos y la liberación original");
 down(&c);ui_pointer_track(DM_UP_LDOWN,0);dm_ui_ptr_at(DM_UP_LUP,.4f,.2f,0);dm_ui_pc_bindings_cancel();CHECK(ui_pointer_pop(&e)&&e.kind==DM_UP_LUP,"UP de pulsación ya entregada permanece en la cola tras cancelar");pc_tabbar_event_enter(&e);ui_pointer_track(e.kind,e.arg);CHECK(step(&c,e.kind,1,1,10,2,0,1)<0&&!g_up_buttons[0],"UP antiguo libera el botón sostenido sin confirmar la pestaña");pc_tabbar_event_leave();
 dm_ui_pc_bindings_cancel();dm_ui_ptr_at(DM_UP_LDOWN,.4f,.2f,0);dm_ui_ptr_at(DM_UP_LUP,.4f,.2f,0);selected=0;
 while(ui_pointer_pop(&e)){if(atomic_exchange(&g_pc_tabbar_cancel,0))step(&c,DM_UP_END,0,0,0,-1,0,0);pc_tabbar_event_enter(&e);step(&c,e.kind,0,1,10,2,0,1);ui_pointer_track(e.kind,e.arg);selected+=step(&c,e.kind,1,1,10,2,0,1)>=0;pc_tabbar_event_leave();}
 CHECK(selected==1&&!g_up_buttons[0],"Un clic nuevo posterior a cancelar funciona sin esperar END");
}
int main(void){pc_tabbar_capture c={0};
 CHECK(step(&c,DM_UP_LUP,1,1,10,2,0,1)<0&&!c.widget,"UP aislado no selecciona ninguna pestaña");down(&c);CHECK(c.widget==10&&c.index==2,"DOWN captura identidad e índice antes del despacho original");
 CHECK(step(&c,DM_UP_LUP,0,1,10,2,0,1)<0&&c.widget==10,"PreUP sólo valida, conserva entrega del UP original");CHECK(step(&c,DM_UP_LUP,1,1,10,2,0,1)==2&&!c.widget,"PostUP selecciona exactamente una vez después del evento original");CHECK(step(&c,DM_UP_LUP,1,1,10,2,0,1)<0,"UP repetido no reenvía mensajes de selección");
 down(&c);step(&c,DM_UP_MOVE,0,1,0,-1,0,1);step(&c,DM_UP_MOVE,0,1,10,2,0,1);CHECK(step(&c,DM_UP_LUP,1,1,10,2,0,1)<0,"Salir de la pestaña cancela aunque el puntero vuelva antes de UP");
 down(&c);CHECK(step(&c,DM_UP_LUP,1,1,10,3,0,1)<0&&!c.widget,"Soltar sobre otra pestaña no cambia ninguna");
 down(&c);CHECK(step(&c,DM_UP_LUP,1,1,11,2,0,1)<0&&!c.widget,"Otra barra con mismo índice no hereda la captura");
 down(&c);CHECK(step(&c,DM_UP_LUP,1,2,10,2,0,1)<0&&!c.widget,"Cambio de raíz invalida puntero capturado");
 down(&c);CHECK(step(&c,DM_UP_LUP,1,1,10,2,0,2)<0&&!c.widget,"Cambio de generación cancela incluso si se reutiliza la dirección");
 down(&c);CHECK(step(&c,DM_UP_LUP,1,1,10,2,1,1)<0&&!c.widget,"Navegación externa durante la pulsación no se sobrescribe");
 down(&c);step(&c,DM_UP_END,0,0,0,-1,0,0);CHECK(step(&c,DM_UP_LUP,1,1,10,2,0,1)<0&&!c.widget,"END y pérdida de foco cancelan sin activar");
 step(&c,DM_UP_LDOWN,0,1,0,-1,0,1);step(&c,DM_UP_LDOWN,1,1,10,2,0,1);CHECK(step(&c,DM_UP_LUP,1,1,10,2,0,1)<0,"Una pestaña aparecida por el DOWN no captura ese clic");
 step(&c,DM_UP_LDOWN,0,1,10,2,2,1);CHECK(step(&c,DM_UP_LUP,1,1,10,2,2,1)<0&&!c.widget,"Pestaña ya seleccionada no reenvía CheckChanges");
 down(&c);step(&c,DM_UP_LUP,0,1,10,2,0,1);CHECK(step(&c,DM_UP_LUP,1,1,0,-1,0,1)<0,"UP que destruye u oculta la barra no conserva un puntero para seleccionar");
 module_t m={.size=0x2f05460,.text_end=0x1b80000};m.base=calloc(1,m.size);if(!m.base)return 2;
 _Alignas(8)uint8_t root[256]={0},panel[256]={0},tabs[0xeb0]={0},image[0x140]={0},text[0x140]={0};uint64_t r=(uint64_t)root,p=(uint64_t)panel,t=(uint64_t)tabs,im=(uint64_t)image,tx=(uint64_t)text,children[]={t};
 *(uint64_t*)tabs=(uint64_t)m.base+0x20ba3a8;tabs[0x50]=tabs[0x51]=1;*(int32_t*)(tabs+0x558)=1;tabs[0xe88]=1;*(uint64_t*)(tabs+0xdc8)=im;*(uint64_t*)(tabs+0xe08)=tx;
 *(uint64_t*)image=*(uint64_t*)text=(uint64_t)m.base+0x1000;*(uint64_t*)(m.base+0x1048)=(uint64_t)m.base+0x2000;*(uint64_t*)(image+0x130)=1;root[0xb9]=1;
 CHECK(pc_tabbar_hit_ready(&m,r,t),"PC valida clase/visibilidad/count y Image nativa de HD");*(uint64_t*)(image+0x130)=0;CHECK(pc_tabbar_hit_ready(&m,r,t),"Sin gráfico HD acepta Text como hace el original");*(uint64_t*)(tabs+0xe08)=0;CHECK(!pc_tabbar_hit_ready(&m,r,t),"Text nulo impide invocar hit nativo que lo desreferenciaría");*(uint64_t*)(tabs+0xe08)=tx;
 *(uint64_t*)(image+0x138)=1;root[0xb9]=0;CHECK(pc_tabbar_hit_ready(&m,r,t),"SD selecciona su propio recurso gráfico");*(uint64_t*)(tabs+0xdc8)=0;CHECK(!pc_tabbar_hit_ready(&m,r,t),"Image nula se rechaza incluso si Text existe");*(uint64_t*)(tabs+0xdc8)=im;
 *(int32_t*)(tabs+0x558)=9;CHECK(!pc_tabbar_hit_ready(&m,r,t),"Capacidad fija de ocho no se rebasa");*(int32_t*)(tabs+0x558)=-1;CHECK(!pc_tabbar_hit_ready(&m,r,t),"Count negativo no llega al original");*(int32_t*)(tabs+0x558)=1;
 *(uint64_t*)(m.base+0x1048)=(uint64_t)m.base+m.text_end;CHECK(!pc_tabbar_hit_ready(&m,r,t),"Hit de hijo fuera de texto ejecutable no se invoca");*(uint64_t*)(m.base+0x1048)=(uint64_t)m.base+0x2000;
 panel[0x50]=panel[0x51]=1;*(uint64_t*)(panel+0x58)=(uint64_t)children;*(uint64_t*)(panel+0x60)=1;int index=-1;hit_result=0;uint64_t xy=0x1234567887654321;
 CHECK(pc_tabbar_hit_walk(&m,r,p,xy,0,&index,hit)==t&&index==0&&hit_xy==xy,"Recorrido usa hit nativo y pasa XY de64bits sin aproximar coordenadas");hit_result=UINT64_MAX;CHECK(!pc_tabbar_hit_walk(&m,r,p,xy,0,&index,hit),"Miss nativo conserva ausencia de pestaña");hit_result=0x100000000;CHECK(!pc_tabbar_hit_walk(&m,r,p,xy,0,&index,hit),"High32 negativo/no válido no se interpreta como índice0");hit_result=0;tabs[0xe88]=0;CHECK(!pc_tabbar_hit_walk(&m,r,p,xy,0,&index,hit),"Activo0 impide seleccionar aunque el hit simulado devuelva índice");tabs[0xe88]=1;
 panel[0x51]=0;int previous=hit_calls;CHECK(!pc_tabbar_hit_walk(&m,r,p,xy,0,&index,hit)&&hit_calls==previous,"Visibilidad de la cadena impide hit de barra oculta");panel[0x51]=1;*(uint64_t*)(panel+0x60)=257;CHECK(!pc_tabbar_hit_walk(&m,r,p,xy,0,&index,hit),"Árbol malformado no se recorre");*(uint64_t*)(panel+0x60)=1;CHECK(!pc_tabbar_hit_walk(&m,r,p,xy,15,&index,hit),"Profundidad acotada evita recursión ilimitada");
 const uint32_t hc[]={0xa9bb67fa,0xa9015ff8,0xa90257f6,0xa9034ff4},sc[]={0xa9be4ff4,0xa9017bfd,0x910043fd,0xb94dc408};memcpy(m.base+0xad9eb8,hc,sizeof hc);memcpy(m.base+0xad9b80,sc,sizeof sc);CHECK(pc_tabbar_mouse_abi(&m),"Ambos prólogos originales se reconocen");m.base[0xad9ec0]^=1;CHECK(!pc_tabbar_mouse_abi(&m),"ABI de hit distinta bloquea ruta");m.base[0xad9ec0]^=1;m.base[0xad9b84]^=1;CHECK(!pc_tabbar_mouse_abi(&m),"ABI de selección distinta bloquea ruta");m.base[0xad9b84]^=1;m.size--;CHECK(!pc_tabbar_mouse_abi(&m)&&!pc_tabbar_mouse_abi(NULL),"Módulo truncado o ausente falla cerrado");m.size++;
 pc_binding_record *records=(void*)(m.base+0x2086f64);for(int i=0;i<138;i++)records[i]=(pc_binding_record){(uint32_t)(i/2),-1,(uint32_t)(i&1?0:1)};root[0xd9]=1;
 *(uint64_t*)(tabs+0xb50)=ui_hash("SettingsPanelMessage");*(uint64_t*)(tabs+0xb58)=ui_hash("CheckChanges");*(const char**)(tabs+0xb60)="KeyBindingMenu";tabs[0xb68]=1;
 CHECK(pc_tabbar_target_allowed(&m,r,t,0),"Controls se puede seleccionar con perfil nativo jugable vigente");ingame=0;CHECK(!pc_tabbar_target_allowed(&m,r,t,0),"Commit revalida Controls al salir de partida aunque siga activo en la barra");ingame=1;root[0xd9]=0;CHECK(!pc_tabbar_target_allowed(&m,r,t,0),"ToFrontEnd durante clic impide abrir editor por la pestaña");root[0xd9]=1;records[137].type=2;CHECK(!pc_tabbar_target_allowed(&m,r,t,0),"Tabla nativa incompleta también bloquea Controls al commit");ingame=0;*(const char**)(tabs+0xb60)="VideoOptionsPanel";CHECK(pc_tabbar_target_allowed(&m,r,t,0),"Vídeo y demás pestañas frontend conservan selección normal");free(m.base);
 pending_blur();
 printf("PC TABBAR MOUSE %d comprobaciones, %d fallos\n",checks,failures);return!!failures;}
