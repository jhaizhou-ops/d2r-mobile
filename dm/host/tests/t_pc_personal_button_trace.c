// Memoria y logger reales; no ejecuta el despacho ARM ni la GUI.
#include "../dm_cursor.c"
#include <unistd.h>
module_t g_mod[MAX_MOD];int g_nmod;
static int checks,failures,reads;
#define CHECK(c,t) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
#define Q(p,o,v) (*(uint64_t*)((p)+(o))=(uint64_t)(v))
static int counted(uint64_t p,void *out,size_t n){reads++;return pc_tooltip_read(p,out,n);}
int main(void){
 module_t m={.size=0x2f05460};m.base=calloc(1,m.size);if(!m.base)return 2;
 uint8_t root[0x200]={0},fm[0x200]={0},button[0x600]={0};char left[16]="TouchStashLeft",right[16]="TouchStashRight",other[16]="SharedStashRight";
 Q(m.base,0x2f05458,root);Q(root,0xd0,fm);Q(fm,0x178,button);Q(button,8,right);Q(button,0,(uint64_t)m.base+0x20b7b00);Q(button,0x558,ui_hash("PersonalStashMessage"));Q(button,0x560,ui_hash("NextPage"));
 uint64_t down=ui_hash("LeftMouseButtonDown"),up=ui_hash("LeftMouseButtonUp");unsigned count=0;pc_personal_button_snapshot before,after;
 reads=0;CHECK(!pc_personal_button_trace_begin(0,1,(void*)1,1,down,&count,counted,&before)&&!reads&&!count,"Desactivado no lee módulo, memoria ni presupuesto");
 CHECK(!pc_personal_button_trace_begin(1,0,(void*)1,1,down,&count,counted,&before)&&!reads,"Legacy no lee memoria");
 CHECK(!pc_personal_button_trace_begin(1,1,&m,(uint64_t)root,ui_hash("MouseMove"),&count,counted,&before)&&!reads&&!count,"MouseMove no lee ni consume");
 CHECK(!pc_personal_button_trace_begin(1,1,&m,(uint64_t)root,ui_hash("RightMouseButtonDown"),&count,counted,&before)&&!reads,"RMB queda fuera");
 Q(button,8,other);for(int i=0;i<100;i++)pc_personal_button_trace_begin(1,1,&m,(uint64_t)root,down,&count,counted,&before);CHECK(!count,"Otros widgets no gastan presupuesto");Q(button,8,right);
 CHECK(pc_personal_button_trace_begin(1,1,&m,(uint64_t)root,down,&count,counted,&before)&&count==1&&before.role==2&&before.valid,"Lector real captura botón derecho");
 CHECK(before.vt==(uint64_t)m.base+0x20b7b00&&before.category==ui_hash("PersonalStashMessage")&&before.action==ui_hash("NextPage")&&!before.pressed&&!before.disabled&&!before.consumed,"Snapshot conserva callback y flags");
 button[0x579]=1;root[0xb8]=1;CHECK(pc_personal_button_capture(1,1,&m,(uint64_t)root,down,before.widget,counted,&after)&&after.valid&&after.pressed&&after.consumed,"Post observa pressed y consumed cambiados por fixture");
 CHECK(pc_personal_button_trace_begin(1,1,&m,(uint64_t)root,up,&count,counted,&before)&&before.pressed,"UP captura pressed previo");button[0x579]=0;
 CHECK(pc_personal_button_capture(1,1,&m,(uint64_t)root,up,before.widget,counted,&after)&&!after.pressed,"PostUP registra liberación");
 button[0x57a]=1;CHECK(pc_personal_button_capture(1,1,&m,(uint64_t)root,down,0,counted,&before)&&before.disabled,"Disabled no silencia causa buscada");button[0x57a]=0;Q(button,0x558,0);Q(button,0x560,0);
 CHECK(pc_personal_button_capture(1,1,&m,(uint64_t)root,down,0,counted,&before)&&before.valid&&!before.category&&!before.action,"Callback vacío observable");
 Q(button,0,123);CHECK(pc_personal_button_capture(1,1,&m,(uint64_t)root,down,0,counted,&before)&&before.vt==123,"VT inesperada observable sin llamadas");Q(button,0,(uint64_t)m.base+0x20b7b00);
 Q(button,8,left);CHECK(pc_personal_button_capture(1,1,&m,(uint64_t)root,down,0,counted,&before)&&before.role==1,"Nombre izquierdo exacto");
 Q(fm,0x178,0);CHECK(pc_personal_button_capture(1,1,&m,(uint64_t)root,down,(uint64_t)button,counted,&after)&&!after.hover,"Post conserva target al cambiar hover");
 CHECK(!pc_personal_button_capture(1,1,&m,(uint64_t)root,down,0,counted,&after),"Sin hover no adivina target");Q(fm,0x178,button);
 Q(m.base,0x2f05458,0);CHECK(!pc_personal_button_capture(1,1,&m,(uint64_t)root,down,(uint64_t)button,counted,&after),"Root sustituido rechaza contexto");Q(m.base,0x2f05458,root);
 Q(root,0xd0,1);CHECK(!pc_personal_button_capture(1,1,&m,(uint64_t)root,down,0,counted,&after),"FM ilegible protegido");Q(root,0xd0,fm);
 Q(button,8,1);CHECK(!pc_personal_button_capture(1,1,&m,(uint64_t)root,down,0,counted,&after),"Nombre ilegible protegido");Q(button,8,right);
 CHECK(pc_personal_button_capture(1,1,&m,(uint64_t)root,down,1,counted,&after)&&!after.valid,"Widget post ilegible produce valid0");
 count=0;for(int i=0;i<100;i++)pc_personal_button_trace_begin(1,1,&m,(uint64_t)root,i&1?up:down,&count,counted,&before);CHECK(count==16,"Presupuesto DownUp16");reads=0;
 CHECK(!pc_personal_button_trace_begin(1,1,(void*)1,1,down,&count,counted,&before)&&!reads,"Agotado no lee módulo ni memoria");
 uint8_t sr[sizeof root],sf[sizeof fm],sb[sizeof button];memcpy(sr,root,sizeof root);memcpy(sf,fm,sizeof fm);memcpy(sb,button,sizeof button);
 count=0;CHECK(pc_personal_button_trace_begin(1,1,&m,(uint64_t)root,down,&count,counted,&before),"Snapshot para logger real");
 int p[2];CHECK(!pipe(p),"Pipe disponible");int old=dup(2);dup2(p[1],2);close(p[1]);pc_personal_button_trace_end(&m,down,781,1142,count,&before,counted);fflush(stderr);dup2(old,2);close(old);char log[2048]={0};ssize_t got=read(p[0],log,sizeof log-1);close(p[0]);
 CHECK(got>0&&strstr(log,"PC_PERSONAL_BUTTON 1 right down XY=781,1142")&&strstr(log,"callback=0:0->0:0")&&strstr(log,"context=1"),"Logger registra callback vacío");
 CHECK(!memcmp(sr,root,sizeof root)&&!memcmp(sf,fm,sizeof fm)&&!memcmp(sb,button,sizeof button),"Snapshot y logger no mutan juego");
 free(m.base);printf("PC PERSONAL BUTTON TRACE %d comprobaciones, %d fallos\n",checks,failures);return !!failures;
}
