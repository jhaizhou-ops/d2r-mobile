#include "../dm_vault_ui.c"
static int checks,failures;
static int desktop=1;
int dm_ui_pc_desktop(void){return desktop;}
#define CHECK(c,msg) do {checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",msg);}}while(0)
static uint32_t ticks(vu_deposit *s,int n,uint32_t held,uint32_t drop){uint32_t result=0;while(n--){uint32_t id=vu_deposit_step(s,1,held,drop);if(id){CHECK(!result,"Un único depósito por ventana de espera");result=id;}}return result;}
int main(void){
    vu_deposit s={0};
    CHECK(!ticks(&s,12,0,41),"Sin held muestreado ni registro explícito no se inventa depósito");
    vu_deposit_returning(&s,41);
    CHECK(!ticks(&s,7,0,41),"Pickup/putback entre muestras espera siete frames completos");
    CHECK(ticks(&s,1,0,41)==41,"Octavo frame emite objeto registrado antes del putback");
    CHECK(!ticks(&s,20,0,41),"Muestreos posteriores no duplican depósito");
    s=(vu_deposit){0};vu_deposit_returning(&s,42);vu_deposit_returning(&s,42);
    CHECK(!ticks(&s,4,0,42),"Registro repetido no adelanta depósito");
    vu_deposit_returning(&s,42);
    CHECK(ticks(&s,4,0,42)==42,"Registro repetido durante espera no reinicia ni duplica temporizador");
    CHECK(!ticks(&s,12,0,42),"El depósito repetido no vuelve a emitirse");
    s=(vu_deposit){0};vu_deposit_returning(&s,43);
    CHECK(!ticks(&s,4,43,43),"Objeto todavía sostenido no empieza el plazo");
    CHECK(!ticks(&s,7,0,43)&&ticks(&s,1,0,43)==43,"Plazo comienza después de soltar realmente");
    s=(vu_deposit){0};vu_deposit_returning(&s,44);ticks(&s,3,0,44);
    CHECK(!ticks(&s,20,0,0),"Cambiar intención a cero cancela depósito pendiente");
    s=(vu_deposit){0};vu_deposit_returning(&s,45);ticks(&s,3,0,45);
    CHECK(!ticks(&s,20,0,46),"Cambiar id de destino cancela objeto anterior");
    s=(vu_deposit){0};vu_deposit_returning(&s,47);ticks(&s,3,0,47);
    CHECK(!ticks(&s,20,48,47),"Nuevo objeto sostenido cancela espera anterior");
    s=(vu_deposit){0};vu_deposit_returning(&s,49);ticks(&s,3,0,49);vu_deposit_returning(&s,0);
    CHECK(!ticks(&s,20,0,49),"Cancelación explícita elimina held esperado y plazo");
    s=(vu_deposit){0};vu_deposit_returning(&s,50);ticks(&s,3,0,50);vu_deposit_returning(&s,51);
    CHECK(!ticks(&s,7,0,51)&&ticks(&s,1,0,51)==51,"Nueva devolución sustituye intención anterior y tiene plazo propio");
    s=(vu_deposit){0};
    CHECK(!vu_deposit_step(&s,1,52,0)&&!ticks(&s,7,0,52)&&ticks(&s,1,0,52)==52,"Transición held→vacío observada conserva depósito PC sin API");
    s=(vu_deposit){0};vu_deposit_step(&s,0,53,0);
    CHECK(!vu_deposit_step(&s,0,0,53),"Legacy empieza espera al ver transición existente");
    for(int i=0;i<6;i++)CHECK(!vu_deposit_step(&s,0,0,0),"Legacy conserva plazo aunque cambie marcador como upstream");
    CHECK(vu_deposit_step(&s,0,0,0)==53,"Legacy conserva emisión original al octavo frame");
    CHECK(!vu_deposit_step(&s,0,0,0),"Legacy no repite emisión");
    s=(vu_deposit){0};vu_deposit_step(&s,1,54,0);vu_deposit_step(&s,1,0,54);
    CHECK(!ticks(&s,20,0,0),"PC cancela también una transición muestreada sin registro explícito");
    dm_vault_ui_returning(55);
    CHECK(g_deposit.held==55&&g_deposit.returning==55&&!g_deposit.wait,"API pública prepara exactamente el estado usado por pump");
    CHECK(!ticks(&g_deposit,7,0,55),"API pública comparte espera del pump");
    dm_vault_ui_returning(55);
    CHECK(ticks(&g_deposit,1,0,55)==55,"API pública repetida no retrasa la emisión compartida");
    dm_vault_ui_returning(56);dm_vault_ui_returning(0);
    CHECK(!ticks(&g_deposit,20,0,56),"API pública cero cancela intención PC");
    desktop=0;dm_vault_ui_returning(57);
    CHECK(!g_deposit.held&&!g_deposit.returning&&!g_deposit.wait,"API nueva no cambia estado en modo legacy");
    printf("PC VAULT DEPOSIT %d comprobaciones, %d fallos\n",checks,failures);return failures!=0;
}
