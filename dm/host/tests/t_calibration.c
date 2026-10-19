#include "../dm_cursor.c"
#include <stdio.h>
static int checks,failures,writes,first_time,readable=1,write_ok=1;
static int saved_first_time;
static int original_get(const char *key,int *value){if(strcmp(key,"First Time")||!readable)return 0;*value=first_time;return 1;}
static int original_save(int force){writes++;if(force!=1||!write_ok)return 0;saved_first_time=first_time;return 1;}
#define CHECK(c,t) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}else printf("PASS %s\n",t);}while(0)
static void note(const char *act,const char *panel){calibration_note("PanelManager",act,panel);}
static void reset(void){memset(&g_calibration,0,sizeof g_calibration);writes=first_time=saved_first_time=0;readable=write_ok=1;}
static void accept_all(void){note("OpenPanel","GammaCalibrationPanel");note("UnloadPanel","GammaCalibrationPanel");note("OpenPanel","ScreenSpaceCalibrationPanel");note("UnloadPanel","ScreenSpaceCalibrationPanel");note("OpenPanel","ColorBlindCalibrationPanel");calibration_note("OptionsPanelMessage","AcceptChanges","");note("UnloadPanel","ColorBlindCalibrationPanel");}
int main(void){
 reset();first_time=1;calibration_flush(1,original_get,original_save);CHECK(!writes,"Sin calibración completada no guarda ni crea indicador");
 reset();accept_all();calibration_flush(1,original_get,original_save);CHECK(!writes,"Aceptar última calibración espera indicador establecido por juego");first_time=1;calibration_flush(62,original_get,original_save);CHECK(writes==1&&saved_first_time==1,"Completar flujo persiste First Time nativo mediante guardado original");
 calibration_flush(123,original_get,original_save);CHECK(writes==1,"Calibración se guarda una sola vez");
 reset();note("OpenPanel","GammaCalibrationPanel");note("OpenPanel","ScreenSpaceCalibrationPanel");note("OpenPanel","ColorBlindCalibrationPanel");note("UnloadPanel","ColorBlindCalibrationPanel");first_time=1;calibration_flush(1,original_get,original_save);CHECK(!writes,"Cancelar última calibración no equivale a consentimiento");
 reset();note("OpenPanel","ColorBlindCalibrationPanel");calibration_note("OptionsPanelMessage","AcceptChanges","");note("UnloadPanel","ColorBlindCalibrationPanel");first_time=1;calibration_flush(1,original_get,original_save);CHECK(!writes,"Calibración aislada de opciones no inventa flujo inicial");
 reset();accept_all();first_time=1;readable=0;calibration_flush(1,original_get,original_save);CHECK(!writes,"Fallo del getter no provoca escritura");readable=1;write_ok=0;calibration_flush(62,original_get,original_save);CHECK(writes==1&&!g_calibration.done,"Fallo de guardado no certifica persistencia");write_ok=1;calibration_flush(123,original_get,original_save);CHECK(writes==2&&g_calibration.done,"Reintento usa ruta original sin modificar indicador");
 reset();FILE *f=fopen("calibration_messages.tsv","r");CHECK(f!=NULL,"Fixture sintético disponible para regresión");if(f){char line[512],cat[128],act[128],arg[128];while(fgets(line,sizeof line,f)){if(line[0]=='#')continue;arg[0]=0;int n=sscanf(line,"%127[^\t]\t%127[^\t]\t%127[^\n]",cat,act,arg);if(n>=2)calibration_note(cat,act,arg);}fclose(f);calibration_flush(1,original_get,original_save);CHECK(!writes&&g_calibration.pending,"Fixture sintético acepta flujo sin crear indicador anticipado");first_time=1;calibration_flush(62,original_get,original_save);CHECK(writes==1&&saved_first_time==1,"Fixture sintético guarda al observar marca original después del intro");}
 reset();setenv("DM_APP","1",1);atomic_store(&g_calibration.pending,1);module_t fake={0};ui_calibration_pump(&fake,1);CHECK(!g_calibration.pending&&!writes,"ABI desconocida no ejecuta funciones del juego");
 printf("CALIBRATION %d comprobaciones, %d fallos\n",checks,failures);return failures?1:0;
}
