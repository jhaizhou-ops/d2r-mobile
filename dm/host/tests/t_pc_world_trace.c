// La observación acepta memoria incompleta sin afectar al llamador.
#include "../dm_world.c"
module_t g_mod[MAX_MOD];int g_nmod;
int dm_ui_pc_desktop(void){return 1;}
static int checks,failures;
#define CHECK(c,t) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
static int npc_hit_calls,npc_hit_result;
static int npc_hit(uint64_t unit,int x,int y,int mode,int skill){(void)unit;(void)x;(void)y;CHECK(mode==0&&skill==-1,"Observación conserva consulta nativa espacial");npc_hit_calls++;return npc_hit_result;}
static int npc_prefer(uint64_t manager,uint64_t player,uint64_t unit,uint64_t best){(void)manager;(void)player;(void)unit;(void)best;return 0;}
int main(void){
    uint8_t unit[0x130]={0},path[0x2d0]={0};uint32_t type=0,id=17,mode=0x11,count=3;uint64_t p=(uintptr_t)path;
    uint16_t x=5701,y=5964,last[]={5705,5969};memcpy(unit,&type,4);memcpy(unit+8,&id,4);memcpy(unit+0xc,&mode,4);memcpy(unit+0x38,&p,8);
    memcpy(path+2,&x,2);memcpy(path+6,&y,2);memcpy(path+0x34,&count,4);memcpy(path+0xc8+8,last,4);
    pc_trace_unit s=pc_trace_snapshot((uintptr_t)unit);
    CHECK(s.valid&&s.id==17&&s.mode==0x11&&s.x==5701&&s.y==5964,"Cadáver type0 conserva coordenadas u16 originales");
    CHECK(s.count==3&&s.lastx==5705&&s.lasty==5969,"Último waypoint se lee con count y stride originales");
    count=129;memcpy(path+0x34,&count,4);s=pc_trace_snapshot((uintptr_t)unit);CHECK(s.count==129&&s.lastx==-1,"Cantidad fuera de rango no indexa el camino");
    p=0;memcpy(unit+0x38,&p,8);s=pc_trace_snapshot((uintptr_t)unit);CHECK(s.valid&&s.x==-1&&s.count==-1,"Unidad sin ruta sigue siendo legible");
    CHECK(!pc_trace_snapshot(0).valid&&!pc_trace_snapshot(1).valid&&!pc_trace_snapshot(UINT64_MAX-1).valid,"NULL, memoria no válida y overflow fallan sin dereferencia");
    uint64_t regs[32]={0},before[32];memcpy(before,regs,sizeof regs);dm_world_pc_controller_observe(regs,3);CHECK(!memcmp(regs,before,sizeof regs),"Fuente inválida no modifica registros");
    unsetenv("DM_MAC_POINTER_LOG");pc_trace_begin();CHECK(!pc_trace_active(),"Sin diagnóstico no se arma la ventana");
    setenv("DM_MAC_POINTER_LOG","fixture",1);pc_trace_begin();CHECK(pc_trace_active()&&g_pc_world_trace.seq==1,"Click arma ventana diagnóstica");g_pc_world_trace.mouse=20;pc_trace_begin();CHECK(g_pc_world_trace.seq==2&&!g_pc_world_trace.mouse,"Siguiente click renueva contadores acotados");
    uint8_t npc[0x160]={0},npc_before[sizeof npc],gfx[0x40]={0},entry[16]={0},resource[0x20]={0},box[0x20]={0};
    uint32_t cls=150,render_mode=2;int iso[]={-4051,93627},bounds[]={-20,20,-80,0};uint64_t value;
    type=1;mode=1;memcpy(npc,&type,4);memcpy(npc+4,&cls,4);memcpy(npc+8,&id,4);memcpy(npc+0xc,&mode,4);
    value=(uintptr_t)path;memcpy(npc+0x38,&value,8);memcpy(path+8,iso,sizeof iso);
    value=1;memcpy(npc+0x40,&value,8);memcpy(npc+0x58,&render_mode,4);
    value=(uintptr_t)gfx;memcpy(npc+0x78,&value,8);value=(uintptr_t)entry;memcpy(gfx+0x28,&value,8);value=1;memcpy(gfx+0x30,&value,8);
    memcpy(entry,&render_mode,4);value=(uintptr_t)resource;memcpy(entry+8,&value,8);value=(uintptr_t)box;memcpy(resource+0x18,&value,8);memcpy(box+8,bounds,sizeof bounds);
    pc_trace_npc ns=pc_trace_npc_snapshot((uintptr_t)npc);
    CHECK(ns.valid&&ns.cls==150&&ns.mode==1&&ns.render_mode==2,"NPC observa modo de render separado del modo de unidad");
    CHECK(ns.iso_valid&&ns.iso[0]==iso[0]&&ns.iso[1]==iso[1],"Origen ISO lee path+8/c, sin confundirlo con tile u16");
    CHECK(ns.gfx_valid&&ns.count==1&&ns.box_valid&&!memcmp(ns.box,bounds,sizeof bounds),"Caja gráfica por modo se lee sin getters nativos");
    value=0;memcpy(npc+0x78,&value,8);ns=pc_trace_npc_snapshot((uintptr_t)npc);CHECK(ns.valid&&!ns.gfx_valid&&!ns.box_valid,"NPC sin gráfico conserva diagnóstico con indicadores inválidos");
    value=(uintptr_t)gfx;memcpy(npc+0x78,&value,8);value=4097;memcpy(gfx+0x30,&value,8);ns=pc_trace_npc_snapshot((uintptr_t)npc);CHECK(ns.gfx_valid&&ns.count==4097&&!ns.box_valid,"Vector gráfico fuera de límite nunca se recorre");
    value=1;memcpy(gfx+0x30,&value,8);cls=151;memcpy(npc+4,&cls,4);CHECK(!pc_trace_npc_snapshot((uintptr_t)npc).valid,"Otro NPC no consume el presupuesto de Kashya/Akara/Charsi");
    cls=148;memcpy(npc+4,&cls,4);CHECK(pc_trace_npc_snapshot((uintptr_t)npc).valid,"Akara incluida");cls=154;memcpy(npc+4,&cls,4);CHECK(pc_trace_npc_snapshot((uintptr_t)npc).valid,"Charsi incluida");
    CHECK(!pc_trace_npc_snapshot(0).valid&&!pc_trace_npc_snapshot(UINT64_MAX-1).valid,"Snapshot NPC rechaza NULL y overflow");
    memcpy(npc_before,npc,sizeof npc);uint64_t heads[6*128]={0};heads[128]=(uintptr_t)npc;pc_pick_api picks={npc_hit,npc_prefer};
    for(int result=0;result<2;result++){npc_hit_result=result;CHECK(pc_pick_units(heads,123,456,iso[0],iso[1],&picks)==(result?(uintptr_t)npc:0),"Observador NPC conserva elección hit0/hit1 del selector");}
    CHECK(npc_hit_calls==2&&g_pc_world_trace.npcs==2,"Se registra el hit existente, sin repetir llamadas al juego");
    for(int i=0;i<10;i++)pc_trace_npc_observe((uintptr_t)npc,iso[0],iso[1],0);
    CHECK(g_pc_world_trace.npcs==6,"NPC limitado a seis registros por ventana");CHECK(!memcmp(npc,npc_before,sizeof npc),"Observación NPC nunca escribe la unidad");
    g_pc_world_trace.until=1;pc_trace_npc_observe((uintptr_t)npc,0,0,1);CHECK(g_pc_world_trace.npcs==6,"Ventana vencida evita observar NPC");pc_trace_begin();CHECK(!g_pc_world_trace.npcs,"Nuevo click reinicia presupuesto NPC");
    FILE *fp=fopen("build/main.orig.bin","rb");if(!fp)return 2;fseek(fp,0,SEEK_END);size_t n=ftell(fp);rewind(fp);uint8_t *binary=malloc(n);if(!binary||fread(binary,1,n,fp)!=n)return 2;fclose(fp);
    g_mod[0]=(module_t){.name="main",.base=binary,.size=n};g_nmod=1;
    uint8_t manager[0x2000]={0},manager_before[sizeof manager],unit_before[sizeof unit],path_before[sizeof path];
    memcpy(manager_before,manager,sizeof manager);memcpy(unit_before,unit,sizeof unit);memcpy(path_before,path,sizeof path);
    for(unsigned source=0;source<3;source++){
        memset(regs,0,sizeof regs);regs[source==2?1:0]=(uintptr_t)unit;regs[source==2?0:1]=source==2?0x42:6;regs[19]=(uintptr_t)manager;
        memcpy(before,regs,sizeof regs);dm_world_pc_controller_observe(regs,source);
        CHECK(!memcmp(before,regs,sizeof regs)&&g_pc_world_trace.source[source]==1,"Observación ABI válida conserva todos los registros");
    }
    CHECK(!memcmp(manager,manager_before,sizeof manager)&&!memcmp(unit,unit_before,sizeof unit)&&!memcmp(path,path_before,sizeof path),"Observación no escribe manager, unidad ni ruta");
    binary[0x4ace20]^=1;unsigned seen=g_pc_world_trace.source[0];dm_world_pc_controller_observe(regs,0);CHECK(g_pc_world_trace.source[0]==seen,"ABI distinta no lee contexto ni consume contador");binary[0x4ace20]^=1;
    g_pc_world_trace.source[0]=20;dm_world_pc_controller_observe(regs,0);CHECK(g_pc_world_trace.source[0]==20,"Límite20 evita observaciones posteriores");
    g_nmod=0;free(binary);
    g_pc_world_trace.until=1;CHECK(!pc_trace_active(),"Ventana vencida no mantiene observación por frame");
    printf("PC WORLD TRACE %d comprobaciones, %d fallos\n",checks,failures);return failures?1:0;
}
