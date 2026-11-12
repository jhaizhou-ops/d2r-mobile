#!/usr/bin/env python3
# 出给 S2Arm 启动器用的「包件」：s2arm_bundle.py <壳 zip> <输出 zip> [标签]
#   包件 = bundle.json（标签、提交号、产物代号）+ shell.zip（壳，带预编的翻译工具）+ src/（这个提交的仓库源码，git archive）。
#   S2Arm 按登记的 SHA-256 下载它、核 bundle.json 的提交号，出包时用 S2Arm 自带的 Python 跑 src/dm/tools/d2rm_pack.py（见 S2Arm pack/jobs/downstream.py）。
#   工作树有没提交的改动就不出：包件里的源码必须就是记下的那个提交。
# Make the "bundle" for the S2Arm launcher: s2arm_bundle.py <shell zip> <output zip> [tag]
#   bundle = bundle.json (tag, commit, product codename) + shell.zip (the shell, with the prebuilt translator) + src/ (this commit's repository source, git archive).
#   S2Arm downloads it by its registered SHA-256, checks the commit in bundle.json, and when packing runs src/dm/tools/d2rm_pack.py with its own bundled Python (see S2Arm pack/jobs/downstream.py).
#   It refuses to run with uncommitted changes in the work tree: the source in the bundle must be exactly the recorded commit.
import hashlib, io, json, os, subprocess, sys, tarfile, zipfile

HERE = os.path.dirname(os.path.abspath(__file__)); REPO = os.path.dirname(os.path.dirname(HERE))


def git(*a): return subprocess.run(['git', '-C', REPO, *a], check=True, stdout=subprocess.PIPE).stdout


def main():
    if len(sys.argv) not in (3, 4): sys.exit('用法：s2arm_bundle.py <壳 zip> <输出 zip> [标签]')
    shell, out = sys.argv[1], os.path.abspath(sys.argv[2])
    if git('status', '--porcelain', '--untracked-files=no').strip(): sys.exit('工作树有没提交的改动：先提交，包件里的源码要与记下的提交一致')
    commit = git('rev-parse', 'HEAD').decode().strip()
    tag = sys.argv[3] if len(sys.argv) == 4 else git('describe', '--tags', '--always').decode().strip()
    product = open(os.path.join(REPO, 'dm', 'PRODUCT'), encoding='utf-8').readline().strip()
    src = tarfile.open(fileobj=io.BytesIO(git('archive', '--format=tar', commit)))
    tmp = out + '.part'
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('bundle.json', json.dumps(dict(tag=tag, commit=commit, product=product), indent=1))
        z.write(shell, 'shell.zip')
        n = 0
        for m in src.getmembers():
            if not m.isfile(): continue
            zi = zipfile.ZipInfo('src/' + m.name, date_time=(2026, 1, 1, 0, 0, 0)); zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = (0o755 if m.mode & 0o111 else 0o644) << 16
            z.writestr(zi, src.extractfile(m).read()); n += 1
    os.replace(tmp, out)
    h = hashlib.sha256(open(out, 'rb').read()).hexdigest()
    print('包件：%s（%.1f MB，源码 %d 个文件，提交 %s，标签 %s）\nsha256 %s' % (out, os.path.getsize(out) / 1e6, n, commit[:12], tag, h))


if __name__ == '__main__': main()
