// Prueba de selección de recursos PC; contrato original b88010, sin app ni archivos de juego.
#include "../dm_padmode.c"
static int checks, failures, calls, present, gates;
int gate_obs_present(int id) { return gates && (id==OBS_PC_LAYOUT_EXISTS || id==OBS_PC_LAYOUT_LOAD); }
static const char *queried;
static int exists_root(const void *obj) { calls++; queried=*(const char *const *)obj; const void *nested=dm_ui_pc_layout_exists_arg(obj); if (nested!=obj) abort(); return present; }
#define CHECK(c,t) do { checks++; if (!(c)) { failures++; fprintf(stderr,"FAIL %s\n",t); } } while(0)
int main(void) {
 const char *names[]={"Controller/Console/Switch/HUDPanel", "Controller/Console/PlayerInventoryExpansionLayout", "Controller/SkillsTreePanel"};
 const char *roots[]={"HUDPanel","PlayerInventoryExpansionLayout","SkillsTreePanel"};
 for (int i=0;i<3;i++) { present=1;calls=0;const char *selected=pc_layout_root(names[i],exists_root); CHECK(!strcmp(selected,roots[i])&&calls==1&&!strcmp(queried,roots[i]),"El recurso raíz existente precede al controlador"); }
 const char *infra[]={"Controller/Console/Switch/GlobalData","Controller/Console/GlobalData","Controller/GlobalData","Controller/Console/Switch/FocusManager","Controller/Console/FocusManager","Controller/FocusManager"};
 for(int i=0;i<6;i++){calls=0;present=1;CHECK(pc_layout_root(infra[i],exists_root)==infra[i]&&!calls,"Infraestructura Switch conserva GlobalData y FocusManager originales, HD/SD");}
 present=0;calls=0;const char *custom="Controller/Console/ChroniclePanel";CHECK(pc_layout_root(custom,exists_root)==custom&&calls==1,"Un panel propio sin raíz conserva fallback original");
 calls=0;const char *base="PlayerInventoryOriginalLayout";CHECK(pc_layout_root(base,exists_root)==base&&!calls,"basedOn raíz no vuelve a consultar ni añade prefijos");
 CHECK(!pc_layout_root(NULL,exists_root),"Ruta nula intacta");CHECK(!strcmp(pc_layout_root("Other/Panel",exists_root),"Other/Panel")&&!calls,"Rutas ajenas intactas");
 const char *sample="Controller/Console/HUDPanel";const char *arg[]={sample};
 CHECK(dm_ui_pc_layout_path(sample)==sample && dm_ui_pc_layout_exists_arg(arg)==arg,"Modo desactivado conserva identidad de argumentos");
 module_t module={.base=(uint8_t *)((uintptr_t)exists_root-0xb88010)};g_pc_module=&module;atomic_store(&g_pc_desktop,1);
 calls=0;present=1;const void *borrowed=dm_ui_pc_layout_exists_arg(arg);
 CHECK(borrowed!=arg&&!strcmp(*(const char *const *)borrowed,"HUDPanel")&&calls==1,"Existencia recibe préstamo raíz y reentrada no recursa");
 CHECK(arg[0]==sample,"La consulta no modifica el string ni temporal original");
 calls=0;CHECK(!strcmp(dm_ui_pc_layout_path(sample),"HUDPanel")&&calls==1,"Carga vuelve a consultar una vez con raíz, sin caché stale");
 calls=0;present=0;CHECK(dm_ui_pc_layout_exists_arg(arg)==arg&&calls==1&&dm_ui_pc_layout_path(sample)==sample&&calls==2,"Ambas puertas conservan fallback ausente, una consulta por puerta");
 CHECK(!dm_ui_pc_layout_exists_arg(NULL),"Argumento de existencia nulo intacto");
 unsigned char *bytes=calloc(1,0xb88020);module.base=bytes;module.size=0xb88020;
 const uint32_t tail[]={0xa90167fa,0xa9025ff8,0xa90357f6};memcpy(bytes+0xb88014,tail,sizeof tail);memcpy(bytes+0xb84754,tail,sizeof tail);
 gates=0;CHECK(!pc_layout_abi(&module),"Un producto anterior sin ambas puertas no habilita PC");gates=1;CHECK(pc_layout_abi(&module),"Firma y puertas de producto PC reconocidas");
 bytes[0xb88014]^=1;CHECK(!pc_layout_abi(&module),"Una firma incompatible queda cerrada");bytes[0xb88014]^=1;module.size=1;CHECK(!pc_layout_abi(&module),"Módulo truncado no se lee fuera de límites");free(bytes);
 printf("PC LAYOUT %d comprobaciones, %d fallos\n",checks,failures);return failures?1:0;
}
