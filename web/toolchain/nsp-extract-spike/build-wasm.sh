#!/usr/bin/env bash
# 依据固定 hactool 提交与本地边界源码构建受限 WASM 模块，并保留许可证。
# Build a restricted WASM module from the pinned hactool commit and local boundary sources, preserving licenses.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${NSP_BUILD_DIR:-"$HERE/.build"}
OUT=${NSP_OUTPUT_DIR:-"$HERE/dist"}
PIN=1d64a83450e025622f3468c28fc4164dad2c5ef6
command -v emcc >/dev/null || { echo 'Emscripten must be installed by the CI build environment.' >&2; exit 1; }
mkdir -p "$BUILD" "$OUT"
# 全新源码树可防止误用原生目标文件或重复打补丁。
# Fresh source tree prevents accidental reuse of native objects or double patching.
SRC=$(mktemp -d "$BUILD/hactool.XXXXXX")
git -C "$SRC" init -q
git -C "$SRC" remote add origin https://github.com/SciresM/hactool.git
git -C "$SRC" fetch -q --depth 1 origin "$PIN"
git -C "$SRC" checkout -q --detach FETCH_HEAD
[[ $(git -C "$SRC" rev-parse HEAD) == "$PIN" ]]
# 复用同一固定源码构建可选生产器的隔离散列原语；不接触提取器状态。
# Reuse the pinned source for the optional producer's isolated hashing without touching extractor state.
node "$HERE/../sha256-stream-wasm/build.mjs" --source "$SRC"
node "$HERE/../sha256-stream-wasm/wasm-smoke.mjs"
python3 "$HERE/prepare-source.py" "$SRC"
cat > "$SRC/config.mk" <<CFG
CC = emcc
AR = emar
CFLAGS = -O2 -D_FILE_OFFSET_BITS=64
LDFLAGS =
CFG
emmake make -C "$SRC/mbedtls" lib -j2 CC=emcc AR=emar
# 保持上游文件独立，仅应用已审查的解析器生命周期与头格式边界补丁。
# Keep upstream files distinct and apply only reviewed parser-lifetime and header-format boundary patches.
sources=()
for name in save sha aes extkeys rsa npdm bktr kip packages pki pfs0 hfs0 nca0_romfs romfs utils nax0 nso lz4 nca xci filepath ConvertUTF cJSON; do
  sources+=("$SRC/$name.c")
done
sed -e 's/int main(void)/int nsp_crypto_fixture(void)/' -e '/puts("PASS synthetic/d' "$HERE/crypto-fixture.c" > "$BUILD/crypto-fixture.c"
emcc -O2 -std=gnu11 -D_FILE_OFFSET_BITS=64 -D_DEFAULT_SOURCE \
  -include "$HERE/quiet.h" -I"$SRC" -I"$SRC/mbedtls/include" \
  "${sources[@]}" "$HERE/boundary.c" "$BUILD/crypto-fixture.c" \
  -L"$SRC/mbedtls/library" -lmbedtls -lmbedx509 -lmbedcrypto \
  --no-entry -lworkerfs.js -sMODULARIZE=1 -sEXPORT_ES6=1 "-sENVIRONMENT=${D2R_EMSCRIPTEN_ENVIRONMENT:-worker,node}" \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 -sMAXIMUM_MEMORY=268435456 \
  -sSTACK_SIZE=1048576 -sFORCE_FILESYSTEM=1 -sEXIT_RUNTIME=0 \
  '-sEXPORTED_RUNTIME_METHODS=["FS","WORKERFS","ccall","HEAPU8"]' \
  '-sEXPORTED_FUNCTIONS=["_malloc","_free","_nsp_zero","_nsp_load_keys","_nsp_reset","_nsp_close","_nsp_open_pfs0","_nsp_inspect_nca","_nsp_open_meta","_nsp_open_warlock_meta","_nsp_open_exefs","_nsp_open_romfs","_nsp_open_romfs_hash_verified","_nsp_open_patched_romfs","_nsp_open_patched_romfs_hash_verified","_nsp_bktr_fixture","_nsp_read_romfs","_nsp_ivfc_fixture","_nsp_hash_input","_nsp_hash_entry","_nsp_entry_count","_nsp_entry_info","_nsp_read_entry","_nsp_crypto_fixture"]' \
  -o "$OUT/hactool-boundary.mjs"
mkdir -p "$OUT/licenses"
cp "$SRC/LICENSE" "$OUT/licenses/hactool-ISC.txt"
cp "$SRC/mbedtls/LICENSE" "$OUT/licenses/mbedtls-LICENSE.txt"
cp "$SRC/mbedtls/apache-2.0.txt" "$OUT/licenses/mbedtls-Apache-2.0.txt"
# 保留嵌入源码的完整声明，而不只是仓库根目录许可证。
# Preserve full source-embedded notices, not just the root repository license.
for f in ConvertUTF.c ConvertUTF.h lz4.c lz4.h cJSON.c cJSON.h; do cp "$SRC/$f" "$OUT/licenses/$f.txt"; done
printf '%s\n' "$PIN" > "$OUT/UPSTREAM_COMMIT"
sha256sum "$OUT/hactool-boundary.mjs" "$OUT/hactool-boundary.wasm" > "$OUT/SHA256SUMS"
