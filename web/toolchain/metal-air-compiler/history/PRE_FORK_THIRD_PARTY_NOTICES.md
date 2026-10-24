> Historical pre-fork documentation. Commands and source paths below describe the old build, not the current release consumer. See [the current integration](../../../docs/air-compiler.md).

# Third-party notices

Canonical upstream: https://github.com/a2flo/floor_llvm/tree/4b173e3db383ec7164afb11ee7dc86609c88f052

The prior baseline used https://github.com/gzorin/floor_llvm/tree/758f59eefe5db70f6e8719be8ca1beb939c38adb. Required downstream writer changes retain their source attribution in the rebased patch package; moving the base does not remove their provenance.

- LLVM and Clang files carry their upstream Apache License 2.0 with LLVM exceptions notices. Full text is in `licenses/LLVM-LICENSE.txt`; file-specific notices remain in the fetched source.
- Flo's Open libRary transforms, including `MetalImage.cpp`, `MetalFinal.cpp` and the upstream `MetalLibReflection` implementation, identify copyright 2004–2026 Florian Ziesche and GNU GPL version 2 only. Full text is in `licenses/GPL-2.txt`. Patches to those files retain the corresponding obligations; they are not relicensed by this package.
- The fetched source can contain additional third-party components and file-specific licenses. Preserve its notices and review applicable distribution obligations before shipping compiler binaries/WASM.
- `api/include/metal_stdlib` is an authored compatibility subset with its own MIT SPDX notice, not Apple's Metal SDK header. Its license does not change the licenses of the compiler it calls.

This source package intentionally does not include Apple SDK headers, proprietary compiler binaries, user game sources or reference libraries.

## Distribution scope / 分发范围

The repository MIT license does not relicense third-party compiler code. Preserve the respective component licenses and distribute the exact complete compiler source, modifications and build scripts with the corresponding WASM. The CI source and preview artifacts are temporary testing artifacts; a longer-lived public distribution must retain matching source and notices alongside that distribution. A separate Worker is not itself a legal determination that all components are independent.

仓库的 MIT 许可证不改变第三方编译器代码的许可证。分发对应 WASM 时应保留组件许可证，并同时提供准确完整的编译器源代码、修改和构建脚本。CI 源码和预览构件仅用于临时测试；长期公开分发须保留对应源码与声明。独立 Worker 本身不构成各组件法律上独立的结论。
