// 自定义袋的 PC 坐标回归；只使用合成控件。
// PC custom-pouch placement regression, using synthetic widgets only.
#include "../dm_pouch.c"
void (*g_dm_pre_commit_hook[5])(const char *);
uint64_t dm_fs_commit_count(void){abort();}
void dm_fs_save_poison(int e){(void)e;abort();}
void *dm_memfind(const uint8_t *h,size_t hn,const uint8_t *n,size_t nn){(void)h;(void)hn;(void)n;(void)nn;abort();}
_Alignas(16) static unsigned char box[256],widgets[4][256];static uint64_t children[4],vt[4];static int checks,failures,relayouts;
static void layout(uint64_t w){(void)w;relayouts++;}
#define CHECK(c,t) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
int main(void){
 const char *names[]={"TouchPouch","TouchPouchIcon","TouchPouchCount","TouchStore"};
 vt[2]=(uint64_t)layout;*(uint64_t *)(box+0x58)=(uint64_t)children;*(uint64_t *)(box+0x60)=4;box[0x50]=box[0x51]=1;
 for(int i=0;i<4;i++){children[i]=(uint64_t)widgets[i];*(uint64_t *)widgets[i]=(uint64_t)vt;*(const char **)(widgets[i]+8)=names[i];widgets[i][0x50]=widgets[i][0x51]=1;}
 pouch_place((uint64_t)box,1,1,1);const int pc[4][2]={{90,1344},{106,1352},{204,1344},{800,1348}};
 for(int i=0;i<4;i++)CHECK(!memcmp(widgets[i]+0x70,pc[i],8),"Controles PC respetan rect original de inventario y oro");
 CHECK(relayouts==4,"Cada control propio se repone con callback nativo");pouch_place((uint64_t)box,1,1,1);CHECK(relayouts==4,"Sin cambios no se repite layout");
 pouch_place((uint64_t)box,1,1,0);const int legacy[4][2]={{222,1294},{246,1310},{350,1294},{916,1323}};
 for(int i=0;i<4;i++)CHECK(!memcmp(widgets[i]+0x70,legacy[i],8),"Otra plataforma conserva geometría anterior");
 pouch_place((uint64_t)box,0,0,1);for(int i=0;i<4;i++)CHECK(*(int32_t *)(widgets[i]+0x70)==99999,"Desactivar ajuste retira sus controles propios también en PC");
 printf("PC POUCH %d checks, %d failures\n",checks,failures);return failures?1:0;
}
