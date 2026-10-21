#!/bin/bash
# 从你自己的游戏包里取出本项目要的两部分（更新后的程序文件与资源），放进工作区。
# 用法：extract_game.sh <游戏包 .xci 或 .nsp> [<游戏包> …] <prod.keys>
#   游戏包：你自己导出的《暗黑破坏神 II：狱火重生》Switch 版，本体与 3.3.94012 的更新都要有：可以在同一个包里（.xci 常见的「1G+1U」），
#   也可以分开给（本体一个 .nsp、更新一个 .nsp）。eShop 格式的 .nsp 里带的票据（.tik）会自动取出标题密钥，不用另备 title.keys。
#   prod.keys：从你自己的主机导出的密钥文件，放最后。都只在本机读，不拷贝、不上传。
# 产物：$DM_ROOT/data/switch/3.3.94012/Update/{ExeFS,RomFS}（DM_ROOT 缺省 ~/dm-work）。已有这个目录时不覆盖。
# 需要：hactool（开源的 Switch 文件解包工具，https://github.com/SciresM/hactool ，按它的说明 make 出来）。
#   放进 PATH，或用 HACTOOL=<路径> 指给本脚本。磁盘要留出约 20 GB（中间文件 9 GB，做完自动删）。
# Extract the two parts this project needs (the updated program files and the assets) from your own copy of the game and put them into the workspace.
# Usage: extract_game.sh <game dump .xci or .nsp> [<game dump> ...] <prod.keys>
#   Game dump: your own dump of the Switch version of Diablo II: Resurrected; it must contain both the base game and the 3.3.94012 update, either in one package
#   (the common "1G+1U" form for .xci) or as separate packages (one .nsp for the base game, one for the update). Title keys are taken from the tickets (.tik) in eShop-style .nsp files; no title.keys needed.
#   prod.keys: the key file dumped from your own console, given last. Everything is only read on this machine; nothing is copied or uploaded.
# Output: $DM_ROOT/data/switch/3.3.94012/Update/{ExeFS,RomFS} (DM_ROOT defaults to ~/dm-work). An existing directory is not overwritten.
# Requires: hactool (an open-source tool for unpacking Switch files, https://github.com/SciresM/hactool ; build it with make following its instructions).
#   Put it on PATH, or point this script at it with HACTOOL=<path>. Keep about 20 GB of disk free (9 GB of intermediate files, deleted automatically when done).
set -u
[ $# -ge 2 ] || { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
KEYS=${!#}; PKGS=("${@:1:$#-1}"); DM_ROOT=${DM_ROOT:-$HOME/dm-work}; OUT=$DM_ROOT/data/switch/3.3.94012/Update
die() { echo "✗ $*" >&2; exit 1; }
HT=${HACTOOL:-$(command -v hactool || true)}; [ -n "$HT" ] && [ -x "$HT" ] || die "没找到 hactool：装好后放进 PATH，或设 HACTOOL=<路径>（见本文件开头） | hactool not found: install it and put it on PATH, or set HACTOOL=<path> (see the top of this file)"
for PKG in "${PKGS[@]}"; do [ -f "$PKG" ] || die "没有这个游戏包 | no such game package: $PKG"; done; [ -f "$KEYS" ] || die "没有这个密钥文件 | no such key file: $KEYS"
[ ! -e "$OUT" ] || die "已有 ${OUT}，不覆盖。要重做先自己把它移走 | ${OUT} already exists and is not overwritten; move it away yourself to redo the extraction"
# hactool 只从 $HOME/.switch/title.keys 读标题密钥：给它一个临时 HOME，里面的 title.keys = 你原有的那份 + 从票据取的，不动你自己的 ~/.switch。
# 这个 HOME 单独建在系统临时目录里，只有你自己能读，密钥不进工作区；9 GB 的中间文件仍在工作区
# hactool only reads title keys from $HOME/.switch/title.keys: give it a temporary HOME whose title.keys = your existing one + the keys taken from the tickets, leaving your own ~/.switch untouched.
# That HOME is created on its own in the system temporary folder and only you can read it, so keys never land in the workspace; the 9 GB of intermediate files stay in the workspace
ht() { HOME="$KH" "$HT" -k "$KEYS" --disablekeywarns --suppresskeys "$@"; }
T=$DM_ROOT/build/cache/dm/extract.$$; KH=$(umask 077; mktemp -d "${TMPDIR:-/tmp}/dm-keys.XXXXXX") || die "建不了放密钥的临时目录 | cannot create the temporary folder for the keys"
trap 'rm -rf "$T" "$KH"' EXIT; mkdir -p "$T/nca" "$KH/.switch" || die "建不了中间目录 $T | cannot create the intermediate folder $T"
TK=$KH/.switch/title.keys
if [ -f "$HOME/.switch/title.keys" ]; then
  cat "$HOME/.switch/title.keys" > "$TK"; [ -z "$(tail -c1 "$TK")" ] || echo >> "$TK"   # 原文件末尾没换行时补一个，免得下面接上的票据密钥并进它的最后一行 | add a newline if the file does not end with one, so the ticket keys appended below do not join its last line
fi
ln -s "$(cd "$(dirname "$KEYS")" && pwd)/$(basename "$KEYS")" "$KH/.switch/prod.keys"   # 只是链接，免得 hactool 报「prod.keys 不存在」盖住真正的错 | only a link, so hactool does not warn "prod.keys does not exist" over the real error
say() { echo "[$(date +%T)] $*"; }
# 拆不开时：hactool 嫌某一行密钥长度不对，报的是那行的值（一个密钥）。不把值打出来，按值找回是哪个文件的哪个键：prod.keys，或标题密钥（你的 ~/.switch/title.keys 加票据里取的）
# When unpacking fails: if hactool rejects a key line for its length it reports that line's value (a key). Don't print the value; find by it which key of which file it is: prod.keys, or the title keys (your ~/.switch/title.keys plus those taken from the tickets)
keyhint() { local v n f w; v=$(sed -n 's/.*Key (\([0-9a-fA-F]*\).*) must be.*/\1/p' "$1" | head -1); [ -n "$v" ] || return 0
  for f in "$KEYS" "$TK"; do n=$(grep -is "=[[:space:]]*$v" "$f" | head -1 | sed 's/[[:space:]]*=.*//'); [ -n "$n" ] && break; done
  if [ -z "$n" ]; then w="密钥文件里有一行 | a line in a key file"
  elif [ "$f" = "$TK" ]; then w="标题密钥里 $n 这一行（~/.switch/title.keys 或票据） | the title key line $n (~/.switch/title.keys or a ticket)"
  else w="$(basename "$KEYS") 里的 $n  | $n in $(basename "$KEYS")"; fi
  echo "（${w%% | *}长度不对，hactool 不接受：删掉这行或换一份密钥文件 | ${w#* | } has the wrong length and hactool refuses it: remove that line or use another key file）"; }
# hactool 日志的最后一行报错；是密钥长度错时只说是哪一行（keyhint），不原样打出 | the last error line of a hactool log; for a key length error only say which line it is (keyhint), never print it as is
hterr() { if grep -q 'Key (.*) must be' "$1"; then keyhint "$1"; else tail -1 "$1"; fi; }

say "拆游戏包（约 9 GB） | unpacking the game packages (about 9 GB)"
n=0
for PKG in "${PKGS[@]}"; do
  n=$((n + 1)); L=$T/unpack$n.log
  case "$PKG" in
    *.xci|*.XCI) ht -t xci --securedir="$T/nca" "$PKG" > "$L" 2>&1 || die "游戏包拆不开（密钥不对或文件损坏） | cannot unpack the game package (wrong keys or a damaged file): $(hterr "$L")";;
    *.nsp|*.NSP) ht -t pfs0 --outdir="$T/nca" "$PKG" > "$L" 2>&1 || die "游戏包拆不开（密钥不对或文件损坏） | cannot unpack the game package (wrong keys or a damaged file): $(hterr "$L")";;
    *) die "只认 .xci 或 .nsp | only .xci or .nsp packages are accepted: $PKG";;
  esac
done

# 票据（.tik，eShop 格式的 .nsp 才有）里的标题密钥：按签名类型定出票据数据的起点，再取 密钥种类（+0x141）、权利 ID（+0x160）、密钥（+0x40）。
# 通用票据（种类 0）的密钥能直接用；个人票据（种类 1）绑定导出它的那台主机，取不出来——你的 ~/.switch/title.keys 里已有这个标题的密钥时照样能拆。
# Title keys from the tickets (.tik, only in eShop-style .nsp files): the signature type gives where the ticket data starts, then read the key type (+0x141), the rights ID (+0x160) and the key (+0x40).
# A common ticket (type 0) holds a key that can be used directly; a personalized ticket (type 1) is bound to the console it was dumped from and cannot be read - extraction still works if your ~/.switch/title.keys has the key for that title.
tk=0
for tik in "$T"/nca/*.tik; do
  [ -f "$tik" ] || continue
  case "$(od -An -tx4 -N4 "$tik" | tr -d ' ')" in
    00010000|00010003) base=$((0x240));;   # RSA-4096 签名 | RSA-4096 signature
    00010001|00010004) base=$((0x140));;   # RSA-2048 签名 | RSA-2048 signature
    00010002|00010005) base=$((0x80));;    # ECDSA 签名 | ECDSA signature
    *) say "跳过认不出签名类型的票据 $(basename "$tik") | skipping a ticket with an unknown signature type"; continue;;
  esac
  kind=$(od -An -tu1 -j $((base + 0x141)) -N1 "$tik" | tr -d ' ')
  rid=$(od -An -tx1 -j $((base + 0x160)) -N16 "$tik" | tr -d ' \n')
  if [ "$kind" != 0 ]; then say "票据 $(basename "$tik") 是个人票据（绑定主机），取不出标题密钥 | personalized ticket, its title key cannot be read"; continue; fi
  echo "$rid = $(od -An -tx1 -j $((base + 0x40)) -N16 "$tik" | tr -d ' \n')" >> "$TK"; tk=$((tk + 1))
done
[ $tk = 0 ] || say "从游戏包的票据取到 $tk 个标题密钥 | took $tk title keys from the tickets in the game packages"

# 找出本体与更新的程序内容：按每个内容文件自己报的类型与程序号认，不靠文件名。
# 更新的程序内容报的也是本体的程序号；区别是它的资源段是「补丁」格式（BKTR），要叠在本体上才读得出
# Find the program content of the base game and of the update: identify them by the type and program id each content file reports, not by file name.
# The update's program content also reports the base game's program id; the difference is that its asset section is in "patch" format (BKTR) and can only be read when layered on top of the base game
BASE=; UPD=; NERR=
for f in "$T"/nca/*.nca; do
  case "$f" in *.cnmt.nca) continue;; esac
  info=$(ht -t nca "$f" 2>&1) || { [ -n "$NERR" ] || { echo "$info" > "$T/nca_err.log"; NERR=$(hterr "$T/nca_err.log"); }; }   # 记下第一个读不开的内容文件的报错 | keep the error of the first content file that cannot be read
  echo "$info" | grep -q "Content Type: *Program" || continue
  echo "$info" | grep -q "^Title ID: *0100726014352000" || continue
  if echo "$info" | grep -q "BKTR"; then UPD=$f; else BASE=$f; fi
done
[ -n "$BASE" ] || die "游戏包里没找到这个游戏的本体（本体与更新分开的话，两个包都要给） | the base game is not in the game package (if base game and update are separate packages, give both)${NERR:+；hactool 读内容文件时报错 | hactool error while reading the content files: $NERR}"
[ -n "$UPD" ] || die "游戏包里没有更新：本项目要 3.3.94012 的更新，只有本体不行（更新是单独的包的话，一起给上） | the package has no update: this project needs the 3.3.94012 update, the base game alone is not enough (if the update is a separate package, give it too)"

say "取程序文件与资源（约 9 GB） | extracting the program files and the assets (about 9 GB)"
mkdir -p "$OUT.part" || die "建不了 | cannot create $OUT.part"
ht -t nca --basenca="$BASE" --exefsdir="$OUT.part/ExeFS" --romfsdir="$OUT.part/RomFS" "$UPD" > "$T/extract.log" 2>&1 || { rm -rf "$OUT.part"; die "更新内容取不出来 | cannot extract the update content: $(hterr "$T/extract.log")"; }

# 版本核对：三个程序文件的校验值必须是 3.3.94012 的
# Version check: the checksums of the three program files must be those of 3.3.94012
ok=1
while read -r want name; do
  got=$(shasum -a 256 "$OUT.part/ExeFS/$name" 2> /dev/null | cut -c1-64)
  if [ "$got" = "$want" ]; then echo "  ✓ $name"; else echo "  ✗ $name 不是 3.3.94012 的 | is not the 3.3.94012 one (${got:-文件不在 | file missing})"; ok=0; fi
done <<SUMS
55fd909bb7230b002357e5b0d8499c11f06888f345c49e69eb4734df44a27d0e main
1c9f198a91c0bcfee5cf8f5b53fb941895e91334dfb766ce79fc68576797f017 sdk
67a38fcf89cd13e040a021eb3d157072f38ebf6680a907874c35c7b231b5d4e2 subsdk0
SUMS
[ -f "$OUT.part/RomFS/data/build_manifest.psv" ] || { echo "  ✗ 资源里没有 | the assets have no data/build_manifest.psv"; ok=0; }
[ "$ok" = 1 ] || { rm -rf "$OUT.part"; die "版本不对：游戏包里的更新不是 3.3.94012 | wrong version: the update in the game packages is not 3.3.94012"; }
mv "$OUT.part" "$OUT" || die "改名到 $OUT 没成 | renaming to $OUT failed"
say "好了：${OUT}。接着在仓库根目录运行 ./setup.sh | done: ${OUT}. Next, run ./setup.sh in the repository root"
