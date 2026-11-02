# Playing on a HarmonyOS phone

The HarmonyOS port by [@biglawine](https://github.com/biglawine) shares the host and Vulkan engine with Android. The shell is a precompiled HAP without game content. Re-sign it with your own debug credentials, then import a pack generated from your own game files; rebuilding the application is unnecessary.

中文：[HARMONYOS.zh-CN.md](HARMONYOS.zh-CN.md)

On 2026-10-22, the shell passed re-signing, replacement installation, system folder import, character selection, entering camp, and save-and-exit on a Mate 90 Pro Max (CMM-AL10, HarmonyOS 7.0.0.109, device-reported API 26). Code121 fixed and verified first import into absent runtime directories. 0.19.4/code122 additionally guards map requests outside a game; three consecutive cold starts, entering camp, full-map switching and save-and-exit passed. The final 0.19.6/code125, built from the v2.0 tree, also applies the language chosen in Custom at the next cold start; on 2026-10-23 cold starts in Korean and in Chinese, entering a game, menus and save-and-exit passed, and the 27 existing save files were unchanged after replacing the runtime pack. Screenshots showed approximately 30–60 FPS. This is installation/import validation, not a performance benchmark; the previous debug build's 59.7 FPS belongs to a different test. This shell flow has not yet been repeated on Pura X.

Use the **unsigned HAP** and matching source/tools from the release. If the release has no shell asset yet, wait for the maintainer to publish it or follow the [developer build guide](../dm/harmony/README.md). Do not use private test packages containing game content or someone else's signing profile.

A separate [MatePad mini experimental bring-up report](devices/MATEPAD_MINI.md) records startup blockers and personal-build results. It does not establish compatibility of the published shell.

## Requirements

- A phone meeting the HAP's minimum API, with developer mode and USB debugging enabled. Only the API 26 device above was checked in this run; marketing OS versions do not directly identify SDK APIs.
- DevEco Studio and the matching SDK, including Java, `hap-sign-tool.jar` and `hdc`.
- Your Huawei developer account, debug certificate/private key, and a debug profile containing the target device. DevEco's signing setup can generate these after sign-in. See the [official profile instructions](https://developer.huawei.com/consumer/cn/doc/doccenter-getting-started/agc-help-debug-profile-0000002248181278) for manual management. No account-free installation route was verified.
- Your purchased game's Switch 1.0.34.0 (3.3.94012) `Update/ExeFS/{main,sdk}` and `Update/RomFS/data/`; see [GAME_FILES.md](GAME_FILES.md).
- Python 3.9+ with `pillow numpy lz4 capstone unicorn`, .NET SDK, and `spirv-opt`, `spirv-val`, `glslc`. `spirv-opt` must support `--trim-capabilities`. Reuse an existing working environment.

## 1. Re-sign and install

Check the unsigned HAP's SHA-256 against the release. Use your own certificate, profile and keystore. The profile must match the application's identity and include your device. Replace the paths below in Windows PowerShell; enter passwords at the prompts, never in the script.

```powershell
$DevEco = 'C:\Program Files\Huawei\DevEco Studio'
$Java = "$DevEco\jbr\bin\java.exe"
$SignTool = "$DevEco\sdk\default\openharmony\toolchains\lib\hap-sign-tool.jar"
$Hdc = "$DevEco\sdk\default\openharmony\toolchains\hdc.exe"
$InputHap = 'C:\D2R\D2RMobile-harmony-shell-unsigned.hap'
$OutputHap = 'C:\D2R\D2RMobile-shell-signed.hap'
$Cert = 'C:\D2R\signing\developer.cer'
$Profile = 'C:\D2R\signing\device-debug.p7b'
$KeyStore = 'C:\D2R\signing\developer.p12'
$KeyAlias = 'keyAlias from your signing configuration'
$KeyPassword = [System.Net.NetworkCredential]::new('', (Read-Host 'Key password' -AsSecureString)).Password
$StorePassword = [System.Net.NetworkCredential]::new('', (Read-Host 'Keystore password' -AsSecureString)).Password
& $Java -jar $SignTool sign-app -mode localSign -keyAlias $KeyAlias `
  -appCertFile $Cert -profileFile $Profile -profileSigned 1 -inForm zip `
  -keystoreFile $KeyStore -signAlg SHA256withECDSA -compatibleVersion 260000026 `
  -signCode 1 -keyPwd $KeyPassword -keystorePwd $StorePassword `
  -inFile $InputHap -outFile $OutputHap
$SignResult = $LASTEXITCODE
Remove-Variable KeyPassword,StorePassword
if ($SignResult -ne 0) { throw 'Signing failed' }
& $Java -jar $SignTool verify-app -inFile $OutputHap `
  -outCertChain 'C:\D2R\verified-chain.cer' -outProfile 'C:\D2R\verified-profile.p7b'
if ($LASTEXITCODE -ne 0) { throw 'Signature verification failed' }
& $Hdc list targets
& $Hdc -t '<device identifier printed above>' install -r $OutputHap
```

`260000026` is the encoded compatibility value used for this HAP; follow the HAP/SDK requirements for other releases. `-signCode 1` is part of the command verified here. Check for `install bundle successfully`, not only the command exit code. Starting before import displays a missing-resource message; the executable-memory self-test was `ok` in this run.

Keep the same certificate and application identity for replacement upgrades. A different signature may prevent replacement; do not uninstall an application containing saves to work around a signing error.

## 2. Generate your import pack

From the matching source checkout:

```powershell
$env:DM_ROOT = 'C:\D2R\work'
python -X utf8 -B dm/tools/d2rm_android.py --target ohos 'C:\D2R\Update' 'C:\D2R\import'
```

The tool checks dependencies and writes `C:\D2R\import\harmony\dm`. A fresh run here produced 6,931 XGS shaders, 18 `ui-art` images, `derived.tsv` and `pack_info.txt`. If needed, configure existing tools with the reported `DM_DESWIZZLE_LIB`, `GLSLC` and `PATH` settings.

The executable modules in `out` must be generated for `ohos`; Android modules cannot replace them. Keep the generated pack and original game files on your own computer and phone.

## 3. Transfer and select the enclosing folder

Place this structure somewhere accessible to the phone's system file manager. A ZIP transferred from the computer must be extracted first:

```text
D2R Mobile/
  data/                  <- your complete Update/RomFS/data, including .build.config
  dm/                    <- the complete generated import/harmony/dm
    out/
    shaders/
    overlay/
    ui-art/
    derived.tsv
    pack_info.txt
```

Open the application, tap Import, select **D2R Mobile itself**, and confirm with the checkmark. The selected folder must directly contain both `data` and `dm`. Do not select either child or an extra enclosing ZIP extraction folder. The button's reference to a folder containing `data` also requires the complete sibling `dm` for a shell build.

Wait for copying to finish. The application loads the tables and starts the game in the same launch. Initial loading builds caches and can take time. The source folder is preserved, and saves remain in the application sandbox. Keeping the download ZIP, extracted source and imported copy used about 17.4 GB in this run.

## Errors and upgrades

- `-2` / `-5`: missing game data or build configuration; check the complete `data` and folder selection.
- `-3` / `-4`: missing executable modules or shaders; check the complete `dm`. Version 0.19.4 fixes first import into absent runtime directories.
- `-10` / `-11`: the import pack does not match this app. Regenerate it with matching tools. A wrong PRODUCT triggered `-11` on the test device; restoring the original metadata allowed startup again.
- `-12`: missing or incomplete `derived.tsv`; regenerate the full pack.

The shell checks PRODUCT/TARGET in `pack_info.txt` and the overlay fingerprint. External packs do not need `pack-version.txt`; that file tracks updates of the ZIP embedded in private developer builds.

For upgrades, use matching HAP/tools and the original signing identity, replace the application, and regenerate/import when required. Home-screen cards, system backup restoration, extended gameplay stability and the Pura X fast-forward stutter were not verified in this run.
