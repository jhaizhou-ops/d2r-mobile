#!/usr/bin/env python3
# 把改写后的模块映像直接写成一个苹果格式的动态库 Guest.framework/Guest：只装段，没有符号，没有代码之外的任何东西。
# 只用 Python 标准库，不调用编译器、链接器或任何苹果工具，在 macOS / Linux / Windows 上产出逐字节相同的文件。
# 给谁用：壳安装包（ios/tools/shell_build.sh）不含游戏程序；使用者用自己的游戏文件改写出模块映像后，由 d2rm_pack.py 调本脚本
# 写出这个库、放进安装包的 Frameworks/，再用自签工具整包签名。宿主的装载器（host/loader.c 的 DM_SHELL 分支）加载它并按节名取段。
#
# 节的布局与内嵌做法（ios/tools/gen_embed.py）一致，装载器不用分两套：
#   __TEXT,__dm<名>x0   原 text：[0, 圆整16K(TEXT_END))
#   __DM<名>W,__img     可写段：[圆整16K(TEXT_END), PATCH_OFF)
#   __TEXT,__dm<名>x1   补丁区：[PATCH_OFF, 圆整16K(文件尾))
# 文件格式上要注意的三点（都是真机 / Mac 上实测出来的）：
#   · 必须声明依赖 /usr/lib/libSystem.B.dylib，否则系统拒绝加载（「must link with at least libSystem.dylib」）；
#   · 每个节按 16K 对齐、整页大小：装载器用 vm_remap 按整页取；
#   · __LINKEDIT 放在最后，末尾留一块预留的代码签名区，里面是链接器式的自签（CS_LINKER_SIGNED，与 ld 写的同类），空间充裕，重签工具可原地写入自己的签名；
#     zsign（LiveContainer 等设备端签名器）没法给这个文件新增签名命令，因为它只靠名为 __text 的节找装载命令的空闲空间，而 Guest 没有这个节。
# 用法：guest_dylib.py <产物目录> <输出目录> <模块名...>     输出 <输出目录>/Guest.framework/{Guest,Info.plist}
# Write the rewritten module images directly as one Apple-format dynamic library Guest.framework/Guest: it holds only segments, no symbols, and nothing other than the code.
# Uses only the Python standard library, calls no compiler, linker or any Apple tool, and produces byte-for-byte identical files on macOS / Linux / Windows.
# Who it is for: the shell install package (ios/tools/shell_build.sh) contains no game program; after users rewrite the module images from their own game files, d2rm_pack.py calls this script
# to write this library and put it into the install package's Frameworks/, and then the whole package is signed with a sideloading tool. The host's loader (the DM_SHELL branch of host/loader.c) loads it and fetches the segments by section name.
# The section layout matches the embedded approach (ios/tools/gen_embed.py), so the loader does not need two variants:
# __TEXT,__dm<name>x0   original text: [0, round16K(TEXT_END))
# __DM<name>W,__img     writable segment: [round16K(TEXT_END), PATCH_OFF)
# __TEXT,__dm<name>x1   patch area: [PATCH_OFF, round16K(end of file))
# Three points to note about the file format (all found by measurement on a real device / Mac):
# · a dependency on /usr/lib/libSystem.B.dylib must be declared, otherwise the system refuses to load it ("must link with at least libSystem.dylib");
# · every section is aligned to 16K and is a whole number of pages: the loader takes whole pages with vm_remap;
# · __LINKEDIT goes last and ends with a reserved code-signature area holding a linker-signed ad-hoc signature (CS_LINKER_SIGNED, the same kind ld writes) with room to spare, so a re-signing tool can write its signature in place;
# zsign (LiveContainer and other on-device signers) cannot add a signature command to this file, because it only finds free header space via a section named __text, which Guest does not have.
# Usage: guest_dylib.py <build output directory> <output directory> <module name...>     writes <output directory>/Guest.framework/{Guest,Info.plist}
import hashlib, os, struct, sys

PG = 0x4000
MIN_IOS = 26 << 16   # 与工程的最低系统版本一致（ios/project.yml） | matches the project's minimum system version (ios/project.yml)


def r16(x): return (x + PG - 1) & ~(PG - 1)


def s16(s):
    b = s.encode(); assert len(b) <= 16, s; return b.ljust(16, b'\0')


def segment(name, addr, size, fileoff, filesize, prot, sects):
    b = struct.pack('<II16sQQQQiiII', 0x19, 72 + 80 * len(sects), s16(name), addr, size, fileoff, filesize, prot, prot, len(sects), 0)
    for sn, a, sz, flags in sects:
        b += struct.pack('<16s16sQQIIIIIIII', s16(sn), s16(name), a, sz, a, 14, 0, 0, flags, 0, 0, 0)   # 文件偏移 = 地址；对齐 2^14 | file offset = address; alignment 2^14
    return b


def dylib_cmd(cmd, name):
    name += b'\0' * (8 - len(name) % 8)
    return struct.pack('<IIIIII', cmd, 24 + len(name), 24, 2, 0x10000, 0x10000) + name


def adhoc_sig(data, text_size):
    # 苹果 cs_blobs.h 的 SuperBlob + CodeDirectory 0x20400，SHA-256 页哈希；用链接器式（CS_LINKER_SIGNED），重签工具不用强制标志就能替换它
    # Apple cs_blobs.h SuperBlob + CodeDirectory 0x20400 with SHA-256 page hashes; linker-signed (CS_LINKER_SIGNED), so re-signing tools replace it without needing a force flag
    n = (len(data) + 4095) // 4096; ident = b'Guest\0'; hdr = struct.calcsize('>IIIIIIIIIBBBBIIIIQQQQ'); assert hdr == 88
    cd = struct.pack('>IIIIIIIIIBBBBIIIIQQQQ', 0xfade0c02, hdr + len(ident) + 32 * n, 0x20400, 0x20002, hdr + len(ident), hdr, 0, n, len(data), 32, 2, 0, 12, 0, 0, 0, 0, 0, 0, text_size, 0)
    cd += ident + b''.join(hashlib.sha256(data[i:i + 4096]).digest() for i in range(0, len(data), 4096))
    return struct.pack('>IIIII', 0xfade0cc0, 20 + len(cd), 1, 0, 20) + cd


def build(out, names):
    text, wsegs = [], []
    for n in names:
        hdr = dict(l[2:].split() for l in open(os.path.join(out, n + '.fixups.tsv')) if l.startswith('# '))
        te, po = int(hdr['TEXT_END'], 16), int(hdr['PATCH_OFF'], 16)
        img = open(os.path.join(out, n + '.patched.bin'), 'rb').read(); sz = len(img); a, end = r16(te), r16(sz)
        assert po % PG == 0 and a <= po <= end, (n, hex(te), hex(po), hex(sz))
        cut = lambda lo, hi: img[lo:min(hi, sz)].ljust(hi - lo, b'\0')
        text += [('__dm%sx0' % n, cut(0, a)), ('__dm%sx1' % n, cut(po, end))]
        wsegs.append(('__DM%sW' % n, cut(a, po)))
    off = PG; tsects = []; body = b''          # 第一页：文件头与装载命令 | first page: file header and load commands
    for sn, b in text:
        tsects.append((sn, off, len(b), 0x80000400)); body += b; off += len(b)   # 纯指令节 | pure instructions section
    cmds = [segment('__TEXT', 0, off, 0, off, 5, tsects)]; tsz = off
    for sn, b in wsegs:
        cmds.append(segment(sn, off, len(b), off, len(b), 3, [('__img', off, len(b), 0)])); body += b; off += len(b)
    strtab = b'\0' * 8
    cs_off = (off + len(strtab) + 15) & ~15; cs_size = (((cs_off // 4096 + 1) * 52 + 4095) & ~4095) + 0x40000   # 签名区：16 字节对齐；留足 SHA-1 + SHA-256 页哈希（52 字节/页）外加 256K | signature area: 16-byte aligned; room for SHA-1 + SHA-256 page hashes (52 bytes/page) plus 256K
    uuid = hashlib.sha256(body).digest()[:16]   # 由内容定：同样的输入在哪台机器上出的库都逐字节相同 | determined by content: the same input yields a byte-for-byte identical library on whatever machine it is built
    cmds += [segment('__LINKEDIT', off, r16(cs_off + cs_size - off), off, cs_off + cs_size - off, 1, []),
             dylib_cmd(0xd, b'@rpath/Guest.framework/Guest'),                    # LC_ID_DYLIB
             struct.pack('<II10I', 0x80000022, 48, *([0] * 10)),                 # LC_DYLD_INFO_ONLY：没有重定位、没有绑定、没有导出 | LC_DYLD_INFO_ONLY: no relocations, no bindings, no exports
             struct.pack('<IIIIII', 0x2, 24, off, 0, off, len(strtab)),          # LC_SYMTAB：0 个符号 | LC_SYMTAB: 0 symbols
             struct.pack('<II18I', 0xb, 80, *([0] * 18)),                        # LC_DYSYMTAB
             struct.pack('<II16s', 0x1b, 24, uuid),                              # LC_UUID
             struct.pack('<IIIIII', 0x32, 24, 2, MIN_IOS, MIN_IOS, 0),           # LC_BUILD_VERSION：平台 2 = iOS | LC_BUILD_VERSION: platform 2 = iOS
             dylib_cmd(0xc, b'/usr/lib/libSystem.B.dylib'),                      # LC_LOAD_DYLIB
             struct.pack('<IIII', 0x1d, 16, cs_off, cs_size)]                    # LC_CODE_SIGNATURE：放在最后一条，与 ld 一致 | LC_CODE_SIGNATURE: the last command, as ld places it
    lc = b''.join(cmds); assert 32 + len(lc) <= PG
    mh = struct.pack('<IiiIIIII', 0xfeedfacf, 0x0100000c, 0, 6, len(cmds), len(lc), 0x00100085, 0)   # arm64、动态库 | arm64, dynamic library
    data = ((mh + lc).ljust(PG, b'\0') + body + strtab).ljust(cs_off, b'\0')
    return (data + adhoc_sig(data, tsz)).ljust(cs_off + cs_size, b'\0')   # 签名覆盖 [0, cs_off) | the signature covers [0, cs_off)


INFO_PLIST = '''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>Guest</string>
<key>CFBundleIdentifier</key><string>%s</string>
<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
<key>CFBundleName</key><string>Guest</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>CFBundleVersion</key><string>1</string>
<key>MinimumOSVersion</key><string>26.0</string>
<key>CFBundleSupportedPlatforms</key><array><string>iPhoneOS</string></array>
</dict></plist>
'''


def write(out, dst, names, bundle_id='local.guest'):
    fw = os.path.join(dst, 'Guest.framework'); os.makedirs(fw, exist_ok=True)
    data = build(out, names)
    with open(os.path.join(fw, 'Guest'), 'wb') as f: f.write(data)
    with open(os.path.join(fw, 'Info.plist'), 'w', encoding='utf-8', newline='\n') as f: f.write(INFO_PLIST % bundle_id)
    return fw, len(data)


if __name__ == '__main__':
    if len(sys.argv) < 4: sys.exit(__doc__ or '用法：guest_dylib.py <产物目录> <输出目录> <模块名...>')
    fw, n = write(sys.argv[1], sys.argv[2], sys.argv[3:])
    print('%s：%d 字节' % (fw, n))
