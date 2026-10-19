#import "../dm_controls_mac.h"
#include "../dm_input.h"
#include "../dm_i18n.h"
#include "../touch_mac.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int checks, failures;
#define CHECK(c, text) do { checks++; if (!(c)) { failures++; fprintf(stderr,"FAIL %s\n",text); } else printf("PASS %s\n",text); } while(0)
static int native_mouse_used(int input){return input==258;}
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    @autoreleasepool {
        atomic_store(&g_dm_lang,0);
        NSData *legacyData = nil;
        const int previous[30][2]={{13,-1},{1,-1},{0,-1},{2,-1},{18,49},{19,-1},{21,-1},{20,-1},{3,-1},{5,-1},{48|DM_CONTROL_SHIFT,-1},{23,14},{6,-1},{22,-1},{24,69},{48,46},{123,-1},{126,-1},{124,-1},{125,-1},{36,-1},{53,-1},{256,-1},{257,-1},{512,-1},{513,-1},{34,-1},{8,-1},{17,-1},{12,-1}};
        if (!strcmp(argv[1],"olddefault") || !strcmp(argv[1],"oldcustom")) {
            NSMutableArray *array=[NSMutableArray array];for(int i=0;i<30;i++)[array addObject:@[@(i==DM_CTL_X&&!strcmp(argv[1],"oldcustom")?35:previous[i][0]),@(previous[i][1])]];
            legacyData=[NSJSONSerialization dataWithJSONObject:@{@"version":@1,@"bindings":array} options:0 error:nil];
            [NSFileManager.defaultManager createDirectoryAtPath:@(argv[2]) withIntermediateDirectories:YES attributes:nil error:nil];[legacyData writeToFile:[@(argv[2]) stringByAppendingPathComponent:@"controls_mac.json"] atomically:YES];
        }
        if (!strcmp(argv[1],"legacy")) {
            const int old[26][2]={{13,-1},{1,-1},{0,-1},{2,-1},{38,49},{40,-1},{32,-1},{34,-1},{3,-1},{5,-1},{12,-1},{14,-1},{6,-1},{8,-1},{24,69},{48,-1},{123,-1},{126,-1},{124,-1},{125,-1},{36,-1},{53,-1},{256,-1},{257,-1},{512,-1},{513,-1}};
            NSMutableArray *array=[NSMutableArray array];for(int i=0;i<26;i++)[array addObject:@[@(old[i][0]),@(old[i][1])]];
            legacyData=[NSJSONSerialization dataWithJSONObject:@{@"version":@1,@"bindings":array} options:0 error:nil];
            [NSFileManager.defaultManager createDirectoryAtPath:@(argv[2]) withIntermediateDirectories:YES attributes:nil error:nil];[legacyData writeToFile:[@(argv[2]) stringByAppendingPathComponent:@"controls_mac.json"] atomically:YES];
        }
        const int previousPC[34][2]={{-1,-1},{-1,-1},{-1,-1},{-1,-1},{18,-1},{19,-1},{21,-1},{20,-1},{9,-1},{7,-1},{48|DM_CONTROL_SHIFT,-1},{23,-1},{6,-1},{22,-1},{24,69},{48,46},{123,-1},{126,-1},{124,-1},{125,-1},{36,-1},{53,-1},{256,-1},{257,-1},{512,-1},{513,-1},{34,-1},{8,-1},{17,-1},{37,-1},{12,-1},{13,-1},{14,-1},{15,-1}};
        if (!strcmp(argv[1],"pc34default") || !strcmp(argv[1],"pc34custom") || !strcmp(argv[1],"pc34invalid")) {
            NSMutableArray *array=[NSMutableArray array];for(int i=0;i<34;i++)[array addObject:@[@(i==DM_CTL_X&&!strcmp(argv[1],"pc34custom")?35:previousPC[i][0]),@(previousPC[i][1])]];
            legacyData=[NSJSONSerialization dataWithJSONObject:@{@"version":@2,@"bindings":array,@"legacy_navigation":(!strcmp(argv[1],"pc34invalid")?(id)@[]:(id)@NO)} options:0 error:nil];
            [NSFileManager.defaultManager createDirectoryAtPath:@(argv[2]) withIntermediateDirectories:YES attributes:nil error:nil];[legacyData writeToFile:[@(argv[2]) stringByAppendingPathComponent:@"controls_mac.json"] atomically:YES];
        }
        dm_controls_init(argv[2]);
        int values[DM_CTL_COUNT][2]; char error[512], label[256]; dm_controls_get(values);
        if(!strcmp(argv[1],"mouse-write")) {
            CHECK(dm_controls_commit(values,error,sizeof error),"Fixture conserva perfil fallback propio");
            NSString *path=[@(argv[2])stringByAppendingPathComponent:@"controls_mac.json"];NSMutableDictionary *before=[[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:path] options:NSJSONReadingMutableContainers error:nil]mutableCopy];before[@"extension_existente"]=@{@"valor":@42};[[NSJSONSerialization dataWithJSONObject:before options:0 error:nil]writeToFile:path atomically:YES];
            int mouse[2][2]={{258,-1},{257,-1}};CHECK(!dm_controls_mouse_commit(mouse,native_mouse_used,error,sizeof error),"Botón reservado por native69 no activa doble ruta");mouse[0][0]=259;CHECK(dm_controls_mouse_commit(mouse,native_mouse_used,error,sizeof error),"Botón físico libre se guarda como extensión de ratón");
            NSDictionary *after=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:path] options:0 error:nil];CHECK([before[@"bindings"]isEqual:after[@"bindings"]]&&[before[@"extension_existente"]isEqual:after[@"extension_existente"]],"Aplicar ratón conserva todas las asignaciones fallback y campos desconocidos");
            CHECK(dm_controls_mouse_lookup(259)==DM_CTL_PRIMARY&&dm_controls_lookup(256)==DM_CTL_PRIMARY,"Entrada PC de ratón y perfil fallback consultan fuentes separadas");
            mouse[0][0]=35;CHECK(!dm_controls_mouse_commit(mouse,NULL,error,sizeof error),"Extensión PC no acepta teclas que pertenecen a native69");
        } else if(!strcmp(argv[1],"mouse-read")) {
            CHECK(dm_controls_mouse_lookup(259)==DM_CTL_PRIMARY&&dm_controls_mouse_lookup(257)==DM_CTL_SECONDARY,"Botones PC persisten en otro proceso");
            NSString *path=[@(argv[2])stringByAppendingPathComponent:@"controls_mac.json"];NSData *before=[NSData dataWithContentsOfFile:path];dm_controls_mouse_get((int[2][2]){{0}});CHECK([before isEqual:[NSData dataWithContentsOfFile:path]],"Cargar botones PC no reescribe JSON");
            CHECK(dm_controls_commit(values,error,sizeof error),"Aplicar fallback conserva extensión independiente");NSDictionary *after=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:path] options:0 error:nil];CHECK(after[@"pc_mouse"]!=nil&&after[@"extension_existente"]!=nil,"Aplicar fallback no borra botones PC ni metadatos ajenos");
        } else if (!strcmp(argv[1],"pc34invalid")) {
            int expected[DM_CTL_COUNT][2];dm_controls_defaults(expected);CHECK(!memcmp(expected,values,sizeof values),"Metadata inválida no dispara selectors Objective-C sobre otros tipos");
            CHECK([legacyData isEqual:[NSData dataWithContentsOfFile:[@(argv[2]) stringByAppendingPathComponent:@"controls_mac.json"]]],"Metadata inválida no reescribe configuración");
        } else if (!strcmp(argv[1],"pc34default")) {
            int expected[DM_CTL_COUNT][2];dm_controls_defaults(expected);CHECK(!memcmp(expected,values,sizeof values),"Perfil PC34 exacto migra G/X/Z conservando IDs");
            CHECK([legacyData isEqual:[NSData dataWithContentsOfFile:[@(argv[2]) stringByAppendingPathComponent:@"controls_mac.json"]]],"Cargar PC34 no reescribe archivo");
            CHECK(dm_controls_commit(values,error,sizeof error),"Aplicar migración PC34 persiste 35 acciones");
        } else if (!strcmp(argv[1],"pc34custom")) {
            int expected[34][2];memcpy(expected,previousPC,sizeof expected);expected[DM_CTL_X][0]=35;
            CHECK(!memcmp(expected,values,sizeof expected),"Una personalización PC34 conserva los 34 pares exactos");
            CHECK(values[DM_CTL_ZOOM][0]<0 && dm_controls_lookup(6)==DM_CTL_ZL && dm_controls_lookup(9)==DM_CTL_LS,"Zoom nuevo no roba Z existente ni migra V personalizado");
            CHECK([legacyData isEqual:[NSData dataWithContentsOfFile:[@(argv[2]) stringByAppendingPathComponent:@"controls_mac.json"]]],"Carga PC34 personalizada no reescribe archivo");
            CHECK(dm_controls_commit(values,error,sizeof error),"Aplicar personalización PC34 añade fila nueva sin cambiar pares");
        } else if (!strcmp(argv[1],"legacy")) {
            CHECK(dm_controls_lookup(32)==DM_CTL_X && dm_controls_lookup(34)==DM_CTL_Y,"Perfil guardado anterior conserva U e I de habilidades");
            CHECK(dm_controls_lookup(12)==DM_CTL_L && dm_controls_lookup(8)==DM_CTL_ZR,"Perfil guardado conserva Q/C anteriores");
            CHECK(values[DM_CTL_INVENTORY][0]<0 && values[DM_CTL_CHARACTER][0]<0,"Nuevos atajos no pisan teclas personalizadas antiguas");
            CHECK([legacyData isEqual:[NSData dataWithContentsOfFile:[@(argv[2]) stringByAppendingPathComponent:@"controls_mac.json"]]],"Cargar perfil anterior no reescribe el archivo guardado");
        } else if (!strcmp(argv[1],"olddefault")) {
            int defaults[DM_CTL_COUNT][2];dm_controls_defaults(defaults);
            CHECK(!memcmp(defaults,values,sizeof values),"Solo perfil anterior exacto migra todos sus valores al perfil Resurrected");
            CHECK(dm_controls_lookup(13)==DM_CTL_POTION_2 && dm_controls_button(DM_CTL_UP)==DM_MAC_NAV_UP,"Migración exacta separa W poción de flecha arriba");
            CHECK([legacyData isEqual:[NSData dataWithContentsOfFile:[@(argv[2]) stringByAppendingPathComponent:@"controls_mac.json"]]],"Migración en memoria no reescribe el archivo al cargar");
            CHECK(dm_controls_commit(values,error,sizeof error),"Aplicar perfil migrado escribe esquema nuevo");
            NSDictionary *saved=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[@(argv[2]) stringByAppendingPathComponent:@"controls_mac.json"]] options:0 error:nil];
            CHECK([saved[@"version"] isEqual:@2] && [saved[@"bindings"] count]==DM_CTL_COUNT && ![saved[@"legacy_navigation"] boolValue],"Esquema 2 guarda todas las acciones con navegación separada");
        } else if (!strcmp(argv[1],"oldcustom") || !strcmp(argv[1],"oldcustom-read")) {
            int expected[30][2];memcpy(expected,previous,sizeof expected);expected[DM_CTL_X][0]=35;
            CHECK(!memcmp(expected,values,sizeof expected),"Una sola personalización conserva íntegros los 30 pares anteriores");
            CHECK(dm_controls_lookup(13)==DM_CTL_MOVE_UP && dm_controls_lookup(12)==DM_CTL_QUESTS && dm_controls_lookup(35)==DM_CTL_X,"Perfil personalizado no pierde WASD, Q ni P originales");
            CHECK(dm_controls_button(DM_CTL_UP)==DM_BTN_UP && values[DM_CTL_POTION_1][0]<0 && values[DM_CTL_POTION_4][0]<0,"Compatibilidad conserva dirección/poción antigua sin añadir QWER");
            dm_controls_action_label(DM_CTL_UP,label,sizeof label);CHECK(strstr(label,"Potion 2")!=NULL,"Etiqueta de perfil antiguo describe su acción contextual conservada");
            if(legacyData) CHECK([legacyData isEqual:[NSData dataWithContentsOfFile:[@(argv[2]) stringByAppendingPathComponent:@"controls_mac.json"]]],"Carga personalizada no reescribe JSON anterior");
            CHECK(dm_controls_commit(values,error,sizeof error),"Aplicar perfil personalizado conserva semántica en esquema 2");
            NSDictionary *saved=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[@(argv[2]) stringByAppendingPathComponent:@"controls_mac.json"]] options:0 error:nil];
            CHECK([saved[@"version"] isEqual:@2] && [saved[@"legacy_navigation"] boolValue],"Esquema 2 registra compatibilidad de navegación personalizada");
        } else if (!strcmp(argv[1],"oldcustom-reset")) {
            int defaults[DM_CTL_COUNT][2];dm_controls_defaults(defaults);CHECK(dm_controls_commit(defaults,error,sizeof error),"Restaurar predeterminados aplica el nuevo perfil completo");
            CHECK(dm_controls_lookup(13)==DM_CTL_POTION_2 && dm_controls_button(DM_CTL_UP)==DM_MAC_NAV_UP,"Restaurar abandona compatibilidad antigua y activa separación PC");
        } else if (!strcmp(argv[1], "resurrected")) {
            const int skills[6]={18,19,20,21,23,22}, actions[6]={DM_CTL_A,DM_CTL_B,DM_CTL_Y,DM_CTL_X,DM_CTL_R,DM_CTL_ZR};
            for(int i=0;i<6;i++) CHECK(dm_controls_lookup(skills[i])==actions[i],"Perfil Resurrected mantiene habilidades 1–6 en orden del HUD");
            const int potions[4]={12,13,14,15}, belts[4]={DM_CTL_POTION_1,DM_CTL_POTION_2,DM_CTL_POTION_3,DM_CTL_POTION_4};
            for(int i=0;i<4;i++) CHECK(dm_controls_lookup(potions[i])==belts[i],"Perfil Resurrected QWER usa las cuatro columnas del cinturón");
            CHECK(values[DM_CTL_MOVE_UP][0]<0 && values[DM_CTL_MOVE_DOWN][0]<0 && values[DM_CTL_MOVE_LEFT][0]<0 && values[DM_CTL_MOVE_RIGHT][0]<0,"Movimiento WASD queda sin asignar por defecto y sigue disponible para remapear");
            CHECK(dm_controls_lookup(34)==DM_CTL_INVENTORY && dm_controls_lookup(8)==DM_CTL_CHARACTER && dm_controls_lookup(17)==DM_CTL_SKILLS && dm_controls_lookup(37)==DM_CTL_QUESTS,"Paneles I/C/T/L no ocupan pociones QWER");
            CHECK(dm_controls_lookup(6)==DM_CTL_ZOOM && values[DM_CTL_ZL][0]<0,"Zoom Z dispone de acción propia sin heredar grupo de mando");
            CHECK(dm_controls_lookup(5)==DM_CTL_LS && dm_controls_lookup(9)==-1 && dm_controls_lookup(7)==DM_CTL_RS,"Correr en G y cambiar armas en X respetan el perfil PC original");
            CHECK(dm_controls_lookup(DM_CONTROL_MOUSE)==DM_CTL_PRIMARY && dm_controls_lookup(DM_CONTROL_MOUSE+1)==DM_CTL_SECONDARY,"Perfil Resurrected conserva clic primario y secundario");
            CHECK(dm_controls_validate(values,error,sizeof error),"Perfil Resurrected no contiene asignaciones duplicadas");
            int seen[DM_CTL_COUNT]={0};for(int row=0;row<DM_CTL_COUNT;row++){int action=dm_controls_display_action(row);if(action>=0&&action<DM_CTL_COUNT)seen[action]++;}int complete=1;for(int i=0;i<DM_CTL_COUNT;i++)complete&=seen[i]==1;
            CHECK(complete,"Panel nativo expone cada acción exactamente una vez, incluidas las cuatro pociones");
            CHECK(dm_controls_lookup(123)==DM_CTL_LEFT && dm_controls_button(DM_CTL_LEFT)==DM_MAC_NAV_LEFT && dm_controls_button(DM_CTL_POTION_1)==DM_MAC_POTION_1,"Flechas y pociones publican señales distintas antes del hilo del juego");
        } else if (!strcmp(argv[1], "glyph")) {
            NSMutableString *manifest=[NSMutableString string];const int keys[]={21,18,35,12,13,14,15};for(unsigned i=0;i<sizeof keys/sizeof *keys;i++){dm_controls_label(keys[i],label,sizeof label);[manifest appendFormat:@"%d\t%s\n",keys[i],label];}
            [NSFileManager.defaultManager createDirectoryAtPath:@(argv[2]) withIntermediateDirectories:YES attributes:nil error:nil];[manifest writeToFile:[@(argv[2]) stringByAppendingPathComponent:@"inputglyphs.tsv"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
            setenv("DM_APP","pad1",1); setenv("DM_ROMFS_OVERLAY",argv[2],1); dm_controls_refresh_glyphs();
            const char *original="Controller/ControllerButtons/Switch/ButtonTop";
            CHECK(dm_controls_glyph_path(original)==original,"Ruta de mando conserva identidad y contenido");
            CHECK(!strcmp(dm_controls_glyph_path("Ztouch_controller_buttons_ui/switchKButtonTop"),"Ztouch_controller_buttons_ui/switchKInput0015"),"Ruta dinámica X resuelve tecla 4 actual");
            CHECK(!strcmp(dm_controls_glyph_path("ztouch_controller_buttons_ui/switchkButtonRight.sprite"),"Ztouch_controller_buttons_ui/switchKInput0012.sprite"),"Ruta literal con extensión conserva extensión");
            CHECK(!strcmp(dm_controls_glyph_path("Ztouch_controller_buttons_ui/switchKStickLeft"),"Ztouch_controller_buttons_ui/switchKMove"),"Movimiento usa flechas genéricas sin WASD fijo");
            CHECK(!strcmp(dm_controls_glyph_path("Ztouch_controller_buttons_ui/switchKDPadLeft"),"Ztouch_controller_buttons_ui/switchKInput000c"),"Glifo de poción 1 muestra Q en el HUD");
            CHECK(!strcmp(dm_controls_glyph_path("Ztouch_controller_buttons_ui/switchKDPadDown"),"Ztouch_controller_buttons_ui/switchKInput000e"),"Glifo de poción 3 muestra E respetando orden del cinturón");
            CHECK(dm_controls_glyph_path(NULL)==NULL,"Ruta nula es válida");
            char long_path[4096]; memset(long_path,'a',sizeof long_path); long_path[sizeof long_path-1]=0; CHECK(dm_controls_glyph_path(long_path)==long_path,"Ruta larga ajena permanece intacta");
            const char *unknown="Ztouch_controller_buttons_ui/switchKButtonTopOther"; CHECK(dm_controls_glyph_path(unknown)==unknown,"Prefijo parcial no sustituye otro recurso");
            values[DM_CTL_X][0]=35; CHECK(dm_controls_commit(values,error,sizeof error),"Aplicar reasignación cambia revisión y atlas efectivo");
            CHECK(!strcmp(dm_controls_glyph_path("Ztouch_controller_buttons_ui/switchKButtonTop"),"Ztouch_controller_buttons_ui/switchKInput0023"),"Tras Aplicar nueva ruta usa P sin reiniciar");
            values[DM_CTL_X][0]=-1; CHECK(dm_controls_commit(values,error,sizeof error) && strstr(dm_controls_glyph_path("Ztouch_controller_buttons_ui/switchKButtonTop"),"Unbound"),"Sin asignar muestra símbolo explícito");
        } else if (!strcmp(argv[1], "read")) {
            CHECK(dm_controls_lookup(35) == DM_CTL_X, "Nueva pulsación de P persiste al abrir otro proceso");
            CHECK(dm_controls_lookup(DM_CONTROL_MOUSE + 4) == DM_CTL_A, "Botón5 del ratón persiste");
            CHECK(dm_controls_lookup(21) == -1, "La antigua tecla 4 queda liberada");
            CHECK(dm_controls_lookup(34)==DM_CTL_INVENTORY && dm_controls_lookup(8)==DM_CTL_CHARACTER && dm_controls_lookup(17)==DM_CTL_SKILLS && dm_controls_lookup(37)==DM_CTL_QUESTS,"Perfil PC conserva I/C/T/L al abrir otro proceso");
        } else {
            const int pc_keys[6]={18,19,20,21,23,22}, pc_actions[6]={DM_CTL_A,DM_CTL_B,DM_CTL_Y,DM_CTL_X,DM_CTL_R,DM_CTL_ZR};
            for(int i=0;i<6;i++) CHECK(dm_controls_lookup(pc_keys[i])==pc_actions[i], "Perfil PC 1–6 coincide con orden real de habilidades del HUD");
            CHECK(dm_controls_validate(values,error,sizeof error),"Perfil PC completo no contiene conflictos");
            CHECK(dm_controls_lookup(34)==DM_CTL_INVENTORY && dm_controls_lookup(8)==DM_CTL_CHARACTER && dm_controls_lookup(17)==DM_CTL_SKILLS && dm_controls_lookup(37)==DM_CTL_QUESTS,"Atajos PC I/C/T/L ocupan acciones dedicadas");
            CHECK(dm_controls_lookup(48)==DM_CTL_MINUS && dm_controls_lookup(46)==DM_CTL_MINUS,"Tab y M comparten mapa original");
            CHECK(dm_controls_lookup(36)==DM_CTL_CONFIRM && dm_controls_lookup(53)==DM_CTL_BACK,"Retorno y Esc mantienen navegación contextual");
            CHECK(dm_controls_display_action(0)==DM_CTL_PRIMARY && dm_controls_display_action(1)==DM_CTL_SECONDARY,"Perfil presenta ratón antes de movimiento alternativo");
            CHECK(dm_controls_lookup(18) == DM_CTL_A && dm_controls_lookup(49) == -1, "Solo la tecla 1 activa de inicio la primera habilidad");
            CHECK(dm_controls_lookup(DM_CONTROL_MOUSE) == DM_CTL_PRIMARY && dm_controls_lookup(DM_CONTROL_MOUSE+1) == DM_CTL_SECONDARY, "Botones de ratón mantienen acciones contextuales iniciales");
            CHECK(dm_controls_button(DM_CTL_CONFIRM) == 1ull<<40 && dm_controls_button(DM_CTL_X) == DM_BTN_X, "Confirmación contextual y habilidades usan los bits existentes");
            values[DM_CTL_X][0] = 18;
            CHECK(!dm_controls_validate(values,error,sizeof error) && strstr(error,"already assigned"), "Conflictos se rechazan con explicación de la acción existente");
            CHECK(dm_controls_lookup(21) == DM_CTL_X, "Un conflicto no altera los controles efectivos");
            dm_controls_defaults(values); values[DM_CTL_X][0] = 35; values[DM_CTL_A][1] = DM_CONTROL_MOUSE + 4;
            values[DM_CTL_Y][1] = 17 | DM_CONTROL_SHIFT | DM_CONTROL_CONTROL;
            CHECK(dm_controls_commit(values,error,sizeof error), "Asignación de tecla, ratón y modificadores se guarda atómicamente");
            CHECK(dm_controls_lookup(35) == DM_CTL_X && dm_controls_lookup(21) == -1, "Tecla reasignada entra por la acción original");
            CHECK(dm_controls_lookup(17|DM_CONTROL_SHIFT|DM_CONTROL_CONTROL)==DM_CTL_Y, "Las combinaciones con modificadores son independientes");
            CHECK(dm_controls_lookup(17)==DM_CTL_SKILLS, "La tecla sin modificadores no activa una combinación");
            NSEvent *event = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:NSEventModifierFlagControl|NSEventModifierFlagShift timestamp:0 windowNumber:0 context:nil characters:@"t" charactersIgnoringModifiers:@"t" isARepeat:NO keyCode:17];
            CHECK(dm_controls_event_input(event)==(17|DM_CONTROL_SHIFT|DM_CONTROL_CONTROL), "Captura NSEvent conserva tecla y modificadores");
            NSEvent *command = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:NSEventModifierFlagCommand timestamp:0 windowNumber:0 context:nil characters:@"q" charactersIgnoringModifiers:@"q" isARepeat:NO keyCode:12];
            CHECK(dm_controls_event_input(command)==-1, "Los atajos de macOS con Comando se conservan");
            dm_controls_label(53,label,sizeof label); CHECK(!strcmp(label,"Esc"), "La captura de Escape puede identificarse sin texto ambiguo");
            dm_controls_label(52,label,sizeof label); CHECK(strstr(label,"Key")!=NULL, "Teclas sin nombre no introducen caracteres de control");
            for(int key=54;key<=63;key++) if(key==54||key==55||key==57||key==63){ int reserved[DM_CTL_COUNT][2];dm_controls_defaults(reserved);reserved[DM_CTL_X][0]=key;CHECK(!dm_controls_validate(reserved,error,sizeof error),"Comando, Bloq Mayús y Fn no se aceptan como asignaciones inactivas"); }
            NSEvent *leftUp=[NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint modifierFlags:NSEventModifierFlagShift|4 timestamp:0 windowNumber:0 context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:56];
            CHECK(!dm_controls_modifier_down(leftUp),"Mayús izquierda se libera aunque derecha siga pulsada");
            NSEvent *rightDown=[NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint modifierFlags:NSEventModifierFlagShift|4 timestamp:0 windowNumber:0 context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:60];
            CHECK(dm_controls_modifier_down(rightDown),"Mayús derecha usa su bit físico independiente");
            int reset[DM_CTL_COUNT][2]; dm_controls_defaults(reset);
            CHECK(reset[DM_CTL_X][0]==21 && reset[DM_CTL_A][1]==-1, "Restaurar prepara valores originales sin aplicarlos");
            CHECK(dm_controls_lookup(35)==DM_CTL_X, "Preparar predeterminados no pierde la configuración antes de Aplicar");
        }
    }
    printf("CONTROLS %d comprobaciones, %d fallos\n",checks,failures); return failures?1:0;
}
