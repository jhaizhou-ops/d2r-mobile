// Frontera Crear/Intro PC: el proveedor y la validación siguen siendo los originales.
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD];int g_nmod;
static int desktop=1,ingame,busy,checks,failures,calls,cancel;
int dm_ui_pc_desktop(void){return desktop;}int dm_uiscreen_ingame(void){return ingame;}int dm_padmode_busy(void){return busy;}
#define CHECK(c,t)do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
#define Q(p,o,v) (*(uint64_t*)((uint8_t*)(p)+(o))=(uint64_t)(v))
static uint64_t *root_field;static char expected[64];
static int read_fixture(uint64_t p,void*out,size_t n){if(p<4096)return 0;memcpy(out,(void*)p,n);int c=cancel;cancel=0;if(c==1)atomic_fetch_add(&g_ui_gen,1);if(c==2)atomic_fetch_add(&g_pc_tabbar_epoch,1);if(c==3)*root_field=0;if(c==4)busy=1;if(c==5)atomic_store(&g_load_up,1);if(c==6)ingame=1;return 1;}
static int read_none(uint64_t p,void*out,size_t n){(void)p;(void)out;(void)n;return 0;}
static void receive(uint64_t panel,void*opaque){uint64_t *msg=opaque;calls++;CHECK(panel!=0,"Mensaje dirigido al panel de creación vigente");CHECK(msg[0]==ui_hash("ConsoleCharacterCreate")&&msg[1]==ui_hash("TryCreateCharacter"),"Usa el contrato original que copia y valida nombre antes del proveedor");CHECK(msg[3]==1&&msg[4]==1&&!strcmp((const char*)msg[2],expected),"Nombre y validez de payload conservados sin crear un héroe desde host");}
int main(void){
 module_t m={.size=0x2f14300};m.base=calloc(1,m.size);if(!m.base)return 2;FILE*f=fopen("build/main.orig.bin","rb");if(!f)f=fopen("dm/host/tests/build/main.orig.bin","rb");if(!f)return 2;CHECK(fread(m.base,1,m.size,f)==m.size,"Fixture usa bytes originales, no prólogos inventados");fclose(f);
 _Alignas(8)uint8_t root[0x200]={0},panel[0x240]={0},box[0x600]={0},other[0x100]={0};uint64_t roots[]={(uint64_t)panel},children[]={(uint64_t)box};char name[64]="PruebaPC";
 uint64_t base=(uint64_t)m.base,r=(uint64_t)root,p=(uint64_t)panel,msg[5]={ui_hash("CharacterCreate"),ui_hash("Create"),0,1,1};
 Q(root,0x58,roots);Q(root,0x60,1);root[0xd8]=1;Q(panel,0,base+0x20c6e40);Q(panel,8,"CharacterCreatePanel");Q(panel,0x58,children);Q(panel,0x60,1);panel[0x50]=panel[0x51]=1;Q(box,0,base+0x20b8718);Q(box,8,"InputText");box[0x50]=box[0x51]=1;Q(box,0x558,name);Q(box,0x560,strlen(name));Q(m.base,0x2f05458,root);root_field=(uint64_t*)(m.base+0x2f05458);atomic_store(&g_ui_gen,10);atomic_store(&g_pc_tabbar_epoch,20);
#define SEND() pc_create_commit_dispatch(&m,p,(uint64_t)msg,desktop,read_fixture,receive)
 CHECK(pc_create_commit_abi(&m),"Caller y destino originales reconocidos");strcpy(expected,name);
 CHECK(atomic_load(&g_load_up)==-1&&SEND()&&calls==1,"Click Crear funciona en primer frontend frío");
 CHECK(SEND()&&calls==2,"Intro comparte el mismo mensaje Create sin otra ruta de guardado");
 atomic_store(&g_load_up,0);CHECK(SEND()&&calls==3,"Retorno al frontend con carga0 conserva el commit");
 const char *names[]={"","1","A!","Muñeco","LargoNombreQuince","PruebaPC"};
 for(unsigned i=0;i<sizeof names/sizeof *names;i++){strcpy(name,names[i]);strcpy(expected,name);Q(box,0x560,strlen(name));int before=calls;CHECK(SEND()&&calls==before+1,"Vacío/inválido/Unicode se entrega al validador nativo sin filtro host alternativo");}
 int before=calls;msg[1]=ui_hash("ConfirmedCreate");CHECK(!SEND()&&calls==before,"Confirmación original vuelve a c2569c sin repetir Try ni popup");msg[0]=ui_hash("ConsoleCharacterCreate");msg[1]=ui_hash("TryCreateCharacter");CHECK(!SEND()&&calls==before,"Try reentrante ejecuta constructor original una vez, sin recursión");msg[0]=ui_hash("CharacterCreate");msg[1]=ui_hash("Create");
 desktop=0;CHECK(!SEND()&&calls==before,"Legacy conserva BL original");desktop=1;CHECK(pc_create_commit_dispatch(&m,p,(uint64_t)msg,1,read_none,receive)&&calls==before,"Lectura indefinida en PC no inicia otro personaje");
 for(int c=1;c<=6;c++){cancel=c;CHECK(SEND()&&calls==before,"Cambio de contexto durante lectura cancela sin ejecutar proveedor");*root_field=r;busy=ingame=0;atomic_store(&g_load_up,0);}
 Q(m.base,0x2f05458,0);CHECK(SEND()&&calls==before,"Root anterior no llega al constructor");Q(m.base,0x2f05458,root);root[0xd9]=1;CHECK(SEND()&&calls==before,"ToGame ya iniciado no crea dos veces");root[0xd9]=0;
 Q(panel,0,base+0x20af548);CHECK(SEND()&&calls==before,"Panel de otra clase no recibe Try");Q(panel,0,base+0x20c6e40);Q(root,0x60,0);CHECK(SEND()&&calls==before,"Panel retirado no se reutiliza");Q(root,0x60,1);
 Q(box,0,base+DM_VT_SLOT);CHECK(SEND()&&calls==before,"Widget no InputText se rechaza");Q(box,0,base+0x20b8718);box[0x51]=0;CHECK(SEND()&&calls==before,"InputText oculto no entrega texto obsoleto");box[0x51]=1;Q(box,0x560,64);CHECK(SEND()&&calls==before,"Longitud fuera del buffer no se trunca ni crea otro nombre");Q(box,0x560,strlen(name));name[strlen(name)]=1;CHECK(SEND()&&calls==before,"Texto sin terminador coherente no se entrega");strcpy(name,"PruebaPC");Q(box,0x560,strlen(name));
 Q(panel,0x58,(uint64_t[]){(uint64_t)other});Q(other,8,"OtraEntrada");CHECK(SEND()&&calls==before,"Falta de InputText no cae en Create con nombre vacío");Q(panel,0x58,children);
 for(unsigned i=0;i<4;i++){size_t at=((size_t[]){0xc267c8,0xc2569c,0xc26e08,0xc26e10})[i];m.base[at]^=1;CHECK(SEND()&&calls==before,"ABI distinta se bloquea antes de llamada nativa");m.base[at]^=1;}
 uint64_t saved[5];memcpy(saved,msg,sizeof msg);strcpy(expected,name);CHECK(SEND()&&calls==before+1,"El siguiente evento válido se recupera después de cancelación");CHECK(!memcmp(saved,msg,sizeof msg),"Mensaje original y payload originales no se mutan");
 free(m.base);printf("PC CREATE COMMIT %d comprobaciones, %d fallos\n",checks,failures);return!!failures;
}
