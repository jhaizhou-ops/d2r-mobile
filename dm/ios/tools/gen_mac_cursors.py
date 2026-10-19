#!/usr/bin/env python3
# 从用户自己的 RomFS 提取 Mac 光标；依据 3.3.94012 的 SpA1 头和 romfs_art 解码器。
# Extract Mac cursors from the user's own RomFS, using the 3.3.94012 SpA1 header and romfs_art decoder.
import argparse
from pathlib import Path
import struct
import romfs_art


def generate(romfs, output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    for sprite, name, hotspot in (("protate", "normal", (3, 14)), ("ppress", "pressed", (4, 14))):
        logical = "data/hd/global/ui/CURSOR/" + sprite + ".sprite"
        raw = romfs_art.read(str(romfs), logical)
        if raw[:4] != b"SpA1" or struct.unpack_from("<H", raw, 4)[0] != 61:
            raise ValueError("Unsupported cursor format: " + logical)
        width, atlas_width, height = struct.unpack_from("<HII", raw, 6)
        count = struct.unpack_from("<H", raw, 20)[0]
        if (width, height) != (69, 64) or struct.unpack_from("<hh", raw, 16) != hotspot or not count or atlas_width % count:
            raise ValueError("Unsupported cursor geometry: " + logical)
        image = romfs_art.sprite(str(romfs), logical).crop((0, 0, width, height))
        if image.mode != "RGBA":
            raise ValueError("Cursor alpha must be preserved: " + logical)
        image.save(output / (name + ".png"))
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Extract Mac cursors from your own RomFS")
    parser.add_argument("romfs", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    generate(args.romfs, args.output)
