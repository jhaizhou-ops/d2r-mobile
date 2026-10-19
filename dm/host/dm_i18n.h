// Textos del port: tabla dm/i18n/ui_text.tsv y generador dm/ios/tools/dm_i18n.py.
#pragma once
#include <stdatomic.h>
#include <stdint.h>
extern _Atomic int g_dm_lang; // Índice del juego (0…12); -1 hasta observar SetLanguage.
int dm_voice_saved(void); // VoiceLanguage guardado válido; -1 si falta.
int dm_voice_index(void); // DM_LANG > Custom al arrancar > VoiceLanguage guardado > idioma inicial.
int dm_lang_saved(void); // Language guardado, sin DM_LANG ni preferencias iniciales; -1 si no existe.
int dm_lang_index(void); // Juego > DM_LANG explícita > Custom al arrancar > ajustes guardados > idioma inicial.
void dm_lang_startup_set(int index); // Preferencia provisional para menús nativos anteriores al montaje; -1 la desactiva.
int dm_lang_zh(void); // zhCN y zhTW usan los textos chinos del port.
int dm_lang_es(void); // esES y esMX usan los textos españoles del port.
int dm_lang_from_tag(const char *tag); // BCP 47 o etiqueta del juego → índice original.
int dm_sys_lang(void); // DM_LANG > idioma iOS; en Mac la variante privada usa esES (2).
const char *dm_str(uint8_t *main_base, const char *key); // Tabla del juego con respaldo del host.
const char *dm_ui(const char *key); // Textos nativos del host en chino, inglés o español.
