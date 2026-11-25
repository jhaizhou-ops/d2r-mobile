#!/usr/bin/env bash
# Stage a complete private runtime pack for default/selfuse builds. Never commit the generated resources.
# 打包完整的私用运行组件，含版本信息和衍生表；生成资源不入库。
set -euo pipefail
src=${1:?usage: stage_debug_pack.sh <pack dir>}
here=$(cd "$(dirname "$0")" && pwd)
exec "${PYTHON:-python}" -I "$here/stage_runtime_pack.py" "$src"
