#import "../front_mac.m"
static int checks, failures;
void mtl_set_ui_rect_fn(void (*fn)(double*,double*)){(void)fn;}
#define CHECK(c,t) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}else printf("PASS %s\n",t);}while(0)
int main(int argc,char **argv){if(argc!=2)return 2;@autoreleasepool{
 [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
 CHECK(game_cursor_load(@(argv[1])),"Recursos originales cargan ambos estados de cursor");
 CHECK(NSEqualSizes(g_game_cursor.image.size,NSMakeSize(34.5,32)),"Cursor conserva 69x64 píxeles a escala Retina 2x");
 CHECK(NSEqualPoints(g_game_cursor.hotSpot,NSMakePoint(1.5,7)),"Punto activo original normal 3,14 se expresa en puntos");
 CHECK(NSEqualPoints(g_game_cursor_pressed.hotSpot,NSMakePoint(2,7)),"Punto activo presionado conserva 4,14");
 NSBitmapImageRep *bitmap=[NSBitmapImageRep imageRepWithData:[NSData dataWithContentsOfFile:[@(argv[1]) stringByAppendingPathComponent:@"normal.png"]]];
 CHECK(bitmap.pixelsWide==69 && bitmap.pixelsHigh==64 && bitmap.hasAlpha && [bitmap colorAtX:68 y:63].alphaComponent==0,"Sprite conserva tamaño y transparencia original");
 g_live=1;g_aspect=16.0/9;g_view=[[DmGameView alloc]initWithFrame:NSMakeRect(0,0,1280,900)];
 CHECK(game_cursor_for_point(NSMakePoint(640,450),NO,NO)==g_game_cursor,"Área de juego usa cursor Diablo");
 CHECK(game_cursor_for_point(NSMakePoint(640,450),YES,NO)==g_game_cursor_pressed,"Pulsación usa estado original presionado");
 CHECK(game_cursor_for_point(NSMakePoint(640,30),NO,NO)==NSCursor.arrowCursor,"Margen fuera del juego usa cursor del sistema");
 CHECK(game_cursor_for_point(NSMakePoint(640,450),NO,YES)==NSCursor.arrowCursor,"Hoja nativa conserva cursor del sistema");
 g_live=0;CHECK(game_cursor_for_point(NSMakePoint(640,450),NO,NO)==NSCursor.arrowCursor,"Entrada no interactiva conserva cursor del sistema");g_live=1;
 [g_view updateTrackingAreas];NSTrackingArea *area=g_view.trackingAreas.lastObject;
 CHECK(NSEqualRects(area.rect,game_rect()) && (area.options&NSTrackingCursorUpdate) && (area.options&NSTrackingEnabledDuringMouseDrag),"Seguimiento coincide con área útil y contempla arrastres");
 [g_view setFrameSize:NSMakeSize(1280,720)];[g_view updateTrackingAreas];
 CHECK(g_view.trackingAreas.count==1 && NSEqualRects(g_view.trackingAreas.lastObject.rect,game_rect()),"Redimensionar reemplaza seguimiento sin acumular regiones");
 NSEvent *down=[NSEvent mouseEventWithType:NSEventTypeRightMouseDown location:NSMakePoint(500,300) modifierFlags:0 timestamp:0 windowNumber:0 context:nil eventNumber:0 clickCount:1 pressure:1];
 NSEvent *up=[NSEvent mouseEventWithType:NSEventTypeRightMouseUp location:NSMakePoint(-20,300) modifierFlags:0 timestamp:0 windowNumber:0 context:nil eventNumber:0 clickCount:1 pressure:0];
 game_cursor_event(down);CHECK(g_cursor_buttons==2,"Botón derecho activa el estado presionado original");
 game_cursor_event(up);CHECK(!g_cursor_buttons,"Soltar fuera del juego libera el estado presionado");
 }printf("CURSOR MAC %d comprobaciones, %d fallos\n",checks,failures);return failures?1:0;}
