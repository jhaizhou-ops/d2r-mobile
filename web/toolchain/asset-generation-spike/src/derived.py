"""通过未修改的 shell_stubs 函数提取完整运行时表。样例模式检查独立预期；生成模式绑定源码哈希并派生计数。生成收据只证明来源，不是独立一致性或模板兼容性证据。只有已审计解析器将输入 C 视为文本，不编译或执行内容。
Complete runtime table extraction through unchanged shell_stubs functions.

Fixture mode checks independent expectations; generated mode binds source hashes and derives counts.
Generated receipts are provenance, not independent parity or shell compatibility evidence.
Only the audited parser sees input C as text; nothing is compiled or executed.
"""

import hashlib
import io
import json

import native_functions as native

SOURCE_NAMES = ("dm_hud_exp.h", "dm_vault_tab.h", "dm_touch_native_art.c")
TAGS = ("EXP", "VTPART", "VTROW", "VTSET", "TAROW")
SOURCE_LIMIT = 2 * 1024 * 1024
OUTPUT_LIMIT = 8 * 1024 * 1024


def require(condition, code):
    if not condition:
        raise ValueError(code)


class BoundedOutput(io.TextIOBase):
    """仅追加的 UTF-8 接收端，在保留每个有界块之前拒绝溢出。
    Append-only UTF-8 sink; reject overflow before retaining each bounded chunk."""

    def __init__(self, limit=OUTPUT_LIMIT):
        super().__init__()
        self.limit = limit
        self.buffer = bytearray()

    def write(self, text):
        if self.closed:
            raise ValueError("write to closed output")
        for offset in range(0, len(text), 1024):
            chunk = text[offset : offset + 1024].encode("utf-8")
            require(len(self.buffer) + len(chunk) <= self.limit, "DERIVED_OUTPUT_LIMIT")
            self.buffer.extend(chunk)
        return len(text)


def generate(payload, options):
    require(len(payload) <= 8 * 1024 * 1024, "DERIVED_INPUT_LIMIT")
    sources = json.loads(payload.decode("utf-8"))
    require(
        isinstance(sources, dict) and set(sources) == set(SOURCE_NAMES),
        "DERIVED_SOURCE_SET",
    )
    generated = options.get("mode") == "generated"
    if generated:
        require(set(options) == {"mode", "expectedSourceSha256"}, "DERIVED_OPTIONS")
    expected = options.get("expectedCounts")
    hashes = options.get("expectedSourceSha256")
    if not generated:
        require(
            isinstance(expected, dict) and set(expected) == set(TAGS),
            "DERIVED_COUNTS_REQUIRED",
        )
    require(
        isinstance(hashes, dict) and set(hashes) == set(SOURCE_NAMES),
        "DERIVED_HASHES_REQUIRED",
    )
    if not generated:
        require(
            all(type(v) is int and 0 < v <= 100_000 for v in expected.values()),
            "DERIVED_COUNTS_RANGE",
        )
        require(
            expected["EXP"] == 100 and expected["VTPART"] <= 256 and expected["VTSET"] <= 32768,
            "DERIVED_COUNTS_RANGE",
        )
    for name in SOURCE_NAMES:
        require(isinstance(sources[name], str), "DERIVED_SOURCE_TYPE")
        encoded = sources[name].encode("utf-8")
        require(len(encoded) <= SOURCE_LIMIT, "DERIVED_SOURCE_LIMIT")
        actual_hash = hashlib.sha256(encoded).hexdigest()
        require(hashes[name] == actual_hash, "DERIVED_SOURCE_HASH")

    source_paths = dict(zip(native.paths("/input"), (sources[name] for name in SOURCE_NAMES)))
    output = BoundedOutput()

    def memory_open(path, mode="r", **_kwargs):
        if path in source_paths and mode == "r":
            return io.StringIO(source_paths[path])
        if path == "/output/derived.tsv" and mode == "w":
            return output
        raise ValueError("DERIVED_IO_BOUNDARY")

    # 本模块只用于串行线程队列，不猴子补丁修改 builtins。
    # This module is used only by the serial worker queue. No builtins monkeypatch.
    native.open = memory_open
    try:
        exp, parts, rows, sets, trows, _ = native.parse("/input")
        counts = dict(zip(TAGS, map(len, (exp, parts, rows, sets, trows))))
        require(
            all(0 < n <= 100_000 for n in counts.values())
            and counts["EXP"] == 100
            and counts["VTPART"] <= 256
            and counts["VTSET"] <= 32768,
            "DERIVED_COUNTS_RANGE",
        )
        if not generated:
            require(counts == expected, "DERIVED_COUNT_MISMATCH")
        require(all(0 <= n <= 0xFFFFFFFF for n in exp), "DERIVED_EXP_RANGE")
        require(
            all(
                0 <= r[0] <= 1
                and 0 <= r[1] < len(parts)
                and 0 <= r[2] <= 2
                and 0 <= r[3] <= 65535
                and 0 <= r[4] <= 65535
                and -1 <= r[5] < len(sets)
                for r in rows
            ),
            "DERIVED_VAULT_RANGE",
        )
        require(
            all(
                0 <= r[0] <= 0xFFFFFFFF and -1 <= r[1] <= 0x7FFFFFFF and -1 <= r[2] <= 0x7FFFFFFF
                for r in trows
            ),
            "DERIVED_SKILL_RANGE",
        )
        require(len({r[0] for r in trows}) == len(trows), "DERIVED_DUPLICATE_SKILL")
        fields = parts + sets + [text for row in rows for text in row[6:]]
        require(
            all(not any(ord(c) < 32 or ord(c) == 127 for c in text) for text in fields),
            "DERIVED_CONTROL_CHARACTER",
        )
        emitted_counts = native.data("/input", "/output/derived.tsv")
        require(tuple(counts.values()) == emitted_counts, "DERIVED_EMISSION_COUNTS")
        result = bytes(output.buffer)
        require(len(result) <= OUTPUT_LIMIT, "DERIVED_OUTPUT_LIMIT")
        require(
            all(len(line) <= 8190 for line in result.splitlines()),
            "DERIVED_RUNTIME_LINE_LIMIT",
        )
        return {
            "files": {"derived.tsv": result},
            "counts": counts,
            "sourceSha256": dict(hashes),
            "evidence": "generated-source-receipt"
            if generated
            else "expected-source-and-count-match",
        }
    finally:
        del native.open
