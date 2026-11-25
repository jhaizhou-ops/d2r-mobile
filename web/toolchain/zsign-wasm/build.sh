#!/usr/bin/env bash
# 固定来源构建浏览器签名核心；不运行第三方预编译程序，不接受签名凭据。
# Build pinned browser signing sources; never run downloaded binaries or accept credentials.
set -euo pipefail
cd "$(dirname "$0")"
command -v emcc >/dev/null
command -v em++ >/dev/null
command -v emar >/dev/null
command -v emranlib >/dev/null
mkdir -p .build dist
emcc --version > .build/compiler-version.txt
if ! head -n 1 .build/compiler-version.txt | grep -Eq '^emcc .* 4\.0\.20([[:space:]]|$)'; then
  printf '%s\n' 'Expected Emscripten 4.0.20' >&2
  exit 1
fi
# 统一读取已审核的固定版本，不使用浮动分支、标签或未标版本的归档。
# One reviewed pin source, never a moving branch/tag or unversioned archive.
pin() { python3 -c 'import json,sys; print(json.load(open("pins.json"))[sys.argv[1]][sys.argv[2]])' "$1" "$2"; }
checkout() {
  local name="$1" dir="$PWD/.build/$1-source" revision
  revision="$(pin "$name" commit)"
  if [[ ! -d "$dir/.git" ]]; then git init -q "$dir"; fi
  test -z "$(git -C "$dir" status --porcelain)"
  if ! git -C "$dir" cat-file -e "$revision^{commit}" 2>/dev/null; then
    git -C "$dir" fetch --depth 1 "$(pin "$name" repository)" "$revision"
  fi
  git -C "$dir" checkout --detach "$revision"
  test "$(git -C "$dir" rev-parse HEAD)" = "$revision"
  test -z "$(git -C "$dir" status --porcelain)"
}
checkout zsign
checkout port
checkout openssl
export SOURCE_DATE_EPOCH="$(git -C .build/zsign-source show -s --format=%ct "$(pin zsign commit)")"
export TZ=UTC LC_ALL=C
# 重新生成独立构建目录，避免配置修改或重试复用过时结果。
# Fresh out-of-tree outputs make configuration changes/retries unambiguous.
rm -rf .build/openssl-build .build/patched
mkdir -p .build/openssl-build
python3 prepare-source.py .build/zsign-source .build/port-source .build/patched
(
  cd .build/openssl-build
  # no-module 静态包含旧版 P12 加密所需的 legacy 提供程序。
  # no-module statically includes the legacy provider needed by older P12 files.
  # 不得启用 no-legacy；仅链接 libcrypto，不引入 TLS、libssl 或 WASI 垫片。
  # Do NOT use no-legacy. We link libcrypto only, never TLS/libssl or WASI shims.
  # OpenSSL prepends CROSS_COMPILE to CC. emconfigure supplies both an em-prefix
  # and an absolute emcc path, producing a nonexistent concatenated path.
  # Configure all tools explicitly, then invoke the generated Makefile directly.
  env -u CROSS_COMPILE CC="$(command -v emcc)" AR="$(command -v emar)" \
    RANLIB="$(command -v emranlib)" perl ../openssl-source/Configure linux-generic32 \
    no-asm no-shared no-module no-dso no-engine no-threads no-async \
    no-sock no-tests no-apps no-ui-console \
    -DOPENSSL_NO_SECURE_MEMORY
  make -j2 build_libs
)
S="$PWD/.build/patched"
O="$PWD/.build/openssl-build"
# 排除命令行、归档、元数据、OCSP、外部 WASI ABI 垫片和网络封装。
# No CLI, archive, metadata, OCSP, foreign WASI ABI shims or network wrapper.
SOURCES=(archo.cpp macho.cpp openssl.cpp signing.cpp bundle.cpp zsign_export.cpp
  common/fs.cpp common/json.cpp common/log.cpp common/sha.cpp common/timer.cpp common/util.cpp)
INPUTS=()
for source in "${SOURCES[@]}"; do INPUTS+=("$S/$source"); done
em++ --no-entry -std=c++17 -O3 -Wno-unused-result \
  -I"$S" -I"$S/common" -I"$O/include" -I"$PWD/.build/openssl-source/include" \
  "${INPUTS[@]}" "$O/libcrypto.a" \
  -sERROR_ON_UNDEFINED_SYMBOLS=1 -sALLOW_MEMORY_GROWTH=1 \
  -sINITIAL_MEMORY=33554432 -sMAXIMUM_MEMORY=536870912 -sSTACK_SIZE=1048576 \
  -sENVIRONMENT="${D2R_EMSCRIPTEN_ENVIRONMENT:-worker}" \
  -sFORCE_FILESYSTEM=1 -sMODULARIZE=1 -sEXPORT_ES6=1 -sDYNAMIC_EXECUTION=0 \
  -sEXPORT_NAME=createZsignModule -sINVOKE_RUN=0 -sFILESYSTEM=1 \
  -sEXPORTED_FUNCTIONS='["_zsign_sign_bundle","_malloc","_free"]' \
  -sEXPORTED_RUNTIME_METHODS='["FS","MEMFS","ccall","UTF8ToString","HEAPU8"]' \
  -o dist/zsign.mjs
cp .build/zsign-source/LICENSE dist/ZSIGN-LICENSE.txt
cp .build/port-source/LICENSE dist/PORT-LICENSE.txt
cp .build/openssl-source/LICENSE.txt dist/OPENSSL-LICENSE.txt
# 保留上游提供的声明文件，并保持输出清单稳定。
# Preserve upstream notice material when shipped, with a stable output inventory.
if [[ -f .build/openssl-source/NOTICE ]]; then
  cp .build/openssl-source/NOTICE dist/OPENSSL-NOTICE.txt
else
  printf '%s\n' 'OpenSSL 3.5.9: see OPENSSL-LICENSE.txt and https://www.openssl.org/ . No upstream NOTICE file in this pinned source.' > dist/OPENSSL-NOTICE.txt
fi
cp pins.json .build/patch-report.json .build/compiler-version.txt dist/
python3 - <<'PY'
import hashlib, json
from pathlib import Path
out = Path('dist')
report = {'sourcePins': json.loads(Path('pins.json').read_text()),
          'browserExecutionVerified': False, 'deviceInstallVerified': False,
          'files': {p.name: {'bytes': p.stat().st_size, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                    for p in sorted(out.iterdir()) if p.is_file() and p.name != 'build-report.json'}}
(out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
PY
