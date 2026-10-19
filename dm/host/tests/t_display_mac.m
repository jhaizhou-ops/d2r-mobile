#import "../front_mac.m"
int dm_ui_pc_desktop(void){return 0;}
void dm_touch_host_pointer(float x,float y,int valid){(void)x;(void)y;(void)valid;}
void dm_touch_host_pc_mouse(int b,int p,float x,float y,int s){(void)b;(void)p;(void)x;(void)y;(void)s;}
static int checks,failures,res_changes; static uint64_t notified_resolution;
static int present_discarded,present_policy_calls;
void mtl_present_discard(int discard){present_discarded=discard;present_policy_calls++;}
extern unsigned test_cursor_scroll_count(void);
extern int test_cursor_scroll_arg(void);
extern uint64_t test_cursor_position(void);
void mtl_set_ui_rect_fn(void (*fn)(double*,double*)){(void)fn;}
void dm_oe_res_changed(void){res_changes++;notified_resolution=atomic_load(&g_render_size);}
void dm_input_set(uint64_t b,int32_t x,int32_t y,int32_t rx,int32_t ry){(void)b;(void)x;(void)y;(void)rx;(void)ry;}
void dm_touch_host_aim(uint64_t b,float x,float y){(void)b;(void)x;(void)y;}
void dm_touch_host_live(int op,float x,float y){(void)op;(void)x;(void)y;}
void dm_padmode_request_kbd(int value){(void)value;}
int dm_screen_real(int*w,int*h){*w=1920;*h=1080;return 1;}
void dm_ui_rect(int*x,int*y,int*w,int*h){*x=*y=0;*w=1920;*h=1080;}
#define CHECK(c,t) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}else printf("PASS %s\n",t);}while(0)
@interface TestDisplayScreen:NSObject @end
@implementation TestDisplayScreen
- (NSRect)frame{return NSMakeRect(0,0,1512,982);}
- (CGFloat)backingScaleFactor{return 2;}
@end
@interface TestDisplayWindow:NSWindow
@property BOOL testZoomed,testFull;
@property int fullTransitions;
@property CGFloat testScale;
@property BOOL testVisible,testKey,testMini;
@property NSWindowOcclusionState testOcclusion;
@end
@implementation TestDisplayWindow
- (BOOL)isZoomed{return self.testZoomed;}
- (void)zoom:(id)s{(void)s;self.testZoomed=!self.testZoomed;}
- (NSWindowStyleMask)styleMask{return [super styleMask] | (self.testFull?NSWindowStyleMaskFullScreen:0);}
- (void)toggleFullScreen:(id)s{(void)s;self.fullTransitions++;self.testFull=!self.testFull;}
- (CGFloat)backingScaleFactor{return self.testScale ?: 2;}
- (BOOL)isVisible{return self.testVisible;}
- (BOOL)isKeyWindow{return self.testKey;}
- (BOOL)isMiniaturized{return self.testMini;}
- (NSWindowOcclusionState)occlusionState{return self.testOcclusion;}
@end
@interface TestScroll:NSEvent
@property CGFloat testDX,testDY;
@property BOOL testPrecise;
@property NSEventPhase testMomentum,testPhase;
@end
@implementation TestScroll
- (NSEventType)type{return NSEventTypeScrollWheel;}
- (CGFloat)scrollingDeltaX{return self.testDX;}
- (CGFloat)scrollingDeltaY{return self.testDY;}
- (BOOL)hasPreciseScrollingDeltas{return self.testPrecise;}
- (NSEventPhase)momentumPhase{return self.testMomentum;}
- (NSEventPhase)phase{return self.testPhase;}
- (NSTimeInterval)timestamp{return 1;}
- (NSEventModifierFlags)modifierFlags{return 0;}
- (NSPoint)locationInWindow{return NSMakePoint(640,360);}
@end
int main(int argc,char**argv){if(argc!=2 && argc!=4)return 2;@autoreleasepool{
 [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
 if(argc==4){
  g_display_prefs=[[NSUserDefaults alloc]initWithSuiteName:@(argv[1])];
  TestDisplayWindow *cold=[[TestDisplayWindow alloc]initWithContentRect:NSMakeRect(0,0,1280,720) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];cold.alphaValue=0;g_win=cold;
  [cold setFrame:NSScreen.mainScreen.visibleFrame display:NO];g_windowed_frame=NSZeroRect;display_load_window_frame(YES);NSInteger mode=[g_display_prefs integerForKey:@"DisplayWindowMode"];display_window_mode((int)mode);
  NSRect expected=display_bound_frame(NSRectFromString(@(argv[2])),NSScreen.mainScreen.visibleFrame);
  BOOL ok=mode==atoi(argv[3]) && NSEqualRects(g_windowed_frame,expected) && (mode==1?cold.zoomed:NSEqualRects(cold.frame,expected));printf("COLD WINDOW %s mode=%ld\n",ok?"PASS":"FAIL",(long)mode);return ok?0:1;
 }
 NSString *suite=[NSString stringWithFormat:@"org.d2r.tests.display.%@",NSUUID.UUID.UUIDString];g_display_prefs=[[NSUserDefaults alloc]initWithSuiteName:suite];
 TestDisplayScreen *screen=[TestDisplayScreen new];g_aspect=1512.0/(982-37);display_load((NSScreen*)screen);
 CHECK(g_resolution_choice==0 && g_render_cap.width==3024 && g_render_cap.height==1890,"Configuración nueva usa Retina de pantalla al arrancar sin reservar 4K");
 CHECK((uint32_t)front_game_res(1)==3024 && (front_game_res(1)>>32)==1890,"Juego recibe resolución Retina nativa");
 CHECK(display_size(99999,16.0/9).width==4096 && display_size(4096,.5).height==4096,"Resoluciones limitan ambas dimensiones a 4096");
 CHECK(display_resolution(1920) && res_changes==1 && (uint32_t)notified_resolution==1920,"Bajar resolución publica tamaño antes del aviso original");
 CHECK(display_resolution(2560) && res_changes==2,"Subir dentro de capacidad inicial se aplica durante la partida");
 CHECK(!display_resolution(3840) && res_changes==2 && (uint32_t)front_game_res(1)==2560 && display_pending(),"Superar capacidad guarda preferencia sin alterar render activo");
 CHECK([g_display_prefs integerForKey:@"DisplayRenderWidth"]==3840,"Resolución pendiente persiste sin cerrar juego");
 display_load((NSScreen*)screen);CHECK(g_render_cap.width==3840 && !display_pending(),"Reabrir aplica resolución previamente guardada");
 [g_display_prefs setInteger:123 forKey:@"DisplayRenderWidth"];display_load((NSScreen*)screen);CHECK(g_resolution_choice==0,"Preferencia desconocida vuelve a Automática");
 TestDisplayWindow *win=[[TestDisplayWindow alloc]initWithContentRect:NSMakeRect(0,0,1280,720) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];win.alphaValue=0;g_win=win;g_view=win.contentView;
 DmWinCtl *ctl=[DmWinCtl new];NSMenu *menu=display_menu(ctl);CHECK(menu.numberOfItems==6 && menu.itemArray[4].submenu.numberOfItems==7,"Visualización reutiliza un menú con tres estados y siete resoluciones");
 NSRect normal=NSMakeRect(100,140,1000,650),maximized=NSScreen.mainScreen.visibleFrame;
 g_windowed_frame=normal;[win setFrame:maximized display:NO];display_window_mode(0);
 CHECK(NSEqualRects(win.frame,normal),"Ventana restaura tamaño normal aunque autosave máximo no sea isZoomed");
 [win setFrame:maximized display:NO];win.testZoomed=YES;display_window_mode(0);
 CHECK(!win.zoomed && NSEqualRects(win.frame,normal),"Ventana impone tamaño normal después del zoom nativo");
 [win setFrame:maximized display:NO];win.testFull=YES;int exits=win.fullTransitions;display_window_mode(0);
 CHECK(win.fullTransitions==exits+1 && NSEqualRects(win.frame,maximized),"Ventana espera la salida nativa de pantalla completa antes de restaurar frame");
 [ctl windowDidExitFullScreen:[NSNotification notificationWithName:NSWindowDidExitFullScreenNotification object:win]];
 CHECK(NSEqualRects(win.frame,normal),"Salida diferida de pantalla completa restaura el frame normal guardado");
 win.fullTransitions=0;
 [g_display_prefs removeObjectForKey:@"DisplayNormalFrame"];g_windowed_frame=NSZeroRect;[win setFrame:maximized display:NO];display_load_window_frame(YES);display_window_mode(0);
 NSRect initial=win.frame;
 CHECK(initial.size.width==fmin(1280,maximized.size.width) && !NSEqualRects(initial,maximized) && NSContainsRect(maximized,initial),"Frío con autosave máximo y sin zoom recupera ventana inicial acotada");
 [win setFrame:normal display:NO];[ctl windowDidEndLiveResize:[NSNotification notificationWithName:NSWindowDidEndLiveResizeNotification object:win]];
 CHECK(NSRectFromString([g_display_prefs stringForKey:@"DisplayNormalFrame"]).size.width==normal.size.width && NSEqualRects(g_windowed_frame,normal),"Redimensionado normal persiste geometría independiente de autosave");
 [g_display_prefs synchronize];NSTask *cold=[NSTask new];cold.executableURL=[NSURL fileURLWithPath:@(argv[0])];cold.arguments=@[suite,NSStringFromRect(normal),@"0"];NSError *coldError=nil;BOOL launched=[cold launchAndReturnError:&coldError];if(launched)[cold waitUntilExit];
 CHECK(launched && cold.terminationStatus==0,"Proceso frío restaura geometría normal persistida aunque autosave arranque máximo");
 [win setFrame:NSOffsetRect(normal,25,15) display:NO];[ctl windowDidMove:[NSNotification notificationWithName:NSWindowDidMoveNotification object:win]];
 NSRect moved=win.frame;CHECK(NSEqualRects(g_windowed_frame,moved),"Mover una ventana normal conserva su posición");
 [win setFrame:maximized display:NO];[ctl windowDidEndLiveResize:[NSNotification notificationWithName:NSWindowDidEndLiveResizeNotification object:win]];[ctl windowDidMove:[NSNotification notificationWithName:NSWindowDidMoveNotification object:win]];
 CHECK(NSEqualRects(g_windowed_frame,moved),"Autosave máximo sin isZoomed no sustituye el tamaño normal");
 [win setFrame:normal display:NO];win.testZoomed=YES;[ctl windowDidMove:[NSNotification notificationWithName:NSWindowDidMoveNotification object:win]];win.testZoomed=NO;
 CHECK(NSEqualRects(g_windowed_frame,moved),"Mover en estado zoom no sustituye el tamaño normal");
 win.testFull=YES;[ctl windowDidMove:[NSNotification notificationWithName:NSWindowDidMoveNotification object:win]];win.testFull=NO;g_window_transition=1;[ctl windowDidMove:[NSNotification notificationWithName:NSWindowDidMoveNotification object:win]];g_window_transition=0;
 CHECK(NSEqualRects(g_windowed_frame,moved),"Pantalla completa y sus transiciones preservan el frame normal");
 [g_display_prefs setObject:@[@1,@2] forKey:@"DisplayNormalFrame"];display_load_window_frame(NO);
 CHECK(NSEqualRects(g_windowed_frame,initial),"Preferencia con tipo inválido vuelve al respaldo inicial");
 [g_display_prefs setObject:@"{{nan, 0}, {1200, 700}}" forKey:@"DisplayNormalFrame"];display_load_window_frame(NO);
 CHECK(NSEqualRects(g_windowed_frame,initial),"Preferencia con coordenadas inválidas no llega a AppKit");
 [g_display_prefs setObject:NSStringFromRect(NSMakeRect(-20000,30000,900,600)) forKey:@"DisplayNormalFrame"];display_load_window_frame(NO);
 CHECK(NSContainsRect(maximized,g_windowed_frame) && g_windowed_frame.size.width==900,"Pantalla desconectada recoloca el tamaño normal dentro de la pantalla actual");
 NSRect small=display_bound_frame(normal,NSMakeRect(-900,20,800,600)),minimum=display_bound_frame(NSMakeRect(-500,-500,1,1),maximized);
 CHECK(NSContainsRect(NSMakeRect(-900,20,800,600),small) && [win contentRectForFrameRect:small].size.width>=640 && [win contentRectForFrameRect:small].size.height>=360,"Pantalla menor acota el frame manteniendo el mínimo del juego");
 CHECK([win contentRectForFrameRect:minimum].size.width==640 && [win contentRectForFrameRect:minimum].size.height==360,"Acotado aplica mínimo de contenido 640 por 360");
 [g_display_prefs removeObjectForKey:@"DisplayNormalFrame"];[win setFrame:normal display:NO];display_load_window_frame(YES);
 CHECK(NSEqualRects(g_windowed_frame,normal),"Primera migración conserva autosave normal válido");
 [win setFrame:maximized display:NO];display_window_mode(2);
 CHECK(win.testFull && NSEqualRects(g_windowed_frame,normal),"Entrar a pantalla completa desde autosave máximo conserva tamaño normal");
 display_window_mode(0);[ctl windowDidExitFullScreen:[NSNotification notificationWithName:NSWindowDidExitFullScreenNotification object:win]];win.fullTransitions=0;
 [win setFrame:NSOffsetRect(normal,10,10) display:NO];[ctl windowWillEnterFullScreen:[NSNotification notificationWithName:NSWindowWillEnterFullScreenNotification object:win]];
 CHECK(g_window_transition && NSEqualRects(g_windowed_frame,win.frame),"Entrada nativa a pantalla completa conserva tamaño normal antes de la transición");
 g_window_transition=0;normal=win.frame;
 NSWindow *native=[[NSWindow alloc]initWithContentRect:NSMakeRect(0,0,1280,720) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];native.alphaValue=0;g_win=native;g_view=native.contentView;
 [native setFrame:maximized display:NO];g_windowed_frame=normal;native.delegate=ctl;display_window_mode(0);
 CHECK(!native.zoomed && NSEqualRects(native.frame,normal),"NSWindow real restaura frame propio después de deshacer zoom de autosave");
 [native zoom:nil];CHECK(native.zoomed,"Zoom nativo real conserva maximización del sistema");
 CHECK([g_display_prefs integerForKey:@"DisplayWindowMode"]==1,"Zoom nativo persiste modo maximizado sin depender de EndLiveResize");
 [g_display_prefs synchronize];NSTask *maxCold=[NSTask new];maxCold.executableURL=[NSURL fileURLWithPath:@(argv[0])];maxCold.arguments=@[suite,NSStringFromRect(normal),@"1"];BOOL maxLaunched=[maxCold launchAndReturnError:&coldError];if(maxLaunched)[maxCold waitUntilExit];
 CHECK(maxLaunched && maxCold.terminationStatus==0,"Proceso frío posterior al zoom nativo conserva modo maximizado y frame normal");
 [native zoom:nil];NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:1];while(g_window_transition && deadline.timeIntervalSinceNow>0)[NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:deadline];
 CHECK(!g_window_transition && !native.zoomed && NSEqualRects(native.frame,normal),"Desmaximizar nativo real usa tamaño normal propio sin heredar frame casi máximo");
 CHECK([g_display_prefs integerForKey:@"DisplayWindowMode"]==0,"Desmaximizar nativo persiste modo ventana tras restauración diferida");
 native.delegate=nil;[native orderOut:nil];g_win=win;g_view=win.contentView;g_windowed_frame=normal;
 display_window_mode(1);CHECK(win.zoomed && [g_display_prefs integerForKey:@"DisplayWindowMode"]==1,"Maximizar conserva modo sin entrar a un Space");
 display_window_mode(0);CHECK(!win.zoomed,"Ventana restaura desde maximización");
 display_window_mode(2);CHECK(win.testFull && win.fullTransitions==1,"Pantalla completa usa toggle nativo");
 display_window_mode(1);[ctl windowDidExitFullScreen:[NSNotification notificationWithName:NSWindowDidExitFullScreenNotification object:win]];CHECK(!win.testFull && win.zoomed && win.fullTransitions==2,"Cambiar de pantalla completa a maximizada espera salida nativa");
 NSMenuItem *item=menu.itemArray[1];CHECK([ctl validateMenuItem:item] && item.state==NSControlStateValueOn,"Menú marca modo maximizado efectivo");
 setenv("DM_GAME_RES","1600x900",1);CHECK(![ctl validateMenuItem:menu.itemArray[4].submenu.itemArray[0]],"Override de diagnóstico deshabilita resolución persistente");unsetenv("DM_GAME_RES");
 g_aspect=16.0/9;[g_view setFrameSize:NSMakeSize(1000,900)];NSRect rect=game_rect();CHECK(rect.size.width==1000 && rect.size.height==563 && rect.origin.y==168,"Ventana libre conserva aspecto por bandas sin bloquear redimensionado");
 g_layer=[CAMetalLayer layer];layout_game();CHECK(g_layer.drawableSize.width==2000 && g_layer.drawableSize.height==1126,"Drawable Retina usa píxeles físicos de la ventana");win.testScale=1;layout_game();CHECK(g_layer.drawableSize.width==1000 && g_layer.drawableSize.height==563,"Cambiar pantalla actualiza backing aunque rectángulo lógico sea idéntico");
 g_wheel_remainder=0;CHECK(wheel_steps(0,8,YES,NSEventPhaseBegan,1)==0 && wheel_steps(0,8,YES,NSEventPhaseChanged,1.01)==0 && wheel_steps(0,16,YES,NSEventPhaseChanged,1.02)==1,"Trackpad acumula desplazamientos precisos hasta un paso");
 CHECK(wheel_steps(0,32000,YES,NSEventPhaseChanged,1.03)==3 && g_wheel_remainder==0,"Delta extremo se limita a tres pasos sin acumular saltos pendientes");
 CHECK(wheel_steps(0,-1,NO,NSEventPhaseNone,2)==-1,"Rueda convencional conserva un paso y dirección");
 CHECK(wheel_steps(80,1,YES,NSEventPhaseBegan,3)==0,"Desplazamiento horizontal no genera rueda vertical accidental");
 wheel_steps(0,24,YES,NSEventPhaseBegan,4);CHECK(wheel_steps(0,-16,YES,NSEventPhaseChanged,4.01)==0,"Cambiar dirección elimina resto previo");
 CHECK(wheel_steps(0,32,YES,NSEventPhaseCancelled,4.02)==0 && g_wheel_remainder==0,"Cancelar gesto elimina desplazamiento pendiente");
 dm_controls_init(argv[1]);g_view=[[NSView alloc]initWithFrame:NSMakeRect(0,0,1280,720)];g_full=0;TestScroll *event=[TestScroll new];event.testDY=1;on_scroll(event);CHECK(test_cursor_scroll_count()==1 && test_cursor_scroll_arg()==120,"Rueda se encola como MouseScroll original sin eje de habilidades");
 uint64_t xy=test_cursor_position();CHECK(fabs(((xy>>16)&65535)/65535.0-.5)<.00002 && fabs((xy&65535)/65535.0-.5)<.00002,"Rueda actualiza posición en cola real de dm_cursor antes del evento");
 event.testMomentum=NSEventPhaseChanged;on_scroll(event);CHECK(test_cursor_scroll_count()==2,"Inercia conserva desplazamiento de listas original");
 int bindings[DM_CTL_COUNT][2];char error[256];dm_controls_get(bindings);bindings[DM_CTL_SCROLL_UP][0]=-1;bindings[DM_CTL_A][1]=DM_CONTROL_WHEEL_UP;CHECK(dm_controls_commit(bindings,error,sizeof error),"Rueda sigue disponible para reasignación explícita");
 on_scroll(event);CHECK(!g_actions[DM_CTL_A],"Inercia nunca pulsa habilidad reasignada");event.testMomentum=NSEventPhaseNone;on_scroll(event);CHECK(g_actions[DM_CTL_A]==1,"Gesto deliberado activa habilidad reasignada");controls_release();
 NSString *path=[@(argv[1]) stringByAppendingPathComponent:@"raton.jsonl"];setenv("DM_MAC_POINTER_LOG",path.UTF8String,1);NSEvent *down=[NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:NSMakePoint(640,360) modifierFlags:0 timestamp:1 windowNumber:0 context:nil eventNumber:0 clickCount:1 pressure:1];pointer_probe("DOWN",down,.5,.5);unsetenv("DM_MAC_POINTER_LOG");
 NSString *line=[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];NSDictionary *row=line?[NSJSONSerialization JSONObjectWithData:[line dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil]:nil;CHECK([row[@"phase"] isEqual:@"DOWN"] && [row[@"normalized"] count]==2 && [row[@"location"] count]==2 && row[@"uiRect"] && row[@"layerFrame"],"Probe JSONL conserva coordenadas originales, normalizadas y geometría");
 NSString *keys=[@(argv[1]) stringByAppendingPathComponent:@"teclas.jsonl"];setenv("DM_MAC_POINTER_LOG",keys.UTF8String,1);g_live=1;controls_release();
 NSEvent *(^key)(NSEventType,NSUInteger,NSInteger)=^NSEvent*(NSEventType type,NSUInteger mods,NSInteger number){return [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:mods timestamp:2 windowNumber:number context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:19];};
 NSEvent *kd=key(NSEventTypeKeyDown,0,win.windowNumber),*ku=key(NSEventTypeKeyUp,0,win.windowNumber);
 CHECK(on_keyboard_event(kd,win)==nil && g_actions[DM_CTL_B]==1,"Monitor real consume tecla 2 y publica habilidad B");
 CHECK(on_keyboard_event(ku,win)==ku && g_actions[DM_CTL_B]==0,"Monitor real libera tecla y conserva retorno original");
 NSEvent *cmd=key(NSEventTypeKeyDown,NSEventModifierFlagCommand,win.windowNumber);CHECK(on_keyboard_event(cmd,win)==cmd && !g_actions[DM_CTL_B],"Probe no intercepta atajos con Comando");
 NSEvent *other=key(NSEventTypeKeyDown,0,0);CHECK(on_keyboard_event(other,win)==other && !g_actions[DM_CTL_B],"Monitor registra ventana equivocada sin inyectar habilidad");unsetenv("DM_MAC_POINTER_LOG");
 NSString *keylines=[NSString stringWithContentsOfFile:keys encoding:NSUTF8StringEncoding error:nil];NSArray *rows=[keylines componentsSeparatedByString:@"\n"];NSDictionary *kr=rows.count?[NSJSONSerialization JSONObjectWithData:[rows[0] dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil]:nil;
 CHECK([kr[@"phase"] isEqual:@"KEY_DOWN"] && [kr[@"keyCode"] intValue]==19 && [kr[@"action"] intValue]==DM_CTL_B && [kr[@"handled"] boolValue] && ![kr[@"returned"] boolValue] && kr[@"textResponder"] && !kr[@"characters"],"Keyprobe guarda códigos, asignación y ruta sin registrar texto");
 win.testVisible=YES;win.testKey=NO;win.testOcclusion=0;display_present_occlusion(win,1);CHECK(present_discarded,"Ventana cubierta mantiene descarte de presentación original");
 int policy_calls=present_policy_calls;CHECK(!display_present_interactive(win,"segundo plano")&&present_discarded&&present_policy_calls==policy_calls,"Ventana de fondo cubierta no se reactiva por reconciliación");
 win.testKey=YES;win.testVisible=NO;CHECK(!display_present_interactive(win,"oculta")&&present_discarded,"Ventana retirada conserva descarte aunque mantenga flag clave transitorio");win.testVisible=YES;win.testMini=YES;CHECK(!display_present_interactive(win,"minimizada")&&present_discarded,"Minimizar no se deshace por reconciliación de presentación");win.testMini=NO;
 on_keyboard_event(other,win);on_keyboard_event(ku,win);CHECK(present_discarded,"Otra ventana y liberaciones de teclas no acreditan nueva interacción");
 win.testKey=YES;on_keyboard_event(kd,win);on_keyboard_event(ku,win);CHECK(!present_discarded,"Ventana visible y clave recupera presentación al recibir una tecla aunque occlusionState siga transitorio");
 policy_calls=present_policy_calls;CHECK(!display_present_interactive(win,"repetición")&&present_policy_calls==policy_calls,"Reconciliación estable no repite cambios en cada evento");
 display_present_occlusion(win,1);CHECK(present_discarded,"Una nueva oclusión conserva el descarte original tras recuperarse");CHECK(display_present_interactive(win,"ventana clave")&&!present_discarded,"Recuperar ventana clave reanuda sin necesitar resize ni otra notificación de oclusión");
 display_present_occlusion(win,1);CHECK(display_present_interactive(win,"ratón")&&!present_discarded,"Interacción de ratón en ventana clave permite la misma recuperación");
 win.testOcclusion=NSWindowOcclusionStateVisible;display_present_occlusion(win,1);CHECK(!present_discarded,"Notificación visible conserva recuperación original");win.testOcclusion=0;policy_calls=present_policy_calls;display_present_occlusion(win,0);CHECK(!present_discarded&&present_policy_calls==policy_calls,"DM_OCCL=0 mantiene comparación sin modificar backend");
 win.testScale=2;g_view=win.contentView;g_view.wantsLayer=YES;g_layer=[CAMetalLayer layer];[g_view.layer addSublayer:g_layer];g_aspect=1512.0/950;[win setContentSize:NSMakeSize(1280,804)];layout_game();
 CHECK(NSEqualRects(g_layer.frame,NSMakeRect(0,0,1280,804))&&CGSizeEqualToSize(g_layer.drawableSize,CGSizeMake(2560,1608)),"Arranque frío en ventana inicializa frame y drawable Retina sin esperar maximización");
 [win setContentSize:NSMakeSize(1512,834)];layout_game();CHECK(NSEqualRects(g_layer.frame,NSMakeRect(92,0,1327,834))&&CGSizeEqualToSize(g_layer.drawableSize,CGSizeMake(2654,1668)),"Maximizar actualiza drawable y letterbox con la misma transformación");
 [win setContentSize:NSMakeSize(1280,804)];layout_game();CHECK(CGSizeEqualToSize(g_layer.drawableSize,CGSizeMake(2560,1608)),"Volver a ventana restaura drawable sin depender de presentación o foco");[g_layer removeFromSuperlayer];g_layer=nil;
 [g_display_prefs removePersistentDomainForName:suite];[win orderOut:nil];
 }printf("DISPLAY MAC %d comprobaciones, %d fallos\n",checks,failures);return failures?1:0;}
