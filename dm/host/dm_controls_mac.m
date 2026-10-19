#import "dm_controls_mac.h"
#import <Carbon/Carbon.h>
#import <IOKit/hidsystem/IOLLEvent.h>
#include "dm_input.h"
#include "touch_mac.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const int k_defaults[DM_CTL_COUNT][2] = {
    {-1,-1}, {-1,-1}, {-1,-1}, {-1,-1}, {18,-1}, {19,-1}, {21,-1}, {20,-1}, {5,-1}, {7,-1},
    {48|DM_CONTROL_SHIFT,-1}, {23,-1}, {-1,-1}, {22,-1}, {24,69}, {48,46}, {123,-1}, {126,-1}, {124,-1}, {125,-1},
    {36,-1}, {53,-1}, {256,-1}, {257,-1}, {512,-1}, {513,-1}, {34,-1}, {8,-1}, {17,-1}, {37,-1},
    {12,-1}, {13,-1}, {14,-1}, {15,-1}, {6,-1}
};
static const int k_previous_pc_defaults[34][2] = {
    {-1,-1}, {-1,-1}, {-1,-1}, {-1,-1}, {18,-1}, {19,-1}, {21,-1}, {20,-1}, {9,-1}, {7,-1},
    {48|DM_CONTROL_SHIFT,-1}, {23,-1}, {6,-1}, {22,-1}, {24,69}, {48,46}, {123,-1}, {126,-1}, {124,-1}, {125,-1},
    {36,-1}, {53,-1}, {256,-1}, {257,-1}, {512,-1}, {513,-1}, {34,-1}, {8,-1}, {17,-1}, {37,-1},
    {12,-1}, {13,-1}, {14,-1}, {15,-1}
};
static const int k_previous_defaults[30][2] = {
    {13,-1}, {1,-1}, {0,-1}, {2,-1}, {18,49}, {19,-1}, {21,-1}, {20,-1}, {3,-1}, {5,-1},
    {48|DM_CONTROL_SHIFT,-1}, {23,14}, {6,-1}, {22,-1}, {24,69}, {48,46}, {123,-1}, {126,-1}, {124,-1}, {125,-1},
    {36,-1}, {53,-1}, {256,-1}, {257,-1}, {512,-1}, {513,-1}, {34,-1}, {8,-1}, {17,-1}, {12,-1}
};
static const char *const k_actions[DM_CTL_COUNT] = {
    "Moverse arriba (alternativa)", "Moverse abajo (alternativa)", "Moverse a la izquierda (alternativa)", "Moverse a la derecha (alternativa)",
    "Habilidad 1", "Habilidad 2", "Habilidad 4", "Habilidad 3", "Correr / andar", "Cambiar armas",
    "Pestaña anterior", "Habilidad 5", "Cambiar grupo de habilidades", "Habilidad 6", "Abrir funciones / aceptar", "Mapa",
    "Navegar a la izquierda", "Navegar arriba", "Navegar a la derecha", "Navegar abajo",
    "Confirmar según la página", "Volver / abrir opciones", "Moverse, atacar e interactuar", "Habilidad secundaria seleccionada", "Desplazar lista arriba", "Desplazar lista abajo",
    "Inventario", "Personaje", "Árbol de habilidades", "Misiones",
    "Poción 1", "Poción 2", "Poción 3", "Poción 4", "Acercar / alejar"
};
static const int k_display_order[DM_CTL_COUNT] = {
    DM_CTL_PRIMARY,DM_CTL_SECONDARY,DM_CTL_A,DM_CTL_B,DM_CTL_Y,DM_CTL_X,DM_CTL_R,DM_CTL_ZR,
    DM_CTL_POTION_1,DM_CTL_POTION_2,DM_CTL_POTION_3,DM_CTL_POTION_4,
    DM_CTL_INVENTORY,DM_CTL_CHARACTER,DM_CTL_SKILLS,DM_CTL_QUESTS,DM_CTL_MINUS,DM_CTL_BACK,DM_CTL_CONFIRM,DM_CTL_PLUS,
    DM_CTL_LS,DM_CTL_RS,DM_CTL_ZOOM,DM_CTL_ZL,DM_CTL_L,DM_CTL_LEFT,DM_CTL_UP,DM_CTL_DOWN,DM_CTL_RIGHT,DM_CTL_SCROLL_UP,DM_CTL_SCROLL_DOWN,
    DM_CTL_MOVE_UP,DM_CTL_MOVE_DOWN,DM_CTL_MOVE_LEFT,DM_CTL_MOVE_RIGHT
};
int dm_controls_display_action(int row) { return row >= 0 && row < DM_CTL_COUNT ? k_display_order[row] : -1; }
static const uint64_t k_buttons[DM_CTL_COUNT] = {
    0,0,0,0, DM_BTN_A,DM_BTN_B,DM_BTN_X,DM_BTN_Y,DM_BTN_LS,DM_BTN_RS,DM_BTN_L,DM_BTN_R,DM_BTN_ZL,DM_BTN_ZR,DM_BTN_PLUS,DM_BTN_MINUS,
    DM_MAC_NAV_LEFT,DM_MAC_NAV_UP,DM_MAC_NAV_RIGHT,DM_MAC_NAV_DOWN,1ull<<40,1ull<<41,0,0,0,0,1ull<<42,1ull<<43,1ull<<44,1ull<<45,
    DM_MAC_POTION_1,DM_MAC_POTION_2,DM_MAC_POTION_3,DM_MAC_POTION_4,DM_MAC_ZOOM
};
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_bindings[DM_CTL_COUNT][2], g_initialized;
static int g_pc_mouse_bindings[2][2]={{256,-1},{257,-1}};
static NSString *g_path;
static _Atomic unsigned g_revision;
static _Atomic int g_legacy_navigation;
static void layout_changed(CFNotificationCenterRef center, void *observer, CFStringRef name, const void *object, CFDictionaryRef info) {
    (void)center; (void)observer; (void)name; (void)object; (void)info;
    dispatch_async(dispatch_get_main_queue(), ^{ dm_controls_refresh_glyphs(); atomic_fetch_add(&g_revision, 1); });
}

void dm_controls_defaults(int out[DM_CTL_COUNT][2]) { memcpy(out, k_defaults, sizeof k_defaults); }
static int valid_input(int input) {
    if (input == -1) return 1;
    if (input < 0 || input & ~8191) return 0;
    int base = input & 1023;
    return (base < 128 && base != 54 && base != 55 && base != 57 && base != 63) || (base >= 256 && base < 288) || base == 512 || base == 513;
}
void dm_controls_action_label(int action, char *out, size_t cap) {
    static const char *const legacy[] = {"Poción 1 / navegar izquierda", "Poción 2 / navegar arriba", "Poción 4 / navegar derecha", "Poción 3 / navegar abajo"};
    const char *name = action >= 0 && action < DM_CTL_COUNT ? k_actions[action] : "Acción desconocida";
    if (atomic_load(&g_legacy_navigation) && action >= DM_CTL_LEFT && action <= DM_CTL_DOWN) name = legacy[action-DM_CTL_LEFT];
    snprintf(out, cap, "%s", name);
}
static NSString *key_name(int code) {
    NSDictionary *special = @{@36:@"Retorno", @48:@"Tab", @49:@"Espacio", @51:@"Borrar", @53:@"Esc", @55:@"⌘", @56:@"Mayús", @58:@"⌥", @59:@"⌃", @60:@"Mayús der.", @61:@"⌥ der.", @62:@"⌃ der.", @63:@"Fn", @69:@"+ num.", @71:@"Borrar num.", @76:@"Intro num.", @114:@"Ayuda", @115:@"Inicio", @116:@"RePág", @117:@"Supr", @119:@"Fin", @121:@"AvPág", @123:@"←", @124:@"→", @125:@"↓", @126:@"↑", @122:@"F1", @120:@"F2", @99:@"F3", @118:@"F4", @96:@"F5", @97:@"F6", @98:@"F7", @100:@"F8", @101:@"F9", @109:@"F10", @103:@"F11", @111:@"F12", @105:@"F13", @107:@"F14", @113:@"F15", @106:@"F16", @64:@"F17", @79:@"F18", @80:@"F19", @90:@"F20"};
    NSString *name = special[@(code)]; if (name) return name;
    TISInputSourceRef source = TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
    CFDataRef layout = source ? TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData) : NULL;
    UniChar chars[8]; UniCharCount count = 0; UInt32 dead = 0;
    OSStatus status = layout ? UCKeyTranslate((const UCKeyboardLayout *)CFDataGetBytePtr(layout), code, kUCKeyActionDisplay, 0, LMGetKbdType(), kUCKeyTranslateNoDeadKeysBit, &dead, 8, &count, chars) : -1;
    if (source) CFRelease(source);
    if (status == noErr && count) {
        NSString *translated = [[NSString stringWithCharacters:chars length:count] uppercaseString];
        if ([translated rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location == NSNotFound) return translated;
    }
    return [NSString stringWithFormat:@"Tecla %d", code];
}
int dm_controls_virtual_modifiers(NSEvent *event) {
    return (event.modifierFlags&NSEventModifierFlagShift?0x1000:0)|(event.modifierFlags&NSEventModifierFlagControl?0x2000:0)|(event.modifierFlags&NSEventModifierFlagOption?0x4000:0);
}
int dm_controls_virtual_key(NSEvent *event) {
    if(event.modifierFlags&NSEventModifierFlagCommand)return -1;
    static const struct {uint16_t code,vk;} special[]={
        {18,0x31},{19,0x32},{20,0x33},{21,0x34},{23,0x35},{22,0x36},{26,0x37},{28,0x38},{25,0x39},{29,0x30},
        {36,0x0d},{48,0x09},{49,0x20},{51,0x08},{53,0x1b},{56,0x10},{60,0x10},{59,0x11},{62,0x11},{58,0x12},{61,0x12},
        {114,0x2d},{115,0x24},{116,0x21},{117,0x2e},{119,0x23},{121,0x22},{123,0x25},{124,0x27},{125,0x28},{126,0x26},
        {122,0x70},{120,0x71},{99,0x72},{118,0x73},{96,0x74},{97,0x75},{98,0x76},{100,0x77},{101,0x78},{109,0x79},{103,0x7a},{111,0x7b},{105,0x7c},{107,0x7d},{113,0x7e},{106,0x7f},{64,0x80},{79,0x81},{80,0x82},{90,0x83},
        {82,0x60},{83,0x61},{84,0x62},{85,0x63},{86,0x64},{87,0x65},{88,0x66},{89,0x67},{91,0x68},{92,0x69},
        {65,0x6e},{67,0x6a},{69,0x6b},{75,0x6f},{76,0x0d},{78,0x6d},{71,0x0c},{81,0xbb},{10,0xe2}
    };
    unsigned code=event.keyCode;if(code>=128||code==54||code==55||code==57||code==63)return -1;
    for(unsigned i=0;i<sizeof special/sizeof *special;i++)if(code==special[i].code)return special[i].vk;
    NSString *name=key_name(code);if(name.length==1){unichar c=[name characterAtIndex:0];if((c>='A'&&c<='Z')||(c>='0'&&c<='9'))return c;}
    // Los OEM conservan su posición física; la tabla nativa define sus etiquetas.
    static const struct {uint16_t code,vk;} oem[]={{24,0xbb},{27,0xbd},{30,0xdd},{33,0xdb},{39,0xde},{41,0xba},{42,0xdc},{43,0xbc},{44,0xbf},{47,0xbe},{50,0xc0}};
    for(unsigned i=0;i<sizeof oem/sizeof *oem;i++)if(code==oem[i].code)return oem[i].vk;
    return -1;
}
void dm_controls_label(int input, char *out, size_t capacity) {
    @autoreleasepool {
        if (input < 0) { snprintf(out, capacity, "Sin asignar"); return; }
        int base = input & 1023; NSString *name = base < 128 ? key_name(base) : base < 288 ? (base == 256 ? @"Ratón izquierdo" : base == 257 ? @"Ratón derecho" : [NSString stringWithFormat:@"Ratón %d", base - 255]) : base == 512 ? @"Rueda arriba" : @"Rueda abajo";
        NSString *prefix = [NSString stringWithFormat:@"%@%@%@", input & DM_CONTROL_CONTROL ? @"⌃" : @"", input & DM_CONTROL_OPTION ? @"⌥" : @"", input & DM_CONTROL_SHIFT ? @"⇧" : @""];
        snprintf(out, capacity, "%s%s", prefix.UTF8String, name.UTF8String);
    }
}
int dm_controls_validate(const int bindings[DM_CTL_COUNT][2], char *error, size_t capacity) {
    for (int a = 0; a < DM_CTL_COUNT; a++) for (int slot = 0; slot < 2; slot++) {
        int value = bindings[a][slot]; if (!valid_input(value)) { snprintf(error, capacity, "Asignación inválida en %s.", k_actions[a]); return 0; }
        if (value < 0) continue;
        for (int other = 0; other <= a; other++) for (int s = 0; s < 2; s++) {
            if (other == a && s >= slot) break;
            if (bindings[other][s] == value) { char label[128]; dm_controls_label(value, label, sizeof label); snprintf(error, capacity, "%s ya está asignado a «%s». Quita primero esa asignación.", label, k_actions[other]); return 0; }
        }
    }
    if (error && capacity) error[0] = 0;
    return 1;
}
int dm_controls_mouse_validate(const int values[2][2],dm_controls_mouse_reserved_fn reserved,char *error,size_t cap) {
    for(int a=0;a<2;a++)for(int slot=0;slot<2;slot++) {
        int input=values[a][slot];if(input==-1)continue;
        if(input<256 || input>=288){snprintf(error,cap,"Elige un botón físico del ratón, sin modificadores. Teclado y rueda se editan en Controles del juego.");return 0;}
        if(reserved&&reserved(input)){snprintf(error,cap,"Este botón ya tiene una asignación en Controles del juego. Libérala allí antes de usarlo aquí.");return 0;}
        for(int other=0;other<=a;other++)for(int s=0;s<2;s++)if((other<a||s<slot)&&values[other][s]==input){snprintf(error,cap,"Ese botón ya está asignado a otra entrada del ratón.");return 0;}
    }
    if(error&&cap)error[0]=0;return 1;
}
void dm_controls_init(const char *directory) {
    @autoreleasepool {
        pthread_mutex_lock(&g_lock);
        if (g_initialized) { pthread_mutex_unlock(&g_lock); return; }
        memcpy(g_bindings, k_defaults, sizeof g_bindings);
        NSString *dir = directory ? @(directory) : (getenv("DM_APP_SUPPORT") ? @(getenv("DM_APP_SUPPORT")) : [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject stringByAppendingPathComponent:@"D2R Mobile"]);
        g_path = [dir stringByAppendingPathComponent:@"controls_mac.json"];
        NSData *data = [NSData dataWithContentsOfFile:g_path];
        NSDictionary *json = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        BOOL version1 = [json isKindOfClass:NSDictionary.class] && [json[@"version"] isEqual:@1], version2 = [json isKindOfClass:NSDictionary.class] && [json[@"version"] isEqual:@2];
        NSArray *values = version1 || version2 ? json[@"bindings"] : nil;
        if ([values isKindOfClass:NSArray.class] && ((version2 && (values.count == 34 || values.count == DM_CTL_COUNT)) || (version1 && (values.count == 30 || values.count == DM_CTL_INVENTORY)))) {
            int candidate[DM_CTL_COUNT][2]; memcpy(candidate, k_defaults, sizeof candidate); int valid = 1;
            for (int a = 0; a < (int)values.count; a++) {
                NSArray *pair = values[a];
                if (![pair isKindOfClass:NSArray.class] || pair.count != 2 || ![pair[0] isKindOfClass:NSNumber.class] || ![pair[1] isKindOfClass:NSNumber.class]) { valid = 0; break; }
                for (int s = 0; s < 2; s++) { double v = [pair[s] doubleValue]; if (v != [pair[s] intValue]) valid = 0; candidate[a][s] = [pair[s] intValue]; }
            }
            id legacy = json[@"legacy_navigation"];
            BOOL legacyValid = !version2 || !legacy || [legacy isKindOfClass:NSNumber.class];
            BOOL previousDefault = (version1 && values.count == 30 && !memcmp(candidate,k_previous_defaults,sizeof k_previous_defaults)) || (version2 && legacyValid && values.count == 34 && ![legacy boolValue] && !memcmp(candidate,k_previous_pc_defaults,sizeof k_previous_pc_defaults));
            if (previousDefault) memcpy(candidate,k_defaults,sizeof candidate);
            else if (version1) {
                for (int a = DM_CTL_POTION_1; a < DM_CTL_COUNT; a++) candidate[a][0] = candidate[a][1] = -1;
                for (int a = (int)values.count; a < DM_CTL_COUNT; a++) for (int slot = 0; slot < 2; slot++)
                    for (int old = 0; old < (int)values.count; old++) if (candidate[a][slot] >= 0 && (candidate[a][slot] == candidate[old][0] || candidate[a][slot] == candidate[old][1])) candidate[a][slot] = -1;
            }
            if (!previousDefault) for (int a=(int)values.count;a<DM_CTL_COUNT;a++) for (int slot=0;slot<2;slot++)
                for (int old=0;old<(int)values.count;old++) if (candidate[a][slot]>=0 && (candidate[a][slot]==candidate[old][0] || candidate[a][slot]==candidate[old][1])) candidate[a][slot]=-1;

            char error[256]; if (valid && legacyValid && dm_controls_validate(candidate, error, sizeof error)) {
                memcpy(g_bindings, candidate, sizeof candidate);
                atomic_store(&g_legacy_navigation, version1 ? !previousDefault : [legacy boolValue]);
            }
        }
        int mouse[2][2];for(int a=0;a<2;a++)for(int slot=0;slot<2;slot++)mouse[a][slot]=g_bindings[DM_CTL_PRIMARY+a][slot];
        char mouse_error[384];if(dm_controls_mouse_validate(mouse,NULL,mouse_error,sizeof mouse_error))memcpy(g_pc_mouse_bindings,mouse,sizeof mouse);
        NSArray *mouse_values=[json isKindOfClass:NSDictionary.class]?json[@"pc_mouse"]:nil;
        if([mouse_values isKindOfClass:NSArray.class]&&mouse_values.count==2) {
            BOOL valid=YES;for(int a=0;a<2;a++) {id pair=mouse_values[a];if(![pair isKindOfClass:NSArray.class]||[pair count]!=2){valid=NO;break;}
                for(int slot=0;slot<2;slot++){id v=pair[slot];if(![v isKindOfClass:NSNumber.class]||[v doubleValue]!=[v intValue]){valid=NO;break;}mouse[a][slot]=[v intValue];}}
            if(valid&&dm_controls_mouse_validate(mouse,NULL,mouse_error,sizeof mouse_error))memcpy(g_pc_mouse_bindings,mouse,sizeof mouse);
        }
        g_initialized = 1; atomic_store(&g_revision, 1); pthread_mutex_unlock(&g_lock);
        dm_controls_refresh_glyphs();
        CFNotificationCenterAddObserver(CFNotificationCenterGetDistributedCenter(), NULL, layout_changed, kTISNotifySelectedKeyboardInputSourceChanged, NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
    }
}
void dm_controls_get(int out[DM_CTL_COUNT][2]) { dm_controls_init(NULL); pthread_mutex_lock(&g_lock); memcpy(out, g_bindings, sizeof g_bindings); pthread_mutex_unlock(&g_lock); }
void dm_controls_mouse_get(int out[2][2]) {dm_controls_init(NULL);pthread_mutex_lock(&g_lock);memcpy(out,g_pc_mouse_bindings,sizeof g_pc_mouse_bindings);pthread_mutex_unlock(&g_lock);}
int dm_controls_mouse_lookup(int input) {int values[2][2];dm_controls_mouse_get(values);for(int a=0;a<2;a++)for(int slot=0;slot<2;slot++)if(input>=0&&values[a][slot]==input)return DM_CTL_PRIMARY+a;return -1;}
int dm_controls_mouse_commit(const int values[2][2],dm_controls_mouse_reserved_fn reserved,char *error,size_t cap) {
    if(!dm_controls_mouse_validate(values,reserved,error,cap))return 0;dm_controls_init(NULL);
    @autoreleasepool {
        NSData *old=[NSData dataWithContentsOfFile:g_path];id parsed=old?[NSJSONSerialization JSONObjectWithData:old options:NSJSONReadingMutableContainers error:nil]:nil;
        NSMutableDictionary *root=[parsed isKindOfClass:NSDictionary.class]?[parsed mutableCopy]:[NSMutableDictionary dictionary];
        root[@"pc_mouse"]=@[@[@(values[0][0]),@(values[0][1])],@[@(values[1][0]),@(values[1][1])]];
        NSError *failure=nil;NSData *data=[NSJSONSerialization dataWithJSONObject:root options:NSJSONWritingPrettyPrinted error:&failure];
        if(data&&![NSFileManager.defaultManager createDirectoryAtPath:g_path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:&failure])data=nil;
        if(!data||![data writeToFile:g_path options:NSDataWritingAtomic error:&failure]){snprintf(error,cap,"No se pudieron guardar los botones del ratón: %s",failure.localizedDescription.UTF8String?:"error de escritura");return 0;}
        pthread_mutex_lock(&g_lock);memcpy(g_pc_mouse_bindings,values,sizeof g_pc_mouse_bindings);pthread_mutex_unlock(&g_lock);atomic_fetch_add(&g_revision,1);
    }
    if(error&&cap)error[0]=0;return 1;
}
int dm_controls_commit(const int values[DM_CTL_COUNT][2], char *error, size_t capacity) {
    if (!dm_controls_validate(values, error, capacity)) return 0;
    dm_controls_init(NULL);
    @autoreleasepool {
        NSMutableArray *array = [NSMutableArray array]; for (int a = 0; a < DM_CTL_COUNT; a++) [array addObject:@[@(values[a][0]), @(values[a][1])]];
        BOOL legacy = atomic_load(&g_legacy_navigation) && memcmp(values,k_defaults,sizeof k_defaults) != 0;
        NSData *existing=[NSData dataWithContentsOfFile:g_path];id parsed=existing?[NSJSONSerialization JSONObjectWithData:existing options:NSJSONReadingMutableContainers error:nil]:nil;
        NSMutableDictionary *root=[parsed isKindOfClass:NSDictionary.class]?[parsed mutableCopy]:[NSMutableDictionary dictionary];root[@"version"]=@2;root[@"bindings"]=array;root[@"legacy_navigation"]=@(legacy);
        NSError *failure = nil; NSData *data = [NSJSONSerialization dataWithJSONObject:root options:NSJSONWritingPrettyPrinted error:&failure];
        [[NSFileManager defaultManager] createDirectoryAtPath:g_path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:&failure];
        if (!data || ![data writeToFile:g_path options:NSDataWritingAtomic error:&failure]) { snprintf(error, capacity, "No se pudieron guardar los controles: %s", failure.localizedDescription.UTF8String ?: "error de escritura"); return 0; }
        pthread_mutex_lock(&g_lock); memcpy(g_bindings, values, sizeof g_bindings); atomic_store(&g_legacy_navigation,legacy); pthread_mutex_unlock(&g_lock);
    }
    dm_controls_refresh_glyphs();
    atomic_fetch_add(&g_revision, 1);
    return 1;
}
int dm_controls_lookup(int input) { dm_controls_init(NULL); pthread_mutex_lock(&g_lock); int found = -1; for (int a = 0; a < DM_CTL_COUNT; a++) for (int slot = 0; slot < 2; slot++) if (input >= 0 && g_bindings[a][slot] == input) found = a; pthread_mutex_unlock(&g_lock); return found; }
uint64_t dm_controls_button(int action) {
    static const uint64_t legacy[] = {DM_BTN_LEFT,DM_BTN_UP,DM_BTN_RIGHT,DM_BTN_DOWN};
    if (atomic_load(&g_legacy_navigation) && action >= DM_CTL_LEFT && action <= DM_CTL_DOWN) return legacy[action-DM_CTL_LEFT];
    return action >= 0 && action < DM_CTL_COUNT ? k_buttons[action] : 0;
}
int dm_controls_primary_input(int action) { int values[DM_CTL_COUNT][2]; dm_controls_get(values); return action >= 0 && action < DM_CTL_COUNT ? (values[action][0] >= 0 ? values[action][0] : values[action][1]) : -1; }
unsigned dm_controls_revision(void) { return atomic_load(&g_revision); }
BOOL dm_controls_modifier_down(NSEvent *event) {
    NSUInteger mask = event.keyCode == 56 ? NX_DEVICELSHIFTKEYMASK : event.keyCode == 60 ? NX_DEVICERSHIFTKEYMASK : event.keyCode == 59 ? NX_DEVICELCTLKEYMASK : event.keyCode == 62 ? NX_DEVICERCTLKEYMASK : event.keyCode == 58 ? NX_DEVICELALTKEYMASK : event.keyCode == 61 ? NX_DEVICERALTKEYMASK : 0;
    return mask && (event.modifierFlags & mask);
}
int dm_controls_event_input(NSEvent *event) {
    if (event.modifierFlags & NSEventModifierFlagCommand) return -1;
    int modifiers = (event.modifierFlags & NSEventModifierFlagShift ? DM_CONTROL_SHIFT : 0) | (event.modifierFlags & NSEventModifierFlagControl ? DM_CONTROL_CONTROL : 0) | (event.modifierFlags & NSEventModifierFlagOption ? DM_CONTROL_OPTION : 0);
    switch (event.type) {
        case NSEventTypeFlagsChanged: return event.keyCode == 56 || event.keyCode == 58 || event.keyCode == 59 || event.keyCode == 60 || event.keyCode == 61 || event.keyCode == 62 ? event.keyCode : -1;
        case NSEventTypeKeyDown: case NSEventTypeKeyUp: return event.keyCode < 128 ? event.keyCode | modifiers : -1;
        case NSEventTypeLeftMouseDown: case NSEventTypeLeftMouseUp: case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDown: case NSEventTypeRightMouseUp: case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDown: case NSEventTypeOtherMouseUp: case NSEventTypeOtherMouseDragged: return event.buttonNumber < 32 ? (DM_CONTROL_MOUSE + (int)event.buttonNumber) | modifiers : -1;
        case NSEventTypeScrollWheel: return event.scrollingDeltaY == 0 ? -1 : (event.scrollingDeltaY > 0 ? DM_CONTROL_WHEEL_UP : DM_CONTROL_WHEEL_DOWN) | modifiers;
        default: return -1;
    }
}
BOOL dm_controls_text_responder(NSResponder *responder) { return [responder isKindOfClass:NSText.class] || [responder isKindOfClass:NSTextField.class] || [responder conformsToProtocol:@protocol(NSTextInputClient)]; }

static char g_glyph_paths[DM_CTL_COUNT][128];
static int g_glyph_available;
static NSString *manifest_unescape(NSString *text) {
    NSMutableString *out = [NSMutableString string];
    for (NSUInteger i = 0; i < text.length; i++) { unichar c = [text characterAtIndex:i]; if (c == '\\' && i + 1 < text.length) { c = [text characterAtIndex:++i]; c = c == 't' ? '\t' : c == 'n' ? '\n' : c == 'r' ? '\r' : c; } [out appendFormat:@"%C", c]; }
    return out;
}
void dm_controls_refresh_glyphs(void) {
    @autoreleasepool {
        NSString *manifest = nil; const char *overlay = getenv("DM_ROMFS_OVERLAY");
        for (NSString *root in [@(overlay ?: "") componentsSeparatedByString:@":"]) {
            NSString *candidate = [root stringByAppendingPathComponent:@"inputglyphs.tsv"];
            manifest = [NSString stringWithContentsOfFile:candidate encoding:NSUTF8StringEncoding error:nil]; if (manifest) break;
        }
        NSMutableDictionary<NSString *, NSNumber *> *labels = [NSMutableDictionary dictionary];
        for (NSString *line in [manifest componentsSeparatedByString:@"\n"]) {
            NSRange tab = [line rangeOfString:@"\t"]; if (tab.location == NSNotFound) continue;
            NSString *number = [line substringToIndex:tab.location], *label = [line substringFromIndex:tab.location + 1];
            label = manifest_unescape(label);
            int value = number.intValue; if (valid_input(value) && value >= 0 && !labels[label]) labels[label] = @(value);
        }
        int bindings[DM_CTL_COUNT][2]; dm_controls_get(bindings); char paths[DM_CTL_COUNT][128];
        for (int action = 0; action < DM_CTL_COUNT; action++) {
            int input = bindings[action][0] >= 0 ? bindings[action][0] : bindings[action][1];
            if (input < 0) snprintf(paths[action], sizeof paths[action], "Ztouch_controller_buttons_ui/switchKUnbound");
            else {
                char native[256]; dm_controls_label(input, native, sizeof native); NSNumber *match = labels[@(native)];
                int base = input & 1023;
                if (match) snprintf(paths[action], sizeof paths[action], "Ztouch_controller_buttons_ui/switchKInput%04x", match.intValue);
                else if (base < 128) snprintf(paths[action], sizeof paths[action], "Ztouch_controller_buttons_ui/switchKCode%04x", input);
                else snprintf(paths[action], sizeof paths[action], "Ztouch_controller_buttons_ui/switchKInput%04x", input);
            }
        }
        pthread_mutex_lock(&g_lock); memcpy(g_glyph_paths, paths, sizeof paths); g_glyph_available = labels.count > 0; pthread_mutex_unlock(&g_lock);
    }
}
const char *dm_controls_glyph_path(const char *original) {
    if (!original || !getenv("DM_APP")) return original;
    static const char prefix[] = "Ztouch_controller_buttons_ui/switchK";
    if (strncasecmp(original, prefix, sizeof prefix - 1)) return original;
    const char *name = original + sizeof prefix - 1;
    static const struct { const char *name; int action; } names[] = {
        {"ButtonRight",DM_CTL_A}, {"ButtonBottom",DM_CTL_B}, {"ButtonTop",DM_CTL_X}, {"ButtonLeft",DM_CTL_Y},
        {"StickLeftButton",DM_CTL_LS}, {"StickRightButton",DM_CTL_RS}, {"BumperLeft",DM_CTL_L}, {"BumperLeftRectangle",DM_CTL_L},
        {"BumperRight",DM_CTL_R}, {"BumperRightRectangle",DM_CTL_R}, {"TriggerLeft",DM_CTL_ZL}, {"TriggerRight",DM_CTL_ZR},
        {"ButtonStart",DM_CTL_PLUS}, {"ButtonSelect",DM_CTL_MINUS}, {"DPadLeft",DM_CTL_POTION_1}, {"DPadUp",DM_CTL_POTION_2}, {"DPadRight",DM_CTL_POTION_4}, {"DPadDown",DM_CTL_POTION_3}
    };
    int action = -1;
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) { size_t length = strlen(names[i].name); if (!strncasecmp(name, names[i].name, length) && (!name[length] || !strcasecmp(name + length, ".sprite"))) { action = names[i].action; break; } }
    if (atomic_load(&g_legacy_navigation) && action >= DM_CTL_POTION_1 && action <= DM_CTL_POTION_4) { static const int legacy[] = {DM_CTL_LEFT,DM_CTL_UP,DM_CTL_DOWN,DM_CTL_RIGHT}; action = legacy[action-DM_CTL_POTION_1]; }
    if (action < 0 && strcasecmp(name, "StickLeft") && strcasecmp(name, "StickLeft.sprite")) return original;
    static _Thread_local char path[144]; pthread_mutex_lock(&g_lock);
    if (!g_glyph_available) { pthread_mutex_unlock(&g_lock); return original; }
    snprintf(path, sizeof path, "%s%s", action < 0 ? "Ztouch_controller_buttons_ui/switchKMove" : g_glyph_paths[action], strcasestr(name, ".sprite") ? ".sprite" : ""); pthread_mutex_unlock(&g_lock);
    return path;
}

@interface DmControlsPanel : NSWindowController <NSTableViewDataSource, NSTableViewDelegate, NSWindowDelegate> {
@public int pending[DM_CTL_COUNT][2];
}
@property BOOL mouseOnly;
@property dm_controls_mouse_reserved_fn mouseReserved;
@property NSTableView *table;
@property NSTextField *status;
@property NSButton *captureCancel;
@property NSInteger captureRow, captureSlot;
@property id monitor;
@property (copy) void (^onApply)(void);
- (BOOL)captureInput:(int)input;
- (NSEvent *)captureEvent:(NSEvent *)event;
@end
@implementation DmControlsPanel
- (instancetype)init {return [self initMouseOnly:NO reserved:NULL];}
- (instancetype)initMouseOnly:(BOOL)mouse reserved:(dm_controls_mouse_reserved_fn)reserved {
    NSPanel *panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0,0,760,620) styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
    self = [super initWithWindow:panel]; if (!self) return nil;
    self.mouseOnly=mouse;self.mouseReserved=reserved;panel.title = mouse?@"Botones del ratón":@"Controles"; panel.delegate = self; panel.hidesOnDeactivate = NO; panel.accessibilityIdentifier = @"D2RControlsSheet"; self.captureRow = -1; dm_controls_get(pending);
    if(mouse){int values[2][2];dm_controls_mouse_get(values);for(int a=0;a<2;a++)for(int slot=0;slot<2;slot++)pending[DM_CTL_PRIMARY+a][slot]=values[a][slot];[panel setContentSize:NSMakeSize(760,320)];}
    NSView *content = panel.contentView;
    NSTextField *intro = [NSTextField wrappingLabelWithString:@"Perfil Resurrected: clic izquierdo para moverse, atacar e interactuar. Habilidades 1–6; pociones QWER; paneles I/C/T/L. Movimiento por teclado opcional, sin asignar de inicio.\nPulsa una asignación para cambiarla. Admite Mayús, Control y Opción; ⌘ se reserva para macOS."];
    if(mouse)intro.stringValue=@"Aquí solo se asignan los botones físicos para mover, atacar e interactuar y para la habilidad secundaria. Las 69 acciones, teclado y rueda se editan en Controles del juego.";
    intro.frame = NSMakeRect(20,mouse?242:542,720,58); [content addSubview:intro];
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(20,124,720,mouse?104:404)]; scroll.hasVerticalScroller = !mouse; scroll.hasHorizontalScroller = NO; scroll.horizontalScrollElasticity = NSScrollElasticityNone; scroll.borderType = NSBezelBorder;
    self.table = [[NSTableView alloc] initWithFrame:NSMakeRect(0,0,scroll.contentSize.width,scroll.contentSize.height)]; self.table.autoresizingMask=NSViewWidthSizable; self.table.columnAutoresizingStyle=NSTableViewLastColumnOnlyAutoresizingStyle; self.table.rowHeight = 36; self.table.usesAlternatingRowBackgroundColors = YES; self.table.delegate = self; self.table.dataSource = self;
    for (NSArray *spec in @[@[@"action",@"Acción",@340], @[@"primary",@"Principal",@180], @[@"secondary",@"Alternativa",@180]]) { NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:spec[0]]; column.title = spec[1]; column.width = [spec[2] doubleValue]; [self.table addTableColumn:column]; }
    scroll.documentView = self.table; [content addSubview:scroll]; [scroll tile]; [self.table sizeLastColumnToFit];
    self.status = [NSTextField wrappingLabelWithString:@"Los cambios se guardan al pulsar Aplicar. Para liberar una entrada, usa el menú contextual de su botón."]; self.status.frame = NSMakeRect(20,62,510,52); [content addSubview:self.status];
    if(mouse){int values[2][2];for(int a=0;a<2;a++)for(int slot=0;slot<2;slot++)values[a][slot]=pending[DM_CTL_PRIMARY+a][slot];char error[384];if(!dm_controls_mouse_validate(values,reserved,error,sizeof error))self.status.stringValue=@(error);}
    self.captureCancel = [NSButton buttonWithTitle:@"Cancelar captura" target:self action:@selector(cancelCapture:)]; self.captureCancel.frame = NSMakeRect(558,80,182,30); self.captureCancel.hidden = YES; [content addSubview:self.captureCancel];
    NSButton *defaults = [NSButton buttonWithTitle:@"Restaurar predeterminados" target:self action:@selector(defaults:)]; defaults.frame = NSMakeRect(20,20,222,30); [content addSubview:defaults];
    NSButton *cancel = [NSButton buttonWithTitle:@"Cancelar" target:self action:@selector(cancel:)]; cancel.keyEquivalent = @"\e"; cancel.frame = NSMakeRect(530,20,100,30); [content addSubview:cancel];
    NSButton *apply = [NSButton buttonWithTitle:@"Aplicar" target:self action:@selector(apply:)]; apply.frame = NSMakeRect(640,20,100,30); apply.keyEquivalent = @"\r"; [content addSubview:apply];
    __weak DmControlsPanel *weak = self;
    self.monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskFlagsChanged | NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown | NSEventMaskOtherMouseDown | NSEventMaskScrollWheel handler:^NSEvent *(NSEvent *event) {
        DmControlsPanel *strong = weak; return strong ? [strong captureEvent:event] : event;
    }];
    return self;
}
- (NSEvent *)captureEvent:(NSEvent *)event {
    if (event.modifierFlags & NSEventModifierFlagCommand) return event;
    if (event.window != self.window || self.captureRow < 0) return event;
    if(self.mouseOnly && event.type==NSEventTypeKeyDown && event.keyCode==53){[self cancelCapture:nil];return nil;}
        if (event.type == NSEventTypeLeftMouseDown) { NSPoint p = [self.window.contentView convertPoint:event.locationInWindow fromView:nil]; if (NSPointInRect(p, self.captureCancel.frame)) return event; }
        if (event.type == NSEventTypeFlagsChanged) {
            NSUInteger flag = event.keyCode == 56 || event.keyCode == 60 ? NSEventModifierFlagShift : event.keyCode == 59 || event.keyCode == 62 ? NSEventModifierFlagControl : event.keyCode == 58 || event.keyCode == 61 ? NSEventModifierFlagOption : 0;
            if (!flag) { self.status.stringValue = @"Esta tecla se reserva para macOS. Elige otra."; return nil; }
            if (dm_controls_modifier_down(event)) return nil;
        }
        [self captureInput:dm_controls_event_input(event)]; return nil;
}
- (BOOL)captureInput:(int)input {
    if (self.captureRow < 0) return NO;
    if (input < 0) { self.status.stringValue = @"Esta entrada se reserva para macOS. Elige otra."; return NO; }
    int candidate[DM_CTL_COUNT][2]; memcpy(candidate, pending, sizeof candidate); candidate[self.captureRow][self.captureSlot] = input;
    int mouse[2][2];for(int a=0;a<2;a++)for(int slot=0;slot<2;slot++)mouse[a][slot]=candidate[DM_CTL_PRIMARY+a][slot];
    char error[384]; if (!(self.mouseOnly?dm_controls_mouse_validate(mouse,self.mouseReserved,error,sizeof error):dm_controls_validate(candidate, error, sizeof error))) { self.status.stringValue = @(error); return NO; }
    memcpy(pending, candidate, sizeof candidate); [self cancelCapture:nil]; self.status.stringValue = @"Asignación preparada. Pulsa Aplicar para guardarla."; [self.table reloadData]; return YES;
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)table { (void)table; return self.mouseOnly?2:DM_CTL_COUNT; }
- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    (void)table; row = self.mouseOnly?DM_CTL_PRIMARY+row:dm_controls_display_action((int)row); if ([column.identifier isEqual:@"action"]) { NSTextField *label = [NSTextField labelWithString:@(k_actions[row])]; label.font = [NSFont systemFontOfSize:13]; return label; }
    int slot = [column.identifier isEqual:@"primary"] ? 0 : 1; char label[128]; dm_controls_label(pending[row][slot], label, sizeof label);
    NSButton *button = [NSButton buttonWithTitle:@(label) target:self action:@selector(capture:)]; button.tag = row * 2 + slot; button.font = [NSFont systemFontOfSize:13]; button.accessibilityLabel = [NSString stringWithFormat:@"%@, %@: %@", @(k_actions[row]), column.title, @(label)];
    NSMenu *menu = [NSMenu new]; NSMenuItem *clear = [menu addItemWithTitle:@"Quitar asignación" action:@selector(clear:) keyEquivalent:@""]; clear.target = self; clear.tag = button.tag; button.menu = menu;
    return button;
}
- (void)capture:(NSButton *)sender { self.captureRow = sender.tag / 2; self.captureSlot = sender.tag % 2; self.captureCancel.hidden = NO; self.status.stringValue = [NSString stringWithFormat:@"Asignando «%@». Pulsa una entrada o Cancela la captura.", @(k_actions[self.captureRow])]; }
- (void)cancelCapture:(id)sender { (void)sender; self.captureRow = -1; self.captureCancel.hidden = YES; }
- (void)clear:(NSMenuItem *)sender { [self cancelCapture:nil]; pending[sender.tag / 2][sender.tag % 2] = -1; [self.table reloadData]; }
- (void)defaults:(id)sender { (void)sender; [self cancelCapture:nil]; if(self.mouseOnly){pending[DM_CTL_PRIMARY][0]=256;pending[DM_CTL_PRIMARY][1]=-1;pending[DM_CTL_SECONDARY][0]=257;pending[DM_CTL_SECONDARY][1]=-1;}else dm_controls_defaults(pending); [self.table reloadData]; self.status.stringValue = @"Valores iniciales preparados. Pulsa Aplicar para guardarlos."; }
- (void)finish:(NSModalResponse)response {
    [self cancelCapture:nil];
    if (self.monitor) [NSEvent removeMonitor:self.monitor]; self.monitor = nil;
    NSWindow *parent = self.window.sheetParent;
    if (parent) { [parent endSheet:self.window returnCode:response]; [self.window orderOut:nil]; }
    else [self.window close];
}
- (void)cancel:(id)sender { (void)sender; [self finish:NSModalResponseCancel]; }
- (void)apply:(id)sender { (void)sender; int mouse[2][2];for(int a=0;a<2;a++)for(int slot=0;slot<2;slot++)mouse[a][slot]=pending[DM_CTL_PRIMARY+a][slot];char error[384]; if (!(self.mouseOnly?dm_controls_mouse_commit(mouse,self.mouseReserved,error,sizeof error):dm_controls_commit(pending, error, sizeof error))) { self.status.stringValue = @(error); return; } if (self.onApply) self.onApply(); [self finish:NSModalResponseOK]; }
- (BOOL)windowShouldClose:(NSWindow *)window { if (window.sheetParent) { [self finish:NSModalResponseCancel]; return NO; } return YES; }
- (void)windowWillClose:(NSNotification *)notification { (void)notification; if (self.monitor) [NSEvent removeMonitor:self.monitor]; self.monitor = nil; }
@end
void dm_controls_show(NSWindow *parent, void (^on_apply)(void)) {
    static DmControlsPanel *controller;
    if (controller.window.visible || controller.window.sheetParent) { [controller.window makeKeyWindow]; return; }
    controller = [DmControlsPanel new]; controller.onApply = on_apply;
    if (parent) [parent beginSheet:controller.window completionHandler:nil];
    else { [controller.window center]; [controller showWindow:nil]; }
}

void dm_controls_show_mouse(NSWindow *parent,dm_controls_mouse_reserved_fn reserved,void (^on_apply)(void)) {
    static DmControlsPanel *controller;
    if(controller.window.visible||controller.window.sheetParent){[controller.window makeKeyWindow];return;}
    controller=[[DmControlsPanel alloc]initMouseOnly:YES reserved:reserved];controller.onApply=on_apply;
    if(parent)[parent beginSheet:controller.window completionHandler:nil];else{[controller.window center];[controller showWindow:nil];}
}
