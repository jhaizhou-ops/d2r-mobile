#import "../dm_controls_mac.m"
#include <stdio.h>
#import <objc/runtime.h>
static IMP originalRead; static unsigned expectedRevision; static BOOL revisionObserved, pathsOrdered;
static NSString *observe_refresh(id cls, SEL selector, NSString *path, NSStringEncoding encoding, NSError **error) {
 if ([path.lastPathComponent isEqual:@"inputglyphs.tsv"]) { revisionObserved=YES; pathsOrdered=dm_controls_revision()==expectedRevision; }
 return ((NSString *(*)(id,SEL,NSString *,NSStringEncoding,NSError **))originalRead)(cls,selector,path,encoding,error);
}
static int checks, failures;
#define CHECK(c,t) do {checks++; if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}else printf("PASS %s\n",t);}while(0)
static int reserved_mouse(int input){return input==258;}
static void check_mouse_columns(DmControlsPanel *panel) {
 NSScrollView *scroll=panel.table.enclosingScrollView;
 for(NSNumber *style in @[@(NSScrollerStyleLegacy),@(NSScrollerStyleOverlay)]){
  scroll.scrollerStyle=style.integerValue;[scroll tile];[panel.window.contentView layoutSubtreeIfNeeded];[panel.table layoutSubtreeIfNeeded];
  NSView *alternative=[panel.table viewAtColumn:2 row:0 makeIfNecessary:YES];[panel.window makeFirstResponder:alternative];[panel.table scrollColumnToVisible:2];
  NSRect visible=scroll.contentView.bounds,first=[panel.table rectOfColumn:0],last=[panel.table rectOfColumn:2];
  printf("TABLE style=%ld scale=%.1f clip=%s action=%s alternative=%s\n",(long)style.integerValue,panel.window.backingScaleFactor,NSStringFromRect(visible).UTF8String,NSStringFromRect(first).UTF8String,NSStringFromRect(last).UTF8String);
  CHECK(fabs(visible.origin.x)<0.5,"Enfocar Alternativa no desplaza ni recorta el inicio de Acción");
  CHECK(NSMinX(first)>=NSMinX(visible)-0.5&&NSMaxX(last)<=NSMaxX(visible)+0.5,"Las tres columnas completas caben en el clip real con ambos estilos de scroller");
  for(NSTableColumn *column in panel.table.tableColumns)CHECK(column.headerCell.cellSize.width<=column.width,"La cabecera completa cabe dentro del ancho de su columna");
  for(NSInteger row=0;row<2;row++){NSTextField *label=(NSTextField*)[panel.table viewAtColumn:0 row:row makeIfNecessary:YES];NSSize text=[label.stringValue sizeWithAttributes:@{NSFontAttributeName:label.font}];CHECK(text.width<=first.size.width-4,"Los nombres completos de las dos acciones caben sin truncarse");}
  for(NSInteger row=0;row<2;row++)for(NSInteger column=0;column<3;column++)(void)[panel.table viewAtColumn:column row:row makeIfNecessary:YES];
 }
}
int main(int argc,char **argv){ if(argc!=3)return 2; @autoreleasepool {
 [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited]; dm_controls_init(argv[1]);
 DmControlsPanel *mouse=[[DmControlsPanel alloc]initMouseOnly:YES reserved:reserved_mouse];
 CHECK([mouse numberOfRowsInTableView:mouse.table]==2 && mouse.window.contentView.bounds.size.height==320,"Extensión PC expone solo dos acciones de ratón en hoja ajustada");
 check_mouse_columns(mouse);
 NSButton *mouseSender=[NSButton new];mouseSender.tag=DM_CTL_PRIMARY*2;[mouse capture:mouseSender];
 check_mouse_columns(mouse);
 CHECK(![mouse captureInput:35] && ![mouse captureInput:512] && ![mouse captureInput:259|DM_CONTROL_SHIFT],"Hoja PC rechaza teclado, rueda y modificadores");
 CHECK(![mouse captureInput:258] && [mouse.status.stringValue containsString:@"Controles del juego"],"Conflicto nativo conserva captura y explica dónde liberarlo");
 CHECK([mouse captureInput:259],"Botón físico libre se prepara sin duplicar acciones nativas");
 check_mouse_columns(mouse);
 [mouse.table layoutSubtreeIfNeeded];NSView *mouseView=mouse.window.contentView;NSBitmapImageRep *mouseImage=[mouseView bitmapImageRepForCachingDisplayInRect:mouseView.bounds];[mouseView cacheDisplayInRect:mouseView.bounds toBitmapImageRep:mouseImage];CHECK([[mouseImage representationUsingType:NSBitmapImageFileTypePNG properties:@{}]writeToFile:[@(argv[2])stringByAppendingString:@".mouse.png"] atomically:YES],"Captura real de hoja PC de dos acciones sin mostrarla");
 int oldBindings[DM_CTL_COUNT][2];dm_controls_get(oldBindings);[mouse apply:nil];int preserved[DM_CTL_COUNT][2];dm_controls_get(preserved);
 CHECK(!memcmp(oldBindings,preserved,sizeof preserved) && dm_controls_mouse_lookup(259)==DM_CTL_PRIMARY,"Aplicar ratón PC conserva completo el perfil fallback");
 DmControlsPanel *mouseReset=[[DmControlsPanel alloc]initMouseOnly:YES reserved:reserved_mouse];[mouseReset defaults:nil];[mouseReset apply:nil];CHECK(dm_controls_mouse_lookup(256)==DM_CTL_PRIMARY && dm_controls_mouse_lookup(257)==DM_CTL_SECONDARY,"Predeterminados PC restauran únicamente izquierdo y derecho");
 DmControlsPanel *panel=[DmControlsPanel new]; CHECK(!panel.window.visible,"Panel de prueba no aparece en pantalla");
 check_mouse_columns(panel);
 NSButton *sender=[NSButton new]; sender.tag=DM_CTL_X*2; [panel capture:sender];
 for (NSNumber *code in @[@12,@13,@43]) { NSEvent *command=[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:NSEventModifierFlagCommand timestamp:0 windowNumber:panel.window.windowNumber context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:code.unsignedShortValue]; CHECK([panel captureEvent:command]==command && panel.captureRow==DM_CTL_X,"Monitor deja pasar Comando sin cancelar captura pendiente"); }
 CHECK(![panel captureInput:18] && panel.captureRow==DM_CTL_X,"Conflicto mantiene captura y muestra explicación");
 CHECK([panel.status.stringValue containsString:@"ya está asignado"],"Mensaje de conflicto identifica entrada ocupada");
 CHECK(![panel captureInput:-1],"Comando reservado se rechaza sin perder captura");
 CHECK([panel captureInput:35] && panel.captureRow<0,"Captura válida de P completa edición");
 CHECK(dm_controls_lookup(21)==DM_CTL_X && dm_controls_lookup(35)<0,"Edición pendiente no modifica juego");
 [panel.table layoutSubtreeIfNeeded]; NSView *view=panel.window.contentView; NSBitmapImageRep *image=[view bitmapImageRepForCachingDisplayInRect:view.bounds]; [view cacheDisplayInRect:view.bounds toBitmapImageRep:image];
 CHECK([[image representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[2]) atomically:YES],"Captura PNG del panel nativo real sin mostrar ventana");
 __block BOOL applied=NO; panel.onApply=^{applied=YES;};
 setenv("DM_ROMFS_OVERLAY",argv[1],1); expectedRevision=dm_controls_revision();
 Method readMethod=class_getClassMethod(NSString.class,@selector(stringWithContentsOfFile:encoding:error:)); originalRead=method_setImplementation(readMethod,(IMP)observe_refresh);
 [panel apply:nil]; method_setImplementation(readMethod,originalRead);
 CHECK(revisionObserved && pathsOrdered,"Revisión no cambia mientras se preparan las nuevas rutas de glifos");
 CHECK(dm_controls_revision()==expectedRevision+1,"Revisión se publica tras terminar actualización de glifos");
 CHECK(applied && dm_controls_lookup(35)==DM_CTL_X && dm_controls_lookup(21)<0,"Aplicar guarda y notifica al frontend");
 DmControlsPanel *cancel=[DmControlsPanel new]; [cancel defaults:nil]; [cancel cancel:nil]; CHECK(dm_controls_lookup(35)==DM_CTL_X,"Cancelar restauración conserva reasignación");
 DmControlsPanel *reset=[DmControlsPanel new]; [reset defaults:nil]; [reset apply:nil]; CHECK(dm_controls_lookup(21)==DM_CTL_X && dm_controls_lookup(35)<0,"Restaurar y Aplicar recupera teclas predeterminadas");
 NSTextField *field=[NSTextField new]; CHECK(dm_controls_text_responder(field),"Escritura nativa de nombres queda fuera de hotkeys"); CHECK(!dm_controls_text_responder([NSView new]),"Vista del juego permite hotkeys");
 CHECK(panel.monitor==nil && cancel.monitor==nil && reset.monitor==nil,"Cerrar elimina monitores de captura");
 NSWindow *parent=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1000,720) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO]; parent.alphaValue=0;
 dm_controls_show(parent,nil); NSWindow *sheet=parent.attachedSheet; sheet.alphaValue=0;
 CHECK(sheet!=nil && sheet.sheetParent==parent,"Controles se presenta como hoja adjunta al juego");
 if(sheet) {
   DmControlsPanel *shown=(DmControlsPanel *)sheet.windowController;
   CHECK(sheet.visible && [sheet.accessibilityRole isEqual:@"AXSheet"],"Hoja visible se identifica como AXSheet nativa");
   dm_controls_show(parent,nil); CHECK(parent.attachedSheet==sheet,"Abrir de nuevo conserva una sola hoja");
   [shown cancel:nil]; CHECK(parent.attachedSheet==nil && !sheet.visible && shown.monitor==nil,"Cancelar desadjunta hoja y retira captura");
   dm_controls_show(parent,nil); NSWindow *reopened=parent.attachedSheet; reopened.alphaValue=0;
   CHECK(reopened!=nil && reopened!=sheet,"Controles vuelve a abrir tras cancelar");
   DmControlsPanel *again=(DmControlsPanel *)reopened.windowController; [again apply:nil];
   CHECK(parent.attachedSheet==nil && !reopened.visible && again.monitor==nil,"Aplicar guarda y cierra hoja sin dejar monitor");
   dm_controls_show(parent,nil); NSWindow *closed=parent.attachedSheet;closed.alphaValue=0;
   NSEvent *escape=[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:closed.windowNumber context:nil characters:@"\e" charactersIgnoringModifiers:@"\e" isARepeat:NO keyCode:53];
   CHECK([closed performKeyEquivalent:escape],"Escape activa Cancelar cuando no se captura una asignación");
   [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.15]];
   CHECK(parent.attachedSheet==nil && !closed.visible,"Escape cancela también la hoja adjunta");
 } else { for(NSWindow *w in NSApp.windows) if([w.title isEqual:@"Controles"]) [w close]; }
 [parent orderOut:nil];

 } printf("CONTROLS UI %d comprobaciones, %d fallos\n",checks,failures); return failures?1:0;}
