// Callback de página personal PC: sólo pide el cambio al módulo existente.
#include "../dm_cursor.c"
#include "../diag.c"
#undef off
module_t g_mod[MAX_MOD];int g_nmod;
static int ingame=1,busy,checks,failures,flips,last_dir,change;static uint64_t *root_field;static uint32_t *tab_field;
int dm_uiscreen_ingame(void){return ingame;}int dm_padmode_busy(void){return busy;}
#define CHECK(c,t)do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
#define Q(p,o,v) (*(uint64_t*)((p)+(o))=(uint64_t)(v))
static void flip(int dir){flips++;last_dir=dir;}
int dm_ui_pc_desktop(void){return 1;}
void dm_pages_request_flip(int dir){flip(dir);}
static int read_item(uint64_t p,void *out,size_t n){int mode=change;change=0;if(mode==1)atomic_fetch_add(&g_pc_tabbar_epoch,1);if(mode==2)atomic_fetch_add(&g_ui_gen,1);if(mode==3)*root_field=0;if(mode==4)*tab_field=1;if(mode==5)busy=1;if(mode==6)atomic_store(&g_load_up,1);if(mode==7)ingame=0;memcpy(out,(void*)p,n);return 1;}
static int unknown(uint64_t p,void*out,size_t n){(void)p;(void)out;(void)n;return 0;}
int main(int argc,char **argv){
 module_t m={.size=0x2f05460};m.base=calloc(1,m.size);if(!m.base)return 2;
 uint8_t root[0x200]={0},bank[0x200]={0},tabs[0xe00]={0},player[0x200]={0},inventory[0x100]={0},modal[0x200]={0},leaf[0x100]={0};
 uint64_t top[]={(uint64_t)bank,(uint64_t)modal},children[]={(uint64_t)tabs},modal_children[]={(uint64_t)leaf};uint64_t r=(uint64_t)root;
 Q(m.base,0x2f05458,root);Q(root,0x58,top);Q(root,0x60,2);root[0xd9]=1;Q(bank,8,"BankExpansionLayout");Q(bank,0x58,children);Q(bank,0x60,1);bank[0x50]=bank[0x51]=tabs[0x50]=tabs[0x51]=1;Q(tabs,0,(uint64_t)m.base+0x20ba3a8);Q(tabs,8,"BankTabs");
 *(uint32_t*)(player+8)=1;Q(player,0x90,inventory);*(uint32_t*)inventory=0x01020304;Q(m.base,0x2d603a0+1*8,player);*(uint32_t*)(m.base+0x2d20e4c)=0;*(uint32_t*)(m.base+0x2d60380)=1;
 root_field=(uint64_t*)(m.base+0x2f05458);tab_field=(uint32_t*)(tabs+0xdc4);atomic_store(&g_ui_gen,12);atomic_store(&g_pc_tabbar_epoch,7);atomic_store(&g_load_up,0);
 const char *category="PanelManager",*action="ClosePanel",*arg="TouchPersonalStashNext";int desktop=1;
#define ROUTE() pc_personal_page_route(&m,r,category,action,arg,desktop,read_item,flip)
 g_mod[0]=m;strcpy(g_mod[0].name,"main");g_mod[0].ro_off=0x1000;g_mod[0].data_off=0x1100;const char names[]="PanelManager\0ClosePanel\0BankPanelMessage\0SharedStashRight\0";memcpy(m.base+0x1000,names,sizeof names);g_nmod=1;uih_build();
 char cat_buffer[24],act_buffer[24];const char *resolved_category=uih_name(ui_hash("PanelManager"),cat_buffer),*resolved_action=uih_name(ui_hash("ClosePanel"),act_buffer);
 CHECK(!strcmp(resolved_category,"PanelManager")&&!strcmp(resolved_action,"ClosePanel"),"Bus real resuelve nombres nativos sin registro privado");
 pc_personal_page_message(resolved_category,resolved_action,"TouchPersonalStashNext");CHECK(flips==1&&last_dir==1,"Entrada real del observer solicita Next una sola vez");
 pc_personal_page_message(resolved_category,resolved_action,"TouchPersonalStashPrevious");CHECK(flips==2&&last_dir==-1,"Entrada real del observer conserva Previous");
 CHECK(!strcmp(uih_name(ui_hash("BankPanelMessage"),cat_buffer),"BankPanelMessage")&&!strcmp(uih_name(ui_hash("SharedStashRight"),act_buffer),"SharedStashRight"),"Resolver conserva nombres originales compartidos");
 CHECK(ROUTE()&&flips==3&&last_dir==1,"Índice local0→ID1 y mano vacía permite una petición Next");arg="TouchPersonalStashPrevious";CHECK(ROUTE()&&flips==4&&last_dir==-1,"Previous conserva dirección sin escribir contador UI");arg="TouchPersonalStashNext";
 for(unsigned tab=1;tab<=6;tab++){*tab_field=tab;CHECK(!ROUTE(),"Compartido/Tesoro/Crónica/tabs desconocidos no cambian página personal");}*tab_field=0;
 category="BankPanelMessage";action="SharedStashRight";CHECK(!ROUTE(),"Compartido conserva su ruta nativa");category="PanelManager";action="ClosePanel";arg="TouchStashRight";CHECK(!ROUTE(),"No-op anterior no cambia páginas");arg="TouchPersonalStashNextExtra";CHECK(!ROUTE(),"Sufijo parecido no cambia páginas");category="PersonalStashMessage";action="NextPage";arg="TouchPersonalStashNext";CHECK(!ROUTE(),"Categoría antigua no registrada se rechaza");category="PanelManager";action="OpenPanel";CHECK(!ROUTE(),"Sólo ClosePanel exacto admite el protocolo");action="ClosePanel";
 desktop=0;CHECK(!ROUTE(),"Móvil/mando conservan su ruta anterior");desktop=1;ingame=0;CHECK(!ROUTE(),"Frontend no encola cambio");ingame=1;busy=1;CHECK(!ROUTE(),"Transición de modo no encola cambio");busy=0;atomic_store(&g_load_up,1);CHECK(!ROUTE(),"Carga no encola cambio");atomic_store(&g_load_up,0);
 for(unsigned i=0;i<2;i++){bank[0x50+i]=0;CHECK(!ROUTE(),"Banco oculto/inactivo no cambia página");bank[0x50+i]=1;tabs[0x50+i]=0;CHECK(!ROUTE(),"Pestañas ocultas/inactivas no cambian página");tabs[0x50+i]=1;}
 Q(tabs,0,1);CHECK(!ROUTE(),"Clase desconocida no interpreta selected");Q(tabs,0,(uint64_t)m.base+0x20ba3a8);Q(tabs,8,"BankTabs");Q(bank,0x60,0);CHECK(!ROUTE(),"Tabs retiradas no cambian página");Q(bank,0x60,1);*root_field=1;CHECK(!ROUTE(),"Root anterior no se desreferencia");*root_field=r;
 Q(inventory,0x40,leaf);CHECK(!ROUTE(),"Índice local 0 resuelve ID 1 y su ítem sostenido veta el cambio de página");Q(inventory,0x40,0);*(uint32_t*)(m.base+0x2d20e4c)=8;CHECK(!ROUTE(),"Índice local fuera de 0–7 no accede a otra unidad");*(uint32_t*)(m.base+0x2d20e4c)=0;*(uint32_t*)(m.base+0x2d60380)=UINT32_MAX;CHECK(!ROUTE(),"ID local indefinido no se interpreta como mano vacía");*(uint32_t*)(m.base+0x2d60380)=2;CHECK(!ROUTE(),"ID local sin unidad no permite cambiar página");*(uint32_t*)(m.base+0x2d60380)=1;CHECK(!pc_personal_page_route(&m,r,category,action,arg,1,unknown,flip),"Held indefinido no se adivina como mano vacía");
 Q(modal,8,"ConfirmationModal");Q(modal,0x58,modal_children);Q(modal,0x60,1);modal[0x50]=modal[0x51]=leaf[0x50]=leaf[0x51]=1;CHECK(!ROUTE(),"Modal nativo visible no se cierra ni se atraviesa");modal[0x50]=modal[0x51]=0;
 g_pc_options_event_scope=(pc_options_event_scope){.active=1,.epoch=6};CHECK(!ROUTE(),"UP de época anterior no cambia página");g_pc_options_event_scope=(pc_options_event_scope){0};
 for(int mode=1;mode<=7;mode++){change=mode;CHECK(!ROUTE(),"Epoch/generación/root/tab/modo/carga/partida cambiados durante lectura cancelan petición");atomic_store(&g_pc_tabbar_epoch,7);atomic_store(&g_ui_gen,12);*root_field=r;*tab_field=0;busy=0;ingame=1;atomic_store(&g_load_up,0);}
 CHECK(flips==4&&!TAB_SEL(tabs),"Todos los negativos conservan páginas y contador, sin callback duplicado");CHECK(ROUTE()&&flips==5&&last_dir==1,"Nuevo callback válido funciona después de cancelaciones");
 if(argc>1){FILE *f=fopen(argv[1],"rb");if(!f)return 2;uint64_t hashes[2];char argument[64];int32_t expected;unsigned records=0;
  while(fread(hashes,sizeof hashes,1,f)==1){if(fread(argument,sizeof argument,1,f)!=1||fread(&expected,sizeof expected,1,f)!=1||argument[63])return 2;int old_flips=flips;const char *cat=uih_name(hashes[0],cat_buffer),*act=uih_name(hashes[1],act_buffer);pc_personal_page_message(cat,act,argument);CHECK(flips-old_flips==(expected!=0)&&(!expected||last_dir==expected),"Registro del parser+Button ARM atraviesa resolver+observer C reales una sola vez");records++;}fclose(f);CHECK(records==4,"Cuatro registros ARM reales incluidos controles negativos");
 }
 free(g_uih);free(m.base);printf("PC STASH PAGES %d comprobaciones, %d fallos\n",checks,failures);return!!failures;
}
