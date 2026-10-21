// 我们自己加的界面文字取串（中 / 英 / 西 / 俄；表 = dm/i18n/ui_text.tsv，生成器 dm/ios/tools/dm_i18n.py）
// String lookup for UI text we added ourselves (Chinese / English / Spanish / Russian; table = dm/i18n/ui_text.tsv, generator dm/ios/tools/dm_i18n.py)
#pragma once
#include <stdatomic.h>
#include <stdint.h>
extern _Atomic int g_dm_lang;                         // 原版当前语言号（0 enUS 1 deDE 2 esES 3 frFR 4 itIT 5 koKR 6 plPL 7 ruRU 8 zhCN 9 zhTW 10 esMX 11 jaJP 12 ptBR），SetLanguage 观测门写（diag.c）；-1 = 游戏还没设过 | Original current language number (0 enUS 1 deDE 2 esES 3 frFR 4 itIT 5 koKR 6 plPL 7 ruRU 8 zhCN 9 zhTW 10 esMX 11 jaJP 12 ptBR), written by the SetLanguage observation gate (diag.c); -1 = the game hasn't set it yet
int dm_lang_index(void);                              // 当前原版语言号（游戏设过 > 设置文件 > 首次安装按系统语言） | current language index of the original game (set by the game > settings file > system language on first install)
int dm_lang_zh(void);                                 // 1 = 我们的字用中文（zhCN、zhTW） | 1 = our text uses Chinese (zhCN, zhTW)
enum { DM_LG_EN, DM_LG_ZH, DM_LG_ES, DM_LG_MX, DM_LG_RU };
int dm_lang_group(void);                              // 我们的字用哪一套：中文（zhCN、zhTW）/ 西语（esES）/ 拉美西语（esMX）/ 俄语（ruRU）/ 其余英文 | Which set our text uses: Chinese (zhCN, zhTW) / Spanish (esES) / Latin American Spanish (esMX) / Russian (ruRU) / English otherwise
int dm_lang_group_of(int lang);                       // 同上，给定原版语言号 | Same as above, given an original language number
const char *dm_ui_group(const char *key, int group);  // 编译期表按指定的那一套取（DM_LG_*） | Look up the compile-time table in the specified set (DM_LG_*)
const char *dm_untag(const char *s);                  // 跳过名字串开头的语法性别标记（西语等：[ms] [fs] [mp] [fp]，可连写），自己拼文字时用；没有标记原样返回 | Skip grammatical gender markers at the start of a name string (Spanish etc.: [ms] [fs] [mp] [fp], may be chained), used when composing text ourselves; returned unchanged if there is no marker
int dm_lang_from_tag(const char *tag);                // 语言标记（en-US / zh-Hant-TW / es-419 …）→ 原版语言号：中文 / 汉语方言 = 8 zhCN，es-ES / es = 2 esES，其余西语 = 10 esMX，ru = 7 ruRU，其余 = 0 enUS | Language tag (en-US / zh-Hant-TW / es-419 ...) -> original language number: Chinese / Chinese dialects = 8 zhCN, es-ES / es = 2 esES, other Spanish = 10 esMX, ru = 7 ruRU, otherwise = 0 enUS
int dm_sys_lang(void);                                // 系统语言 → 原版语言号（DM_LANG > iOS 首选语言 > zh-Hans） | system language → the original game's language index (DM_LANG > iOS preferred language > zh-Hans)
const char *dm_str(uint8_t *main_base, const char *key);   // 游戏里画的字：原版取串（表已追加进原版 ui.json，跟原版一起换语言） | text drawn inside the game: the original string lookup (the table is appended to the original ui.json and switches language together with the original game)
const char *dm_ui(const char *key);                   // 游戏外画的字（UIKit / 资源下载页 / 底栏字形）：编译期表，按 dm_lang_group() 取 | Text drawn outside the game (UIKit / resource download page / bottom-bar glyphs): compile-time table, looked up by dm_lang_group()
