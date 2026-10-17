"""Read every shipped XGS shader and inventory requirements before device startup."""
import argparse
import collections
import json
import pathlib
import struct


def audit(root):
    caps = collections.defaultdict(list)
    stages = collections.Counter()
    descriptors = collections.Counter()
    subgroup_ops = collections.defaultdict(list)
    extensions = collections.defaultdict(list)
    compute_sizes = []
    count = 0
    for path in sorted(root.glob('*.xgs')):
        data = path.read_bytes()
        magic, version, stage, bindings, inputs, constants, size, _ = struct.unpack_from('<8I', data)
        if magic != 0x31534758 or version != 1 or stage > 5:
            raise ValueError(f'{path.name}: invalid header')
        offset = 32 + bindings * 32 + inputs * 4 + constants
        if offset + size != len(data) or size % 4:
            raise ValueError(f'{path.name}: invalid payload bounds')
        for i in range(bindings):
            binding = struct.unpack_from('<8I', data, 32 + i * 32)
            descriptors[binding[1]] += 1
        words = struct.unpack_from(f'<{size // 4}I', data, offset)
        if len(words) < 5 or words[0] != 0x07230203:
            raise ValueError(f'{path.name}: invalid SPIR-V')
        cursor = 5
        found = set()
        uses_subgroup = False
        while cursor < len(words):
            length, opcode = words[cursor] >> 16, words[cursor] & 65535
            if not length or cursor + length > len(words):
                raise ValueError(f'{path.name}: invalid instruction bounds')
            if opcode == 17:
                if length != 2:
                    raise ValueError(f'{path.name}: invalid OpCapability')
                found.add(words[cursor + 1])
            if 333 <= opcode <= 366:
                uses_subgroup = True
            if opcode == 10:
                name = struct.pack(f'<{length - 1}I', *words[cursor + 1:cursor + length]).split(b'\0')[0].decode('utf-8')
                extensions[name].append(path.stem)
            if opcode == 16 and length == 6 and words[cursor + 2] == 17:
                x, y, z = words[cursor + 3:cursor + 6]
                compute_sizes.append({'shader': path.stem, 'size': [x, y, z], 'invocations': x * y * z})
            cursor += length
        if uses_subgroup:
            subgroup_ops[stage].append(path.stem)
        for capability in found:
            caps[capability].append(path.stem)
        stages[stage] += 1
        count += 1
    if not count:
        raise ValueError('No shaders found')
    return {
        'scope': 'Complete offline inventory; does not prove GPU execution or feature support',
        'shaders': count, 'stages': dict(stages), 'descriptor_types': dict(descriptors),
        'actual_subgroup_operations_by_stage': {str(k): {'count': len(v), 'examples': v[:3]} for k, v in sorted(subgroup_ops.items())},
        'declared_extensions': {k: {'count': len(v), 'examples': v[:3]} for k, v in sorted(extensions.items())},
        'compute_local_sizes': compute_sizes,
        'capabilities': {str(k): {'count': len(v), 'examples': v[:3]} for k, v in sorted(caps.items())},
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('library', type=pathlib.Path)
    parser.add_argument('--output', required=True, type=pathlib.Path)
    args = parser.parse_args()
    result = audit(args.library)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))
