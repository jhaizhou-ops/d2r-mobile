#!/usr/bin/env python3
# 牛门来源难度修改原型：仅改玩家本地配方源码的单一 op 单元格，不修改游戏/存档；未接生产构建。
# Cow source-difficulty prototype: changes one op cell in the user's local recipe source, never game/save files; not wired into production builds.
import argparse
from pathlib import Path


def patch_recipe(data: bytes) -> bytes:
    """严格验证后清除单一牛门行的 op28；不推断二进制格式。 | Clear one cow row's op28 after strict validation; never infer a binary layout."""
    data.decode('utf-8-sig')
    lines = data.splitlines(keepends=True)
    if not lines:
        raise ValueError('Empty recipe table')
    header = lines[0].removeprefix(b'\xef\xbb\xbf').rstrip(b'\r\n').decode().split('\t')
    if len(header) != len(set(header)):
        raise ValueError('Duplicate columns')
    columns = {name: i for i, name in enumerate(header)}
    required = {'enabled', 'numinputs', 'op', 'param', 'value', 'input 1', 'input 2', 'output'}
    if not required <= columns.keys():
        raise ValueError('Unknown recipe schema')
    candidates = []
    for index, line in enumerate(lines[1:], 1):
        cells = line.rstrip(b'\r\n').split(b'\t')
        if len(cells) != len(header):
            if line.strip():
                raise ValueError('Ragged recipe row')
            continue
        get = lambda key: cells[columns[key]].decode().strip()
        if get('output').casefold() != 'cow portal' or get('enabled') != '1':
            continue
        inputs = [get(name) for name in header if name.startswith('input ')]
        if sorted(x for x in inputs if x) != ['leg', 'tbk'] or get('numinputs') != '2':
            raise ValueError('Unknown cow recipe inputs')
        if any(get(name) for name in ('output b', 'output c') if name in columns):
            raise ValueError('Unexpected extra output')
        if get('op') not in ('28', '') or get('param') not in ('', '0') or get('value') not in ('', '0'):
            raise ValueError('Unknown cow recipe condition; native inspection required')
        candidates.append((index, cells))
    if len(candidates) != 1:
        raise ValueError('Expected exactly one enabled cow recipe')
    index, cells = candidates[0]
    if not cells[columns['op']].strip():
        return data
    cells[columns['op']] = b''
    ending = lines[index][len(lines[index].rstrip(b'\r\n')):]
    lines[index] = b'\t'.join(cells) + ending
    return b''.join(lines)


def main():
    parser = argparse.ArgumentParser(description='Experimental cow recipe source patch; does not activate the feature in-game.')
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        patched = patch_recipe(args.source.read_bytes())
        with args.output.open('xb') as stream:
            stream.write(patched)
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')
    print('Recipe source written. Native compilation, eligibility checks and device validation are still required.')


if __name__ == '__main__':
    main()
