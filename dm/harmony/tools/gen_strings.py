#!/usr/bin/env python3
"""从统一中英表生成鸿蒙界面文案（含共用文案与鸿蒙壳专用文案）；不读取游戏文件。
Generate HarmonyOS UI strings from the shared string table (shared strings and HarmonyOS shell strings); no game files.
用法 / usage: python dm/harmony/tools/gen_strings.py   → dm/harmony/app/entry/src/main/ets/generated/UiText.ts
"""
import importlib.util
import json
from pathlib import Path


def main():
    dm = Path(__file__).resolve().parents[2]
    spec = importlib.util.spec_from_file_location("dm_i18n", dm / "ios/tools/dm_i18n.py")
    table = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(table)
    shared = {"dmInputTitle", "dmNameTitle", "dmNameRule", "dmQtyTitle", "dmNameBlankRandom",
              "dmNamePickIme", "dmNameTooShort", "dmNameEdge", "dmCancel", "dmConfirm", "dmTcCancel", "dmHudLife", "dmHudMana"}
    rows = [r for r in table.rows() if r[0].startswith(("dmAndroid", "dmHarmony")) or r[0] in shared]
    zh = {r[0]: r[1] for r in rows}
    en = {r[0]: r[2] for r in rows}
    out = dm / "harmony/app/entry/src/main/ets/generated/UiText.ts"
    out.write_text(
        "// 由 dm/harmony/tools/gen_strings.py 从 dm/i18n/ui_text.tsv 生成，勿手改 | Generated from dm/i18n/ui_text.tsv by dm/harmony/tools/gen_strings.py; do not edit\n"
        f"export const UI_TEXT_ZH: Record<string, string> = {json.dumps(zh, ensure_ascii=False, indent=1)};\n"
        f"export const UI_TEXT_EN: Record<string, string> = {json.dumps(en, ensure_ascii=False, indent=1)};\n",
        encoding="utf-8")
    print(f"{out}: {len(rows)} strings")


if __name__ == "__main__":
    main()
