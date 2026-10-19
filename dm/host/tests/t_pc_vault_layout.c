#include "../dm_vault_ui.c"
static int checks, failures;
#define CHECK(c, msg) do { checks++; if (!(c)) { failures++; fprintf(stderr,"FAIL %s\n",msg); } } while (0)
int main(void) {
    for (int zh=0;zh<2;zh++) for(int sub=0;sub<2;sub++) {
        vu_layout legacy=vu_geometry(0,zh,sub);
        CHECK(legacy.tiers_x==(zh?938:878)&&legacy.tiers_y==444,"Legacy conserva posición de filtros por idioma");
        CHECK(legacy.checks[0]==0&&legacy.checks[1]==(zh?200:240)&&legacy.checks[2]==(zh?400:430),"Legacy conserva columnas originales");
        CHECK(legacy.unique_x==156&&legacy.unique_y==516&&legacy.sets_x==156&&legacy.sets_y==296&&legacy.label_y==-5,"Legacy conserva listas y etiquetas originales");
        vu_layout pc=vu_geometry(1,zh,sub);
        CHECK(pc.stats_x==105&&pc.stats_y==(sub?325:505),"PC separa estadísticas según subpágina");
        CHECK(pc.tiers_x==105&&pc.tiers_y==557&&pc.checks[0]==0&&pc.checks[1]==440&&pc.checks[2]==880&&pc.label_y==0,"PC usa filtros completos en fila propia");
        CHECK(pc.unique_x==113&&pc.unique_y==(sub?377:638)&&pc.sets_x==105&&pc.sets_y==377,"PC conserva geometría nativa sin compensar escalas móviles");
        CHECK(pc.unique_y+4*130+3*8<=1200&&pc.sets_y+7*108+6*8<=1200,"Las doce celdas y siete filas de seis piezas quedan dentro de la máscara");
    }
    printf("PC VAULT GEOMETRY %d comprobaciones, %d fallos\n",checks,failures);
    return failures!=0;
}
