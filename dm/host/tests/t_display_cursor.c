#include "../dm_cursor.c"
void dm_custom_pointer_input(void) { }

unsigned test_cursor_scroll_count(void) { return atomic_load(&g_up_w); }
int test_cursor_scroll_arg(void) { unsigned w=atomic_load(&g_up_w); return w ? g_up_q[(w-1)%32].arg : 0; }
uint64_t test_cursor_position(void) { return atomic_load(&g_up_xy); }
