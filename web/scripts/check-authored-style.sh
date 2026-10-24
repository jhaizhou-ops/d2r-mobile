#!/usr/bin/env bash
# 根据仓库的 Ruff 和 clang-format 配置及 Bash 语法规则，检查列出的自有源码。
# Check the listed authored sources against repository Ruff and clang-format configurations and Bash syntax rules.
set -euo pipefail
cd "$(dirname "$0")/../.."

python_sources=(
  web/toolchain/zsign-wasm/prepare-source.py
  web/toolchain/astc-encoder-wasm/bc3-native-reference.py
  web/toolchain/sha256-stream-wasm/native-check.py
  web/scripts/generate-i18n.py
  web/toolchain/nsp-extract-spike/prepare-source.py
  web/toolchain/synthetic-signed-nca/inject-test-root.py
  web/toolchain/synthetic-signed-nca/verify-build-isolation.py
  web/toolchain/spirv-cross-spike/native-oracle.py
  web/toolchain/spirv-cross-spike/provenance.py
  web/toolchain/shell-release-evidence/verify-v2-shell-evidence.py
  web/toolchain/asset-generation-spike/scripts/package-assets.py
  web/toolchain/asset-generation-spike/src/core.py
  web/toolchain/asset-generation-spike/src/hud.py
  web/toolchain/asset-generation-spike/src/casc_overlay.py
  web/toolchain/asset-generation-spike/test/casc-overlay-oracle.py
  web/toolchain/asset-generation-spike/src/vault.py
  web/toolchain/asset-generation-spike/src/mercenary.py
  web/toolchain/asset-generation-spike/src/remaining_art.py
  web/toolchain/asset-generation-spike/src/overlay_recipe.py
  web/toolchain/asset-generation-spike/src/map_goal.py
  web/toolchain/asset-generation-spike/src/native_art.py
  web/toolchain/asset-generation-spike/src/derived.py
  web/toolchain/program-rewrite-spike/audit-source.py
  web/toolchain/program-rewrite-spike/make-fixtures.py
  web/toolchain/texture-spike/bounded_encoder.py
  web/toolchain/texture-spike/native-encoder-fixtures.py
  web/toolchain/texture-spike/package-encoder.py
  web/toolchain/texture-spike/native-fixtures.py
  web/toolchain/art-kernels/art_pixels.py
  web/toolchain/art-kernels/migrate.py
  web/toolchain/asset-transforms-js/test/reference.py
  web/toolchain/casc-overlay-js/test/fixture.py
  web/toolchain/casc-overlay-js/test/prebuilt-fixture.py
  web/toolchain/font-rasterizer-exact/test/native-equivalence.py
  web/toolchain/image-kernels-rust/test/make-fixtures.py
  web/toolchain/nsp-extract-spike/native-key-parser-test.py
  web/toolchain/overlay-recipe-js/scripts/extract-touch-data.py
  web/toolchain/png-codec-exact/test/native-equivalence.py
  web/toolchain/program-rewrite-spike/audit-js-source.py
)
ruff check --no-cache --config web/toolchain/style/ruff.toml "${python_sources[@]}"
ruff format --check --no-cache --config web/toolchain/style/ruff.toml "${python_sources[@]}"

clang-format --dry-run --Werror --style=file:web/toolchain/style/clang-format.yaml \
  web/toolchain/zsign-wasm/profile-compatibility.inc \
  web/toolchain/zsign-wasm/profile-compatibility-test.cpp \
  web/toolchain/zsign-wasm/stream-sha.inc \
  web/toolchain/zsign-wasm/bounded-map.inc \
  web/toolchain/sha256-stream-wasm/bridge.c \
  web/toolchain/sha256-stream-wasm/sha256_config.h \
  web/toolchain/astc-encoder-wasm/bridge.cpp \
  web/toolchain/astc-encoder-wasm/native-smoke.cpp \
  web/toolchain/nsp-extract-spike/boundary.c \
  web/toolchain/nsp-extract-spike/bktr-reader.h \
  web/toolchain/nsp-extract-spike/crypto-fixture.c \
  web/toolchain/nsp-extract-spike/quiet.h \
  web/toolchain/program-rewrite-spike/bridge.c \
  web/toolchain/texture-spike/boundary.c \
  web/toolchain/font-rasterizer-exact/src/font_rasterizer.c \
  web/toolchain/png-codec-exact/src/generic_zlib.c \
  web/toolchain/png-codec-exact/src/png_codec.c

bash -n web/toolchain/zsign-wasm/build.sh
bash -n web/toolchain/astc-encoder-wasm/build.sh
bash -n web/toolchain/astc-encoder-wasm/native-smoke.sh
bash -n web/toolchain/nsp-extract-spike/build-wasm.sh
bash -n web/toolchain/synthetic-signed-nca/run-ci.sh
bash -n web/toolchain/spirv-cross-spike/build.sh
bash -n web/toolchain/program-rewrite-spike/build-wasm.sh
bash -n web/toolchain/texture-spike/build-wasm.sh
bash -n web/scripts/check-authored-style.sh

for script in web/verification/*.sh web/verification/recipes/*.sh; do
  bash -n "$script"
done
