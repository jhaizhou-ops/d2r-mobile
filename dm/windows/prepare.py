# 从玩家自己的 3.3.94012 文件准备 Windows 目标，复用改写器、数据表、桌面覆盖层与 Vulkan 翻译链；产物只写仓外。
# Prepare Windows from the player's own 3.3.94012 files using the shared rewriter, tables, desktop overlay and Vulkan chain; output stays outside the repository.
import argparse
import hashlib
import importlib
import os
from pathlib import Path
import re
import shutil
import sys
import sysconfig
import tempfile

DM = Path(__file__).resolve().parents[1]
REPO = DM.parent
TOOLS = DM / "tools"
IOS = DM / "ios" / "tools"
GFX = DM / "gfx" / "tools"
NSO_SHA = {
    "main": "55fd909bb7230b002357e5b0d8499c11f06888f345c49e69eb4734df44a27d0e",
    "sdk": "1c9f198a91c0bcfee5cf8f5b53fb941895e91334dfb766ce79fc68576797f017",
}


def main():
    ap = argparse.ArgumentParser(description="Prepare genuine Windows ARM64 game inputs outside the repository")
    ap.add_argument("game", type=Path, help="Update directory containing ExeFS and RomFS")
    ap.add_argument("root", type=Path, help="external Windows work directory")
    ap.add_argument("--toolchain", type=Path, required=True, help="LLVM-MinGW UCRT root")
    ap.add_argument("--spirv-tools", type=Path, required=True, help="directory containing spirv-opt.exe, spirv-val.exe and glslc.exe")
    ap.add_argument("--dotnet", default="dotnet")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--verify", action="store_true", help="also run full per-site equivalence and mutation checks")
    args = ap.parse_args()
    if os.name != "nt":
        ap.error("run this preparation entry on Windows")
    if not 1 <= args.jobs <= 256:
        ap.error("--jobs must be between 1 and 256")
    game, root = args.game.resolve(), args.root.resolve()
    if root.is_relative_to(REPO):
        ap.error("output must be outside the repository")
    for module, expected in NSO_SHA.items():
        source = game / "ExeFS" / module
        if not source.is_file() or hashlib.sha256(source.read_bytes()).hexdigest() != expected:
            ap.error(f"{source}: expected the supported Switch 1.0.34.0 / 3.3.94012 module")
    romfs = game / "RomFS"
    if not (romfs / "data" / "build_manifest.psv").is_file():
        ap.error(f"missing {romfs / 'data' / 'build_manifest.psv'}")
    for module in ("PIL", "numpy", "lz4", "capstone", "unicorn"):
        try:
            importlib.import_module(module)
        except ImportError as error:
            ap.error(f"missing Python dependency {module}: {error}")
    arch = {"win-arm64": "aarch64", "win-amd64": "x86_64"}.get(sysconfig.get_platform())
    if arch is None:
        ap.error("use ARM64 or x64 Python; its architecture must match the asset decoder DLL")
    cc = args.toolchain.resolve() / "bin" / f"{arch}-w64-mingw32-clang.exe"
    shader_tools = {key: args.spirv_tools.resolve() / name for key, name in (
        ("SPIRV_OPT", "spirv-opt.exe"), ("SPIRV_VAL", "spirv-val.exe"), ("GLSLC", "glslc.exe"))}
    for tool in (cc, *shader_tools.values()):
        if not tool.is_file():
            ap.error(f"missing tool: {tool}")
    dotnet = shutil.which(args.dotnet)
    if not dotnet:
        ap.error(f".NET SDK not found: {args.dotnet}")
    root.mkdir(parents=True, exist_ok=True)
    os.environ.update(DM_ROOT=str(root), DM_GEN_ASSETS=str(root / "assets"),
                      DM_ROMFS_SRC=str(romfs), DM_OVERLAY_MAC="1",
                      DM_TARGET="windows", DM_RULESET="touch-run-v4",
                      PYTHONDONTWRITEBYTECODE="1")
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(TOOLS))
    from d2rm_pack import PY, MODS, run, say

    logs = root / "prepare-logs"
    logs.mkdir(exist_ok=True)

    def execute(name, command, env=None):
        say(name)
        return run([str(x) for x in command], env=env, log=str(logs / (name + ".log")), what=name)

    if "--trim-capabilities" not in execute("spirv-opt-check", [shader_tools["SPIRV_OPT"], "--help"]):
        ap.error("spirv-opt is too old: --trim-capabilities is required")
    out, generated = root / "out", root / "generated"
    out.mkdir(exist_ok=True)
    generated.mkdir(exist_ok=True)
    for module in MODS:
        execute("rewrite-" + module, [PY, TOOLS / "patch_nso.py", game / "ExeFS" / module, out, module])
        if not any(line.split() == ["#", "TARGET", "0x3"] for line in
                   (out / (module + ".fixups.tsv")).read_text(encoding="utf-8").splitlines()):
            ap.error(f"{module}: the rewriter did not produce Windows target 3")
        if args.verify:
            execute("equiv-" + module, [PY, DM / "verify" / "equiv.py", out, module,
                                       game / "ExeFS" / module, "--jobs", args.jobs,
                                       "--tsv", root / (module + ".equiv.tsv")])
    for name, script in (
        ("dm_vault_tab.h", IOS / "gen_vault_table.py"),
        ("dm_hud_exp.h", IOS / "gen_hud_exp.py"),
        ("dm_bright_levels.inc", TOOLS / "gen_bright_levels.py"),
    ):
        text = execute("generate-" + name, [PY, script, romfs])
        (generated / name).write_text(text, encoding="utf-8", newline="\n")

    tools = root / "tools"
    tools.mkdir(exist_ok=True)
    decoder = tools / ("deswizzle-" + arch + ".dll")
    execute("asset-decoder", [cc, "-O2", "-shared", "-Wl,--export-all-symbols",
                             "-I", DM / "gfx" / "nvn", "-I", DM / "gfx" / "tex", "-o", decoder,
                             DM / "gfx" / "tex" / "nvn_deswizzle.c", DM / "gfx" / "nvn" / "nvn_tex.c"])
    execute("desktop-overlay", [PY, IOS / "gen_romfs_overlay.py", romfs, root / "overlay"],
            {"DM_DESWIZZLE_LIB": str(decoder)})

    packs = root / "shader-packs"
    packs.mkdir(exist_ok=True)
    env = {"DMX_ROOT": str(romfs / "data")}
    names = [line.split("\t")[0] for line in execute(
        "shader-list", [PY, TOOLS / "dmx.py", "list", "data:data/hd/shaders/*.bin"], env).splitlines() if line.strip()]
    if len(names) != 162:
        ap.error(f"expected 162 shader packages, found {len(names)}")
    for index, name in enumerate(names, 1):
        execute(f"shader-package-{index:03}", [PY, TOOLS / "dmx.py", "get", name, packs / Path(name).name], env)
    translator = tools / "dmsh_xlate"
    execute("shader-translator", [dotnet, "publish", GFX / "dmsh_xlate" / "dmsh_xlate.csproj",
                                 "-c", "Release", "-o", translator,
                                 "--artifacts-path", root / "dotnet-artifacts", "-v", "quiet"])
    shaders = root / "shaders"
    execute("vulkan-shaders", [PY, GFX / "dmsh_vk.py", packs, shaders], {
        **{key: str(value) for key, value in shader_tools.items()},
        "XLATE": str(translator / "dmsh_xlate.exe"), "MAIN": str(game / "ExeFS" / "main"),
        "WORK": str(root / "shader-work"), "J": str(args.jobs)})
    with tempfile.TemporaryDirectory(prefix="windows-queries-", dir=root) as temporary:
        queries = Path(temporary) / "queries"
        execute("builtin-queries", [PY, GFX / "pack_builtin_queries.py", "--output", queries,
                                    "--glslc", shader_tools["GLSLC"]])
        for file in queries.glob("*.xgs"):
            shutil.copyfile(file, shaders / file.name)
    sys.path.insert(0, str(GFX))
    from pack_builtin_queries import kernel_keys
    keys = {line.split("\t")[3] for line in (DM / "gfx" / "nvn" / "shader_map.tsv").read_text(
        encoding="utf-8").splitlines() if len(line.split("\t")) > 4 and re.fullmatch(r"[0-9a-f]{16}", line.split("\t")[3])}
    keys.update(kernel_keys().values())
    for key in keys:
        file = shaders / (key + ".xgs")
        if not file.is_file() or file.read_bytes()[:4] != b"XGS1":
            ap.error(f"missing or invalid Vulkan shader bundle: {file}")
    say(f"Ready: {root}; generated headers: {generated}; shader bundles: {len(keys)}")
    say("Build with build.ps1 -BuildHost -GeneratedHeaders <root>\\generated, then use run.ps1.")
    say("Prepared output contains your game content; do not distribute it.")


if __name__ == "__main__":
    main()
