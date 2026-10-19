#import "../front_mac.m"

void mtl_set_ui_rect_fn(void (*fn)(double*,double*)) { (void)fn; }
static int checks, failures;
#define CHECK(c,t) do { checks++; if (!(c)) { failures++; fprintf(stderr,"FAIL %s\n",t); } } while (0)
static NSArray *probe_rows(NSString *path) {
    NSString *text=[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    NSMutableArray *rows=[NSMutableArray new];
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) if (line.length) {
        NSDictionary *row=[NSJSONSerialization JSONObjectWithData:[line dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
        CHECK(row!=nil,"Cada fila conserva JSON válido"); if(row)[rows addObject:row];
    }
    return rows;
}
int main(int argc,char **argv) { if(argc!=2)return 2; @autoreleasepool {
    NSString *path=[@(argv[1]) stringByAppendingPathComponent:@"probe.jsonl"];
    [[NSFileManager defaultManager] createDirectoryAtPath:@(argv[1]) withIntermediateDirectories:YES attributes:nil error:nil];
    NSDictionary *move=@{@"phase":@"MOVE",@"type":@(NSEventTypeMouseMoved)};
    unsetenv("DM_MAC_POINTER_LOG");for(int i=0;i<4096;i++)input_probe_write(move);
    setenv("DM_MAC_POINTER_LOG","",1);for(int i=0;i<4096;i++)input_probe_write(move);
    setenv("DM_MAC_POINTER_LOG",path.UTF8String,1);
    for(int i=0;i<4096;i++)input_probe_write(move);
    NSArray *phases=@[@"DOWN",@"FINAL_MOVE",@"UP",@"KEY_DOWN",@"KEY_UP",@"WHEEL"];
    NSArray *types=@[@(NSEventTypeLeftMouseDown),@(NSEventTypeLeftMouseDragged),@(NSEventTypeLeftMouseUp),@(NSEventTypeKeyDown),@(NSEventTypeKeyUp),@(NSEventTypeScrollWheel)];
    for(NSUInteger i=0;i<phases.count;i++)input_probe_write(@{@"phase":phases[i],@"type":types[i]});
    NSArray *rows=probe_rows(path);
    CHECK(rows.count==128+phases.count,"El hover agotado conserva presupuesto para eventos discretos tardíos");
    CHECK(rows.count>=phases.count,"El clic tardío completo no desaparece");
    if(rows.count>=phases.count)for(NSUInteger i=0;i<phases.count;i++)CHECK([rows[rows.count-phases.count+i][@"phase"] isEqual:phases[i]],"Orden de DOWN, posición final, UP, teclas y rueda conservado");
    for(int i=0;i<4096;i++){input_probe_write(@{@"phase":@"HOVER",@"type":@(NSEventTypeMouseMoved)});input_probe_write(@{@"phase":@"DRAG",@"type":@(NSEventTypeRightMouseDragged)});}
    CHECK(probe_rows(path).count==rows.count,"HOVER y arrastre continuo comparten límite de movimiento sin gastar reserva discreta");
    for(int i=0;i<4096;i++)input_probe_write(@{@"phase":@"DOWN",@"type":@(NSEventTypeLeftMouseDown)});
    rows=probe_rows(path);CHECK(rows.count==128+1024,"Ambos presupuestos están acotados a 1152 filas por proceso");
    for(NSDictionary *row in rows)CHECK(!row[@"characters"]&&!row[@"text"],"La traza conserva sólo códigos y geometría, sin texto tecleado");
    unsetenv("DM_MAC_POINTER_LOG");printf("INPUT PROBE MAC %d comprobaciones, %d fallos\n",checks,failures);return failures?1:0;
} }
