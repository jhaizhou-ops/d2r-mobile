#pragma once
// 文字框颜色码对照表（实现与说明见 dm_textbox_core.c）
// Text box color code mapping table (implementation and notes in dm_textbox_core.c)
#include <stddef.h>
int dm_textbox_load(const char *path);    // 读表，返回条数；打不开返回 -1（表清空） | Read the table, return the entry count; return -1 if it cannot be opened (table cleared)
void dm_textbox_reset(void);
int dm_textbox_count(void);
// 按表改写：0 = 不动（*out 为空）；1 = 整串命中；2 = 换了其中的注释版子串。*out 由调用者 free
// Rewrite by the table: 0 = unchanged (*out is NULL); 1 = whole-string hit; 2 = annotated substrings inside were replaced. *out is freed by the caller
int dm_textbox_rewrite(const char *s, size_t n, char **out, size_t *on);
// 读对照表（只读一次，不挂门；实现在 dm_textbox.c）：返回 1 = 有表，0 = 没有（物品注释关着 / 没有覆盖层）
// Read the mapping table (read only once, does not hook the gate; implemented in dm_textbox.c): returns 1 = table present, 0 = none (item annotations off / no overlay)
int dm_textbox_ensure_loaded(void);

// Mapa independiente de nombres oficiales de colección; sólo coincidencia exacta.
// El consumidor debe comprobar PC/español y el widget vivo antes de usarlo.
int dm_collection_names_load(const char *path);
void dm_collection_names_reset(void);
int dm_collection_names_rewrite(const char *s,size_t n,char **out,size_t *on);
