// 门快照中的 x18 必须是客体槽值；Windows 活寄存器 x18 始终保留 TEB。
// Gate snapshots must contain the guest x18 slot; the live Windows x18 register always retains the TEB.
#include "dm_windows.h"
#include "../host/dm.h"
uint64_t dm_windows_gate_dispatch(uint64_t address, uint64_t *registers) {
    uint64_t teb = registers[18];
    registers[18] = (uintptr_t)pthread_getspecific(g_key18);
    uint64_t result = gate_dispatch(address, registers);
    if (pthread_setspecific(g_key18, (void *)(uintptr_t)registers[18])) die("Windows gate x18 write-back failed");
    registers[18] = teb;
    return result;
}
