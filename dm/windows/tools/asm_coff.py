# Mach-O 汇编转 ARM64 COFF；仅转换符号和段语法，线程入口由 Windows 专用栈桥提供。
# Convert Mach-O assembly to ARM64 COFF, changing symbols and sections only; Windows supplies its own thread stack bridge.
import argparse
from pathlib import Path
import re


def convert(text, thread_bridge=False):
    if thread_bridge:
        marker = "    .globl _dm_thread_enter"
        if marker not in text:
            raise ValueError("entry.S is missing dm_thread_enter")
        text = text[:text.index(marker)]
    out = []
    for line in text.splitlines():
        code, separator, comment = line.partition("//")
        tokens = re.split(r'("(?:\\.|[^"\\])*")', code)
        for i in range(0, len(tokens), 2):
            token = re.sub(r"(\w+)@PAGEOFF\b", r":lo12:\1", tokens[i])
            token = re.sub(r"(\w+)@PAGE\b", r"\1", token)
            tokens[i] = re.sub(r"(?<![\w.$])_(\w+)", r"\1", token)
        code = "".join(tokens)
        if thread_bridge:
            code = re.sub(r"\bbl\s+gate_dispatch\b", "bl dm_windows_gate_dispatch", code)
        code = re.sub(r"^(\s*)\.cstring\b", r'\1.section .rdata,"dr"', code)
        code = re.sub(r"^\s*\.private_extern\b.*", "", code)
        out.append(code + separator + comment)
    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--thread-bridge", action="store_true")
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(convert(args.source.read_text(encoding="utf-8"), args.thread_bridge), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
