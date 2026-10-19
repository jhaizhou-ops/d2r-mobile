// Inventario PC: mismo toggle nativo que los otros paneles, sin UISwitcher.
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD];int g_nmod;
static int desktop=1,checks,failures,calls,id,state;
int dm_ui_pc_desktop(void){return desktop;}int dm_uiscreen_ingame(void){return 1;}
#define CHECK(c,t)do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
static void toggle(int panel,int value){calls++;id=panel;state=value;}
int main(void){
 CHECK(!pc_inventory_route(0,1,toggle)&&!calls,"Legacy continúa la construcción original de UISwitcher sin callback extra");
 CHECK(pc_inventory_route(1,0,toggle)&&!calls,"PC sin perfil bloquea apertura y nunca cae al menú Controller");
 CHECK(pc_inventory_route(1,1,toggle)&&calls==1&&id==1&&state==1,"PC delega una vez459ad0 con Inventory1 y argumento original1");
 CHECK(pc_inventory_route(1,1,toggle)&&calls==2,"Segunda pulsación vuelve al toggle original, permite cerrar sin estado host paralelo");
 CHECK(pc_inventory_route(1,1,NULL)&&calls==2,"Frontera incompleta no invoca puntero nulo");
 module_t m={.size=0x2f05460};m.base=calloc(1,m.size);if(!m.base)return 2;
 const uint32_t callback[]={0xd10103ff,0xa9024ff4,0xa9037bfd,0x9100c3fd},native[]={0xd10183ff,0xa9025ff8,0xa90357f6,0xa9044ff4};memcpy(m.base+0x368d20,callback,sizeof callback);memcpy(m.base+0x459ad0,native,sizeof native);
 CHECK(pc_inventory_abi(&m),"Reconoce callback y toggle originales antes de cualquier llamada");m.base[0x368d2c]^=1;CHECK(!pc_inventory_abi(&m),"Callback con versión diferente falla cerrado");m.base[0x368d2c]^=1;m.base[0x459ad4]^=1;CHECK(!pc_inventory_abi(&m),"Toggle con versión diferente falla cerrado");m.base[0x459ad4]^=1;m.size--;CHECK(!pc_inventory_abi(&m)&&!pc_inventory_abi(NULL),"Módulo incompleto no se lee");m.size++;
 g_mod[0]=m;strcpy(g_mod[0].name,"main");g_nmod=1;uint64_t regs[31],saved[31];for(int i=0;i<31;i++)regs[i]=0x100+i;memcpy(saved,regs,sizeof regs);
 desktop=0;dm_ui_pc_inventory(regs);CHECK(!regs[8],"Host legacy siempre selecciona la instrucción original");desktop=1;dm_ui_pc_inventory(regs);CHECK(regs[8]==1,"Sin root PC toma epílogo y no abre Controller por fallback");
 for(int i=0;i<31;i++)if(i!=8)CHECK(regs[i]==saved[i],"Únicamente cambia la decisión temporal x8; parámetros/registros del callback intactos");
 free(m.base);printf("PC INVENTORY %d comprobaciones, %d fallos\n",checks,failures);return!!failures;
}
