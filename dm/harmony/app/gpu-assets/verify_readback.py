"""Independently compare captured GPU bytes; truncated status tails are reported."""
import hashlib
import json
import re
import sys
from pathlib import Path

log, output = map(Path, sys.argv[1:3])
chunks = {}
current = None
for line in log.read_text(encoding='utf-8-sig').splitlines():
    if 'XblGpu: ' not in line:
        continue
    payload = line.split('XblGpu: ', 1)[1]
    match = re.match(r'gpu\[(\d+)/(\d+)\]: (.*)', payload)
    if match:
        current = int(match[1])
        if current == 1:
            chunks = {}
        chunks[current] = [match[3]]
    elif current is not None:
        chunks[current].append(payload)
parts = []
for lines in chunks.values():
    part = '\n'.join(lines)
    parts.append(part + ('\n' if len(part) == 1499 else ''))
raw = ''.join(parts)
output.mkdir(parents=True, exist_ok=True)
(output / 'gpu-reconstructed.txt').write_text(raw, encoding='utf-8')
result = {'statusTailComplete': 'gpuProbe=PASS' in raw, 'readbacks': {}}
names = ['q_val0', 'q_val8', 'q_dlt0', 'q_dlt8']
fixtures = Path(__file__).parent / 'fixtures'
pixels = [
    (255,0,0,255),(0,255,0,255),(0,0,255,255),(255,255,255,255),
    (255,255,0,255),(255,0,255,255),(0,255,255,255),(0,0,0,255),
    (255,0,0,85),(0,255,0,170),(0,0,255,0),(255,255,255,255),
    (255,128,64,255),(64,128,255,255),(128,64,255,255),(32,64,128,255)]
for name, hexdata in re.findall(r'readbackHex (\w+)=([0-9a-f]+)', raw):
    actual = bytes.fromhex(hexdata)
    (output / (name + '.actual.bin')).write_bytes(actual)
    expected = ((fixtures / (name + '.expected.bin')).read_bytes() if name in names else
                bytes(c for y in range(32) for x in range(32) for c in pixels[y//8*4+x//8]))
    row = {'bytes': len(actual), 'sha256': hashlib.sha256(actual).hexdigest(),
           'exactMatch': actual == expected}
    if name in names:
        row['sentinel'] = actual[296:] == b'\xaa' * 216
        row['matrix'] = [int(actual == (fixtures / (n + '.expected.bin')).read_bytes()) for n in names]
    result['readbacks'][name] = row
(output / 'readback-verification.json').write_text(json.dumps(result, indent=2))
print(json.dumps(result, indent=2))
assert set(result['readbacks']) == set(names + ['rgba32'])
assert all(row['exactMatch'] for row in result['readbacks'].values())
