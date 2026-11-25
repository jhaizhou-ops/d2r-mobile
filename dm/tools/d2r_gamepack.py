#!/usr/bin/env python3
# 把游戏数据做成一个可以在手机上直接导入的游戏包（.d2r），同 S2Arm 的 .s2arm：一个不压缩的 PAX tar，第一项是 d2r-pack.json。
#   用法：d2r_gamepack.py <来源> <输出.d2r> [--no-dm] [--split-gb N]
#   来源可以是「D2R Mobile」文件夹（里面 data/ 与可选的 dm/）、data 文件夹本身，或 Switch 导出的 RomFS 目录（里面有 data/）。
#   dm/ 只收 out、shaders、overlay、ui-art、derived.tsv、pack_info.txt（与文件夹导入相同）；内置运行组件的安装包导入时会跳过 dm/。
#   --split-gb N：每 N GB 拆一片（name.d2r.001、.002…），手机上一起选中。缺省不拆。
#   包里有游戏内容，只自己用，不要分发。
# Make game data into one package (.d2r) the phone imports directly, like S2Arm's .s2arm: an uncompressed PAX tar whose first entry is d2r-pack.json.
#   Usage: d2r_gamepack.py <source> <output.d2r> [--no-dm] [--split-gb N]
#   The source is the "D2R Mobile" folder (data/ and an optional dm/), the data folder itself, or a Switch RomFS export (containing data/).
#   dm/ takes only out, shaders, overlay, ui-art, derived.tsv, pack_info.txt (same as the folder import); builds with a bundled runtime skip dm/ on import.
#   --split-gb N: split every N GB (name.d2r.001, .002, ...); select all parts on the phone. Not split by default.
#   The package contains game content: keep it for yourself and do not distribute it.
import io, json, os, sys, tarfile, time

DM_ENTRIES = ['out', 'shaders', 'overlay', 'ui-art', 'derived.tsv', 'pack_info.txt']


class SplitWriter(io.RawIOBase):
    """Write-only file that rolls over to name.001, .002, ... every `limit` bytes (limit 0 = one file)."""
    def __init__(self, path, limit):
        self.path, self.limit, self.n, self.f, self.left, self.paths = path, limit, 0, None, 0, []
        self._next()

    def _next(self):
        if self.f: self.f.close()
        self.n += 1
        p = self.path if not self.limit else '%s.%03d' % (self.path, self.n)
        self.f = open(p, 'wb'); self.paths.append(p); self.left = self.limit or (1 << 62)

    def writable(self): return True

    def write(self, b):
        b = memoryview(b); done = 0
        while done < len(b):
            if self.left == 0: self._next()
            k = min(self.left, len(b) - done)
            self.f.write(b[done:done + k]); done += k; self.left -= k
        return done

    def close(self):
        if self.f: self.f.close(); self.f = None
        super().close()


def find_roots(src):
    """Returns (data_dir, dm_dir or None)."""
    src = os.path.abspath(src)
    if os.path.basename(src).lower() == 'data' and os.path.isdir(src):
        return src, None
    data = next((os.path.join(src, n) for n in os.listdir(src) if n.lower() == 'data' and os.path.isdir(os.path.join(src, n))), None)
    if not data:
        sys.exit('[x] %s 里没有 data 文件夹 | no data folder in %s' % (src, src))
    dm = os.path.join(src, 'dm')
    return data, dm if os.path.isdir(dm) else None


def walk(root):
    for d, dirs, files in os.walk(root):
        dirs.sort()
        for f in sorted(files):
            yield os.path.join(d, f)


def main():
    if sys.stdout.encoding and sys.stdout.encoding.lower() != 'utf-8': sys.stdout.reconfigure(encoding='utf-8')
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__ or open(__file__, encoding='utf-8').read().split('import io')[0]); sys.exit(1)
    src, out = a[0], a[1]
    no_dm = '--no-dm' in a
    split = int(float(a[a.index('--split-gb') + 1]) * (1 << 30)) if '--split-gb' in a else 0
    data, dm = find_roots(src)
    if no_dm: dm = None
    items = [(p, 'data/' + os.path.relpath(p, data).replace(os.sep, '/')) for p in walk(data)]
    if dm:
        for n in DM_ENTRIES:
            p = os.path.join(dm, n)
            if os.path.isfile(p): items.append((p, 'dm/' + n))
            elif os.path.isdir(p): items += [(q, 'dm/' + n + '/' + os.path.relpath(q, p).replace(os.sep, '/')) for q in walk(p)]
    total = sum(os.path.getsize(p) for p, _ in items)
    manifest = json.dumps({'format': 'd2r-pack', 'version': 1,
                           'files': len(items), 'bytes': total, 'hasRuntime': bool(dm)}, ensure_ascii=False, indent=1).encode()
    w = SplitWriter(out, split)
    t0 = time.time(); last = 0; done = 0
    with tarfile.open(fileobj=w, mode='w|', format=tarfile.PAX_FORMAT, bufsize=1 << 20) as tf:
        ti = tarfile.TarInfo('d2r-pack.json'); ti.size = len(manifest); ti.mtime = 0
        tf.addfile(ti, io.BytesIO(manifest))   # 必须是第一项 | must be the first entry
        for p, name in items:
            ti = tf.gettarinfo(p, arcname=name); ti.uid = ti.gid = 0; ti.uname = ti.gname = ''; ti.mode = 0o644
            with open(p, 'rb') as f: tf.addfile(ti, f)
            done += ti.size
            if time.time() - last > 2:
                last = time.time(); print('  %5.1f%%  %d / %d MB' % (100.0 * done / max(1, total), done >> 20, total >> 20), flush=True)
    w.close()
    print('[ok] %d 个文件，%.2f GB，%.0f 秒 → %s' % (len(items), total / (1 << 30), time.time() - t0, ', '.join(w.paths)))
    print('     数据 data/：%s%s' % (data, '；运行包 dm/：' + dm if dm else '（不含运行包）'))


if __name__ == '__main__':
    main()
