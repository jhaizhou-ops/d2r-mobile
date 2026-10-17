/* Android 的语言面（P5）：hle_fs.c 首次建设置文件时要问系统语言（dm_sys_lang），
 * 语言开关（自定义页那一格，没移植前 = 没点过「应用」→ 返回 -1 = 一个字不动）。
 * 口径与 dm/host/dm_i18n.c 的 dm_lang_from_tag 一致：中文及汉语方言 → 8（简中），其余 → 0（英文）；
 * DM_LANG（宿主 / 测试）优先，其次 app_jni 传进来的系统 locale tag（DM_LOCALE_TAG），都没有按简中。
 * Android language side (P5): the settings seeding asks the system language (dm_sys_lang);
 * the language switch (the Custom page cell, not ported yet = never tapped -> -1 = touch nothing).
 * Same convention as dm_lang_from_tag: Chinese / Sinitic -> 8 (Simplified Chinese), everything else -> 0 (English). */
/* Android 侧只剩语言开关的桩：取串本身编真实现 dm_i18n.c（dm_i18n_tab.h 在仓库里，dm_sys_lang / dm_lang_from_tag / dm_ui / dm_str 全是它）。 */
