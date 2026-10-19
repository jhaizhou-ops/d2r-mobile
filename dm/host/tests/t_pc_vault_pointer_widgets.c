// 真实编年史指针 API 与合成控件树及 hit-test；不使用存档。
// Production Chronicle pointer API with a synthetic widget tree and hit test; no saves.
#include "../dm_vault_ui.c"
module_t g_mod[MAX_MOD];int g_nmod=1;
static int pc=1,panels=DM_PS_VAULT,checks,failures,stored,withdrawals,withdraw_kind,withdraw_row;
int dm_pc_items_enabled(void){return pc;}
int dm_ui_panel_state(void){return panels;}
int dm_ui_top_chk_on(void){return 0;}
void dm_ui_top_chk(uint64_t root,const char *name,uint64_t got){(void)root;(void)name;(void)got;}
int dm_vault_gen(void){return 3;}
int dm_vault_has(int kind,int idx){(void)kind;(void)idx;return stored;}
void dm_vault_request_withdraw(int kind,int row){(void)kind;(void)row;abort();}
void dm_vault_request_desc(int kind,int row){(void)kind;(void)row;abort();}
void dm_vault_request_withdraw_at(int kind,int row,int x,int y,int cols,int rows){withdrawals++;withdraw_kind=kind;withdraw_row=row;(void)x;(void)y;(void)cols;(void)rows;}
float dm_ui_drag_lift_get(void){return 0;}
int dm_ui_bag_point(int x,int y,int *fx,int *fy,int *cols,int *rows){*fx=100;*fy=100;*cols=10;*rows=4;return x==900&&y==1400;}
const char *dm_str(uint8_t *base,const char *key){(void)base;return key;}
#define CHECK(c,msg) do {checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",msg);}}while(0)
_Alignas(16) static unsigned char widgets[24][0x100];static uint64_t children[24][16],vt[16];static int n;
static int hit(uint64_t w,uint64_t xy){
    for(uint64_t p=w;p;p=*(uint64_t *)(p+0x30))if(!*(uint8_t *)(p+0x50))return 0;
    int x=(int32_t)xy,y=(int32_t)(xy>>32),*r=(int *)(w+0x70);
    return x>=r[0]&&y>=r[1]&&x<r[0]+r[2]&&y<r[1]+r[3];
}
static uint64_t widget(uint64_t parent,const char *name,int x,int y,int width,int height){
    int i=n++;uint64_t w=(uint64_t)widgets[i];*(uint64_t *)w=(uint64_t)vt;*(const char **)(w+8)=name;
    *(uint64_t *)(w+0x30)=parent;*(uint8_t *)(w+0x50)=1;*(uint64_t *)(w+0x58)=(uint64_t)children[i];
    int *r=(int *)(w+0x70);r[0]=x;r[1]=y;r[2]=width;r[3]=height;
    if(parent){uint64_t count=*(uint64_t *)(parent+0x60);((uint64_t *)*(uint64_t *)(parent+0x58))[count]=w;*(uint64_t *)(parent+0x60)=count+1;}
    return w;
}
int main(void){
    uint8_t *base=calloc(1,0x3000000);if(!base)return 2;g_mod[0].base=base;strcpy(g_mod[0].name,"main");vt[0x48/8]=(uint64_t)hit;
    uint64_t root=widget(0,"Root",0,0,1000,1600),bank=widget(root,"BankExpansionLayout",0,0,1000,1600),vault=widget(bank,"TouchVault",0,0,1000,1600);
    widget(vault,"TouchV_Back",50,200,900,1000);widget(vault,"TouchV_Cover",50,1200,900,300);
    uint64_t part1=widget(vault,"TouchV_PT1",100,300,200,80);widget(part1,"Hit",100,300,200,80);
    uint64_t part2=widget(vault,"TouchV_PT2",350,300,200,80);widget(part2,"Hit",350,300,200,80);
    widget(vault,"TouchV_SubU",100,220,200,60);widget(vault,"TouchV_PgRHit",650,1250,100,100);
    *(uint64_t *)(base+0x2f05458)=root;*(int *)(base+0x211da38)=1000;*(int *)(base+0x211da3c)=1600;
    CHECK(!dm_vault_ui_pointer(DM_UP_LUP,400,340),"La API no activa una categoría con UP huérfano");
    CHECK(dm_vault_ui_pointer(DM_UP_LDOWN,400,340),"La API captura DOWN de categoría por hit-test");
    CHECK(dm_vault_ui_pointer(DM_UP_LUP,400,340)&&g_part==2,"UP categoría exacta cambia la parte real");
    CHECK(dm_vault_ui_pointer(DM_UP_LDOWN,150,340),"Captura segunda categoría");
    CHECK(dm_vault_ui_pointer(DM_UP_LUP,400,340)&&g_part==2,"UP sobre otra categoría no cambia la parte");
    CHECK(!dm_vault_ui_pointer(DM_UP_LDOWN,150,240),"SubU nativo conserva DOWN Root");
    CHECK(!dm_vault_ui_pointer(DM_UP_LUP,150,240),"SubU nativo conserva UP Root");
    CHECK(dm_vault_ui_pointer(DM_UP_LDOWN,850,1100)&&dm_vault_ui_pointer(DM_UP_LUP,850,1100),"Fondo absorbe pareja completa sin Cube");
    CHECK(dm_vault_ui_pointer(DM_UP_LDOWN,700,1300)&&dm_vault_ui_pointer(DM_UP_LUP,700,1300)&&atomic_load(&g_flip)==1,"Flecha pasa por la API original de paginación");
    dm_vault_ui_pointer(DM_UP_LDOWN,150,340);g_part=3;
    CHECK(dm_vault_ui_pointer(DM_UP_LUP,150,340)&&g_part==3,"Cambio de vista real invalida la activación");
    dm_vault_ui_pointer(DM_UP_LDOWN,150,340);*(uint8_t *)(bank+0x50)=0;
    CHECK(dm_vault_ui_pointer(DM_UP_LUP,150,340)&&g_part==3,"Banco cerrado consume UP y no activa widget oculto");
    *(uint8_t *)(bank+0x50)=1;dm_vault_ui_pointer(DM_UP_LDOWN,150,340);
    CHECK(dm_vault_ui_pointer(-1,0,0)&&!dm_vault_ui_pointer(DM_UP_LUP,150,340)&&g_part==3,"Cancelación pública elimina captura sin activar");
    CHECK(!dm_vault_ui_pointer(DM_UP_LDOWN,-1,340),"Coordenada negativa no llama hit-test de destino");
    CHECK(!dm_vault_ui_pointer(DM_UP_LDOWN,1000,340),"Borde exterior del viewport no captura");
    pc=0;CHECK(!dm_vault_ui_pointer(DM_UP_LDOWN,150,340),"Modo legacy conserva interacción anterior");
    pc=1;panels=0;CHECK(!dm_vault_ui_pointer(DM_UP_LDOWN,150,340),"Sin VAULT no intercepta el inventario");
    panels=DM_PS_VAULT;stored=1;
    uint64_t uniques=widget(vault,"TouchV_Uniq",100,600,800,500),cell=widget(uniques,"TouchV_U0",200,650,400,100);widget(cell,"Bg",200,650,400,100);g_cell_row[0]=0;
    CHECK(dm_vault_ui_pointer(DM_UP_LDOWN,300,700)&&!g_dg.on,"Celda almacena captura sin retirar en DOWN");
    CHECK(dm_vault_ui_pointer(DM_UP_MOVE,900,1400)&&g_dg.on,"API inicia arrastre real de celda a mochila");
    CHECK(g_dg.local_x==82&&g_dg.local_y==219&&g_dg.ax>0&&g_dg.ay>0,"Arrastre utiliza máscara PC y no coordenadas móviles");
    CHECK(dm_vault_ui_pointer(DM_UP_LUP,900,1400)&&!g_dg.on&&withdrawals==1&&withdraw_kind==vt_rows[0].kind&&withdraw_row==vt_rows[0].row,"UP en bolsa válida solicita exactamente la pieza de DOWN");
    dm_vault_ui_pointer(DM_UP_LDOWN,300,700);dm_vault_ui_pointer(DM_UP_MOVE,900,1400);
    CHECK(dm_vault_ui_pointer(-1,0,0)&&!g_dg.on&&withdrawals==1,"Cancelación de API real elimina arrastre sin solicitud");
    dm_vault_ui_pointer(DM_UP_LDOWN,300,700);g_cell_row[0]=1;
    CHECK(dm_vault_ui_pointer(DM_UP_MOVE,900,1400)&&!g_dg.on,"Cambio de registro en origen impide iniciar arrastre obsoleto");
    CHECK(dm_vault_ui_pointer(DM_UP_LUP,900,1400)&&withdrawals==1,"UP obsoleto no retira registro de reemplazo");
    printf("PC VAULT WIDGETS %d comprobaciones, %d fallos\n",checks,failures);free(base);return failures!=0;
}
