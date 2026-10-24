# Browser IPA packager

中文: [WEB_IPA_PACKAGER.zh-CN.md](WEB_IPA_PACKAGER.zh-CN.md)

Build an IPA from your own game files in desktop Chromium on Windows or macOS. Processing stays local; no game files or keys are uploaded. You do not need Xcode, Python, .NET or a compiler. The generated app runs natively on iOS, not in the browser, and the maintainer has confirmed it works on a real device.

## Prepare

Put these files directly in one folder:

- The supported base NSP and compatible update NSP(s).
- `prod.keys`.
- Optional compatible DLC and `title.keys`. Embedded tickets can supply title keys; rights-managed inputs still require a usable ticket or matching key.

Use your own legally obtained game files and keys. This repository does not distribute them. The packager selects compatible content from authenticated metadata, not filenames.

## Generate and install

1. Open the packager over HTTPS in a supported desktop browser. It checks browser capabilities and loads verified public runtimes before you select private inputs.
2. Select the input folder and grant read/write access. Original NSPs and key files are not overwritten.
3. Choose the target and optional settings, then generate. Settings remain editable until generation starts.
4. Download the completed IPA. If automatic download is blocked, use the visible download link. The browser controls where the IPA is saved.
5. Sign and install the IPA. Optional browser-local signing accepts your P12/PFX, export password and matching provisioning profile; see [signing preparation](../web/docs/ipa-signing.md). Credentials are not sent to a signing server. The signed IPA includes the provisioning profile, never the private signing identity or password. Without local signing, use your preferred signing tool separately.
6. Copy the completed `data` folder from the input folder into **On My iPhone / D2R Mobile**, or the corresponding app folder on iPad, using Files. Keep its name and contents intact. Game resources are separate from the IPA.

An existing `data` folder is reused only after a full bounded comparison with the current input. Mismatches stop export without changing it. If you explicitly skip original-resource export, you must already have resources for the same game, update and DLC; the browser does not verify them in that mode.

The experimental **Older-GPU option** also produces a separate `Legacy Device Pack` folder (`-2`, `-3`, etc. if the name exists) and matching IPA overlays. Copy the completed pack alongside `data`, preserving its own `data` child and `manifest.json`. Use the IPA and pack from the same run. This option is not covered by the reported device test.

## Limits and recovery

- The IPA uses ZIP32 and a 512 MiB archive limit. Its final download Blob can occupy that much memory. Bulk resources stream separately; disk space, browser quota and other component limits still apply.
- Generation is single-use. Cancellation closes the session; reload to start again.
- Never copy an incomplete resource export. A completed resource folder may remain valid even if a later IPA step fails.
- Public runtimes download before private processing. There is no remote conversion service or runtime CDN fallback. JavaScript cleanup is best effort, not secure memory erasure.

## Development

See [developer commands](../web/README.md), [production architecture](../web/docs/browser-lifecycle.md), [runtime builds](../web/docs/build-policy.md) and [testing](../web/docs/build-policy.md#verification-and-e2e-tooling).
