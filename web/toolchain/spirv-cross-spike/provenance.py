"""记录原生源码链约定，绝不修改或替换它。
Record, never change or replace, the native source-chain contract."""

import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True

root = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("native_chain", root / "dm/gfx/tools/dmsh_chain.py")
chain = importlib.util.module_from_spec(spec)
spec.loader.exec_module(chain)
version = chain.version()
header = Path(chain.HEADER).read_text()
expected = re.search(r'#define DMSH_CHAIN "([a-f0-9]+)"', header).group(1)
files = {
    str((Path(chain.GFX) / name).resolve().relative_to(root)): hashlib.sha256(
        (Path(chain.GFX) / name).read_bytes()
    ).hexdigest()
    for name in chain.FILES
}
print(
    json.dumps(
        {
            "schema": "spirv-cross-stage-provenance-v1",
            "observedNativeSourceChain": version,
            "nativeHeaderChain": expected,
            "nativeHeaderMatchesSource": expected == version,
            "nativeOptions": chain.options(),
            "nativeSourceSha256": files,
            "productionChainAccepted": False,
            "reason": "Diagnostic provenance only. WASM binary is not the locked native executable; final shell source digest validation remains required.",
        },
        indent=2,
    )
)
