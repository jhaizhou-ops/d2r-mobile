#include "../dm_vault_ui.c"
static int checks,failures;
#define CHECK(c,msg) do { checks++; if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",msg);} } while(0)
static int step(vu_pointer *s,int kind,int same,int target,int row,int stored){return vu_pointer_step(s,kind,same,target,row,stored,20,30);}
int main(void){
    vu_pointer p={0};
    CHECK(!step(&p,DM_UP_LUP,1,1000,-1,0),"UP sin DOWN no activa nada");
    CHECK(!step(&p,DM_UP_LDOWN,1,0,-1,0),"Controles nativos conservan Root");
    CHECK(step(&p,DM_UP_LDOWN,1,1000,-1,0)==VU_PC_CONSUME,"Categoría captura DOWN");
    CHECK(step(&p,DM_UP_LUP,1,1000,-1,0)==(VU_PC_CONSUME|VU_PC_TAP),"Categoría activa en UP del mismo destino");
    CHECK(!p.active,"Captura termina después de UP");
    step(&p,DM_UP_LDOWN,1,1000,-1,0);
    CHECK(step(&p,DM_UP_LUP,1,1001,-1,0)==VU_PC_CONSUME,"UP en categoría distinta no activa ninguna");
    step(&p,DM_UP_LDOWN,1,2000,-1,0);step(&p,DM_UP_MOVE,1,0,-1,0);
    CHECK(step(&p,DM_UP_LUP,1,2000,-1,0)==VU_PC_CONSUME,"Salir y volver no convierte drag-out en clic");
    step(&p,DM_UP_LDOWN,1,100,4,0);
    CHECK(step(&p,DM_UP_LUP,1,100,5,0)==VU_PC_CONSUME,"Cambio de registro en una celda no activa registro nuevo");
    step(&p,DM_UP_LDOWN,1,100,4,0);
    CHECK(step(&p,DM_UP_LUP,0,100,4,0)==VU_PC_CONSUME,"Cambio de vista cancela clic");
    step(&p,DM_UP_LDOWN,1,100,4,1);
    CHECK(step(&p,DM_UP_MOVE,1,0,-1,0)==(VU_PC_CONSUME|VU_PC_DRAG_START|VU_PC_DRAG_MOVE),"Celda almacenada inicia drag al salir");
    CHECK(step(&p,DM_UP_MOVE,1,0,-1,0)==(VU_PC_CONSUME|VU_PC_DRAG_MOVE),"Drag mueve sin Root");
    CHECK(step(&p,DM_UP_LUP,1,0,-1,0)==(VU_PC_CONSUME|VU_PC_DRAG_END),"Drag solo termina por UP");
    step(&p,DM_UP_LDOWN,1,100,4,1);step(&p,DM_UP_MOVE,1,0,-1,0);
    CHECK(step(&p,-1,1,0,-1,0)==(VU_PC_CONSUME|VU_PC_DRAG_CANCEL),"Cancelación nunca solicita retirada");
    CHECK(!step(&p,DM_UP_LUP,1,100,4,1),"UP tras cancelación no activa nada");
    step(&p,DM_UP_LDOWN,1,100,4,1);step(&p,DM_UP_MOVE,1,0,-1,0);
    CHECK(step(&p,DM_UP_MOVE,0,100,4,1)==(VU_PC_CONSUME|VU_PC_DRAG_CANCEL),"Cambio de vista cancela drag sin UP implícito");
    CHECK(step(&p,DM_UP_LUP,1,100,4,1)==VU_PC_CONSUME,"UP del drag cancelado se consume");
    step(&p,DM_UP_LDOWN,1,100,4,0);step(&p,DM_UP_MOVE,1,0,-1,0);
    CHECK(step(&p,DM_UP_LUP,1,100,4,0)==VU_PC_CONSUME,"Objeto no almacenado nunca inicia retirada");
    step(&p,DM_UP_LDOWN,1,2,-1,0);
    CHECK(step(&p,DM_UP_LUP,1,2,-1,0)==VU_PC_CONSUME,"Cerrar tooltip no activa acción debajo");
    step(&p,DM_UP_LDOWN,1,1000,-1,0);step(&p,DM_UP_LDOWN,1,1000,-1,0);
    CHECK(step(&p,DM_UP_LUP,1,1000,-1,0)==VU_PC_CONSUME,"DOWN duplicado cancela activación");
    step(&p,DM_UP_LDOWN,1,1000,-1,0);
    CHECK(step(&p,DM_UP_END,1,1000,-1,0)==VU_PC_CONSUME,"Salida de ventana cancela categoría");
    CHECK(!step(&p,DM_UP_LUP,1,1000,-1,0),"No hay clic tras salida de ventana");
    step(&p,DM_UP_LDOWN,1,100,4,1);step(&p,DM_UP_MOVE,1,0,-1,0);
    CHECK(step(&p,DM_UP_END,1,0,-1,0)==(VU_PC_CONSUME|VU_PC_DRAG_CANCEL),"Salida de ventana cancela drag sin retirada");
    step(&p,DM_UP_LDOWN,1,100,4,1);step(&p,DM_UP_MOVE,1,0,-1,0);
    CHECK(step(&p,DM_UP_LDOWN,1,100,4,1)==(VU_PC_CONSUME|VU_PC_DRAG_CANCEL),"DOWN duplicado cancela también el drag visual");
    CHECK(step(&p,DM_UP_LUP,1,100,4,1)==VU_PC_CONSUME,"UP tras DOWN duplicado no retira ni activa");
    step(&p,DM_UP_LDOWN,1,1000,-1,0);
    CHECK(!step(&p,DM_UP_KEYDOWN,1,0,-1,0),"Captura propia no absorbe eventos de teclado");
    CHECK(step(&p,DM_UP_LUP,1,1000,-1,0)==(VU_PC_CONSUME|VU_PC_TAP),"Evento de teclado no desequilibra DOWN/UP");
    step(&p,DM_UP_LDOWN,1,1,-1,0);
    CHECK(step(&p,DM_UP_LUP,1,1,-1,0)==VU_PC_CONSUME,"Fondo opaco consume pareja sin invocar Cube");
    printf("PC VAULT POINTER %d comprobaciones, %d fallos\n",checks,failures);return failures!=0;
}
