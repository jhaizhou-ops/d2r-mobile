"""使用原始 CASC 函数的有界序列化器，不访问文件系统或网络。JS 在调用此纯操作前解析异步认证记录；载荷依次连接配置、解码的 VFS1/根及有序替换项。选项指定精确连续段，不允许稀疏或补零输入。
Bounded serializer using original CASC functions, no filesystem/network access.

JS resolves asynchronous authenticated records before calling this pure operation.
Payload is concatenated config, decoded VFS1/root, then ordered replacements.
Options identify exact contiguous segments; no sparse or zero-filled input exists.
"""

import hashlib
import re
import struct
import casc_overlay_original as original

MAX_INPUT = 64 * 1024 * 1024
MAX_OUTPUT = 64 * 1024 * 1024
MAX_TABLE = 16 * 1024 * 1024
MAX_REPLACEMENT = 16 * 1024 * 1024
MAX_JOBS = 4096


def fail():
    raise ValueError("CASC_OVERLAY_INVALID")


def table(raw):
    if (
        not 46 <= len(raw) <= MAX_TABLE
        or raw[:4] != b"TVFS"
        or raw[4] != 1
        or raw[5] < 46
        or raw[5] > len(raw)
        or raw[6:8] != b"\x10\x00"
        or struct.unpack_from(">I", raw, 8)[0] != 10
    ):
        fail()
    # 只支持原始单字节编码规范索引及固定 25 字节 CFT 布局。
    # Only original one-byte encoding-spec index / fixed 25-byte CFT layout.
    path, path_len, vfs, vfs_len, cft, cft_len = struct.unpack_from(">6I", raw, 12)
    est, est_len = struct.unpack_from(">2I", raw, 38)
    for start, size in (
        (path, path_len),
        (vfs, vfs_len),
        (cft, cft_len),
        (est, est_len),
    ):
        if start < 46 or start + size > len(raw):
            fail()
    if not 0 < est_len <= 255 or cft_len % 25:
        fail()
    spans = sorted(
        (start, start + size)
        for start, size in (
            (path, path_len),
            (vfs, vfs_len),
            (cft, cft_len),
            (est, est_len),
        )
        if size
    )
    if any(a[1] > b[0] for a, b in zip(spans, spans[1:])):
        fail()
    specs = bytes(raw[est : est + est_len]).split(b"\0")
    for pos in range(cft, cft + cft_len, 25):
        index = raw[pos + 20]
        if index >= len(specs) or specs[index] not in (b"n", b"z", b"b:256K*=z"):
            fail()
    return cft, cft + cft_len, specs


def path_checked(path):
    if not isinstance(path, str) or len(path.encode()) > 4096 or original.norm_path(path) != path:
        fail()
    if any(x in ("", ".", "..") for x in path.split("/")) or any(
        ord(c) < 32 or c in ":\\" for c in path
    ):
        fail()
    return path


def generate(payload, options):
    if not isinstance(payload, bytes) or len(payload) > MAX_INPUT or not isinstance(options, dict):
        fail()
    parts = options.get("parts")
    jobs = options.get("jobs")
    location = options.get("location")
    if (
        not isinstance(parts, list)
        or not isinstance(jobs, list)
        or not 1 <= len(jobs) <= MAX_JOBS
        or len(parts) != 3 + len(jobs)
    ):
        fail()
    if (
        not isinstance(location, list)
        or len(location) != 2
        or any(type(x) is not int or not 0 <= x <= 255 for x in location)
        or location[1] == 0
    ):
        fail()
    slices, cursor = [], 0
    for index, length in enumerate(parts):
        limit = 1024 * 1024 if index == 0 else MAX_TABLE if index < 3 else MAX_REPLACEMENT
        if type(length) is not int or not 0 <= length <= limit or cursor + length > len(payload):
            fail()
        slices.append(payload[cursor : cursor + length])
        cursor += length
    if cursor != len(payload):
        fail()
    lines = slices[0].decode("utf-8").splitlines(keepends=True)
    values = {}
    for line in lines:
        if " = " in line and not line.startswith("#"):
            key, value = line.rstrip("\n").split(" = ", 1)
            if key in values:
                fail()
            values[key] = value
    for name, raw in (("vfs-1", slices[1]), ("vfs-root", slices[2])):
        if not re.fullmatch(r"[0-9a-fA-F]{32} [0-9a-fA-F]{32}", values.get(name, "")):
            fail()
        if not re.fullmatch(r"[0-9]+ [0-9]+", values.get(name + "-size", "")):
            fail()
        if hashlib.md5(raw).hexdigest() != values[name].split()[0].lower() or len(raw) != int(
            values[name + "-size"].split()[0]
        ):
            fail()
    vfs, root = bytearray(slices[1]), bytearray(slices[2])
    cft, end, specs = table(vfs)
    root_cft, root_end, _ = table(root)
    new_data = bytearray()
    inventory = []
    seen = set()

    def append(content, spec="b:256K*=z"):
        # 先压缩有界条目再保留，防护累计输出大小。
        # Compress a bounded item before retaining it; guard cumulative output.
        encoded = original.encode_by_espec(bytes(content), spec)
        if len(new_data) + len(encoded) > MAX_OUTPUT:
            fail()
        ekey = original.ekey_for(encoded, *location, len(new_data))
        new_data.extend(encoded)
        return ekey, len(content), len(encoded)

    additions = {}
    # 与原生 rewrite_tvfs 一样精确保留调用方顺序；即使输入交错，新增项也在替换项之后，其 CFT 顺序由原生代码排序。
    # Preserve caller order exactly, as native rewrite_tvfs does. Additions follow
    # replacements even if interleaved in input; their CFT order is sorted natively.
    for want_add in (False, True):
        for job, content in zip(jobs, slices[3:]):
            if not isinstance(job, dict) or type(job.get("add", False)) is not bool:
                fail()
            if job.get("add", False) != want_add:
                continue
            logical = path_checked(job.get("path"))
            if logical in seen:
                fail()
            seen.add(logical)
            if want_add:
                if job.get("oldEkey") is not None:
                    fail()
                spec_index = next((i for i, spec in enumerate(specs) if spec == b"z"), 0)
                pos = None
            else:
                old = job.get("oldEkey", "")
                if not isinstance(old, str) or not re.fullmatch(r"[0-9a-fA-F]{32}", old):
                    fail()
                row = [""] * 11
                row[6] = old
                pos = original.cft_position(vfs, cft, end, {logical: row}, logical)
                if (pos - cft) % 25 or pos + 25 > end:
                    fail()
                spec_index = vfs[pos + 20]
            ekey, decoded, encoded = append(content, specs[spec_index].decode())
            if want_add:
                additions[logical] = (ekey, encoded, spec_index, decoded)
            else:
                vfs[pos : pos + 16] = ekey
                original.set_cft_sizes(vfs, pos, encoded, decoded)
            inventory.append(
                {
                    "path": logical,
                    "add": want_add,
                    "ekey": ekey.hex(),
                    "encodedSize": encoded,
                    "decodedSize": decoded,
                }
            )
    if additions:
        # 进入原始递归路径遍历前限制深度；拒绝无效目录范围，不依赖 Python 切片补零。
        # Bound recursion before original recursive path walking. Reject invalid
        # directory extents rather than relying on Python slice zero-extension.
        validate_path_tree(vfs)
        vfs = original.add_paths(vfs, additions)
        table(vfs)
    vfs_ekey, vd, ve = append(vfs)
    old = bytes.fromhex(values["vfs-1"].split()[1])
    hits = [i for i in range(root_cft, root_end, 25) if root[i : i + 16] == old]
    if len(hits) != 1:
        fail()
    pos = hits[0]
    root[pos : pos + 16] = vfs_ekey
    original.set_cft_sizes(root, pos, ve, vd)
    root_ekey, rd, re_size = append(root)
    lines = original.replace_config(
        lines, "vfs-1", hashlib.md5(vfs).hexdigest() + " " + vfs_ekey.hex()
    )
    lines = original.replace_config(lines, "vfs-1-size", f"{vd} {ve}")
    root_value = hashlib.md5(root).hexdigest() + " " + root_ekey.hex()
    for name in ("vfs-root", "vfs-2"):
        if values.get(name) == values["vfs-root"]:
            if name + "-size" not in values:
                fail()
            lines = original.replace_config(lines, name, root_value)
            lines = original.replace_config(lines, name + "-size", f"{rd} {re_size}")
    config = "".join(lines).encode()
    if len(config) + len(new_data) > MAX_OUTPUT:
        fail()
    return {
        "files": {
            "data/.build.config": config,
            f"data/{location[0]:02d}-{location[1]:08d}.data": bytes(new_data),
        },
        "inventory": inventory,
    }


def validate_path_tree(raw):
    start, size = struct.unpack_from(">II", raw, 12)
    stack = [(start, start + size, 0)]
    count = 0
    while stack:
        pos, end, depth = stack.pop()
        if depth > 128:
            fail()
        while pos < end:
            old = pos
            pos, _, _, _, value = original.path_entry(raw, pos)
            count += 1
            if pos <= old or pos > end or count > 1000000:
                fail()
            if value is not None and value & 0x80000000:
                stop = pos + (value & 0x7FFFFFFF) - 4
                if stop < pos or stop > end:
                    fail()
                stack.append((pos, stop, depth + 1))
                pos = stop
