#!/usr/bin/env python3
"""从统一中英表生成安卓启动界面的资源；不读取游戏文件。
Generate Android startup resources from the shared string table; no game files.
"""
import argparse
import importlib.util
from pathlib import Path
from xml.sax.saxutils import escape


def generate(output):
    dm = Path(__file__).resolve().parents[2]
    spec = importlib.util.spec_from_file_location("dm_i18n", dm / "ios/tools/dm_i18n.py")
    table = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(table)
    shared = {"dmInputTitle", "dmNameTitle", "dmNameRule", "dmQtyTitle", "dmNameBlankRandom",
              "dmNamePickIme", "dmNameTooShort", "dmNameEdge", "dmCancel", "dmConfirm", "dmTcCancel", "dmHudLife", "dmHudMana"}
    rows = [r for r in table.rows() if r[0].startswith("dmAndroid") or r[0] in shared]
    # 葡语用 values-pt（系统 pt-BR / pt-PT 都会落到这一份；项目把 pt 统一映射到原版 ptBR）
    # Portuguese goes to values-pt (both pt-BR and pt-PT systems fall back to it; the project maps every pt to the original ptBR)
    for folder, column in (("values", 2), ("values-zh", 1), ("values-pt", 5)):
        lines = ['<?xml version="1.0" encoding="utf-8"?>', '<resources>']
        for row in rows:
            value = escape(row[column]).replace("\\", "\\\\").replace("'", "\\'").replace('"', '\\"').replace("\n", "\\n")
            # 多参数C格式由Java显式格式化，避免资源编译器误判非位置参数。
            # Java formats shared multi-argument C strings explicitly; skip Android's positional-format check.
            attribute = ' formatted="false"' if row[0] in {"dmHudLife", "dmHudMana", "dmAndroidHudLevel"} else ''
            lines.append(f'    <string name="{row[0]}"{attribute}>{value}</string>')
        lines.append('</resources>')
        directory = output / folder
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "strings.xml").write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    generate(parser.parse_args().output)
