#!/usr/bin/env python3
"""Deterministic build-time overlay contract; never repair resources at startup.

SOURCE_INPUTS contains only repository-owned generator code, layout patches and
public provenance metadata. It never reads RomFS, generated art, saves or private
optional annotation tables. Text is UTF-8 (optional BOM), normalized to LF before
fingerprinting, so Git's CRLF checkout setting does not change the source ID.
Payload file hashes are exact bytes, NOT newline-normalized. Generation must
already have produced identical payload bytes for cross-platform equality.

Manifest v1: ASCII subset of UTF-8, LF, final newline, strictly sorted paths.
  DM_OVERLAY_V1 <64 lower-case hex source fingerprint>
  <64 lower-case hex file digest> <portable relative path>
The marker, string sidecar, build config and at least one data/*.data are required.
All actual files are listed, except this root's overlay.manifest itself. Links,
special files, unsafe/ambiguous paths, absent required files and old manifests
are errors, never warnings. Variants must each be finalized separately.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import stat
import sys
import tempfile

MANIFEST = "overlay.manifest"
MAGIC = "DM_OVERLAY_V1"
MAX_PATH = 1024
MAX_DEPTH = 32
MAX_FILES = 4096
MAX_NODES = MAX_FILES * 2
MAX_MANIFEST = 4 * 1024 * 1024
MAX_FILE = 8 * 1024**3
MAX_TOTAL = 16 * 1024**3
CHUNK = 64 * 1024
MAX_SOURCE = 8 * 1024 * 1024
REQUIRED = frozenset(("ctrl_touch", "touch_strings.tsv", "data/.build.config"))
_HEX = re.compile(r"[0-9a-f]{64}\Z")
_COMPONENT = re.compile(r"[A-Za-z0-9_.+-]+\Z")
_RESERVED = re.compile(r"(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])\Z", re.I)

# Fixed, reviewed dependency closure. Add dependencies here when generators gain
# a new code/layout input; --inputs exposes exactly this list to CMake.
SOURCE_INPUTS = tuple(sorted((
    "dm/ios/tools/overlay_contract.py",
    "dm/android/tools/stage_touch_assets.py",
    "dm/ios/tools/gen_romfs_overlay.py", "dm/ios/tools/gen_assets.py",
    "dm/ios/tools/gen_paths.py", "dm/ios/tools/gen_hud_v4.py",
    "dm/ios/tools/hud_v4_design.py", "dm/ios/tools/layout_patch.py",
    "dm/ios/tools/touchrectv1_patch.json", "dm/ios/tools/touch_layout.py",
    "dm/ios/tools/touch_strings.py", "dm/ios/tools/romfs_art.py",
    "dm/ios/tools/dm_i18n.py", "dm/ios/tools/merc_slots.py",
    "dm/ios/tools/pouch_layout.py", "dm/ios/tools/wide_stash.py",
    "dm/ios/tools/drop_light.py", "dm/ios/tools/kbd_glyphs.py",
    "dm/ios/tools/gameplay_tables.py",
    "dm/ios/tools/gen_widestash_art.py", "dm/ios/tools/gen_vault_cover.py",
    "dm/ios/tools/gen_trove_art.py", "dm/ios/tools/gen_pouch_art.py",
    "dm/ios/tools/gen_fe_rightpanel.py", "dm/ios/tools/gen_map_goal_art.py",
    "dm/ios/tools/gen_merc_art.py", "dm/ios/tools/gen_close_btn.py",
    "dm/ios/tools/annot_strings.py", "dm/ios/tools/annot_ranges.py",
    "dm/ios/tools/annot_drop.py", "dm/ios/tools/annot_world.py",
    "dm/ios/tools/native_art/generate.py", "dm/ios/tools/native_art/implementation.c.inc",
    "dm/ios/Sources/TouchArt/native_art_manifest.json",
    "dm/ios/docs/native_art_v1_geometry.json",
    "dm/ios/docs/touch_layout_spec_native_binding_v5.json",
    "dm/data/casc_overlay/borrowed_paths.tsv", "dm/i18n/ui_text.tsv",
    "dm/tools/casc_overlay.py", "dm/tools/dmx.py", "dm/tools/winlf/sitecustomize.py", "dm/tools/winlf/run.py", "dm/host/dm_kills.c",
    "dm/gfx/nvn/nvn_tex.c", "dm/gfx/nvn/nvn_tex.h",
    "dm/gfx/tex/nvn_deswizzle.c", "dm/gfx/tex/nvn_deswizzle.h",
)))


class ContractError(ValueError):
    """An invalid or incomplete overlay/source tree."""


def valid_relative_path(value: str) -> str:
    """Portable ASCII names avoid Windows aliases and byte-sort differences."""
    if not isinstance(value, str) or not value or len(value) > MAX_PATH:
        raise ContractError("invalid relative path length")
    pieces = value.split("/")
    if len(pieces) > MAX_DEPTH:
        raise ContractError(f"path too deep: {value}")
    for part in pieces:
        if (part in ("", ".", "..") or not _COMPONENT.fullmatch(part)
                or part.endswith(".") or _RESERVED.fullmatch(part.split(".", 1)[0])):
            raise ContractError(f"unsafe or non-portable path: {value}")
    return value


def _regular_info(path: Path, *, directory: bool = False):
    info = path.lstat()
    # Windows junctions/reparse points are not reliably reported as is_symlink.
    if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
        raise ContractError(f"link/reparse point is not allowed: {path.name}")
    if not (stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode)):
        raise ContractError(f"expected {'directory' if directory else 'regular file'}: {path.name}")
    return info


def _within(root: Path, relative: str) -> Path:
    valid_relative_path(relative)
    here = root
    parts = relative.split("/")
    for part in parts[:-1]:
        here /= part
        _regular_info(here, directory=True)
    return here / parts[-1]


def source_fingerprint(repo_root: str | Path) -> str:
    root = Path(repo_root).absolute()
    _regular_info(root, directory=True)
    digest = hashlib.sha256(b"DM_OVERLAY_SOURCE_V1\0")
    for relative in SOURCE_INPUTS:
        path = _within(root, relative)
        info = _regular_info(path)
        if info.st_size > MAX_SOURCE:
            raise ContractError(f"source input too large: {relative}")
        try:
            with path.open("rb") as stream:
                raw = stream.read(MAX_SOURCE + 1)
            if len(raw) > MAX_SOURCE:
                raise ContractError(f"source input grew beyond limit: {relative}")
            text = raw.decode("utf-8-sig")
        except UnicodeError as error:
            raise ContractError(f"source is not UTF-8: {relative}") from error
        data = text.replace("\r\n", "\n").replace("\r", "\n").encode("utf-8")
        digest.update(relative.encode("ascii") + b"\0")
        digest.update(len(data).to_bytes(8, "big"))
        digest.update(data)
    return digest.hexdigest()


def _is_data(path: str) -> bool:
    return path.startswith("data/") and path.count("/") == 1 and path.endswith(".data")


def _require(entries) -> None:
    names = set(entries)
    missing = REQUIRED - names
    if missing:
        raise ContractError("missing required file: " + sorted(missing)[0])
    if not any(_is_data(p) for p in names):
        raise ContractError("missing required data/*.data")


def inventory(overlay: str | Path) -> list[str]:
    root = Path(overlay).absolute()
    _regular_info(root, directory=True)
    found: list[str] = []
    folds: set[str] = set()
    nodes = 0

    def walk(directory: Path, prefix: str, depth: int) -> None:
        nonlocal nodes
        if depth > MAX_DEPTH:
            raise ContractError("overlay directory depth exceeded")
        with os.scandir(directory) as scan:
            children = []
            for entry in scan:
                nodes += 1
                if nodes > MAX_NODES:
                    raise ContractError("too many overlay directory entries")
                children.append(entry)
            children.sort(key=lambda entry: entry.name)
        for entry in children:
            relative = prefix + entry.name
            valid_relative_path(relative)
            if relative.lower() == MANIFEST and relative != MANIFEST:
                raise ContractError(f"case-ambiguous manifest path: {relative}")
            path = directory / entry.name
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                raise ContractError(f"link/reparse point is not allowed: {relative}")
            if stat.S_ISDIR(info.st_mode):
                walk(path, relative + "/", depth + 1)
            elif stat.S_ISREG(info.st_mode):
                if relative == MANIFEST:
                    continue
                if relative.lower() in folds:
                    raise ContractError(f"case-ambiguous path: {relative}")
                folds.add(relative.lower())
                found.append(relative)
                if len(found) > MAX_FILES:
                    raise ContractError("too many overlay files")
            else:
                raise ContractError(f"special file is not allowed: {relative}")
    walk(root, "", 0)
    return sorted(found)


def file_digest(path: Path) -> tuple[str, int]:
    before = _regular_info(path)
    if before.st_size > MAX_FILE:
        raise ContractError(f"payload too large: {path.name}")
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        while chunk := source.read(CHUNK):
            size += len(chunk)
            if size > MAX_FILE or size > before.st_size:
                raise ContractError(f"payload changed while hashing: {path.name}")
            digest.update(chunk)
    after = _regular_info(path)
    if (size != before.st_size or before.st_size != after.st_size
            or before.st_mtime_ns != after.st_mtime_ns or before.st_ino != after.st_ino):
        raise ContractError(f"payload changed while hashing: {path.name}")
    return digest.hexdigest(), size


def _check_sha(value: str) -> str:
    if not _HEX.fullmatch(value):
        raise ContractError("expected lower-case 64-digit SHA256")
    return value


def render_manifest(overlay: str | Path, source_sha: str) -> bytes:
    _check_sha(source_sha)
    root = Path(overlay).absolute()
    names = inventory(root)
    _require(names)
    lines = [f"{MAGIC} {source_sha}\n"]
    total = 0
    for relative in names:
        digest, size = file_digest(_within(root, relative))
        if size == 0 and (relative in REQUIRED or _is_data(relative)):
            raise ContractError(f"empty required payload: {relative}")
        total += size
        if total > MAX_TOTAL:
            raise ContractError("overlay byte limit exceeded")
        lines.append(f"{digest} {relative}\n")
    result = "".join(lines).encode("ascii")
    if len(result) > MAX_MANIFEST:
        raise ContractError("manifest byte limit exceeded")
    return result


def _atomic_write(path: Path, data: bytes) -> None:
    if path.exists() or path.is_symlink():
        info = _regular_info(path)
        if info.st_size == len(data) and path.read_bytes() == data:
            return
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix=".overlay-contract-", dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def write_manifest(overlay: str | Path, source_sha: str) -> Path:
    """Build-time finalization only. Run after all payload/variant sidecars exist."""
    root = Path(overlay).absolute()
    data = render_manifest(root, source_sha)
    path = root / MANIFEST
    _atomic_write(path, data)
    return path


def parse_manifest(data: bytes, expected_source: str) -> dict[str, str]:
    _check_sha(expected_source)
    if len(data) > MAX_MANIFEST or not data.endswith(b"\n") or b"\r" in data or b"\0" in data:
        raise ContractError("invalid manifest size or line ending")
    try:
        lines = data.decode("ascii").splitlines()
    except UnicodeError as error:
        raise ContractError("manifest must use portable ASCII paths") from error
    if not lines or lines[0] != f"{MAGIC} {expected_source}":
        raise ContractError("manifest source fingerprint mismatch")
    if len(lines) - 1 > MAX_FILES:
        raise ContractError("too many manifest entries")
    entries: dict[str, str] = {}
    folded: set[str] = set()
    previous = ""
    for line in lines[1:]:
        if len(line) < 66 or line[64] != " ":
            raise ContractError("malformed manifest entry")
        digest, relative = _check_sha(line[:64]), valid_relative_path(line[65:])
        if relative.lower() == MANIFEST or relative <= previous or relative.lower() in folded:
            raise ContractError(f"duplicate, unordered or excluded path: {relative}")
        previous = relative
        folded.add(relative.lower())
        entries[relative] = digest
    _require(entries)
    return entries


def verify_manifest(overlay: str | Path, expected_source: str) -> dict[str, str]:
    """Build-side verifier. Missing/old manifests are errors, never synthesized."""
    root = Path(overlay).absolute()
    _regular_info(root, directory=True)
    manifest = root / MANIFEST
    if _regular_info(manifest).st_size > MAX_MANIFEST:
        raise ContractError("manifest too large")
    with manifest.open("rb") as stream:
        entries = parse_manifest(stream.read(MAX_MANIFEST + 1), expected_source)
    names = inventory(root)
    if names != list(entries):
        raise ContractError("manifest does not cover exactly the overlay files")
    total = 0
    for relative, expected in entries.items():
        actual, size = file_digest(_within(root, relative))
        if size == 0 and (relative in REQUIRED or _is_data(relative)):
            raise ContractError(f"empty required payload: {relative}")
        total += size
        if total > MAX_TOTAL or actual != expected:
            raise ContractError(f"payload digest mismatch or byte limit: {relative}")
    return entries


def write_header(path: str | Path, source_sha: str) -> None:
    _check_sha(source_sha)
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    _atomic_write(target, ("/* Generated by overlay_contract.py; do not edit. */\n#pragma once\n"
                           f'#define DM_OVERLAY_SOURCE_SHA256 "{source_sha}"\n').encode("ascii"))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--header", type=Path)
    parser.add_argument("--write", type=Path, action="append", default=[])
    parser.add_argument("--verify", type=Path, action="append", default=[])
    parser.add_argument("--inputs", action="store_true", help="print fixed repo-relative dependency paths")
    args = parser.parse_args(argv)
    if args.inputs:
        print("\n".join(SOURCE_INPUTS))
        if not (args.header or args.write or args.verify):
            return 0
    try:
        source_sha = source_fingerprint(args.repo_root)
        if args.header:
            write_header(args.header, source_sha)
        for overlay in args.write:
            write_manifest(overlay, source_sha)
            verify_manifest(overlay, source_sha)
        for overlay in args.verify:
            verify_manifest(overlay, source_sha)
        print(source_sha)
        return 0
    except (OSError, ContractError) as error:
        print(f"overlay contract: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
