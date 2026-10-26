# 通过 PE 文件头拒绝 x64 / ARM64EC 依赖；不执行待检文件。
# Reject x64 / ARM64EC dependencies by their PE headers, without executing them.
from pathlib import Path
import struct
import sys


def check(path):
    with Path(path).open("rb") as file:
        if file.read(2) != b"MZ":
            raise ValueError("not a PE image")
        file.seek(0x3C)
        offset = struct.unpack("<I", file.read(4))[0]
        file.seek(offset)
        if file.read(4) != b"PE\0\0" or struct.unpack("<H", file.read(2))[0] != 0xAA64:
            raise ValueError("native ARM64 PE image required")


if __name__ == "__main__":
    try:
        check(sys.argv[1])
    except (OSError, ValueError, struct.error) as error:
        raise SystemExit(f"{sys.argv[1]}: {error}")
