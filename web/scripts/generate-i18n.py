#!/usr/bin/env python3
"""从浏览器专用文字表生成仅供浏览器使用的中英目录；不读取游戏文件。
Generate the browser-only Chinese/English catalog from the browser-owned UI table, without game files.
"""

import csv
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TABLE = ROOT / "web/i18n/ui_text.tsv"
OUTPUT = ROOT / "web/i18n-catalog.mjs"


def generate():
    """浏览器仅支持中英；不读取原生文字表。 | Browser supports Chinese/English only; no native table dependency."""
    catalog = {}
    with TABLE.open(encoding="utf-8", newline="") as source:
        for row in csv.DictReader(source, delimiter="\t"):
            key = row["键"]
            if not key.startswith("dmWeb"):
                raise ValueError(f"Invalid browser key: {key}")
            if key in catalog:
                raise ValueError(f"Duplicate browser key: {key}")
            catalog[key] = [row["English"].replace("\\n", "\n"), row["中文"].replace("\\n", "\n")]
    return (
        "// 生成文件，勿手改：python3 web/scripts/generate-i18n.py；来源 web/i18n/ui_text.tsv。\n"
        "// Generated; do not edit: python3 web/scripts/generate-i18n.py; source web/i18n/ui_text.tsv.\n"
        "export const catalog = Object.freeze("
        + json.dumps(catalog, ensure_ascii=False, indent=2)
        + ");\n"
    )


if __name__ == "__main__":
    content = generate()
    if sys.argv[1:] == ["--check"]:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != content:
            raise SystemExit("Browser catalog is stale; run python3 web/scripts/generate-i18n.py")
        print("Browser i18n catalog is current")
    elif sys.argv[1:]:
        raise SystemExit("Usage: python3 web/scripts/generate-i18n.py [--check]")
    else:
        OUTPUT.write_text(content, encoding="utf-8")
