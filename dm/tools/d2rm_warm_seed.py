"""Validate Vulkan pipeline warm-up records before adding a seed to an import pack."""
import hashlib
from pathlib import Path
import struct


def load_seed(path):
    path = Path(path)
    with path.open("rb") as stream:
        data = stream.read(64 << 20)
    if not data or len(data) >= 64 << 20:
        raise ValueError("pipeline seed must contain fewer than 64 MiB, matching warm::start")
    pos = 0
    records = 0
    keys = set()

    def word():
        nonlocal pos
        if pos + 4 > len(data):
            raise ValueError("truncated pipeline seed word")
        value, = struct.unpack_from("<I", data, pos)
        pos += 4
        return value

    while pos < len(data):
        if word() != 0x57504C58:
            raise ValueError("invalid pipeline seed magic")
        stages = word()
        if not 1 <= stages <= 5:
            raise ValueError("invalid stage count")
        for _ in range(stages):
            size = word()
            if not 1 <= size <= 256 or pos + size > len(data):
                raise ValueError("invalid shader key length")
            name = data[pos:pos + size].decode("ascii")
            if any(c in name for c in ("\\", "/", "\0")) or name in (".", ".."):
                raise ValueError("invalid shader key")
            keys.add(name)
            pos += size
        count = word()
        if not 1 <= count <= 4096 or pos + count * 4 > len(data):
            raise ValueError("invalid pipeline key extent")
        pos += count * 4
        records += 1
    return data, records, keys


def stage_seed(seed, shader_dir):
    data, records, keys = seed
    shader_dir = Path(shader_dir)
    available = {p.stem for p in shader_dir.glob("*.xgs") if p.is_file()}
    # Reject a mismatched pack, even if a few common shader names happen to exist.
    missing = keys - available
    if missing:
        raise ValueError(f"{len(missing)} seed shader keys missing, e.g. {min(missing)}")
    (shader_dir / "vk-pipeline-list.seed").write_bytes(data)
    return hashlib.sha256(data).hexdigest()
