#define DM_PC_ITEMS_CONNECTED 0
#include "../dm_cursor.c"
static int checks,failures,allocations,appends,constructs,getters[3],calls,oom,missing;
static uint8_t panel[0x300],list[0x100],rows[11][0x100],old[0x98],old_snapshot[0x98];
static uint64_t descriptors[16],row_pointers[11],options[3]={1,0,1};
#define P(x) ((uint64_t)(uintptr_t)(x))
#define CHECK(c,t)do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
static void *allocate(uint64_t a,uint64_t size,uint32_t align){(void)a;CHECK(size==0x98&&align==0x10,"Descriptor usa tamaño y alineación originales");allocations++;return oom?NULL:calloc(1,size);}
static uint64_t lookup(uint64_t p,const char*n){CHECK(p==P(panel),"Registro busca hijos en panel recibido");return missing?0:P(n);}
static uint64_t get0(void){getters[0]++;return P(options);}
static uint64_t get1(void){getters[1]++;return P(options+1);}
static uint64_t get2(void){getters[2]++;return P(options+2);}
static void construct(void *opaque,uint64_t hash,uint64_t option,const void*input){uint8_t *d=opaque;const uint64_t *s=input;constructs++;*(uint64_t*)(d+8)=hash;*(uint64_t*)(d+0x10)=option;*(uint64_t*)(d+0x18)=P(strndup((const char*)s[0],s[1]));*(uint64_t*)(d+0x20)=s[1];CHECK(s[1]==s[2],"NameString24 conserva longitud y capacidad de la entrada prestada");}
static uint64_t append(uint64_t p,void*d){CHECK(p==P(panel),"Append conserva panel original");uint64_t n=*(uint64_t*)(panel+0x190);descriptors[n]=P(d);*(uint64_t*)(panel+0x190)=n+1;appends++;return P(d);}
static void set_row(uint64_t l,int index,int visible){CHECK(l==P(list)&&index>=0&&index<11,"Setter recibe índice real de lista original");calls++;rows[index][0x50]=rows[index][0x51]=visible;}
static void setup(void){
 *(const char**)(panel+8)="GameplayOptionsPanel";*(uint64_t*)(panel+0x180)=P(list);*(uint64_t*)(panel+0x188)=P(descriptors);*(uint64_t*)(panel+0x190)=1;*(uint64_t*)(panel+0x198)=16;descriptors[0]=P(old);*(uint64_t*)(old+8)=ui_hash("SetExisting");*(uint64_t*)(old+0x10)=P(options);*(const char**)(old+0x18)="Existing";*(uint64_t*)(old+0x20)=8;memcpy(old_snapshot,old,sizeof old);
 const char *names[]={"Row Controller Options","Row ControllerCursorSpeed","Row ControllerWeaponSwapSeparateBindings","Row ControllerRumbleEnabled","Row ControllerSwapThumbsticks","Row Controller Feedback","Row Keyboard Options","Row DefaultKeyBindings","Row KeyboardQuickCastEnabled","Row DisplayActiveSkillBindings","Row Other"};
 for(unsigned i=0;i<11;i++){*(const char**)(rows[i]+8)=names[i];rows[i][0x50]=rows[i][0x51]=i<6||i==10;row_pointers[i]=P(rows[i]);}*(uint64_t*)(list+0x58)=P(row_pointers);*(uint64_t*)(list+0x60)=11;
}
int main(void){
 uint8_t owner[0x100]={0};uint64_t pending[]={P(panel)},retired[]={P(panel)},attached[]={P(panel)};
 CHECK(!pc_options_panel_owned(P(owner),P(panel)),"Panel no registrado no recibe política");
 *(uint64_t*)(owner+0x88)=P(pending);*(uint64_t*)(owner+0x90)=1;
 CHECK(!ui_has(P(owner),P(panel),0)&&pc_options_panel_owned(P(owner),P(panel)),"Primer OnOpen acepta propiedad pendiente88 antes del attach58");
 *(uint64_t*)(owner+0x90)=0;*(uint64_t*)(owner+0xa0)=P(retired);*(uint64_t*)(owner+0xa8)=1;
 CHECK(!pc_options_panel_owned(P(owner),P(panel)),"Registro a0 retirado no concede propiedad viva");
 *(uint64_t*)(owner+0x58)=P(attached);*(uint64_t*)(owner+0x60)=1;
 CHECK(!pc_options_panel_owned(P(owner),P(panel)),"Retirada pendiente impide política aunque todavía aparezca attached");
 *(uint64_t*)(owner+0xa8)=0;CHECK(pc_options_panel_owned(P(owner),P(panel)),"Panel attached vivo conserva política");
 *(uint64_t*)(owner+0x90)=257;CHECK(!pc_options_panel_owned(P(owner),P(panel)),"Registro pendiente corrupto falla cerrado sin recorrer fuera de límites");
 CHECK(pc_options_init_class(0x100000000,0x1020c1c48),"Init acepta la tabla virtual cuyo slot10 reloca al Init Gameplay original bb3d10");
 CHECK(!pc_options_init_class(0x100000000,0x1020c1b10),"Init rechaza la clase vecina cuyo Init original es baf008");
 unsigned trace_count=0;unsetenv("DM_MAC_POINTER_LOG");CHECK(!pc_options_trace_budget(&trace_count,2)&&!trace_count,"Sin diagnóstico no se consume presupuesto ni se habilitan trazas");setenv("DM_MAC_POINTER_LOG","/tmp/prueba-no-abierta",1);CHECK(pc_options_trace_budget(&trace_count,2)&&pc_options_trace_budget(&trace_count,2)&&!pc_options_trace_budget(&trace_count,2)&&trace_count==2,"Diagnóstico se limita por fase sin crear archivos ni cambiar política");unsetenv("DM_MAC_POINTER_LOG");
 setup();pc_options_registration_api api={.allocate=allocate,.lookup=lookup,.get_option={get0,get1,get2},.construct=construct,.append=append};
 *(uint64_t*)panel=0x1020c1c48;CHECK(pc_options_rows_class(0x100000000,P(panel)),"Gameplay requiere identidad nativa y nombre coincidentes");*(const char**)(panel+8)="SoundOptionsPanel";CHECK(!pc_options_rows_class(0x100000000,P(panel)),"Nombre Audio sobre clase Gameplay no concede política");*(uint64_t*)panel=0x1020c1988;CHECK(pc_options_rows_class(0x100000000,P(panel)),"Sound requiere su tabla virtual original");*(const char**)(panel+8)="GameplayOptionsPanel";CHECK(!pc_options_rows_class(0x100000000,P(panel)),"Nombre Gameplay sobre clase Audio no concede política");*(uint64_t*)panel=0x1020c1c48;
 CHECK(!pc_options_register(P(panel),0,&api)&&!allocations,"Legacy no registra opciones PC");CHECK(pc_options_register(P(panel),1,&api)==3,"PC registra tres descriptores por constructores originales");CHECK(*(uint64_t*)(panel+0x190)==4&&descriptors[0]==P(old)&&!memcmp(old,old_snapshot,sizeof old),"Vector conserva descriptor previo sin reemplazarlo");CHECK(appends==3&&constructs==3&&getters[0]==1&&getters[1]==1&&getters[2]==1,"Cada opción usa su getter constructor y append una sola vez");CHECK(options[0]==1&&options[1]==0&&options[2]==1,"Registrar no cambia valores ni fuerza QuickCast");CHECK(pc_options_register(P(panel),1,&api)==0&&allocations==3,"Inicialización repetida no duplica descriptores");
 CHECK(pc_options_rows(P(panel),0,set_row)==0&&!calls,"Visibilidad legacy permanece intacta");CHECK(pc_options_rows(P(panel),1,set_row)==10,"Paso PC usa setter para filas controller cabeceras y teclado registrado");for(int i=0;i<6;i++)CHECK(!rows[i][0x50]&&!rows[i][0x51],"Control exclusivo de mando oculto por política PC");for(int i=6;i<10;i++)CHECK(rows[i][0x50]&&rows[i][0x51],"Opción de teclado visible solo tras registro nativo");CHECK(rows[10][0x51],"Filas no relacionadas conservan estado");
 int before=calls;*(const char**)(panel+8)="VideoOptionsPanel";CHECK(!pc_options_rows(P(panel),1,set_row)&&calls==before,"Otra clase de panel no recibe política de filas");*(const char**)(panel+8)="SoundOptionsPanel";CHECK(pc_options_rows(P(panel),1,set_row)==6,"Audio aplica política a la fila nativa Controller Feedback");*(const char**)(panel+8)="GameplayOptionsPanel";
 uint64_t *first=(void*)(uintptr_t)descriptors[1];first[1]^=1;CHECK(pc_options_register(P(panel),1,&api)==-1&&allocations==3,"Conflicto de hash y nombre se rechaza sin duplicar");first[1]^=1;
 for(int i=1;i<4;i++){uint8_t*d=(void*)(uintptr_t)descriptors[i];free(*(void**)(d+0x18));free(d);descriptors[i]=0;}*(uint64_t*)(panel+0x190)=1;oom=1;CHECK(pc_options_register(P(panel),1,&api)==-1&&*(uint64_t*)(panel+0x190)==1&&appends==3,"OOM conserva vector y valores sin append parcial inexistente");oom=0;missing=1;before=allocations;CHECK(pc_options_register(P(panel),1,&api)==-1&&allocations==before,"Widget ausente no recibe descriptor fantasma");missing=0;*(uint64_t*)(panel+0x190)=257;CHECK(pc_options_register(P(panel),1,&api)==-1,"Vector inválido falla antes de leer descriptores");
 FILE *f=fopen(getenv("DM_TEST_MAIN")?getenv("DM_TEST_MAIN"):"build/main.orig.bin","rb");CHECK(f!=NULL,"Fixture original disponible");if(f){fseek(f,0,SEEK_END);size_t n=ftell(f);rewind(f);uint8_t*b=malloc(n);CHECK(b&&fread(b,1,n,f)==n,"Fixture original completo");fclose(f);if(b){module_t m={.base=b,.size=n};CHECK(pc_options_native_abi(&m),"Todos los constructores getters y setter coinciden con ABI original");CHECK(pc_options_filter_abi(&m),"Contexto97 valida prólogo y continuación originales del filtro");b[0xae9510]^=1;CHECK(!pc_options_filter_abi(&m),"Continuación distinta no recibe política97");b[0xae9510]^=1;uint64_t *game=(void*)(b+0x1c05758),*video=(void*)(b+0x1c05440);CHECK(game[1]==0x403&&game[2]==0xbb3d10&&pc_options_init_class(P(b),P(b)+game[0]-0x10),"Guarda acepta VT derivada del RELA canónico hacia Init Gameplay");CHECK(video[1]==0x403&&video[2]==0xbaf008&&!pc_options_init_class(P(b),P(b)+video[0]-0x10),"Guarda rechaza VT derivada del RELA canónico hacia Init Video");b[0x310f30]^=1;CHECK(!pc_options_native_abi(&m),"Getter modificado impide registro con ABI distinta");free(b);}}
 printf("PC OPTIONS ROWS %d comprobaciones, %d fallos\n",checks,failures);return!!failures;
}
