# Browser IPA packager

Package your local game files into an iOS IPA using desktop Chromium on Windows or macOS. Game files, keys and signing credentials stay on your computer. End users do not need Xcode, Python, .NET, Bun or a compiler.

The maintainer has confirmed that the generated IPA runs on a real iOS device.

## Use the packager

See the [packaging and installation guide](../docs/WEB_IPA_PACKAGER.md) ([中文](../docs/WEB_IPA_PACKAGER.zh-CN.md)) for inputs, signing, resource transfer and limits.

## Develop

Requires Bun 1.4.3 and Node 22.12 or later:

```sh
cd web
bun install --frozen-lockfile
bun run dev
bun run build
bun run preview
bun run test
```

The React/TypeScript frontend uses Tailwind and shadcn/ui [source components](SHADCN_PROVENANCE.md). Bun emits static files to `dist/`. Frontend compilation alone does not build the native browser runtimes; follow [runtime assembly](docs/build-policy.md) for a complete deployment. Serve that output over HTTPS. The development server requires page refreshes after rebuilding and must not be deployed.

[Build and verification](docs/build-policy.md) covers tooling, the shared Verify command and optional local checks. GitHub Verify builds and validates `web-preview`; eligible trusted-branch pushes deploy the same output to Pages. Pull requests do not deploy.

## Local end-to-end run

Run against a complete local `web-preview` build and your own input folder. Inputs stay local; `--export-resources` additionally writes the resource folder beside them.

```sh
bun run test:e2e:bun -- \
  --preview-dir /path/to/web-preview \
  --input-dir /path/to/nsp-folder \
  --nsp base.nsp --nsp update.nsp \
  --output /path/to/unsigned.ipa
```

To use a pinned native SPIRV-Cross CLI, set `D2R_E2E_SPIRV_CROSS_MODE=native`, `D2R_E2E_SPIRV_CROSS_CLI` to the executable, and `D2R_E2E_SPIRV_CROSS_SOURCE` to its source checkout. The runner verifies the source commit. This tests IPA assembly under Bun; browser builds use the WASM converter.

## Feature guides / 功能指南

Each guide keeps its English and Chinese documentation together and corresponds to one feature test suite in `tests/`.
每份指南将中英文说明放在同一个文件内，并对应 `tests/` 中的一份功能测试套件。

- [Application UI and customization / 界面与应用自定义](docs/application-ui.md)
- [Browser lifecycle / 浏览器任务生命周期](docs/browser-lifecycle.md)
- [Input validation and extraction / 输入校验与提取](docs/input-extraction.md)
- [Archive readers and codecs / 归档读取与编解码](docs/archive-codecs.md)
- [CASC storage / CASC 存储](docs/casc-storage.md)
- [Packaging and resource export / 打包与资源导出](docs/packaging-export.md)
- [Local IPA signing / 本地 IPA 签名](docs/ipa-signing.md)
- [Asset generation / 资源生成](docs/asset-generation.md)
- [ASTC production / ASTC 转换](docs/astc-production.md)
- [Shader pipeline / 着色器流水线](docs/shader-pipeline.md)
- [AIR compiler / AIR 编译器](docs/air-compiler.md)
- [Program rewriting / 程序改写](docs/program-rewriting.md)
- [Sealed runtime / 隔离运行时](docs/sealed-runtime.md)
- [Shell profiles / 外壳配置](docs/shell-profiles.md)
- [Build and release policy / 构建与发布策略](docs/build-policy.md)
