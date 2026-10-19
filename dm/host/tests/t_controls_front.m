#import "../front_mac.m"
#include <stdio.h>
static int live_ops[32],live_count;static float live_x[32],live_y[32];
static uint64_t aimed_buttons; static float aimed_x,aimed_y;
static int native_mouse_calls,native_mouse_button[32],native_mouse_phase[32],native_mouse_side[32],reserved_button=-1;
static int native_mouse_mods[32];static float native_mouse_x[32],native_mouse_y[32];
static int test_pc_desktop,pc_keys,pc_vk[128],pc_mods[128],pc_down[128],pc_pointer_valid;
int dm_ui_pc_desktop(void){return test_pc_desktop;}
int dm_ui_pc_key(int vk,int mods,int down){int i=pc_keys++;if(i<128){pc_vk[i]=vk;pc_mods[i]=mods;pc_down[i]=down;}return 1;}
static int profile_ready,controls_opened;
int dm_ui_pc_profile_ready(void){return profile_ready;}
int dm_ui_pc_controls(void){controls_opened++;return 1;}
static int native_controls_active;
int dm_ui_pc_controls_active(void){return native_controls_active;}
int dm_ui_pc_mouse_reserved(int button){return button==reserved_button;}
void dm_ui_ptr_at(int kind,float x,float y,int arg){(void)kind;(void)x;(void)y;(void)arg;}
void dm_ui_ptr_at_mods(int kind,float x,float y,int arg,int mods){(void)mods;dm_ui_ptr_at(kind,x,y,arg);}
void dm_ui_ptr(int kind,float x,float y,int arg){(void)kind;(void)x;(void)y;(void)arg;}
int dm_ui_pc_wheel(int steps,int mods,int momentum,float nx,float ny){(void)steps;(void)mods;(void)momentum;(void)nx;(void)ny;return 1;}
void dm_touch_host_pointer(float x,float y,int valid){(void)x;(void)y;pc_pointer_valid=valid;}
void dm_touch_host_pc_mouse_mods(int b,int phase,float x,float y,int side,int mods){int n=native_mouse_calls++;if(n<32){native_mouse_button[n]=b;native_mouse_phase[n]=phase;native_mouse_side[n]=side;native_mouse_mods[n]=mods;native_mouse_x[n]=x;native_mouse_y[n]=y;}}
void dm_touch_host_pc_mouse(int b,int phase,float x,float y,int side){dm_touch_host_pc_mouse_mods(b,phase,x,y,side,0);}
static uint64_t actual_buttons; static int actual_x,actual_y,checks,failures;
void mtl_set_ui_rect_fn(void (*fn)(double*,double*)){(void)fn;}
void dm_input_set(uint64_t buttons,int32_t lx,int32_t ly,int32_t rx,int32_t ry){actual_buttons=buttons;actual_x=lx;actual_y=ly;(void)rx;(void)ry;}
void dm_touch_host_aim(uint64_t skills,float x,float y){aimed_buttons=skills;aimed_x=x;aimed_y=y;}
void dm_touch_host_live(int op,float x,float y){if(live_count<32){live_ops[live_count]=op;live_x[live_count]=x;live_y[live_count]=y;live_count++;}}
void dm_padmode_request_kbd(int value){(void)value;}
void dm_ui_pc_bindings_cancel(void){}
#define CHECK(c,t) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}else printf("PASS %s\n",t);}while(0)
static NSEvent *key(int code,int down,NSUInteger mods){return [NSEvent keyEventWithType:down?NSEventTypeKeyDown:NSEventTypeKeyUp location:NSZeroPoint modifierFlags:mods timestamp:0 windowNumber:0 context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:code];}
@interface TestPCMouse:NSEvent
@property NSInteger testButton;@property NSEventType testType;
@property NSEventModifierFlags testMods;@property NSPoint testPoint;@property BOOL hasPoint;
@end
@implementation TestPCMouse
- (NSInteger)buttonNumber{return self.testButton;}
- (NSEventType)type{return self.testType;}
- (NSEventModifierFlags)modifierFlags{return self.testMods;}
- (NSPoint)locationInWindow{return self.hasPoint?self.testPoint:NSMakePoint(400,300);}
@end
static int unavailable_notices;
@interface TestControlsController:DmWinCtl @end
@implementation TestControlsController
- (void)controlsUnavailable {unavailable_notices++;}
@end
int main(int argc,char**argv){if(argc!=2)return 2;@autoreleasepool{dm_controls_init(argv[1]);g_live=1;controls_release();
 g_mouse_have=1;g_mouse_nx=.8f;g_mouse_ny=.2f;
 int optional[DM_CTL_COUNT][2];char optional_error[256];dm_controls_get(optional);optional[DM_CTL_A][1]=49;optional[DM_CTL_MOVE_UP][0]=0;CHECK(dm_controls_commit(optional,optional_error,sizeof optional_error),"Movimiento por teclado y alternativa Espacio se habilitan solo al reasignarlos");
 on_key(key(18,1,0),1);CHECK((aimed_buttons&DM_BTN_A)&&aimed_x==.8f&&aimed_y==.2f,"Tecla numérica publica última posición de cursor antes de botones");on_key(key(49,1,0),1);CHECK(actual_buttons&DM_BTN_A,"Dos entradas activan misma acción");on_key(key(18,0,0),0);CHECK(actual_buttons&DM_BTN_A,"Soltar una alternativa conserva otra pulsada");on_key(key(49,0,0),0);CHECK(!actual_buttons,"Última alternativa libera acción");
 on_key(key(13,1,0),1);CHECK(!actual_y && actual_buttons==DM_MAC_POTION_2,"W publica la poción 2 sin movimiento WASD");on_key(key(13,0,0),0);
 on_key(key(0,1,0),1);CHECK(actual_y==32767,"Movimiento opcional reasignado a A produce eje original arriba");on_key(key(0,0,NSEventModifierFlagCommand),0);CHECK(!actual_y,"Soltar con Comando no deja movimiento pulsado");
 int bindings[DM_CTL_COUNT][2];char error[256];dm_controls_get(bindings);bindings[DM_CTL_X][0]=35;CHECK(dm_controls_commit(bindings,error,sizeof error),"Reasignación real aplicada");on_key(key(21,1,0),1);CHECK(!actual_buttons,"Tecla anterior 4 no produce habilidad tras reasignar");on_key(key(35,1,0),1);CHECK(actual_buttons&DM_BTN_X,"P produce bit habilidad X original");controls_release();CHECK(!actual_buttons&&!actual_x&&!actual_y,"Pérdida de foco libera botones y ejes");on_key(key(35,0,0),0);CHECK(!actual_buttons,"Release tardío tras cambiar foco es inocuo");
 on_key(key(36,1,0),1);CHECK(actual_buttons==(1ull<<40),"Retorno conserva confirmación contextual Mac");controls_release();g_live=0;on_key(key(36,1,0),1);CHECK(actual_buttons==DM_BTN_PLUS,"Modo CLI conserva Retorno como Plus upstream");controls_release();
 g_live=1;g_view=[[NSView alloc]initWithFrame:NSMakeRect(0,0,1280,720)];controls_release();live_count=0;
 NSEvent *down=[NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:NSMakePoint(896,432) modifierFlags:0 timestamp:0 windowNumber:0 context:nil eventNumber:0 clickCount:1 pressure:1];
 NSEvent *up=[NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:NSMakePoint(512,432) modifierFlags:0 timestamp:0 windowNumber:0 context:nil eventNumber:0 clickCount:1 pressure:0];
 control_action(DM_CTL_PRIMARY,1,down);control_action(DM_CTL_PRIMARY,0,up);
 CHECK(live_count==3 && live_ops[0]==0 && live_ops[1]==1 && live_ops[2]==2,"Release PC encola MOVE final antes de UP sin eventos intermedios");
 CHECK(live_count>=2 && fabsf(live_x[1]-.4f)<.0001f && fabsf(live_y[1]-.4f)<.0001f,"MOVE final conserva destino real del evento mouseUp");
 live_count=0;controls_release();CHECK(live_count==2 && live_ops[0]==2 && live_ops[1]==5,"Cancelar foco no inventa movimiento final ni arrastre");
 test_pc_desktop=1;controls_release();g_mouse_have=1;g_mouse_nx=.6f;g_mouse_ny=.4f;pc_keys=0;
 CHECK(on_pc_key(key(18,1,0),1)&&pc_keys==1&&pc_vk[0]==0x31&&pc_down[0]&&!actual_buttons&&pc_pointer_valid,"PC69 publica VK1 original y cursor sin máscara del perfil Mac35");
 NSEvent *repeat=[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil characters:@"1" charactersIgnoringModifiers:@"1" isARepeat:YES keyCode:18];on_pc_key(repeat,1);CHECK(pc_keys==2&&pc_vk[1]==0x31&&pc_down[1]&&g_pc_vk_count[0x31]==1,"Repeat alcanza editor nativo sin acumular otra pulsación lógica");
 on_pc_key(key(18,0,NSEventModifierFlagShift),0);CHECK(pc_keys==3&&pc_vk[2]==0x31&&!pc_down[2],"UP conserva VK latched aunque cambien modificadores");
 pc_keys=0;on_pc_key(key(56,1,NSEventModifierFlagShift),1);on_pc_key(key(60,1,NSEventModifierFlagShift),1);on_pc_key(key(56,0,NSEventModifierFlagShift),0);CHECK(pc_keys==1&&pc_vk[0]==0x10&&pc_down[0],"Dos Mayús físicas mantienen un solo VK hasta liberar la última");on_pc_key(key(60,0,0),0);CHECK(pc_keys==2&&!pc_down[1],"Última Mayús libera el VK original");
 CHECK(dm_controls_virtual_modifiers(key(0,1,NSEventModifierFlagShift|NSEventModifierFlagControl|NSEventModifierFlagOption))==0x7000,"Máscara original Shift/Ctrl/Alt conserva sus tres bits");
 CHECK(dm_controls_virtual_key(key(122,1,0))==0x70&&dm_controls_virtual_key(key(123,1,0))==0x25&&dm_controls_virtual_key(key(83,1,0))==0x61,"F1, flechas y teclado numérico usan VK oficiales");
 int before=pc_keys;CHECK(!on_pc_key(key(12,1,NSEventModifierFlagCommand),1)&&pc_keys==before,"Comando pertenece a macOS y nunca entra al dispatcher PC");controls_release();CHECK(!pc_pointer_valid,"Perder foco invalida también cursor QuickCast sin gesto inventado");int pcMouse[2][2]={{259,-1},{257,-1}};char mouseError[384];CHECK(dm_controls_mouse_commit(pcMouse,NULL,mouseError,sizeof mouseError),"Extensión PC prepara botón físico4 sin alterar tabla de teclado");
 TestPCMouse *extra=[TestPCMouse new];extra.testButton=3;extra.testType=NSEventTypeOtherMouseDown;native_mouse_calls=0;pc_keys=0;pc_mouse_button(extra,1);extra.testType=NSEventTypeOtherMouseUp;pc_mouse_button(extra,0);
 CHECK(native_mouse_calls==3&&native_mouse_button[0]==3&&native_mouse_side[0]==0&&native_mouse_phase[0]==0&&native_mouse_phase[1]==1&&native_mouse_phase[2]==2&&!pc_keys,"Ratón reasignado emite MOVE/UP exactos y ninguna segunda ruta native69");
 reserved_button=3;extra.testType=NSEventTypeOtherMouseDown;native_mouse_calls=pc_keys=0;pc_mouse_button(extra,1);extra.testType=NSEventTypeOtherMouseUp;pc_mouse_button(extra,0);
 CHECK(!native_mouse_calls&&pc_keys==2&&pc_vk[0]==0x101&&pc_down[0]&&!pc_down[1],"Conflicto con native69 impide activar simultáneamente extensión de ratón");reserved_button=-1;
 native_controls_active=1;extra.testType=NSEventTypeOtherMouseDown;native_mouse_calls=pc_keys=0;pc_mouse_button(extra,1);extra.testType=NSEventTypeOtherMouseUp;pc_mouse_button(extra,0);
 CHECK(!native_mouse_calls&&pc_keys==2&&pc_vk[0]==0x101,"Captura nativa recibe botón incluso si extensión Mac lo tiene asignado");native_controls_active=0;
 TestPCMouse *middle=[TestPCMouse new];middle.testButton=2;middle.testType=NSEventTypeOtherMouseDown;native_mouse_calls=pc_keys=0;pc_mouse_button(middle,1);pc_pointer_exit();
 CHECK(pc_keys==2&&pc_vk[0]==0x100&&pc_down[0]&&pc_vk[1]==0x100&&!pc_down[1]&&!native_mouse_calls,"Salir del área libera por VK el botón enviado a native69");middle.testType=NSEventTypeOtherMouseUp;pc_mouse_button(middle,0);CHECK(pc_keys==2,"MouseUp tardío no duplica liberación nativa");
 native_controls_active=1;extra.testType=NSEventTypeOtherMouseDown;native_mouse_calls=pc_keys=0;pc_mouse_button(extra,1);native_controls_active=0;pc_pointer_exit();
 CHECK(pc_keys==2&&pc_vk[1]==0x101&&!pc_down[1]&&!native_mouse_calls,"Liberación conserva ruta de captura original aunque cierre el editor");
 reserved_button=3;extra.testType=NSEventTypeOtherMouseDown;native_mouse_calls=pc_keys=0;pc_mouse_button(extra,1);reserved_button=-1;controls_release();
 CHECK(pc_keys==2&&pc_vk[1]==0x101&&!pc_down[1]&&!native_mouse_calls,"Pérdida de foco libera VK reservado por su ruta DOWN");
 extra.testType=NSEventTypeOtherMouseDown;native_mouse_calls=pc_keys=0;pc_mouse_button(extra,1);pc_pointer_exit();
 CHECK(!pc_keys&&native_mouse_calls==2&&native_mouse_phase[1]==2,"Botón de extensión conserva su liberación por mundo sin VK ajeno");
 TestPCMouse *left=[TestPCMouse new];left.testButton=0;left.testType=NSEventTypeLeftMouseDown;native_mouse_calls=0;pc_mouse_button(left,1);CHECK(native_mouse_calls==1&&native_mouse_side[0]==-1,"Izquierdo reasignado conserva UI física sin acción de mundo duplicada");left.testType=NSEventTypeLeftMouseUp;pc_mouse_button(left,0);g_mouse_have=1;g_pc_mouse_down[4]=1;g_pc_mouse_side[4]=0;native_mouse_calls=0;pc_pointer_exit();CHECK(!g_mouse_have&&!pc_pointer_valid&&native_mouse_calls==1&&native_mouse_phase[0]==2&&!g_pc_mouse_down[4],"Salir del frame invalida hover y suelta ratón sin comando nuevo");controls_release();test_pc_desktop=0;


 test_pc_desktop=1;TestPCMouse *modified=[TestPCMouse new];modified.testButton=0;modified.testType=NSEventTypeLeftMouseDown;modified.testMods=NSEventModifierFlagShift;modified.hasPoint=YES;modified.testPoint=NSMakePoint(300,300);native_mouse_calls=0;pc_mouse_button(modified,1);
 modified.testType=NSEventTypeLeftMouseUp;modified.testMods=NSEventModifierFlagControl|NSEventModifierFlagOption;modified.testPoint=NSMakePoint(700,400);pc_mouse_button(modified,0);
 CHECK(native_mouse_calls==3&&native_mouse_mods[0]==0x1000&&native_mouse_mods[1]==0x6000&&native_mouse_mods[2]==0x6000,"DOWN, posición final y UP guardan modificadores propios del evento físico");
 CHECK(native_mouse_x[0]!=native_mouse_x[2]&&native_mouse_y[0]!=native_mouse_y[2]&&native_mouse_x[1]==native_mouse_x[2],"Cada snapshot conserva sus coordenadas junto a los modificadores");
 modified.testType=NSEventTypeLeftMouseDown;modified.testMods=NSEventModifierFlagShift;native_mouse_calls=0;pc_mouse_button(modified,1);controls_release();CHECK(native_mouse_calls==2&&native_mouse_phase[1]==2&&!native_mouse_mods[1],"Cancelar foco libera sin heredar modificadores de un clic previo");
 modified.testType=NSEventTypeLeftMouseDown;modified.testMods=NSEventModifierFlagShift;native_mouse_calls=0;pc_mouse_button(modified,1);modified.testType=NSEventTypeLeftMouseDragged;modified.testMods=NSEventModifierFlagControl;modified.testPoint=NSMakePoint(500,350);on_pc_mouse_event(modified);modified.testType=NSEventTypeLeftMouseUp;modified.testMods=NSEventModifierFlagOption;pc_mouse_button(modified,0);
 CHECK(native_mouse_calls==4&&native_mouse_mods[0]==0x1000&&native_mouse_mods[1]==0x2000&&native_mouse_mods[2]==0x4000&&native_mouse_mods[3]==0x4000,"Arrastre físico actualiza modificadores sin alterar los snapshots de DOWN y UP");
 modified.testType=NSEventTypeLeftMouseDown;modified.testMods=NSEventModifierFlagCommand;native_mouse_calls=0;CHECK(on_pc_mouse_event(modified)==modified&&!native_mouse_calls,"Comando con ratón permanece en macOS y no inyecta modificadores al juego");
 g_live=1;profile_ready=0;controls_opened=unavailable_notices=0;TestControlsController *controller=[TestControlsController new];[controller controls:nil];
 CHECK(!controls_opened&&unavailable_notices==1,"Controles del juego fuera de partida muestra aviso sin abrir editor");
 profile_ready=1;[controller controls:nil];CHECK(controls_opened==1&&unavailable_notices==1,"Personaje cargado conserva apertura nativa de controles");
 profile_ready=0;[controller controls:nil];CHECK(controls_opened==1&&unavailable_notices==2,"Volver al frontend bloquea de nuevo el editor");test_pc_desktop=0;
 test_pc_desktop=0;NSMenu *menu=controls_menu(controller);CHECK(menu.numberOfItems==1&&[menu.itemArray.firstObject.title isEqualToString:@"Controles del juego…"],"Menú se construye antes de que el guest publique PC");
 test_pc_desktop=1;[menu.delegate menuNeedsUpdate:menu];NSInteger mouseIndex=[menu indexOfItemWithTarget:controller andAction:@selector(mouseControls:)];CHECK(menu.numberOfItems==2&&mouseIndex>=0,"PC publicado después del arranque hace accesible Botones del ratón al abrir menú");
 if(mouseIndex>=0){NSMenuItem *mouse=[menu itemAtIndex:mouseIndex];CHECK(mouse.target==controller&&mouse.action==@selector(mouseControls:)&&[mouse.title isEqualToString:@"Botones del ratón…"],"Entrada de ratón conserva target y acción de la hoja existente");}
 [menu.delegate menuNeedsUpdate:menu];CHECK(menu.numberOfItems==2,"Reabrir menú no duplica controles");test_pc_desktop=0;[menu.delegate menuNeedsUpdate:menu];CHECK(menu.numberOfItems==1,"Volver a modo legacy conserva un solo editor funcional");
 }printf("CONTROLS FRONT %d comprobaciones, %d fallos\n",checks,failures);return failures?1:0;}
