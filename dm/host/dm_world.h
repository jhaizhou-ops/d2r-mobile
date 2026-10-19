// 点游戏世界里的目标 = 与它交互（实现与依据见 dm_world.c）
// Tapping a target in the game world = interacting with it (implementation and rationale in dm_world.c)
#pragma once
#include <stdint.h>
int dm_world_mouse_aim(float nx, float ny, int32_t *rx, int32_t *ry); // Cursor de pantalla → dirección del apuntado original, desde los pies del jugador.
enum { DM_PC_MOUSE_REJECTED, DM_PC_MOUSE_CONTINUOUS, DM_PC_MOUSE_ONESHOT };
int dm_world_pc_mouse(int secondary, int phase, float nx, float ny); // Hilo del juego: botón PC, pulsar/mantener/soltar (0/1/2), posición de pantalla normalizada; devuelve0 rechazo/1 continuo/2 one-shot.
enum { DM_PC_POINTER_NONE, DM_PC_POINTER_UI, DM_PC_POINTER_WORLD };
void dm_world_pc_hover(float nx,float ny,int pointer_state); // Solo guest: snapshot físico válido en UI/mundo, o liberación sin puntero; no emite órdenes.
void dm_world_pc_controller_observe(const uint64_t *regs,unsigned source); // Observación sin mutaciones; el trampolín ejecuta siempre el BL original.
void dm_world_pc_skill(int slot,int down); // Solo guest: slot PC 0..15, despacho posterior al consumo UI nativo.
void dm_world_pc_skill_cancel(void);
void dm_world_pc_skill_tick(void); // Tras publicar el cursor actual; como máximo un HELD por frame.
void dm_world_pc_skill_pointer(float nx,float ny,int allowed); // Solo guest: cursor de pantalla y permiso de mundo.
unsigned dm_world_tap(float nx, float ny);   // 任意线程：排一笔点按（出图画面归一化坐标 [0,1]）；主线程取手柄处解析目标并发令，没有目标什么都不发。返回序号 | any thread: queue one tap (coordinates normalized to the rendered picture, [0,1]); the main thread resolves the target and sends the command at the controller-poll point, and sends nothing when there is no target. Returns the sequence number
int dm_world_tap_result(unsigned seq);       // -1 还没执行；0 没有目标；1 物品（拾取）2 NPC（对话）3 物件（使用）4 出口 5 尸体（取回） | -1 not executed yet; 0 no target; 1 item (pick up) 2 NPC (talk) 3 object (use) 4 exit 5 corpse (retrieve)
int dm_world_has_target(void);               // 任意线程：按主键会去交互的目标是否存在（游戏手柄版自己的选择） | any thread: whether a target that the primary button would interact with exists (the game's own controller-version selection)
void dm_world_pump(void);                    // 取手柄处每帧调（主线程） | called every frame where the controller is fetched (main thread)
void dm_world_watch(int type, uint32_t cls);     // 测试量具：盯住某类单位（type < 0 取消）；主线程每帧发布它身体框中心的屏幕位置 | test probe: watch one kind of unit (type < 0 cancels); the main thread publishes the screen position of its body box center every frame
int dm_world_watch_pos(float *nx, float *ny);    // 任意线程：盯住的单位在画面内返回 1 并给出归一化坐标 | any thread: returns 1 and gives normalized coordinates when the watched unit is in the picture
// 小地图任务终点（dm_world.c）：主线程每帧发布，前端任意线程取。x / y = 世界格；art ≥ 64 = 塔拉夏符号第 art−64 帧，其余 = 名牌号（tools/gen_map_goal_art.py）
// Minimap quest goals (dm_world.c): published by the main thread every frame, fetched by the front end from any thread. x / y = world cell; art >= 64 = frame art-64 of the Tal Rasha symbols, otherwise = nameplate number (tools/gen_map_goal_art.py)
typedef struct { int n, px, py; float sx, sy; int x[12], y[12]; unsigned char art[12]; } dm_map_goal_view;   // px / py 玩家世界格；sx / sy 玩家脚底在画面上的位置（归一化） | px / py player world cell; sx / sy position of the player's feet on screen (normalized)
int dm_map_goals(dm_map_goal_view *v);      // 返回终点个数（地图标注开关关着、本关没有终点、还没扫完 = 0） | returns the number of goals (map marking switch off, no goal in this level, scan not finished = 0)
