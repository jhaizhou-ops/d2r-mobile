# Working with S2Arm

中文：[S2ARM.zh-CN.md](S2ARM.zh-CN.md)

[S2Arm](https://github.com/jhaizhou-ops/S2Arm) is a general Switch porting launcher. D2R Mobile is registered in it as a "downstream deep port": when a player exports this game in S2Arm and chooses the D2R Mobile route on the confirmation page, S2Arm does the importing, unpacking, signing, installing and pushing of the game resources, and **calls this repository's tool for the packing step**. The two projects release separately and do not copy each other's code; the contract on this page connects them. Read it before changing anything it mentions.

## 1. The bundle

Every release carries `D2RMobile-s2arm-bundle.zip`, which S2Arm downloads by its registered SHA-256 (while it unpacks the game). It holds three things:

| Path | Content |
|---|---|
| `bundle.json` | `tag` (release tag), `commit` (40-character commit), `product` (codename from `dm/PRODUCT`). S2Arm uses it only if `commit` matches what it registered |
| `shell.zip` | the iOS shell (`dm/ios/tools/shell_build.sh ios`), with the prebuilt shader translator at its top-level `Tools/dmsh_xlate/` |
| `src/` | the repository source of that commit (`git archive`) |

Make it with `python3 dm/tools/s2arm_bundle.py <shell zip> <output zip> [tag]`. It refuses to run with uncommitted changes: the source in the bundle must be exactly the recorded commit.

## 2. How S2Arm calls it

```
<S2Arm's bundled Python> src/dm/tools/d2rm_pack.py shell.zip <S2Arm's extraction directory> <output.ipa>
    environment: DM_ROOT=<work directory from S2Arm>  D2RM_S2ARM_PACK=<S2Arm resource pack output directory>
```

- The game directory is S2Arm's extraction, with lower-case `exefs/` and `romfs/`. `d2rm_pack.py` and `d2rm_android.py` both accept it, and on case-sensitive volumes symlink it into `ExeFS/` and `RomFS/`.
- With `D2RM_S2ARM_PACK` set, an S2Arm-format resource pack is written after the install package (section 4).

## 3. Packing uses only what S2Arm ships

The S2Arm launcher brings its own runtime, and nothing extra is installed on the player's machine. So the packing path (`d2rm_pack.py` and the scripts it runs) may use only:

| Dependency | What S2Arm ships now | Notes |
|---|---|---|
| Python | 3.12 with `capstone`, `lz4`, `numpy`, `Pillow` | No other third-party package in the packing path (`unicorn` is for the test and verification scripts only). Checked by `dm/tools/tests/test_s2arm_deps.py` |
| spirv-cross | the Homebrew version at S2Arm's release (now 1.4.363.0) | The shell carries the shader source digest, and a mismatch stops with an explanation. If S2Arm moves to a spirv-cross whose output changes the digest, rebuild the shell with that version |
| .NET | runtime only, no SDK | Hence the prebuilt `dmsh_xlate` in the shell; `DM_XLATE_PREBUILT=<dir>` also works |
| Apple's Metal shader compiler | in Xcode (S2Arm's Mac launcher requires Xcode) | Without it the shaders go into the package as source and are compiled on the phone at first launch |

## 4. How the game resources reach the phone

The S2Arm resource pack is `data/<archive>` (the 18 archives the phone needs, the same list as `need[]` in `dm/host/romfs_validate.c`) + `index.tsv` (path, bytes, SHA-256) + `manifest.json` (`version` 2, `index_sha256` = SHA-256 of the `index.tsv` text).

After installing the app, S2Arm pushes it with its own pusher: pax tars of at most 1 GB into `Documents/资源包/.incoming/`, then `index.tsv` and `manifest.json`, then it launches the app once with `S2A_PACK_APPLY_ONLY=1`. The app (`dm/ios/Sources/s2a_pack_apply.c`) only:

- unpacks each tar in name order, deleting it when done;
- checks every `index.tsv` entry is present with the right size;
- saves `index.applied.tsv` and writes `.applied` = `index_sha256`;
- appends the result line to `Documents/unpack.log`, and exits.

The wording of the log lines is part of the contract; S2Arm reads progress, completion and failure from them: `进度：解开 <tar>（k/n）…`, `解开 N 个包 … .applied = <sha>…`, `解包失败：<reason>`. A truncated tar is dropped together with the later ones and the push record `.pushed`, so S2Arm re-pushes per `index.applied.tsv`. Once unpacked, the resources are in `Documents/资源包/data/`, which rule ② of the resource search in `main.m` finds as usual.

## 5. Versions

- Only game version 1.0.34.0: the rewrite rules are registered by the main program's build ID and check the original bytes at every patch site. For any other build ID, `patch_nso.py` names that build and the supported one, then stops without making a package. S2Arm's confirmation page likewise offers this route only for 1.0.34.0; other versions go the standard route.
- S2Arm's registry (`pack/downstream/schemes.json` in the S2Arm repository) pins this repository's tag, commit, bundle URL and SHA-256. Upgrading means changing that entry through S2Arm's merge process. After players update S2Arm, their next export uses the new version; unchanged game resources are not pushed again.

## 6. The extra release step

1. Build the iOS shell: `dm/ios/tools/shell_build.sh ios`, on a machine whose spirv-cross matches the one S2Arm ships.
2. `python3 dm/tools/s2arm_bundle.py <shell zip> D2RMobile-s2arm-bundle.zip <tag>`.
3. Attach the bundle to the release and upload it to s2arm.com.
4. Open a merge in S2Arm updating the registry entry (tag, commit, URL, SHA-256).

## 7. Not connected yet

The older-API shell for iOS 18 – 25 (the bundle carries only the iOS 26 shell for now), Android (`d2rm_android.py` + APK), HarmonyOS, the native Mac version and S2Arm on Windows are not connected yet; players follow the README sections for those.
