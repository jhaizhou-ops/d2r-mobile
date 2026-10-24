"""通过上游哈希检查后，替换已审计的生成 NumPy 配方。这是显式 AST 移植，不是 numpy 垫片；每个替换语句范围均锚定原始源码，缺少锚点则拒绝。
Replace audited generated NumPy recipes after upstream hash checks.

This is an explicit AST port, not a numpy shim. Every replaced statement range is
anchored by its original source and missing anchors fail closed.
"""

import ast
import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
OUT = HERE.parent / "asset-generation-spike/public/generated"


def replace_function(tree, name, body):
    found = 0
    for node in ast.walk(tree):
        if isinstance(node, ast.FunctionDef) and node.name == name:
            node.body = ast.parse(body).body
            found += 1
    if found != 1:
        raise RuntimeError(f"Expected one {name} recipe, got {found}")


def replace_range(source, start, end, replacement):
    if source.count(start) != 1 or source.count(end) != 1:
        raise RuntimeError(f"Pixel recipe statement drift: {start}")
    first = source.index(start)
    last = source.index(end, first) + len(end)
    return source[:first] + replacement + source[last:]


def migrate():
    for path, expected in json.loads((HERE / "provenance.json").read_text()).items():
        if hashlib.sha256((ROOT / path).read_bytes()).hexdigest() != expected:
            raise RuntimeError(f"Pixel kernel source drift: {path}")
    for name in [
        "native_functions",
        "hud_original",
        "hud_design_original",
        "remaining_art_original",
        "trove_original",
    ]:
        source = (OUT / f"{name}.py").read_text()
        tree = ast.parse(source)
        if name in {"native_functions", "hud_original"}:
            replace_function(tree, "dflt", "return art_pixels.default_state(im)")
        if name == "remaining_art_original":
            replace_function(tree, "vault_cover", "return art_pixels.vault_cover(im)")
        if name == "trove_original":
            replace_function(tree, "_stats", "return art_pixels.stats(im, boxes)")
            replace_function(tree, "_stone", "return art_pixels.stone(csh, w, h, target, ox, oy)")
            replace_function(tree, "_feather", "return art_pixels.feather(sz, n)")
        if name == "hud_design_original":
            replace_function(
                tree,
                "prep",
                "box = im.getchannel('A').point(lambda a: 255 if a > 24 else 0).getbbox()\nif box is None: raise ValueError('Empty art alpha bounds')\nreturn im.crop(box)",
            )
        source = ast.unparse(ast.fix_missing_locations(tree))
        if name == "trove_original":
            source = replace_range(
                source,
                "col = np.asarray(edge.convert('RGB'))[:, -3 if not flip else 2].astype(int)",
                "col = np.asarray(edge.convert('RGB'))[:, -3 if not flip else 2].astype(int)",
                "col = [edge.convert('RGB').getpixel((edge.width - 3 if not flip else 2, yy)) for yy in range(h)]",
            )
        if name == "hud_original":
            source = replace_range(
                source,
                "yy, xx = np.mgrid[0:H, 0:W]",
                "cvg = Image.fromarray(g_)",
                "circles = [(*c, g['BR'] - 2) for c in (g['LBC'], g['RBC'])]\n    cvg = art_pixels.clear_circles(cv, circles)",
            )
            source = replace_range(
                source,
                "a = np.array(cv)",
                "cv = Image.fromarray(a)",
                "full = cvg.copy()\n    cv = art_pixels.clear_circles(cv, circles)",
            )
            source = source.replace("    full = a_glass\n", "")
            source = source.replace(
                "full[y0 + g['IY0']:y0 + g['IY1'] + 1, x0 + g['IX0']:x0 + g['IX1'] + 1, 3] = 0",
                "alpha = full.getchannel('A')\n        alpha.paste(0, (x0 + g['IX0'], y0 + g['IY0'], x0 + g['IX1'] + 1, y0 + g['IY1'] + 1))\n        full.putalpha(alpha)",
            )
            source = source.replace(
                "full[y0:y1, x0:x1, 3] = 0",
                "alpha = full.getchannel('A')\n    alpha.paste(0, (x0, y0, x1, y1))\n    full.putalpha(alpha)",
            )
            source = source.replace("Image.fromarray(full).crop", "full.crop")
            source = replace_range(
                source,
                "im = np.asarray(Image.open",
                "Image.fromarray((np.dstack([im[..., :3], al]) * 255).astype('uint8')).save(os.path.join(UI, 'sil_' + a_['asset']))",
                "im = Image.open(os.path.join(ART, a_['asset'])).convert('RGBA')\n        art_pixels.silhouette(im, a_['frame_width'], a_['frame_pitch']).save(os.path.join(UI, 'sil_' + a_['asset']))",
            )
        if name == "hud_design_original":
            source = replace_range(
                source,
                "ta = np.array(slot).astype(int)",
                "gold = (ta[..., 0] > 90) & (ta[..., 0] - ta[..., 2] > 40)",
                "gold = [[slot.getpixel((x, y))[0] > 90 and slot.getpixel((x, y))[0] - slot.getpixel((x, y))[2] > 40 for x in range(DM)] for y in range(SH)]",
            )
            source = source.replace("gold[SH // 2]", "gold[SH // 2]").replace(
                "gold[:, DM // 2]", "[row[DM // 2] for row in gold]"
            )
            source = replace_range(
                source,
                "a = np.array(m).astype(float) / 255",
                "return Image.fromarray(np.clip(out_, 0, 255).astype('uint8'), 'RGBA')",
                "return art_pixels.metal(m, m.filter(ImageFilter.MaxFilter(5)))",
            )
            source = source.replace(
                "statue = np.zeros((H, W), bool)", "statue = Image.new('L', (W, H))"
            )
            source = source.replace(
                "statue[H - im_.height:H, x_:x_ + im_.width] |= np.array(im_)[..., 3] > 24",
                "mask = im_.getchannel('A').point(lambda a: 255 if a > 24 else 0)\n        statue.paste(255, (x_, H - im_.height), mask)",
            )
            source = source.replace(
                "statue[y0:y1, x0:x1].any()",
                "statue.crop((x0, y0, x1, y1)).getbbox() is not None",
            )
        source = source.replace("import numpy as np", "import art_pixels")
        tree = ast.parse(source)
        if any(isinstance(n, ast.Name) and n.id == "np" for n in ast.walk(tree)):
            raise RuntimeError(f"Unmigrated NumPy use in {name}")
        (OUT / f"{name}.py").write_text(ast.unparse(tree) + "\n")
    (OUT / "art_pixels.py").write_text((HERE / "art_pixels.py").read_text())
    (OUT / "art-kernels.mjs").write_text((HERE / "kernels.mjs").read_text())


if __name__ == "__main__":
    migrate()
