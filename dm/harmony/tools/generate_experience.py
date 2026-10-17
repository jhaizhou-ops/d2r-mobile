#!/usr/bin/env python3
"""由用户导出的experience.txt生成仓外ArkTS表；不附带游戏数据。
Generate an external ArkTS table from the user's exported experience.txt; no game data is supplied.
"""
import argparse
import csv
import io
from pathlib import Path


def generate(source):
    rows = csv.DictReader(io.StringIO(source.read_text(encoding='utf-8-sig')), delimiter='\t')
    values = [int(row['Amazon']) for row in rows if row.get('Level', '').isdigit()][:100]
    if len(values) != 100 or values[0] != 0 or any(a > b for a, b in zip(values, values[1:])):
        raise ValueError('expected 100 ascending Amazon experience values, starting at level 1 = 0')
    return '// 本地生成，不入Git。 | Locally generated; do not commit.\nexport const EXP: number[] = ' + str(values) + ';\n'


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--experience-txt', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    if args.output.resolve().is_relative_to(Path(__file__).resolve().parents[3]):
        ap.error('output must remain outside the checkout')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generate(args.experience_txt), encoding='utf-8')
