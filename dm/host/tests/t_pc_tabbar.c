// PC TabBar init adapter changes one byte of the original object.
#define DM_PC_ITEMS_CONNECTED 0
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD];int g_nmod;
static int desktop,checks,failures;
int dm_ui_pc_desktop(void){return desktop;}
#define CHECK(c,t)do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
int main(void){
 module_t m={.size=0x20ba3f8};m.base=calloc(1,m.size);if(!m.base)return 2;
 const uint32_t tail[]={0xa9086ffc,0xa90967fa,0xa90a5ff8};memcpy(m.base+0xad8a58,tail,sizeof tail);
 _Alignas(8) uint8_t tab[0xeb0],expected[sizeof tab];
 for(int pc=0;pc<2;pc++)for(int switching=0;switching<2;switching++)for(int indicators=0;indicators<2;indicators++){
  memset(tab,0xa5,sizeof tab);*(uint64_t*)tab=(uint64_t)m.base+0x20ba3a8;tab[0xda0]=switching;tab[0xda1]=indicators;memcpy(expected,tab,sizeof tab);if(pc)expected[0xda1]=0;
  CHECK(pc_tabbar_prepare(&m,(uint64_t)tab,pc)==pc,"Sólo modo PC habilita adaptador de inicialización");CHECK(!memcmp(tab,expected,sizeof tab),"Sólo cambia da1; switching da0 y todos los otros bytes permanecen");
  CHECK(!pc || !tab[0xda1],"JSON true queda desactivado después de deserializar antes de crear hijos");
 }
 tab[0xda1]=1;m.base[0xad8a5c]^=1;CHECK(!pc_tabbar_prepare(&m,(uint64_t)tab,1)&&tab[0xda1]==1,"Prólogo distinto impide escritura");m.base[0xad8a5c]^=1;
 *(uint64_t*)tab=(uint64_t)m.base+0x20ba3b0;CHECK(!pc_tabbar_prepare(&m,(uint64_t)tab,1)&&tab[0xda1]==1,"Clase ajena no recibe campo privado TabBar");*(uint64_t*)tab=(uint64_t)m.base+0x20ba3a8;
 m.size--;CHECK(!pc_tabbar_prepare(&m,(uint64_t)tab,1)&&tab[0xda1]==1,"Módulo truncado falla cerrado");m.size++;
 CHECK(!pc_tabbar_prepare(NULL,(uint64_t)tab,1)&&!pc_tabbar_prepare(&m,0,1),"Entrada incompleta no escribe");
 g_mod[0]=m;strcpy(g_mod[0].name,"main");g_nmod=1;uint64_t regs[31],saved[31];for(int i=0;i<31;i++)regs[i]=0x100+i;regs[0]=(uint64_t)tab;memcpy(saved,regs,sizeof regs);
 desktop=0;dm_ui_pc_tabbar_init(regs);CHECK(regs[8]==0&&tab[0xda1]==1,"Legacy siempre devuelve rama con prólogo original sin editar flags");
 desktop=1;regs[8]=0xbeef;dm_ui_pc_tabbar_init(regs);CHECK(regs[8]==0&&!tab[0xda1],"PC también ejecuta el prólogo original tras preparar sólo indicadores");
 for(int i=0;i<31;i++)if(i!=8)CHECK(regs[i]==saved[i],"El handler conserva todos los parámetros y registros salvo decisión x8");
 free(m.base);printf("PC TABBAR %d comprobaciones, %d fallos\n",checks,failures);return!!failures;
}
