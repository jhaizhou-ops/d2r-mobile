import * as diagnosticSourceModule from "../runtime-build/prepare-air-compiler-diagnostic.mjs";
// Feature suite: air-compiler. Original test sections retain independent lexical scopes.
import * as strictModule from "node:assert/strict";
import * as nodeTestModule from "node:test";
import * as checkBufferReflectionModule from "../toolchain/metal-air-compiler/check-buffer-reflection.mjs";
import * as airCompilerAdapterModule from "../air-compiler-adapter.mjs";
import * as nodeChild_processModule from "node:child_process";
import * as assertIsolatedRunModule from "./helpers/assert-isolated-run.mjs";
import * as airCompilerContractModule from "../air-compiler-contract.mjs";
import * as airCompilerRuntimeContractModule from "../air-compiler-runtime-contract.mjs";
import * as transportModule from "../runtime-build/transport.mjs";
import * as clientsModule from "../sealed-runtime/clients.mjs";
import * as shellProfilesModule from "../shell-profiles.mjs";
import * as nodeCryptoModule from "node:crypto";
import * as promisesModule from "node:fs/promises";
import * as nodeOsModule from "node:os";
import * as nodePathModule from "node:path";
import * as nodeEventsModule from "node:events";
import * as promisesModule2 from "node:timers/promises";
import * as airCompilerWorkerClientModule from "../air-compiler-worker-client.mjs";
import * as nodeFsModule from "node:fs";
import * as checkFragmentInputLivenessModule from "../toolchain/metal-air-compiler/check-fragment-input-liveness.mjs";
import * as checkFragmentInputReflectionModule from "../toolchain/metal-air-compiler/check-fragment-input-reflection.mjs";
import * as nodeBufferModule from "node:buffer";
import * as airReleaseContractModule from "../runtime-build/air-release-contract.mjs";
import * as fixtureModule from "./fixtures/air/fixture.mjs";
import * as nodeZlibModule from "node:zlib";
import * as nodeUrlModule from "node:url";
import * as airReleaseTransportModule from "../runtime-build/air-release-transport.mjs";
import * as airReleaseLoaderModule from "../runtime-build/air-release-loader.mjs";

// air-buffer-reflection
{
  // 仅测试合成元数据解析，不执行编译器，也不提供 AIR 证据。
  // Synthetic metadata parser checks only, not compiler execution or AIR evidence.
  const { default: assert } = strictModule;
  const { default: test } = nodeTestModule;
  const { checkReflection, readReflection, checkFloat3Storage } = checkBufferReflectionModule;

  const fixture = {
    entry: "reflection_parser",
    buffer: {
      name: "source",
      binding: 0,
      addressSpace: 2,
      size: 48,
      align: 16,
      fields: [
        { name: "tag", offset: 0, size: 4, count: 0 },
        {
          name: "items",
          offset: 16,
          size: 16,
          count: 2,
          children: [{ name: "payload", offset: 0, size: 16, count: 0 }],
        },
      ],
    },
  };
  const ir = `define void @reflection_parser() { ret void }
!air.kernel = !{!0}
!0 = !{!1}
!1 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, !"air.address_space", i32 2, !"air.arg_type_size", i32 48, !"air.buffer_size", i32 48, !"air.arg_type_align_size", i32 16, !"air.arg_name", !"source", !"air.struct_type_info", !2}
!2 = !{i32 0, i32 4, i32 0, !"uint", !"tag", !"air.struct_type_info", !3, i32 16, i32 16, i32 2, !"Element", !"items"}
!3 = !{i32 0, i32 16, i32 0, !"float4", !"payload"}
`;

  test("reflection parser checks exact buffer and recursive array descriptors", () => {
    checkReflection(ir, fixture);
    assert.deepEqual(readReflection(ir, fixture), { ...fixture.buffer, bufferSize: 48 });
  });

  test("reflection checks reject wrong offsets, strides, counts, sizes, binding and alignment", () => {
    for (const [before, after] of [
      ['i32 16, i32 16, i32 2, !"Element"', 'i32 4, i32 16, i32 2, !"Element"'],
      ['i32 16, i32 16, i32 2, !"Element"', 'i32 16, i32 12, i32 2, !"Element"'],
      ['i32 16, i32 16, i32 2, !"Element"', 'i32 16, i32 16, i32 3, !"Element"'],
      ['!"air.arg_type_size", i32 48', '!"air.arg_type_size", i32 64'],
      ['!"air.buffer_size", i32 48', '!"air.buffer_size", i32 64'],
      ['!"air.arg_type_align_size", i32 16', '!"air.arg_type_align_size", i32 4'],
      ['!"air.location_index", i32 0', '!"air.location_index", i32 1'],
      ['!"air.address_space", i32 2', '!"air.address_space", i32 1'],
    ])
      assert.throws(() => checkReflection(ir.replace(before, after), fixture));
  });

  test("reflection parser rejects missing stage, entry, unique buffer and referenced fields", () => {
    for (const invalid of [
      ir.replace("!air.kernel", "!other"),
      ir.replace("@reflection_parser(", "@unexpected("),
      ir.replace('!"source"', '!"other"'),
      ir.replace('!"air.struct_type_info", !2', '!"air.struct_type_info", !99'),
      ir +
        ir
          .split("\n")
          .find((line) => line.startsWith("!1 ="))
          .replace("!1 =", "!4 ="),
    ])
      assert.throws(() => readReflection(invalid, fixture));
  });

  // 手写的解析输入，不是编译器结果或已编译参考产物。
  // Hand-authored parser input, not a compiler result or a compiled reference.
  const storage = `target datalayout = "e-p:64:64:64-i8:8:8-i32:32:32-f32:32:32-v96:128:128"
%struct.PaddedVectorLayout = type { i32, <3 x float>, [2 x <3 x float>], i32, [12 x i8] }
define void @reflection_float3(%struct.PaddedVectorLayout addrspace(2)* align 16 dereferenceable(80) %source, <4 x float> addrspace(1)* %output) {
  %direction = getelementptr inbounds %struct.PaddedVectorLayout, %struct.PaddedVectorLayout addrspace(2)* %source, i32 0, i32 1
  %path = getelementptr inbounds %struct.PaddedVectorLayout, %struct.PaddedVectorLayout addrspace(2)* %source, i32 0, i32 2, i32 1
  %state = getelementptr inbounds %struct.PaddedVectorLayout, %struct.PaddedVectorLayout addrspace(2)* %source, i32 0, i32 0
  %tail = getelementptr inbounds %struct.PaddedVectorLayout, %struct.PaddedVectorLayout addrspace(2)* %source, i32 0, i32 3
  %a = load <3 x float>, <3 x float> addrspace(2)* %direction, align 16
  %b = load <3 x float>, <3 x float> addrspace(2)* %path, align 16
  %c = load i32, i32 addrspace(2)* %state, align 16
  %d = load i32, i32 addrspace(2)* %tail, align 16
  ret void
}
`;

  test("float3 storage parser derives allocation and source addresses from type syntax", () => {
    checkFloat3Storage(storage);
    checkFloat3Storage(storage.replace(", i32, [12 x i8] }", ", i32 }"));
  });

  test("float3 storage rejects stale padding, wrong stride, wrong GEPs and absent evidence", () => {
    for (const invalid of [
      storage.replace(
        "i32, <3 x float>, [2 x <3 x float>], i32, [12 x i8]",
        "i32, [12 x i8], <3 x float>, [4 x i8], [2 x <3 x float>], [8 x i8], i32, [12 x i8]",
      ),
      storage.replace("v96:128:128", "v96:8:8"),
      storage.replace('target datalayout = "e-', 'target datalayout = "e-a:256:256-'),
      storage.replace('target datalayout = "e-', 'target datalayout = "e-a0:256:256-'),
      storage.replace("i32 2, i32 1", "i32 2, i32 0"),
      storage.replace("i32 0, i32 3", "i32 1, i32 3"),
      storage.replace("i32 0, i32 3", "i32 0, i32 4"),
      storage.replace("i32 2, i32 1", "i32 2, i32 2"),
      storage.replace("@reflection_float3", "@other"),
      storage.replace("v96:128:128", ""),
      storage.replace("%struct.PaddedVectorLayout = type", "%other = type"),
      storage.replace("* %path, align", "* %direction, align"),
      storage.replace("* %state, align", "* %other, align"),
      storage.replace(
        "  ret void",
        "  %extra = bitcast %struct.PaddedVectorLayout addrspace(2)* %source to i8 addrspace(2)*\n  ret void",
      ),
    ])
      assert.throws(() => checkFloat3Storage(invalid));
  });
}

// air-compiler-adapter
{
  // 验证 air-compiler-adapter 的约定与边界；依据 ../air-compiler-adapter.mjs 及本文件的测试断言。
  // Verify air-compiler-adapter contracts and boundaries against ../air-compiler-adapter.mjs and this file's assertions.
  const { default: assert } = strictModule;
  const { default: test } = nodeTestModule;
  const { createAirCompilerAdapter } = airCompilerAdapterModule;
  const fingerprint = "a".repeat(64),
    encoder = new TextEncoder();
  // 这里只模拟 C ABI 和内存增长，不编译着色器，也不构成 WASM 执行证据。
  // These tests simulate the C ABI and memory growth, not shader compilation or WASM execution.
  function fake() {
    const calls = [],
      output = new Uint8Array([1, 2, 3, 4]);
    const module = {
      HEAPU8: new Uint8Array(1024),
      status: 0,
      diagnosticSize: 0,
      resultSize: output.length,
    };
    module.HEAPU8.set(encoder.encode(fingerprint + "\0"), 16);
    const grow = () => {
      const next = new Uint8Array(module.HEAPU8.length * 2);
      next.set(module.HEAPU8);
      structuredClone(module.HEAPU8.buffer, { transfer: [module.HEAPU8.buffer] });
      module.HEAPU8 = next;
    };
    Object.assign(module, {
      _air_compiler_header_sha256: () => 16,
      _malloc(size) {
        module.inputSize = size;
        calls.push(["malloc", size]);
        grow();
        return 128;
      },
      _free(pointer) {
        calls.push(["free", pointer]);
        assert.ok(
          module.HEAPU8.subarray(pointer, pointer + module.inputSize).every((x) => x === 0),
        );
      },
      _air_compile_v1(pointer, size, ios, kind) {
        module.inputSize = size;
        module.input = new TextDecoder().decode(module.HEAPU8.subarray(pointer, pointer + size));
        calls.push(["compile", pointer, size, ios, kind]);
        grow();
        module.HEAPU8.set(output, 512);
        module.HEAPU8.set(encoder.encode("private_shader_identifier"), 768);
        module.onCompile?.();
        return 256;
      },
      _air_result_status: () => module.status,
      _air_result_data() {
        grow();
        return 512;
      },
      _air_result_size: () => module.resultSize,
      _air_result_diagnostics: () => 768,
      _air_result_diagnostics_size: () => module.diagnosticSize,
      _air_result_free(pointer) {
        calls.push(["result_free", pointer]);
        module.HEAPU8.fill(0, 512, 516);
        grow();
      },
    });
    return {
      module,
      calls,
      output,
      grow,
      create: (options = {}) =>
        createAirCompilerAdapter(module, { expectedHeaderSha256: fingerprint, ...options }),
    };
  }
  test("copies before result cleanup and reacquires heap after multiple detached-view growths", () => {
    const f = fake(),
      adapter = f.create();
    const result = adapter.compile("a\r\nb\n");
    assert.equal(f.module.input, "a\nb\n");
    assert.deepEqual(result.bytes, f.output);
    assert.equal(result.status, "experimental-output-unvalidated");
    assert.equal(result.requiresWorkerTermination, true);
    assert.equal(adapter.headerSha256, fingerprint);
    assert.equal(adapter.status, "experimental-floor-metal-3.2-not-apple-msl-compatible");
    assert.equal(adapter.used, true);
    assert.deepEqual(
      f.calls.map((x) => x[0]),
      ["malloc", "compile", "result_free", "free"],
    );
    assert.throws(() => adapter.compile("x"), { code: "AIR_MODULE_ALREADY_USED" });
  });
  test("supports only the exact output-kind, source-language and target ABI", () => {
    for (const options of [
      { output: "spirv" },
      { iosMajor: 17 },
      { sourceLanguage: "apple-msl-2.3" },
      { sourceLanguage: "floor-metal-3.0" },
    ]) {
      const f = fake();
      assert.throws(() => f.create().compile("x", options), { code: "AIR_BAD_OPTIONS" });
      assert.equal(f.calls.length, 0);
    }
    const f = fake();
    f.create().compile("x", {
      iosMajor: 26,
      output: "llvm-text",
      sourceLanguage: "floor-metal-3.2",
    });
    assert.deepEqual(f.calls.find((c) => c[0] === "compile").slice(-2), [26, 1]);
  });
  test("validates the pinned header fingerprint and exports without exposing module exceptions", () => {
    const f = fake();
    assert.throws(
      () => createAirCompilerAdapter(f.module, { expectedHeaderSha256: "b".repeat(64) }),
      { code: "AIR_HEADER_IDENTITY_MISMATCH" },
    );
    f.module._air_compiler_header_sha256 = () => {
      throw new Error("private_shader_identifier");
    };
    assert.throws(
      () => f.create(),
      (e) => e.code === "AIR_RUNTIME_FAILED" && !String(e).includes("private_shader_identifier"),
    );
    delete f.module._air_compile_v1;
    assert.throws(() => f.create(), { code: "AIR_EXPORT_MISSING" });
  });
  test("reports bounded diagnostic count but never raw source identifiers", () => {
    const f = fake();
    f.module.status = 3;
    f.module.diagnosticSize = 25;
    assert.throws(
      () => f.create().compile("x"),
      (e) =>
        e.code === "AIR_FRONTEND_FAILED" &&
        e.diagnosticBytes === 25 &&
        !JSON.stringify(e).includes("private_shader_identifier") &&
        !String(e).includes("private_shader_identifier"),
    );
    assert.deepEqual(
      f.calls.slice(-2).map((x) => x[0]),
      ["result_free", "free"],
    );
  });
  for (const [name, mutate, code] of [
    [
      "invalid output pointer",
      (m) => (m._air_result_data = () => 0xfffffff0),
      "AIR_POINTER_INVALID",
    ],
    ["null output pointer", (m) => (m._air_result_data = () => 0), "AIR_POINTER_INVALID"],
    ["oversized output", (m) => (m.resultSize = 64 * 1024 * 1024 + 1), "AIR_OUTPUT_LIMIT"],
    ["empty output", (m) => (m.resultSize = 0), "AIR_EMPTY_OUTPUT"],
    ["negative output size", (m) => (m.resultSize = -1), "AIR_OUTPUT_LIMIT"],
    ["fractional output size", (m) => (m.resultSize = 1.5), "AIR_OUTPUT_LIMIT"],
    ["invalid status", (m) => (m.status = 99), "AIR_STATUS_INVALID"],
    ["diagnostic overflow", (m) => (m.diagnosticSize = 65537), "AIR_DIAGNOSTIC_LIMIT"],
    [
      "diagnostic invalid pointer",
      (m) => {
        m.diagnosticSize = 1;
        m._air_result_diagnostics = () => 0;
      },
      "AIR_POINTER_INVALID",
    ],
    [
      "getter trap",
      (m) =>
        (m._air_result_size = () => {
          throw new Error("private_shader_identifier");
        }),
      "AIR_RUNTIME_FAILED",
    ],
  ])
    test(`cleans both allocations after ${name}`, () => {
      const f = fake();
      mutate(f.module);
      assert.throws(() => f.create().compile("x"), { code });
      assert.deepEqual(
        f.calls.slice(-2).map((x) => x[0]),
        ["result_free", "free"],
      );
    });
  test("frees input when compiler traps or returns no result", () => {
    for (const compile of [
      () => 0,
      () => {
        throw new Error("private_shader_identifier");
      },
    ]) {
      const f = fake();
      f.module._air_compile_v1 = compile;
      assert.throws(() => f.create().compile("x"));
      assert.equal(f.calls.at(-1)[0], "free");
      assert.equal(
        f.calls.some((x) => x[0] === "result_free"),
        false,
      );
    }
  });
  test("continues input cleanup when result cleanup traps", () => {
    const f = fake();
    f.module._air_result_free = () => {
      throw new Error("private_shader_identifier");
    };
    assert.throws(() => f.create().compile("x"), { code: "AIR_CLEANUP_FAILED" });
    assert.equal(f.calls.at(-1)[0], "free");
  });
  test("rejects reentrant compilation and requires a fresh module after use", () => {
    const f = fake(),
      adapter = f.create();
    f.module.onCompile = () =>
      assert.throws(() => adapter.compile("nested"), { code: "AIR_COMPILER_BUSY" });
    adapter.compile("x");
    assert.throws(() => adapter.compile("again"), { code: "AIR_MODULE_ALREADY_USED" });
  });
  test("pre-abort allocates nothing; abort noticed after synchronous return frees both allocations", () => {
    const f = fake(),
      adapter = f.create(),
      controller = new AbortController();
    controller.abort();
    assert.throws(() => adapter.compile("x", { signal: controller.signal }), {
      code: "AIR_ABORTED",
    });
    assert.equal(f.calls.length, 0);
    assert.equal(adapter.used, false);
    const other = new AbortController();
    f.module.onCompile = () => other.abort();
    assert.throws(() => adapter.compile("x", { signal: other.signal }), { code: "AIR_ABORTED" });
    assert.deepEqual(
      f.calls.slice(-2).map((x) => x[0]),
      ["result_free", "free"],
    );
  });
  test("dispose prevents work and is checked after synchronous compiler returns", () => {
    const f = fake(),
      adapter = f.create();
    f.module.onCompile = () => adapter.dispose();
    assert.throws(() => adapter.compile("x"), { code: "AIR_ADAPTER_DISPOSED" });
    assert.deepEqual(
      f.calls.slice(-2).map((x) => x[0]),
      ["result_free", "free"],
    );
    assert.throws(() => adapter.compile("x"), { code: "AIR_ADAPTER_DISPOSED" });
  });
  test("source limits apply to UTF8 bytes and reject empty or embedded NUL input", () => {
    for (const source of ["", "a\0b", "é".repeat(2097153)]) {
      const f = fake();
      assert.throws(() => f.create().compile(source), { code: "AIR_BAD_INPUT" });
      assert.equal(f.calls.length, 0);
    }
  });

  test("cannot wrap a module twice to evade single-use isolation", () => {
    const f = fake();
    const adapter = f.create();
    adapter.dispose();
    assert.throws(() => f.create(), { code: "AIR_MODULE_ALREADY_WRAPPED" });
  });
  test("allocation failure and invalid allocation pointers do not enter compiler", () => {
    for (const pointer of [0, -1, 0xffffffff]) {
      const f = fake();
      f.module._malloc = () => pointer;
      assert.throws(() => f.create().compile("x"));
      assert.equal(f.calls.length, 0);
    }
  });

  test("does not publish copied output if cancellation happens during cleanup callbacks", () => {
    const f = fake(),
      controller = new AbortController();
    const originalFree = f.module._free;
    f.module._free = (pointer) => {
      originalFree(pointer);
      controller.abort();
    };
    assert.throws(() => f.create().compile("x", { signal: controller.signal }), {
      code: "AIR_ABORTED",
    });
  });

  function fakeV2() {
    const f = fake();
    const compileV1 = f.module._air_compile_v1;
    f.module._air_compiler_header_bundle_version = () => 2;
    f.module._air_compile_v2 = (...args) => {
      const result = compileV1(...args.slice(0, 4));
      const call = f.calls.find((entry) => entry[0] === "compile");
      call[0] = "compile-v2";
      call.push(args[4]);
      return result;
    };
    return f;
  }
  test("v1 remains the default even when the module supports v2", () => {
    const f = fakeV2();
    const adapter = f.create();
    const result = adapter.compile("x");
    assert.equal(adapter.apiVersion, 1);
    assert.equal(result.apiVersion, 1);
    assert.equal(result.mathProfile, "conservative");
    assert.equal(result.headerBundleVersion, undefined);
    assert.equal(f.calls.find((entry) => entry[0] === "compile").length, 5);
  });
  test("explicit v2 selects the exact math profile and records provenance", () => {
    for (const mathProfile of [undefined, "conservative", "fast-f32"]) {
      const f = fakeV2();
      delete f.module._air_compile_v1;
      const adapter = f.create({ apiVersion: 2 });
      const result = adapter.compile("x", { mathProfile, iosMajor: 26, output: "llvm-text" });
      assert.equal(adapter.apiVersion, 2);
      assert.deepEqual(f.calls.find((entry) => entry[0] === "compile-v2").slice(-3), [
        26,
        1,
        mathProfile === "fast-f32" ? 1 : 0,
      ]);
      assert.deepEqual(result.bytes, f.output);
      assert.equal(result.apiVersion, 2);
      assert.equal(result.mathProfile, mathProfile ?? "conservative");
      assert.equal(result.headerBundleVersion, 2);
      assert.equal(result.headerSha256, fingerprint);
      assert.equal(result.status, "experimental-output-unvalidated");
      assert.throws(() => adapter.compile("again"), { code: "AIR_MODULE_ALREADY_USED" });
      assert.throws(() => f.create({ apiVersion: 1 }), { code: "AIR_MODULE_ALREADY_WRAPPED" });
    }
  });
  test("fast math requires explicit v2 and invalid profiles allocate nothing", () => {
    for (const [apiVersion, mathProfile] of [
      [1, "fast-f32"],
      [1, "fast"],
      [2, "fast"],
      [2, null],
      [2, 1],
    ]) {
      const f = fakeV2();
      const adapter = f.create({ apiVersion });
      assert.throws(() => adapter.compile("x", { mathProfile }), { code: "AIR_BAD_OPTIONS" });
      assert.equal(adapter.used, false);
      assert.equal(f.calls.length, 0);
    }
    for (const apiVersion of [0, 3, "2", null, NaN])
      assert.throws(() => fakeV2().create({ apiVersion }), { code: "AIR_BAD_OPTIONS" });
  });
  test("v2 requires its own export and exact header bundle version without fallback", () => {
    for (const name of ["_air_compile_v2", "_air_compiler_header_bundle_version"]) {
      const f = fakeV2();
      delete f.module[name];
      assert.throws(() => f.create({ apiVersion: 2 }), { code: "AIR_EXPORT_MISSING" });
      assert.equal(f.calls.length, 0);
    }
    for (const version of [1, 3, "2", undefined, NaN]) {
      const f = fakeV2();
      f.module._air_compiler_header_bundle_version = () => version;
      assert.throws(() => f.create({ apiVersion: 2 }), {
        code: "AIR_HEADER_BUNDLE_VERSION_MISMATCH",
      });
    }
  });
  test("v2 keeps header pinning and private runtime errors", () => {
    const f = fakeV2();
    assert.throws(() => f.create({ apiVersion: 2, expectedHeaderSha256: "b".repeat(64) }), {
      code: "AIR_HEADER_IDENTITY_MISMATCH",
    });
    f.module._air_compiler_header_bundle_version = () => {
      throw Error("private_shader_identifier");
    };
    assert.throws(
      () => f.create({ apiVersion: 2 }),
      (e) => e.code === "AIR_RUNTIME_FAILED" && !String(e).includes("private_shader_identifier"),
    );
  });
  test("v2 failure and cancellation preserve allocation cleanup and diagnostic privacy", () => {
    for (const abort of [false, true]) {
      const f = fakeV2(),
        controller = new AbortController();
      if (abort) f.module.onCompile = () => controller.abort();
      else {
        f.module.status = 3;
        f.module.diagnosticSize = 25;
      }
      assert.throws(
        () =>
          f
            .create({ apiVersion: 2 })
            .compile("x", { mathProfile: "fast-f32", signal: controller.signal }),
        (e) =>
          e.code === (abort ? "AIR_ABORTED" : "AIR_FRONTEND_FAILED") &&
          !String(e).includes("private_shader_identifier"),
      );
      assert.deepEqual(
        f.calls.slice(-2).map((entry) => entry[0]),
        ["result_free", "free"],
      );
    }
  });

  test("explicit bounded v2 reuse copies each output and stops at its compilation budget", () => {
    const f = fakeV2();
    const adapter = f.create({ apiVersion: 2, maxCompilations: 2 });
    const first = adapter.compile("first", { mathProfile: "fast-f32" });
    const second = adapter.compile("second", { mathProfile: "fast-f32" });
    assert.equal(first.requiresWorkerTermination, false);
    assert.equal(second.requiresWorkerTermination, true);
    assert.equal(adapter.compilationCount, 2);
    assert.deepEqual(first.bytes, f.output);
    assert.deepEqual(second.bytes, f.output);
    assert.notEqual(first.bytes.buffer, second.bytes.buffer);
    assert.throws(() => adapter.compile("third"), { code: "AIR_BATCH_LIMIT" });
  });

  test("bounded reuse is v2-only and poisons the module after a failed compile", () => {
    for (const options of [
      { maxCompilations: 2 },
      { apiVersion: 2, maxCompilations: 0 },
      { apiVersion: 2, maxCompilations: 257 },
    ])
      assert.throws(() => fakeV2().create(options), { code: "AIR_BAD_OPTIONS" });
    const f = fakeV2();
    const adapter = f.create({ apiVersion: 2, maxCompilations: 2 });
    f.module.status = 3;
    assert.throws(() => adapter.compile("invalid"), { code: "AIR_FRONTEND_FAILED" });
    f.module.status = 0;
    assert.throws(() => adapter.compile("valid"), { code: "AIR_ADAPTER_DISPOSED" });
  });

  test("frontend failure retains filtered prose before freeing result memory", () => {
    const f = fake();
    const diagnostic = [
      "unknown type name 'missing_texture'",
      "/Users/private/Game/shader.metal:4:7: error: use of undeclared identifier 'helper_fn'",
      "4 | kernel void SECRET_SOURCE() { secret(); }",
      "  |       ^",
      "error: prod.keys = PRIVATE_KEY_CANARY",
    ].join("\n");
    f.module.status = 3;
    f.module.diagnosticSize = encoder.encode(diagnostic).length;
    f.module.onCompile = () => f.module.HEAPU8.set(encoder.encode(diagnostic), 768);
    assert.throws(
      () => f.create().compile("synthetic"),
      (error) => {
        assert.equal(error.code, "AIR_FRONTEND_FAILED");
        assert.deepEqual(error.shaderDiagnostic.diagnostics, [
          "unknown type name 'missing_texture'",
          "source:4:7: error: use of undeclared identifier 'helper_fn'",
        ]);
        assert.equal(error.shaderDiagnostic.compilerPhase, "compiling");
        assert.doesNotMatch(JSON.stringify(error), /Users|Game|SECRET_SOURCE|PRIVATE_KEY_CANARY/);
        return true;
      },
    );
    assert.deepEqual(
      f.calls.slice(-2).map(([name]) => name),
      ["result_free", "free"],
    );
  });

  test("only the native frontend status admits unfamiliar diagnostic prose", () => {
    const lines = [
      "address space qualified parameter must have pointer type",
      "parameter may not be qualified with an address space",
      "type 'float4' cannot be used as a template argument",
      "non-const reference cannot bind to a temporary value",
      "a previously unseen diagnostic sentence remains available",
    ];
    const diagnostic = lines.join("\n");
    for (const status of [3, 4]) {
      const f = fake();
      f.module.status = status;
      f.module.diagnosticSize = encoder.encode(diagnostic).length;
      f.module.onCompile = () => f.module.HEAPU8.set(encoder.encode(diagnostic), 768);
      assert.throws(
        () => f.create().compile("synthetic"),
        (error) => {
          assert.equal(error.code, status === 3 ? "AIR_FRONTEND_FAILED" : "AIR_INVALID_IR");
          assert.deepEqual(
            error.shaderDiagnostic.diagnostics,
            status === 3 ? lines.map((line) => `frontend diagnostic: ${line}`) : [],
          );
          return true;
        },
      );
    }
    const f = fake();
    f.module.onCompile = () => {
      throw new Error(diagnostic);
    };
    assert.throws(
      () => f.create().compile("synthetic"),
      (error) => {
        assert.equal(error.code, "AIR_RUNTIME_FAILED");
        assert.equal(error.shaderDiagnostic.diagnostics, undefined);
        assert.doesNotMatch(JSON.stringify(error), /address space|parameter may|previously unseen/);
        return true;
      },
    );
  });
}

// air-compiler-lease
{
  // 隔离测试实际密封租约，替换编译和线程，不执行 WASM。
  // Isolate the actual sealed lease with synthetic compilation and workers, never executing WASM.
  const { default: test } = nodeTestModule;
  const { spawnSync } = nodeChild_processModule;
  const { assertIsolatedRun } = assertIsolatedRunModule;

  test("AIR compiler sealed lease handoff races (isolated module mocks)", () => {
    const env = { ...process.env };
    delete env.NODE_TEST_CONTEXT;
    const result = spawnSync(
      process.execPath,
      [
        "--experimental-test-module-mocks",
        "--test",
        "--test-reporter=tap",
        new URL("./fixtures/air/lease.mock.mjs", import.meta.url).pathname,
      ],
      { env, encoding: "utf8", timeout: 30000, maxBuffer: 1024 * 1024 },
    );
    assertIsolatedRun(result);
  });
}

// air-compiler-runtime-contract
{
  // 验证 air-compiler-runtime-contract 的约定与边界；依据 ../air-compiler-contract.mjs、../air-compiler-runtime-contract.mjs 及本文件的测试断言。
  // Verify air-compiler-runtime-contract contracts and boundaries against ../air-compiler-contract.mjs、../air-compiler-runtime-contract.mjs and this file's assertions.
  const { default: assert } = strictModule;
  const { default: test } = nodeTestModule;
  const { airCompilerIdentity, AIR_COMPILER_SOURCE_LANGUAGE } = airCompilerContractModule;
  const {
    AIR_COMPILER_RUNTIME,
    airCompilerReceiptOutput,
    airCompilerReceiptSourceRecord,
    airCompilerRuntimeComponent,
    assertAirCompilerReceiptMatchesComponent,
    validateAirCompilerBuildReceipt,
    validateAirCompilerRuntimeComponent,
  } = airCompilerRuntimeContractModule;

  // 仅使用自编元数据样例，不执行编译器，也不提供着色器输出证据。
  // Authored metadata fixtures only: no compiler execution or shader output evidence.
  const digest = "a".repeat(64);
  function receipt() {
    return {
      version: 2,
      id: AIR_COMPILER_RUNTIME.id,
      apiVersion: 2,
      headerBundleVersion: 2,
      sourceLanguage: "floor-metal-3.2",
      emscripten: "4.0.20",
      headerSha256: digest,
      sourceFingerprintSha256: digest,
      sourceManifestSha256: digest,
      sourceCommit: "a".repeat(40),
      sourceInputs: { "toolchain.mjs": digest },
      files: {
        "d2r_air_wasm.mjs": { bytes: 1, sha256: digest },
        "d2r_air_wasm.wasm": { bytes: 1, sha256: digest },
      },
    };
  }
  test("worker identity and authenticated receipt require the same migrated language", () => {
    assert.equal(AIR_COMPILER_SOURCE_LANGUAGE, "floor-metal-3.2");
    assert.equal(
      airCompilerIdentity({ factorySha256: digest, wasmSha256: digest, headerSha256: digest })
        .sourceLanguage,
      "floor-metal-3.2",
    );
    validateAirCompilerBuildReceipt(receipt());
    assert.throws(() => validateAirCompilerBuildReceipt({ ...receipt(), version: 1 }));
    for (const sourceLanguage of [undefined, "floor-metal-3.0", "apple-msl-3.2"])
      assert.throws(() => validateAirCompilerBuildReceipt({ ...receipt(), sourceLanguage }));
  });
  test("old API-v2 AIR metadata cannot be relabeled by a new loader", () => {
    const report = receipt();
    const component = airCompilerRuntimeComponent(report);
    const manifest = {
      components: { airCompiler: component },
      assets: ["entry", "factory", "wasm", "receipt"].map((name) => ({
        path: component[name],
        bytes: 1,
        sha256: digest,
      })),
    };
    assert.equal(validateAirCompilerRuntimeComponent(manifest), component);
    assertAirCompilerReceiptMatchesComponent(report, component);
    for (const sourceLanguage of [undefined, "floor-metal-3.0"])
      assert.throws(() =>
        validateAirCompilerRuntimeComponent({
          ...manifest,
          components: { airCompiler: { ...component, sourceLanguage } },
        }),
      );
  });
  test("pre-AIR inventory parsing remains compatible without inventing compilation", () => {
    assert.equal(validateAirCompilerRuntimeComponent({ assets: [] }), undefined);
    assert.throws(() => validateAirCompilerRuntimeComponent({ assets: [] }, { required: true }));
  });

  test("fresh fork receipt preserves identity and only aliases the two existing output paths", async () => {
    const { createFreshAirReleaseFixture } = await import("./fixtures/air/fixture.mjs");
    const { receipt } = createFreshAirReleaseFixture(),
      before = JSON.stringify(receipt);
    assert.equal(validateAirCompilerBuildReceipt(receipt), receipt);
    const component = airCompilerRuntimeComponent(receipt);
    assert.equal(component.id, AIR_COMPILER_RUNTIME.id);
    assert.equal(component.factorySha256, receipt.files["air_wasm.mjs"].sha256);
    assert.equal(component.wasmSha256, receipt.files["air_wasm.wasm"].sha256);
    assert.equal(
      airCompilerReceiptOutput(receipt, "d2r_air_wasm.wasm"),
      receipt.files["air_wasm.wasm"],
    );
    assert.throws(() => airCompilerReceiptOutput(receipt, "another.wasm"));
    assert.deepEqual(airCompilerReceiptSourceRecord(receipt), {
      path: "toolchain/metal-air-compiler/dist/source-lock.json",
      sha256: receipt.sourceLockSha256,
    });
    assert.equal(JSON.stringify(receipt), before);
    for (const change of [
      (r) => (r.apiVersion = 1),
      (r) => (r.buildTool.jobs = 0),
      (r) => (r.buildTool.versions.node = "v22.0.0"),
      (r) => (r.verification.smoke.cases = []),
      (r) => (r.verification.smoke.cases[1].status = 0),
      (r) => (r.files["air_wasm.wasm"].bytes = 128 * 1024 ** 2 + 1),
      (r) => (r.headerBundleVersion = 1),
      (r) => (r.sourceTreeClean = false),
      (r) => (r.sourceLockSha256 = "bad"),
      (r) => (r.sourceInputs = {}),
      (r) => (r.sourceInputs["../escape"] = "a".repeat(64)),
      (r) => (r.sourceManifestSha256 = "a".repeat(64)),
      (r) => (r.files["d2r_air_wasm.wasm"] = r.files["air_wasm.wasm"]),
      (r) => (r.buildTool.emscriptenVersion = "4.0.21"),
      (r) => (r.verification.headerSha256 = "0".repeat(64)),
      (r) => (r.verification.deviceExecutionVerified = true),
    ]) {
      const invalid = structuredClone(receipt);
      change(invalid);
      assert.throws(() => validateAirCompilerBuildReceipt(invalid));
    }
  });
}

// air-compiler-runtime-limits
{
  // 纯元数据边界测试：不分配大型载荷、不构建或执行编译器。
  // Metadata-only boundary checks: no large payload allocation, compiler build or execution.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const {
    AIR_COMPILER_RUNTIME,
    airCompilerOutputMaximumBytes,
    runtimeAssetMaximumBytes,
    runtimeTransportMaximumBytes,
    validateAirCompilerBuildReceipt,
  } = airCompilerRuntimeContractModule;
  const { validateRuntimeTransport } = transportModule;
  const { validateRuntimeInventory, validateRuntimeProfileSelection } = clientsModule;
  const { resolveShellProfile } = shellProfilesModule;

  const MiB = 1024 * 1024;
  const digest = "a".repeat(64);
  function fixture(wasmBytes = 80 * MiB) {
    const component = {
      ...AIR_COMPILER_RUNTIME,
      sealedEntry: AIR_COMPILER_RUNTIME.entry,
      sealedJobAdapter: true,
      headerSha256: digest,
      sourceFingerprintSha256: digest,
      factorySha256: "2".repeat(64),
      wasmSha256: "3".repeat(64),
    };
    const profile = resolveShellProfile();
    const assets = ["entry", "factory", "wasm", "receipt"].map((name, index) => ({
      path: component[name],
      bytes: name === "wasm" ? wasmBytes : 1,
      sha256: String(index + 1).repeat(64),
    }));
    assets.push({
      path: profile.archivePath,
      bytes: profile.evidence.archiveBytes,
      sha256: profile.evidence.sha256,
    });
    const files = assets.map((asset) => ({
      path: `payloads/${asset.sha256}.bin`,
      bytes: asset.bytes,
      sha256: asset.sha256,
      encoding: "identity",
      decodedBytes: asset.bytes,
    }));
    const totalBytes = assets.reduce((total, asset) => total + asset.bytes, 0);
    return {
      version: 1,
      assets,
      totalBytes,
      components: { airCompiler: component },
      transport: {
        version: 1,
        totalBytes,
        files,
        entries: assets.map((asset, index) => ({
          bytes: asset.bytes,
          sha256: asset.sha256,
          file: files[index].path,
          offset: 0,
        })),
      },
    };
  }

  test("only the exact compiler WASM path gets the 128 MiB asset allowance", () => {
    assert.equal(runtimeAssetMaximumBytes(AIR_COMPILER_RUNTIME.wasm), 128 * MiB);
    for (const path of [
      "other.wasm",
      AIR_COMPILER_RUNTIME.factory,
      AIR_COMPILER_RUNTIME.wasm + ".bak",
    ])
      assert.equal(runtimeAssetMaximumBytes(path), 64 * MiB);
    assert.equal(airCompilerOutputMaximumBytes("d2r_air_wasm.mjs"), MiB);
    assert.equal(airCompilerOutputMaximumBytes("d2r_air_wasm.wasm"), 128 * MiB);
    assert.throws(() => airCompilerOutputMaximumBytes("other.wasm"));
  });

  test("Node and browser transport accept a sole large compiler payload, not oversized ordinary assets", () => {
    const manifest = fixture();
    validateRuntimeTransport(manifest);
    validateRuntimeInventory(manifest);
    validateRuntimeProfileSelection(manifest);
    const ordinary = structuredClone(manifest);
    ordinary.assets[2].path = "other.wasm";
    assert.throws(() => validateRuntimeTransport(ordinary));
    assert.throws(() => validateRuntimeInventory(ordinary));
    assert.throws(() => validateRuntimeProfileSelection(ordinary));
  });

  test("large compiler payload allowance never applies to mixed, offset or mismatched slices", () => {
    for (const mutate of [
      (manifest) => {
        manifest.transport.entries[2].offset = 1;
      },
      (manifest) => {
        manifest.transport.entries[2].sha256 = digest;
      },
      (manifest) => {
        manifest.transport.files[2].decodedBytes++;
      },
      (manifest) => {
        manifest.transport.entries.push({ ...manifest.transport.entries[2] });
      },
      (manifest) => {
        manifest.transport.entries[1].file = manifest.transport.files[2].path;
      },
    ]) {
      const manifest = fixture();
      mutate(manifest);
      assert.equal(runtimeTransportMaximumBytes(manifest.transport.files[2], manifest), 64 * MiB);
      assert.throws(() => validateRuntimeTransport(manifest));
      assert.throws(() => validateRuntimeProfileSelection(manifest));
    }
  });

  test("the compiler-specific asset and decoded limits still stop at 128 MiB", () => {
    const manifest = fixture(128 * MiB + 1);
    assert.throws(() => validateRuntimeInventory(manifest));
    assert.throws(() => validateRuntimeTransport(manifest));
    assert.throws(() => validateRuntimeProfileSelection(manifest));
  });

  test("receipts separately bound the small factory and compiler module", () => {
    const report = {
      version: 2,
      id: AIR_COMPILER_RUNTIME.id,
      apiVersion: 2,
      headerBundleVersion: 2,
      sourceLanguage: AIR_COMPILER_RUNTIME.sourceLanguage,
      emscripten: "4.0.20",
      headerSha256: digest,
      sourceFingerprintSha256: digest,
      sourceManifestSha256: digest,
      sourceCommit: "a".repeat(40),
      sourceInputs: { "toolchain.mjs": digest },
      files: {
        "d2r_air_wasm.mjs": { bytes: MiB, sha256: digest },
        "d2r_air_wasm.wasm": { bytes: 128 * MiB, sha256: digest },
      },
    };
    validateAirCompilerBuildReceipt(report);
    for (const name of Object.keys(report.files)) {
      const changed = structuredClone(report);
      changed.files[name].bytes++;
      assert.throws(() => validateAirCompilerBuildReceipt(changed));
    }
  });
}

// air-compiler-source-provenance
{
  // 验证 air-compiler-source-provenance 的约定与边界；依据 ./fixtures/air/fixture.mjs、./check-buffer-reflection.mjs 及本文件的测试断言。
  // Verify air-compiler-source-provenance contracts and boundaries against ./fixtures/air/fixture.mjs、./check-buffer-reflection.mjs and this file's assertions.
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { readFile, readdir, mkdtemp, mkdir, writeFile, rm, symlink, truncate } = promisesModule;
  const { tmpdir } = nodeOsModule;
  const { dirname, join } = nodePathModule;
  const { default: test } = nodeTestModule;
  const { sourceReviewOptions, readForkSource, readReviewedSourceLock } = fixtureModule;

  const root = new URL("../toolchain/metal-air-compiler/", import.meta.url);
  const hash = (bytes) => createHash("sha256").update(bytes).digest("hex");

  test(
    "reviewed fragment liveness API, checker and public source keep their frozen identity",
    sourceReviewOptions,
    async () => {
      const expected = {
        "check-fragment-input-liveness.mjs":
          "32b8924e6aaf6b9f98f0211fd3a47a00c2e509f67cdf92454965cd029e6a3eae",
        "check-fragment-input-reflection.mjs":
          "3178cd353cad0f4966e10ec13b30ad4b3f42ea84e61cbbdef162429d8b252907",
        "fixtures/msl-fragment-input-liveness.metal":
          "0820f23da050fa12a4031a45c09dec58a4394f7179032570c1acd85a20d434d5",
      };
      for (const [path, sha256] of Object.entries(expected))
        assert.equal(hash(await readFile(new URL(path, root))), sha256, path);
      assert.equal(
        hash(await readForkSource("wasm/api/air_compiler.cpp")),
        "5d06207b69e57435934bec8e4e67e71c014d6c890fe2ff625e96cc1d4dc7a448",
      );
    },
  );

  // 只检查源码来源，不加载或执行编译器产物。
  // Source-only provenance checks: no compiler artifact is loaded or executed.
  test(
    "reviewed fork source lock authenticates all 50 current inputs and native fixture identities",
    sourceReviewOptions,
    async () => {
      const lock = await readReviewedSourceLock();
      const inputs = {
        ...lock.compilerFiles,
        ...Object.fromEntries(
          Object.entries(lock.sdkFiles).map(([path, sha]) => [`wasm/${path}`, sha]),
        ),
      };
      assert.equal(Object.keys(inputs).length, 50);
      for (const [path, sha256] of Object.entries(inputs)) {
        assert.match(path, /^(?:clang|llvm|wasm)\/[A-Za-z0-9_.+/-]+$/);
        assert(!path.split("/").includes(".."));
        assert.equal(hash(await readForkSource(path)), sha256, path);
      }
      const sdkApiInputs = Object.keys(lock.sdkFiles).filter((path) => path.startsWith("api/"));
      assert.equal(sdkApiInputs.length, 18, "compiler/API sources belong to the generic fork");
      // Reviewed fixture identities include bilingual comments; executable bytes match the original receipt.
      const nativeFixtureDigests = {
        "control_smoke.cpp": "68050d99a3d491015aa9366cd9597c289ec3109c6ad2ca234437ac4b08efbd19",
        "header_smoke.cpp": "7c9767e8966f80f12baabeaeb9625e395f76def68ebd4a28fd863ba485144720",
        "operations_smoke.h": "f4eba86a452a22b52bec7aef08b57c98a5a28a40bc8a8695ed009fae3e663323",
        "smoke.cpp": "30baccb084c3787bdb36a21746891c265b4f3b07004a79f4fe030060485c30cb",
      };
      for (const name of [
        "control_smoke.cpp",
        "header_smoke.cpp",
        "operations_smoke.h",
        "smoke.cpp",
      ]) {
        assert.equal(Object.hasOwn(lock.sdkFiles, `api/${name}`), false);
        assert.equal(
          hash(await readFile(new URL(`fixtures/native-api/${name}`, root))),
          nativeFixtureDigests[name],
          `relocated authored ${name}`,
        );
      }
    },
  );

  test("source review override rejects unknown, missing, tampered and symlinked blobs without fallback", async () => {
    const reviewDirectory = await mkdtemp(join(tmpdir(), "air-source-review-"));
    const options = { reviewDirectory };
    const path = "wasm/api/air_compiler.cpp";
    const target = join(reviewDirectory, path);
    const originalFetch = globalThis.fetch;
    globalThis.fetch = () => {
      throw new Error("source-only review must not fetch or fall back");
    };
    try {
      for (const path of [
        "../manifest.json",
        "/wasm/build.mjs",
        "wasm//build.mjs",
        "wasm/./build.mjs",
      ])
        await assert.rejects(readForkSource(path, options), /unsafe fork path/);
      await assert.rejects(
        readForkSource("wasm/unreviewed.cpp", options),
        /No reviewed source identity/,
      );
      await assert.rejects(readForkSource(path, options), { code: "ENOENT" });
      await assert.rejects(
        readForkSource(path, { reviewDirectory: "" }),
        /empty source review directory/,
      );
      await mkdir(dirname(target), { recursive: true });
      await writeFile(target, "unreviewed source bytes");
      await assert.rejects(readForkSource(path, options), /Reviewed fork source mismatch/);
      await truncate(target, 0);
      await assert.rejects(readForkSource(path, options), /source review blob size/);
      await truncate(target, 16 * 1024 ** 2 + 1);
      await assert.rejects(readForkSource(path, options), /source review blob size/);
      await rm(target);
      await mkdir(target);
      await assert.rejects(
        readForkSource(path, options),
        /source review blob must be a regular file/,
      );
      await rm(target, { recursive: true });
      await symlink(new URL("check-fragment-input-liveness.mjs", root), target);
      await assert.rejects(
        readForkSource(path, options),
        /source review path must not traverse symlinks/,
      );
      await rm(target);
      await rm(dirname(target), { recursive: true });
      await mkdir(join(reviewDirectory, "redirected"));
      await writeFile(
        join(reviewDirectory, "redirected", "air_compiler.cpp"),
        "unreviewed source bytes",
      );
      await symlink(join(reviewDirectory, "redirected"), dirname(target));
      await assert.rejects(
        readForkSource(path, options),
        /source review path must not traverse symlinks/,
      );
      await writeFile(join(reviewDirectory, "wasm", "source-lock.json"), "{}");
      await assert.rejects(
        readForkSource("wasm/source-lock.json", options),
        /Reviewed fork source mismatch/,
      );
    } finally {
      globalThis.fetch = originalFetch;
      await rm(reviewDirectory, { recursive: true, force: true });
    }
  });

  test("review source-directory override is absent from production release modules", async () => {
    for (const name of ["contract", "transport", "loader"]) {
      const source = await readFile(
        new URL(`../runtime-build/air-release-${name}.mjs`, import.meta.url),
        "utf8",
      );
      assert.doesNotMatch(
        source,
        /D2R_AIR_SOURCE_REVIEW_DIR|fixtures\/air\/|generic-no-added-license-labels/,
      );
    }
  });

  test("consumer removes compiler duplication but retains authored fixtures and receipt-bound browser smoke", async () => {
    const entries = await readdir(root);
    for (const obsolete of [
      "api",
      "patches",
      "toolchain.mjs",
      "manifest.json",
      "header-identity.mjs",
      "wasm-node-preflight.mjs",
    ])
      assert(!entries.includes(obsolete), `obsolete local compiler implementation: ${obsolete}`);
    for (const retained of [
      "ci-compile-canary.mjs",
      "check-buffer-reflection.mjs",
      "check-fragment-input-liveness.mjs",
      "check-fragment-input-reflection.mjs",
      "fixtures",
    ])
      assert(entries.includes(retained), `D2R regression coverage retained: ${retained}`);
    assert.deepEqual((await readdir(new URL("fixtures/native-api/", root))).sort(), [
      "control_smoke.cpp",
      "header_smoke.cpp",
      "operations_smoke.h",
      "smoke.cpp",
    ]);
    const server = await readFile(new URL("browser-smoke-server.mjs", root), "utf8");
    for (const marker of [
      'args[0] !== "--build"',
      'args[2] !== "--sha256"',
      "await verifyAirCompilerAssets(build, args[3])",
      "const { apiVersion, headerSha256 } = report",
      'airCompilerReceiptOutput(report, "d2r_air_wasm.mjs").sha256',
      'airCompilerReceiptOutput(report, "d2r_air_wasm.wasm").sha256',
      'server.listen(0, "127.0.0.1"',
      '"shader-diagnostics.mjs"',
    ])
      assert(server.includes(marker), marker);
    assert(
      server.indexOf("await verifyAirCompilerAssets(") < server.indexOf("const routes = new Map()"),
    );
    assert.doesNotMatch(server, /readAirHeaderIdentity|--api-source|api-build-wasm/);
  });

  test("source delivery mirrors exact fresh source and raw receipts without reconstructing a compiler", async () => {
    const source = await readFile(
      new URL("../runtime-build/prepare-air-compiler-source.mjs", import.meta.url),
      "utf8",
    );
    for (const marker of [
      "readFreshAirReleaseMetadata({ download: true })",
      'readFreshAirReleaseAsset(metadata, "correspondingSource", { download: true })',
      "await verifyAirCompilerAssets()",
      "verifyFreshAirReceipt(receipt, metadata.release, metadata.buildInputs)",
      "await put(metadata.release.assets.correspondingSource.name, source.bytes)",
      'await put("compiler-build-report.json", receipt)',
      'await put("release-manifest.json", metadata.manifestBytes)',
      'await put("build-inputs.json", metadata.buildInputsBytes)',
      'kind: "pinned-release-source-mirror"',
      "completeReleaseSourceVerified: true",
      "installedSdkRuntimeSourcesIncluded: true",
      "llvmCompiledByThisStep: false",
      "binaryBuiltFromNewFork: true",
    ])
      assert(source.includes(marker), marker);
    assert(source.indexOf("verifyFreshAirReceipt(receipt") < source.indexOf("await mkdir(output)"));
    assert(
      source.indexOf('readFreshAirReleaseAsset(metadata, "correspondingSource"') <
        source.indexOf("await mkdir(output)"),
    );
    assert.doesNotMatch(
      source,
      /runToolchain|build-host-tools|build-wasm|reconstructedPatchedSourceVerified|originalCompleteSourceZipVerified/,
    );
  });

  test(
    "stage-decorated resource storage retains its exact reviewed missing-stage-in guard",
    sourceReviewOptions,
    async () => {
      const source = await readForkSource("clang/lib/CodeGen/CodeGenModule.cpp");
      assert.equal(
        hash(source),
        "24a2c4602921cb219f27ed3edc2db2db924c000109177e0dc3588155879da678",
      );
      for (const guard of [
        "!Stage && (T->isPointerType() || T->isReferenceType())",
        "HasOnlyBufferBinding &= isa<MetalBufferBindingAttr>(A)",
        "IsStoragePointer && HasOnlyBufferBinding",
        "AS == LangAS::opencl_global || AS == LangAS::opencl_constant",
        "IsStoragePointer && is_kernel",
        "P->hasAttr<MetalThreadgroupBindingAttr>() && AS == LangAS::opencl_local",
        "!IsBoundBuffer && !IsBoundThreadgroup && has_msl_io",
      ])
        assert(source.includes(guard), guard);
    },
  );

  test(
    "ordinary buffer reflection uses reviewed AST layout without changing special paths",
    sourceReviewOptions,
    async () => {
      const source = await readForkSource("clang/lib/CodeGen/CodeGenModule.cpp");
      assert.equal(
        hash(source),
        "24a2c4602921cb219f27ed3edc2db2db924c000109177e0dc3588155879da678",
      );
      for (const guard of [
        "!is_indirect && !indirect_buffer && !align_vectors",
        "!parent_decl.hasAttr<GraphicsStageInputAttr>() && !struct_rdecl.isUnion()",
        "!struct_rdecl.getNumBases() && !struct_rdecl.isPolymorphic()",
        "use_ast_layout &= !field->isBitField()",
        "use_ast_layout ? &Context.getASTRecordLayout(&struct_rdecl) : nullptr",
        "record_layout->getFieldOffset(field->getFieldIndex()) / Context.getCharWidth()",
        "Context.getTypeSizeInChars(field_type).getQuantity()",
        "!clang_pointee_type->isIncompleteType() && !clang_pointee_type->isFunctionType()",
        "Context.getTypeAlignInChars(clang_pointee_type).getQuantity()",
        "indirect_buffer ? uint64_t(8u) : uint64_t(16u)",
      ])
        assert(source.includes(guard), guard);
    },
  );

  test(
    "AIR64 initial/final storage layouts match and fork preparation enforces equality",
    sourceReviewOptions,
    async () => {
      const target = await readForkSource("clang/lib/Basic/Targets/SPIR.h");
      assert.equal(
        hash(target),
        "e90c20a141c7918f6d4c922f73965bfa2e4d540b17c62305665d0460f43ed48a",
      );
      const finalization = await readForkSource("llvm/lib/Transforms/LibFloor/MetalFinal.cpp");
      const initialLayout = target.match(
        /class LLVM_LIBRARY_VISIBILITY AIR64TargetInfo\b[\s\S]*?resetDataLayout\("([^"]+)"\);/,
      );
      const finalLayouts = [...finalization.matchAll(/\bMod\.setDataLayout\("([^"]+)"\);/g)];
      assert(initialLayout);
      assert.equal(finalLayouts.length, 1);
      assert.equal(initialLayout[1], finalLayouts[0][1]);
      assert(initialLayout[1].includes("v96:128:128"));
      assert.deepEqual(
        initialLayout[1].split("-").filter((part) => !part.startsWith("v")),
        "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-n8:16:32".split(
          "-",
        ),
        "pointer, scalar and native-width layout clauses remain unchanged",
      );
      const build = await readForkSource("wasm/build.mjs");
      assert(build.includes('join(source, "clang/lib/Basic/Targets/SPIR.h")'));
      assert(build.includes('join(source, "llvm/lib/Transforms/LibFloor/MetalFinal.cpp")'));
      assert(build.includes("AIR64TargetInfo"));
      assert(
        build.includes("finalLayouts.length !== 1 || initialLayout[1] !== finalLayouts[0][1]"),
      );
      assert(
        build.includes('throw Error("AIR64 initial and final storage DataLayouts must match")'),
      );
    },
  );

  test(
    "writer consumes unused fragment and position flags without changing prior corrections",
    sourceReviewOptions,
    async () => {
      const source = await readForkSource("llvm/lib/Bitcode/MetalLib/MetalLibReflection.cpp");
      assert.equal(
        hash(source),
        "f57d14824b7beb9fc61f1cd527c06aea830302b8f46f840e657601d1b0e4e85b",
      );
      const flag =
        /\} else if \(type_str == "air\.arg_unused"\) \{\s*\/\/ A zero-operand flag; preserve it in the serialized reflection\.\s*if \(node\.unused\.has_value\(\)\) return false;\s*node\.unused = true;\s*return true;/g;
      assert.equal(
        [...source.matchAll(flag)].length,
        2,
        "fragment and position flags reject duplicate markers",
      );
    },
  );

  test("four public reflection probes retain byte identity and existing runner/canary coverage", async () => {
    const inventory = await readFile(new URL("fixtures/buffer-reflection.json", root), "utf8");
    assert.equal(
      hash(inventory),
      "d03a386cf2ccf443c285ce9f41e850a00b7a187c06b4bab8e56f29aa756cd298",
    );
    const fixtures = JSON.parse(inventory);
    const publicHashes = {
      "fixtures/msl-buffer-reflection-mixed.metal":
        "aa008f242806c8e5625db92b364fe87216df3695ebfb1528d393b053ebb4267a",
      "fixtures/msl-buffer-reflection-array.metal":
        "1f43fb6b0d15115e1cbe8ea624c64a6a4eb6f785a9c852a4e5428f7c1491c20f",
      "fixtures/msl-buffer-reflection-float3.metal":
        "f883a9c3d6f0e73681f1c95f6539597ddf31f2f8c4703d9590c9b81e4dfa7733",
      "fixtures/msl-buffer-reflection-nested.metal":
        "357404a163024f6102503ee483cba9e2e6bc51963ae1f13e930e424ffe1ea82b",
    };
    assert.deepEqual(fixtures.map(({ file }) => file).sort(), Object.keys(publicHashes).sort());
    const canary = await readFile(new URL("ci-compile-canary.mjs", root), "utf8");
    for (const fixture of fixtures) {
      const source = await readFile(new URL(fixture.file, root), "utf8");
      assert.equal(hash(source), publicHashes[fixture.file]);
      assert.equal(fixture.sha256, publicHashes[fixture.file]);
      assert(source.includes("SPDX-License-Identifier: MIT"));
      assert(source.includes("#include <metal_stdlib>"));
      assert(source.includes(`kernel void ${fixture.entry}(`));
      assert.equal(fixture.stage, "kernel");
      assert.equal(fixture.expected, "compiled");
      assert.equal(fixture.buffer.align, 16);
      assert(canary.includes(`filename: "${fixture.file.replace("fixtures/", "")}"`));
      assert(canary.includes(`entry: "${fixture.entry}"`));
    }
    const runner = await readFile(new URL("run-msl-fixtures.mjs", root), "utf8");
    assert(runner.includes('"fixtures/buffer-reflection.json"'));
    assert(runner.includes('import { checkReflection } from "./check-buffer-reflection.mjs"'));
    assert(runner.includes("checkReflection(result.stdout, fixture)"));
  });

  test("bundled stage/storage negatives materialize exact pinned source bytes and filenames", async () => {
    const fixtures = JSON.parse(
      await readFile(new URL("fixtures/stage-storage.json", root), "utf8"),
    );
    const negatives = fixtures.filter(({ expected }) => expected === "rejected");
    assert.equal(negatives.length, 15);
    assert.equal(new Set(negatives.map(({ file }) => file)).size, 15);
    const temp = await mkdtemp(join(tmpdir(), "air-stage-storage-"));
    try {
      for (const fixture of negatives) {
        assert.match(fixture.file, /^fixtures\/stage-storage-negative\/[a-z0-9-]+\.metal$/);
        assert(
          Array.isArray(fixture.sourceLines) &&
            fixture.sourceLines.every((line) => typeof line === "string"),
        );
        const source = fixture.sourceLines.join("\n");
        assert.equal(hash(source), fixture.sha256, fixture.file);
        assert(source.includes("SPDX-License-Identifier: MIT"));
        const path = join(temp, fixture.file);
        await mkdir(dirname(path), { recursive: true });
        await writeFile(path, source);
        assert.equal(await readFile(path, "utf8"), source);
      }
      assert.deepEqual(
        (await readdir(join(temp, "fixtures/stage-storage-negative"))).sort(),
        negatives.map(({ file }) => file.split("/").at(-1)).sort(),
      );
    } finally {
      await rm(temp, { recursive: true, force: true });
    }
  });

  test(
    "stage/storage regression inventory preserves all authored source bytes and native wiring",
    sourceReviewOptions,
    async () => {
      const fixtures = JSON.parse(
        await readFile(new URL("fixtures/stage-storage.json", root), "utf8"),
      );
      assert.equal(fixtures.length, 17);
      assert.equal(fixtures.filter(({ expected }) => expected === "compiled").length, 2);
      assert.equal(fixtures.filter(({ expected }) => expected === "rejected").length, 15);
      assert.equal(new Set(fixtures.map(({ file }) => file)).size, fixtures.length);
      assert(
        fixtures
          .filter(({ expected }) => expected === "rejected")
          .every(({ sourceLines }) => Array.isArray(sourceLines)),
      );
      const canary = await readFile(new URL("ci-compile-canary.mjs", root), "utf8");
      const publicHashes = {
        "fixtures/msl-stage-record-threadgroup.metal":
          "f4475a8a668fbf24f1c4b754dbd5fb30d74b934bd6802f7fa10b0734d341b5c5",
        "fixtures/msl-stage-record-buffers.metal":
          "c99dbf7384190f672de1661efc56b38c6a43741c7b411da6c63fdea76f290de8",
      };
      for (const fixture of fixtures) {
        assert.match(fixture.file, /^fixtures\/(?:stage-storage-negative\/)?[a-z0-9-]+\.metal$/);
        const source =
          fixture.expected === "rejected"
            ? fixture.sourceLines.join("\n")
            : await readFile(new URL(fixture.file, root), "utf8");
        assert.equal(hash(source), fixture.sha256, fixture.file);
        assert(source.includes("SPDX-License-Identifier: MIT"));
        assert(source.includes("#include <metal_stdlib>"));
        assert.doesNotMatch(source, /#line|#pragma OPENCL EXTENSION/);
        if (fixture.expected === "rejected") {
          assert.equal(typeof fixture.diagnostic, "string");
          assert(fixture.diagnostic.length > 0);
        } else {
          assert.equal(fixture.stage, "kernel");
          assert.equal(fixture.sha256, publicHashes[fixture.file]);
          assert(canary.includes(`filename: "${fixture.file.replace("fixtures/", "")}"`));
          assert(canary.includes(`entry: "${fixture.entry}"`));
          assert(source.includes(`kernel void ${fixture.entry}(`));
          assert(source.includes("[[stage_in]]"));
        }
      }
      const runner = await readFile(new URL("run-msl-fixtures.mjs", root), "utf8");
      for (const marker of [
        '"fixtures/stage-storage.json"',
        "assert.equal(sha(source), fixture.sha256",
        "assert.ifError(result.error)",
        "assert.equal(result.signal, null",
        "assert.notEqual(result.status, 0",
        "result.stderr.includes(fixture.diagnostic)",
        "assert.equal(result.status, 0",
        '"one stage input field"',
        '"air.buffer"',
        '"air.location_index"',
        '"air.address_space"',
      ])
        assert(runner.includes(marker), marker);
      const build = await readForkSource("wasm/build.mjs");
      assert.doesNotMatch(
        build,
        /run-msl-fixtures|d2r_air|D2R_AIR/,
        "generic producer does not own or silently substitute D2R-authored fixture execution",
      );
    },
  );
}

// air-compiler-worker-client
{
  // 合成协议及取消竞态测试；所有编译及工作线程均为 JS 替身，不执行真实 WASM。
  // Synthetic protocol/cancellation races; compiler and workers are JS substitutes, never actual WASM.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { getEventListeners } = nodeEventsModule;
  const { setImmediate: nextTurn } = promisesModule2;
  const { createAirCompilerWorkerClient } = airCompilerWorkerClientModule;

  const sha = (bytes) => createHash("sha256").update(bytes).digest("hex");
  const factoryBytes = new Uint8Array([1, 2, 3]);
  const wasmBytes = new Uint8Array([4, 5, 6]);
  const assets = {
    factoryBytes,
    wasmBytes,
    factorySha256: sha(factoryBytes),
    wasmSha256: sha(wasmBytes),
    headerSha256: "1".repeat(64),
  };
  const source = "kernel void main0() {}";
  const options = { entry: "main0", stage: "kernel" };

  class ProtocolWorker extends EventTarget {
    requests = [];
    terminations = 0;
    postMessage(message, transfers = []) {
      const copied = structuredClone(message, { transfer: transfers });
      this.requests.push(copied);
      if (copied.type === "initialize")
        queueMicrotask(() => this.reply({ type: "initialized", id: copied.id, heapBytes: 4096 }));
    }
    reply(data) {
      this.dispatchEvent(new MessageEvent("message", { data }));
    }
    terminate() {
      this.terminations++;
    }
  }

  async function setup(t, onProgress) {
    // 显式替换编译操作：这些三字节样例不是 WASM 模块。
    // Explicitly replace compilation: these three-byte fixtures are not a WASM module.
    t.mock.method(WebAssembly, "compile", async () => ({ synthetic: true }));
    const workers = [];
    const client = await createAirCompilerWorkerClient({
      assets,
      onProgress,
      createWorker() {
        const worker = new ProtocolWorker();
        workers.push(worker);
        return worker;
      },
    });
    t.after(() => client.dispose());
    return { client, workers };
  }
  function compiled(worker, recycle = false) {
    return {
      type: "compiled",
      id: worker.requests.at(-1).id,
      bytes: new Uint8Array(88).fill(7).buffer,
      recycle,
      heapBytes: 4096,
      compilationCount: 1,
      inspection: { validation: "container-and-integrity-only", outputSize: 88 },
    };
  }

  for (const mode of ["throw", "reject", "thenable"])
    test(`AIR observers cannot disrupt initialization or output: ${mode}`, async (t) => {
      const phases = [];
      const onProgress = (event) => {
        assert.ok(Object.isFrozen(event));
        assert.deepEqual(Object.keys(event), ["phase"]);
        phases.push(event.phase);
        if (mode === "throw") throw new Error("Synthetic observer throw");
        if (mode === "reject") return Promise.reject(new Error("Synthetic observer rejection"));
        return {
          get then() {
            throw new Error("Synthetic thenable getter");
          },
        };
      };
      const { client, workers } = await setup(t, onProgress);
      const pending = client.compile(source, { ...options, onProgress });
      await nextTurn();
      const worker = workers[0],
        message = compiled(worker);
      for (const phase of ["compiling", "validating"])
        worker.reply({ type: "progress", id: message.id, phase });
      worker.reply(message);
      const output = await pending;
      await nextTurn();
      assert.deepEqual(phases, [
        "verifying",
        "preparing",
        "initializing",
        "compiling",
        "validating",
        "complete",
      ]);
      assert.ok(output.bytes.every((value) => value === 7));
      assert.equal(worker.terminations, 0);
      assert.equal(client.ready, true);
    });

  test("AIR cancellation terminates active work and permits a fresh worker", async (t) => {
    const { client, workers } = await setup(t);
    const controller = new AbortController();
    const pending = client.compile(source, { ...options, signal: controller.signal });
    await nextTurn();
    controller.abort();
    await assert.rejects(pending, { code: "AIR_ABORTED" });
    assert.equal(workers[0].terminations, 1);
    assert.equal(getEventListeners(controller.signal, "abort").length, 0);
    const recovery = client.compile(source, options);
    await nextTurn();
    assert.equal(workers.length, 2);
    workers[1].reply(compiled(workers[1]));
    assert.equal((await recovery).bytes[0], 7);
  });

  for (const mode of ["dispose", "abort"])
    test(`AIR ${mode} after a reply wipes undelivered output`, async (t) => {
      const { client, workers } = await setup(t);
      const controller = new AbortController();
      const pending = client.compile(source, { ...options, signal: controller.signal });
      await nextTurn();
      const message = compiled(workers[0]);
      workers[0].reply(message);
      if (mode === "dispose") client.dispose();
      else controller.abort();
      await assert.rejects(pending, {
        code: mode === "dispose" ? "AIR_COMPILER_DISPOSED" : "AIR_ABORTED",
      });
      assert.ok(new Uint8Array(message.bytes).every((value) => value === 0));
      assert.equal(getEventListeners(controller.signal, "abort").length, 0);
    });

  test("AIR disposal from the completion observer wipes output before delivery", async (t) => {
    const { client, workers } = await setup(t);
    const pending = client.compile(source, {
      ...options,
      onProgress({ phase }) {
        if (phase === "complete") client.dispose();
      },
    });
    await nextTurn();
    const message = compiled(workers[0]);
    workers[0].reply(message);
    await assert.rejects(pending, { code: "AIR_COMPILER_DISPOSED" });
    assert.ok(new Uint8Array(message.bytes).every((value) => value === 0));
  });

  test("AIR worker recycling does not erase transferred caller-owned output", async (t) => {
    const { client, workers } = await setup(t);
    const pending = client.compile(source, options);
    await nextTurn();
    const message = compiled(workers[0], true);
    workers[0].reply(message);
    const output = await pending;
    assert.equal(workers[0].terminations, 1);
    client.dispose();
    assert.ok(output.bytes.every((value) => value === 7));
  });

  test("AIR compilation timeout settles work and removes its abort listener", async (t) => {
    const { client, workers } = await setup(t);
    t.mock.timers.enable({ apis: ["setTimeout"] });
    const controller = new AbortController();
    const pending = client.compile(source, { ...options, signal: controller.signal });
    await nextTurn();
    t.mock.timers.tick(client.limits.compilationMs);
    await assert.rejects(pending, { code: "AIR_COMPILATION_TIMEOUT" });
    assert.equal(workers[0].terminations, 1);
    assert.equal(getEventListeners(controller.signal, "abort").length, 0);
  });

  test("AIR preparation abort preserves in-use public input until compilation settles", async (t) => {
    const entered = Promise.withResolvers(),
      prepared = Promise.withResolvers();
    let input,
      workers = 0;
    t.mock.method(WebAssembly, "compile", (bytes) => {
      input = bytes;
      entered.resolve();
      return prepared.promise;
    });
    const controller = new AbortController();
    const pending = createAirCompilerWorkerClient({
      assets,
      signal: controller.signal,
      createWorker() {
        workers++;
        return new ProtocolWorker();
      },
    });
    await entered.promise;
    controller.abort();
    await assert.rejects(pending, { code: "AIR_ABORTED" });
    assert.deepEqual(input, wasmBytes, "an unresolved compiler still owns its immutable input");
    assert.notEqual(input.buffer, wasmBytes.buffer);
    prepared.resolve({ synthetic: true });
    await nextTurn();
    assert.ok(input.every((value) => value === 0));
    assert.deepEqual(wasmBytes, new Uint8Array([4, 5, 6]), "caller-owned assets are unchanged");
    assert.equal(workers, 0, "abandoned initialization must not create a late worker");
    assert.equal(getEventListeners(controller.signal, "abort").length, 0);
  });

  test("worker failure preserves sanitized compiler prose through client rethrow", async (t) => {
    const { client, workers } = await setup(t);
    const pending = client.compile(source, options);
    await nextTurn();
    workers[0].reply({
      type: "error",
      id: workers[0].requests.at(-1).id,
      code: "AIR_FRONTEND_FAILED",
      shaderDiagnostic: {
        compilerPhase: "compiling",
        errorType: "RuntimeError",
        diagnostics: [
          "error: no matching function for call to 'sample'",
          "kernel void PRIVATE_SOURCE() {}",
        ],
        stack: "/Users/private/path",
        input: "PRIVATE_INPUT",
      },
    });
    await assert.rejects(pending, (error) => {
      assert.equal(error.code, "AIR_FRONTEND_FAILED");
      assert.deepEqual(error.shaderDiagnostic, {
        compilerPhase: "compiling",
        errorType: "RuntimeError",
        diagnostics: ["error: no matching function for call to 'sample'"],
      });
      assert.doesNotMatch(JSON.stringify(error), /PRIVATE|Users/);
      return true;
    });
    assert.equal(workers[0].terminations, 1);
  });

  test("native frontend wording survives worker, client, report and local display revalidation", async (t) => {
    const { sanitizeAirFrontendDiagnostics, shaderFailureDiagnostic } =
      await import("../shader-diagnostics.mjs");
    const { shaderFailureDetails } = await import("../packaging-diagnostics.mjs");
    const raw = [
      "address space qualified parameter must have pointer type",
      "parameter may not be qualified with an address space",
      "type 'float4' cannot be used as a template argument",
      "non-const reference cannot bind to a temporary value",
    ];
    const diagnostics = sanitizeAirFrontendDiagnostics(raw.join("\n"));
    const { client, workers } = await setup(t);
    const pending = client.compile(source, options);
    await nextTurn();
    workers[0].reply({
      type: "error",
      id: workers[0].requests.at(-1).id,
      code: "AIR_FRONTEND_FAILED",
      shaderDiagnostic: { compilerPhase: "compiling", diagnostics },
    });
    await assert.rejects(pending, (error) => {
      const report = {
        stage: "shaders",
        code: "STAGE_FAILED",
        detailCode: error.code,
        shaderDiagnostic: shaderFailureDiagnostic(error, { substep: "compilation", record: 1 }),
      };
      assert.deepEqual(error.shaderDiagnostic.diagnostics, diagnostics);
      assert.deepEqual(report.shaderDiagnostic.diagnostics, diagnostics);
      const displayed = shaderFailureDetails(report);
      for (const line of raw) assert.ok(displayed.includes(`frontend diagnostic: ${line}`));
      assert.ok(diagnostics.every((line) => !line.startsWith("error:")));
      return true;
    });
  });

  // 每个协议替身代表独立实例；测试只控制完成顺序，不衡量真实编译性能。
  // Each protocol substitute represents an isolated instance; these tests control completion order, not compiler performance.
  async function poolSetup(
    t,
    { capabilities = { hardwareConcurrency: 8, deviceMemory: 8 }, createWorker } = {},
  ) {
    const { createAirCompilerWorkerPool } = await import("../air-compiler-worker-client.mjs");
    const compilation = t.mock.method(WebAssembly, "compile", async () => ({ synthetic: true }));
    const workers = [];
    const client = await createAirCompilerWorkerPool({
      assets,
      capabilities,
      createWorker() {
        if (createWorker) return createWorker(workers);
        const worker = new ProtocolWorker();
        workers.push(worker);
        return worker;
      },
    });
    t.after(() => client.dispose());
    return { client, workers, compilation };
  }

  test("AIR pool shares one verified code compilation across four isolated bounded lanes", async (t) => {
    const { client, workers, compilation } = await poolSetup(t);
    assert.equal(client.concurrency, 4);
    assert.equal(workers.length, 4);
    assert.equal(compilation.mock.callCount(), 1);
    const pending = workers.map((_, index) => client.compile(`${source}\n// ${index}`, options));
    await nextTurn();
    assert.ok(workers.every((worker) => worker.requests.length === 2));
    await assert.rejects(client.compile(source, options), { code: "AIR_COMPILER_BUSY" });
    assert.ok(
      workers.every((worker) => worker.requests.length === 2),
      "overflow is not queued",
    );
    for (const index of [3, 1, 2, 0]) {
      const response = compiled(workers[index]);
      new Uint8Array(response.bytes).fill(index + 1);
      workers[index].reply(response);
    }
    const results = await Promise.all(pending);
    assert.deepEqual(
      results.map((result) => result.bytes[0]),
      [1, 2, 3, 4],
    );
    assert.equal(compilation.mock.callCount(), 1);
  });

  test("AIR pool caps device hints without requiring shared memory or cross-origin isolation", async () => {
    const { selectAirCompilerConcurrency: select } =
      await import("../air-compiler-worker-client.mjs");
    assert.equal(select({ hardwareConcurrency: 64, deviceMemory: 8 }), 4);
    assert.equal(select({ hardwareConcurrency: 4, deviceMemory: 8 }), 4);
    assert.equal(select({ hardwareConcurrency: 2, deviceMemory: 8 }), 2);
    assert.equal(select({ hardwareConcurrency: 64, deviceMemory: 4 }), 2);
    assert.equal(select({ hardwareConcurrency: 8, deviceMemory: 2 }), 1);
    assert.equal(select({}), 2);
    assert.equal(select({ hardwareConcurrency: 1 }), 1);
    assert.equal(select({ hardwareConcurrency: Infinity, deviceMemory: NaN }), 2);
  });

  for (const mode of ["abort", "failure", "dispose"])
    test(`AIR pool ${mode} stops all lanes and wipes undelivered replies`, async (t) => {
      const { client, workers } = await poolSetup(t);
      const controller = new AbortController();
      const pending = workers.map(() =>
        client.compile(source, { ...options, signal: controller.signal }),
      );
      const settlement = Promise.allSettled(pending);
      await nextTurn();
      const response = compiled(workers[3]);
      workers[3].reply(response);
      if (mode === "abort") controller.abort();
      else if (mode === "dispose") client.dispose();
      else
        workers[0].reply({
          type: "error",
          id: workers[0].requests.at(-1).id,
          code: "AIR_FRONTEND_FAILED",
        });
      const results = await settlement;
      assert.ok(results.every((result) => result.status === "rejected"));
      assert.ok(workers.every((worker) => worker.terminations === 1));
      assert.ok(new Uint8Array(response.bytes).every((value) => value === 0));
      assert.equal(client.ready, false);
      assert.equal(getEventListeners(controller.signal, "abort").length, 0);
      await assert.rejects(client.compile(source, options), { code: "AIR_COMPILER_DISPOSED" });
    });

  test("AIR pool recycles only an exhausted lane using already verified code", async (t) => {
    const { client, workers, compilation } = await poolSetup(t);
    const first = client.compile(source, options);
    await nextTurn();
    workers[0].reply(compiled(workers[0], true));
    await first;
    const next = client.compile(source, options);
    await nextTurn();
    assert.equal(workers.length, 5);
    assert.equal(workers[0].terminations, 1);
    assert.ok(workers.slice(1).every((worker) => worker.terminations === 0));
    workers[4].reply(compiled(workers[4]));
    await next;
    assert.equal(compilation.mock.callCount(), 1);
  });

  test("AIR pool keeps initialized lanes if another worker cannot be created", async (t) => {
    const { client, workers } = await poolSetup(t, {
      createWorker(workers) {
        if (workers.length === 2) throw new Error("Synthetic worker resource limit");
        const worker = new ProtocolWorker();
        workers.push(worker);
        return worker;
      },
    });
    assert.equal(client.concurrency, 2);
    assert.equal(client.ready, true);
    assert.ok(workers.every((worker) => worker.terminations === 0));
  });

  test("AIR pool never downgrades corrupted worker initialization into success", async (t) => {
    const workers = [];
    t.mock.method(WebAssembly, "compile", async () => ({ synthetic: true }));
    const { createAirCompilerWorkerPool } = await import("../air-compiler-worker-client.mjs");
    await assert.rejects(
      createAirCompilerWorkerPool({
        assets,
        capabilities: { hardwareConcurrency: 8, deviceMemory: 8 },
        createWorker() {
          const worker = new ProtocolWorker();
          if (workers.length === 1)
            worker.postMessage = function (message) {
              queueMicrotask(() =>
                this.reply({ type: "error", id: message.id, code: "AIR_ASSET_MISMATCH" }),
              );
            };
          workers.push(worker);
          return worker;
        },
      }),
      { code: "AIR_ASSET_MISMATCH" },
    );
    assert.equal(workers.length, 2);
    assert.ok(workers.every((worker) => worker.terminations === 1));
  });

  test("AIR pool stops adding workers when initialized heaps exhaust its startup budget", async (t) => {
    const { client, workers } = await poolSetup(t, {
      createWorker(workers) {
        const worker = new ProtocolWorker();
        worker.postMessage = function (message, transfers = []) {
          const data = structuredClone(message, { transfer: transfers });
          this.requests.push(data);
          queueMicrotask(() =>
            this.reply({ type: "initialized", id: data.id, heapBytes: 256 * 1024 * 1024 }),
          );
        };
        workers.push(worker);
        return worker;
      },
    });
    assert.equal(client.concurrency, 2);
    assert.equal(workers.length, 2);
  });

  test("AIR pool timeout terminates every active worker", async (t) => {
    const { client, workers } = await poolSetup(t);
    t.mock.timers.enable({ apis: ["setTimeout"] });
    const pending = workers.map(() => client.compile(source, options));
    const settlement = Promise.allSettled(pending);
    await nextTurn();
    t.mock.timers.tick(client.limits.compilationMs);
    const results = await settlement;
    assert.ok(results.every((result) => result.status === "rejected"));
    assert.ok(workers.every((worker) => worker.terminations === 1));
    assert.equal(client.ready, false);
  });

  for (const mode of ["error", "protocol"])
    test(`AIR pool rejects an accepted lane's ${mode} while another lane initializes`, async (t) => {
      const { createAirCompilerWorkerPool } = await import("../air-compiler-worker-client.mjs");
      t.mock.method(WebAssembly, "compile", async () => ({ synthetic: true }));
      const workers = [];
      await assert.rejects(
        createAirCompilerWorkerPool({
          assets,
          capabilities: { hardwareConcurrency: 8, deviceMemory: 8 },
          createWorker() {
            const worker = new ProtocolWorker();
            if (workers.length === 1)
              worker.postMessage = function (message, transfers = []) {
                this.requests.push(structuredClone(message, { transfer: transfers }));
                queueMicrotask(() => {
                  if (mode === "error") workers[0].dispatchEvent(new Event("error"));
                  else workers[0].reply({ type: "initialized", id: 999, heapBytes: 4096 });
                });
              };
            workers.push(worker);
            return worker;
          },
        }),
        { code: mode === "error" ? "AIR_WORKER_FAILED" : "AIR_PROTOCOL_FAILED" },
      );
      assert.equal(workers.length, 2);
      assert.ok(workers.every((worker) => worker.terminations === 1));
    });

  test("AIR pool rejects a lane stopped after its initialized reply but before startup handoff", async (t) => {
    const { createAirCompilerWorkerPool } = await import("../air-compiler-worker-client.mjs");
    t.mock.method(WebAssembly, "compile", async () => ({ synthetic: true }));
    const worker = new ProtocolWorker();
    worker.postMessage = function (message) {
      queueMicrotask(() => {
        this.reply({ type: "initialized", id: message.id, heapBytes: 4096 });
        this.reply({ type: "initialized", id: message.id, heapBytes: 4096 });
      });
    };
    await assert.rejects(
      createAirCompilerWorkerPool({
        assets,
        capabilities: { hardwareConcurrency: 1 },
        createWorker: () => worker,
      }),
      { code: "AIR_PROTOCOL_FAILED" },
    );
    assert.equal(worker.terminations, 1);
  });

  test("AIR pool cancellation between last initialization and pool handoff rejects and terminates", async (t) => {
    const { createAirCompilerWorkerPool } = await import("../air-compiler-worker-client.mjs");
    t.mock.method(WebAssembly, "compile", async () => ({ synthetic: true }));
    const controller = new AbortController();
    const worker = new ProtocolWorker();
    worker.postMessage = function (message) {
      queueMicrotask(() => {
        this.reply({ type: "initialized", id: message.id, heapBytes: 4096 });
        queueMicrotask(() => queueMicrotask(() => controller.abort()));
      });
    };
    await assert.rejects(
      createAirCompilerWorkerPool({
        assets,
        signal: controller.signal,
        capabilities: { hardwareConcurrency: 1 },
        createWorker: () => worker,
      }),
      { code: "AIR_ABORTED" },
    );
    assert.equal(worker.terminations, 1);
  });
}

// air-fragment-input-liveness
{
  // 仅检查源码/校验器，不执行编译器，也不证明 AIR 可被接受。
  // Source/checker tests only. These do not execute a compiler or prove AIR acceptance.
  const { default: assert } = strictModule;
  const { readFileSync } = nodeFsModule;
  const { default: test } = nodeTestModule;
  const { sourceReviewOptions, readForkSource } = fixtureModule;
  const { checkFragmentInputLiveness } = checkFragmentInputLivenessModule;

  function syntheticIR(keepVarying, keepPosition = keepVarying) {
    const unusedVarying = keepVarying ? "" : ', !"air.arg_unused"';
    const unusedPosition = keepPosition ? "" : ', !"air.arg_unused"';
    let value = "%sum";
    const body = [];
    if (keepVarying) {
      body.push(`  %sum1 = fadd <4 x float> ${value}, %in.1`);
      value = "%sum1";
    }
    if (keepPosition) {
      body.push(`  %sum2 = fadd <4 x float> ${value}, %in.2`);
      value = "%sum2";
    }
    body.push(`  ret <4 x float> ${value}`);
    return `define <4 x float> @fragment_input_liveness(<4 x float> %in.0, <4 x float> %in.1, <4 x float> %in.2, <4 x float> addrspace(2)* %bias, <4 x float> addrspace(2)* %untouched) {
entry:
  %loaded = load <4 x float>, <4 x float> addrspace(2)* %bias
  %sum = fadd <4 x float> %in.0, %loaded
${body.join("\n")}
}
!air.fragment = !{!0}
!0 = !{<4 x float> (<4 x float>, <4 x float>, <4 x float>, <4 x float> addrspace(2)*, <4 x float> addrspace(2)*)* @fragment_input_liveness, !1, !2}
!1 = !{!3}
!2 = !{!4, !5, !6, !7, !8}
!3 = !{i32 0, !"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!4 = !{i32 0, !"air.fragment_input", !"user(locn7)", !"air.flat", !"air.arg_type_name", !"float4", !"air.arg_name", !"kept"}
!5 = !{i32 1, !"air.fragment_input", !"user(locn11)", !"air.flat", !"air.arg_type_name", !"float4", !"air.arg_name", !"optionalValue"${unusedVarying}}
!6 = !{i32 2, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"${unusedPosition}}
!7 = !{i32 3, !"air.buffer", !"air.location_index", i32 9, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"float4", !"air.arg_name", !"bias"}
!8 = !{i32 4, !"air.buffer", !"air.location_index", i32 13, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"float4", !"air.arg_name", !"untouched"}
`;
  }

  test("checker follows entry metadata and distinguishes live/dead input descriptors", () => {
    for (const keepVarying of [false, true]) {
      for (const keepPosition of [false, true]) {
        const result = checkFragmentInputLiveness(syntheticIR(keepVarying, keepPosition), {
          keepVarying,
          keepPosition,
        });
        assert.deepEqual(result.argumentUsed, [true, keepVarying, keepPosition, true, false]);
        if (keepVarying !== keepPosition)
          assert.throws(() =>
            checkFragmentInputLiveness(syntheticIR(keepVarying, keepPosition), {
              keepVarying: keepPosition,
              keepPosition: keepVarying,
            }),
          );
      }
    }
  });

  test("checker rejects missing, duplicate, misplaced and inappropriate unused markers", () => {
    const ir = syntheticIR(false);
    for (const invalid of [
      ir.replaceAll(', !"air.arg_unused"', ""),
      ir.replace(
        '!"optionalValue", !"air.arg_unused"',
        '!"optionalValue", !"air.arg_unused", !"air.arg_unused"',
      ),
      ir.replace(
        '!"air.arg_name", !"optionalValue", !"air.arg_unused"',
        '!"air.arg_unused", !"air.arg_name", !"optionalValue"',
      ),
      ir.replace('!"kept"}', '!"kept", !"air.arg_unused"}'),
      ir.replace('!"untouched"}', '!"untouched", !"air.arg_unused"}'),
      ir.replace('!"color"}', '!"color", !"air.arg_unused"}'),
    ])
      assert.throws(() => checkFragmentInputLiveness(invalid));
    assert.throws(() => checkFragmentInputLiveness(ir, { keepAll: true }));
  });

  test("checker rejects changed indices, semantics, interpolation, types and bindings", () => {
    const ir = syntheticIR(false);
    for (const [before, after] of [
      ["!5 = !{i32 1,", "!5 = !{i32 4,"],
      ['!"user(locn11)"', '!"user(locn12)"'],
      ['!"air.flat"', '!"air.perspective"'],
      ['!"air.no_perspective"', '!"air.perspective"'],
      ['!"air.position"', '!"air.point_coord"'],
      [
        '!"air.arg_type_name", !"float4", !"air.arg_name", !"position"',
        '!"air.arg_type_name", !"half4", !"air.arg_name", !"position"',
      ],
      ['!"air.location_index", i32 13', '!"air.location_index", i32 12'],
      ['!"air.address_space", i32 2', '!"air.address_space", i32 1'],
      ["!2 = !{!4, !5, !6, !7, !8}", "!2 = !{!4, !6, !5, !7, !8}"],
    ])
      assert.throws(() => checkFragmentInputLiveness(ir.replace(before, after)));
  });

  test("checker rejects signature changes and markers contradicting final parameter uses", () => {
    const ir = syntheticIR(false);
    for (const invalid of [
      ir.replace("<4 x float> %in.1, ", ""),
      ir.replace("<4 x float> %in.2", "<2 x float> %in.2"),
      ir.replace(
        "%sum = fadd <4 x float> %in.0, %loaded",
        "%sum = fadd <4 x float> %in.1, %loaded",
      ),
      ir.replace(
        "ret <4 x float> %sum",
        "%unexpected = fadd <4 x float> %in.2, %sum\n  ret <4 x float> %unexpected",
      ),
      ir.replace("addrspace(2)* %bias\n", "addrspace(2)* %untouched\n"),
    ])
      assert.throws(() => checkFragmentInputLiveness(invalid));
  });

  test("checker will not accept orphan matching descriptors or another entry/stage", () => {
    const ir = syntheticIR(false);
    for (const invalid of [
      ir.replace("!air.fragment = !{!0}", "!air.vertex = !{!0}"),
      ir.replace("!air.fragment = !{!0}", "!air.fragment = !{!0, !0}"),
      ir.replace("!0 = !{", "!99 = !{"),
      ir.replace("@fragment_input_liveness, !1, !2", "@another_entry, !1, !2"),
      ir.replace("!2 = !{!4, !5, !6, !7, !8}", "!2 = !{!4, !99, !6, !7, !8}"),
      ir + "!4 = !{i32 0}\n",
    ])
      assert.throws(() => checkFragmentInputLiveness(invalid));
  });

  test(
    "authenticated fork source runs narrow finalization after optimization and before verifier/output",
    sourceReviewOptions,
    async () => {
      const source = await readForkSource("wasm/api/air_compiler.cpp");
      const helper = source.slice(
        source.indexOf("bool finalizeFragmentInputLiveness("),
        source.indexOf("bool hasEntry("),
      );
      assert.match(helper, /getNamedMetadata\("air\.fragment"\)/);
      assert.match(
        helper,
        /semantic->getString\(\) != "air\.fragment_input" &&\s*semantic->getString\(\) != "air\.position"/,
      );
      assert.match(helper, /index->getValue\(\)\.uge\(function->arg_size\(\)\)/);
      assert.match(
        helper,
        /function->getArg\(unsigned\(index->getZExtValue\(\)\)\)->use_empty\(\)/,
      );
      assert.match(helper, /if \(alreadyUnused\) continue;/);
      assert.match(
        helper,
        /for \(const auto &field : input->operands\(\)\) fields.push_back\(field.get\(\)\);\s*fields.push_back\(llvm::MDString::get\(context, "air.arg_unused"\)\);/,
      );
      assert.match(helper, /entryFields\[2\] = llvm::MDNode::get\(context, newInputs\);/);
      assert.doesNotMatch(
        helper,
        /replaceAllUsesWith|replaceOperandWith|mutateType|mutateFunctionType|removeArg|setAttributes|air\.vertex|air\.kernel|air\.function_constants/,
      );
      const positions = [
        "compiler.ExecuteAction(action)",
        "auto module = action.takeModule();",
        "if (!finalizeFragmentInputLiveness(*module))",
        "if (llvm::verifyModule(*module, &errors))",
        "if (!validControlStages(*module))",
        "Output output(result->data);",
        "if (output_kind == AIR_LLVM_TEXT)",
        "writerSucceeded = llvm::WriteMetalLibToFile(*module, output);",
      ].map((needle) => source.indexOf(needle));
      assert(positions.every((p, i) => p >= 0 && (i === 0 || positions[i - 1] < p)));
    },
  );

  test("authored fixture keeps identical live/dead interfaces and out-of-scope resource", () => {
    const source = readFileSync(
      new URL(
        "../toolchain/metal-air-compiler/fixtures/msl-fragment-input-liveness.metal",
        import.meta.url,
      ),
      "utf8",
    );
    assert.match(source, /optionalValue \[\[user\(locn11\), flat\]\]/);
    assert.match(source, /position \[\[position\]\]/);
    assert.match(source, /\*untouched \[\[buffer\(13\)\]\]/);
    assert.match(
      source,
      /#if LIVENESS_KEEP_VARYING && LIVENESS_KEEP_POSITION\s+color \+= input.optionalValue \+ input.position;/,
    );
    assert.match(source, /float4 discarded = input.optionalValue \+ input.position;/);
    assert.match(source, /#elif LIVENESS_KEEP_VARYING\s+color \+= input.optionalValue;/);
    assert.match(source, /#elif LIVENESS_KEEP_POSITION\s+color \+= input.position;/);
  });

  // 探针接线仍由 air-header-operations.test.mjs 覆盖。
  // Canary wiring remains covered by air-header-operations.test.mjs.
}

// air-fragment-input-reflection
{
  // 完全使用合成数据，测试校验器而非编译器或 Metal。
  // Wholly synthetic data. Tests the checker, not a compiler or Metal.
  const { default: assert } = strictModule;
  const { default: test } = nodeTestModule;
  const { checkFragmentInputReflection, inspectFragmentReflection } =
    checkFragmentInputReflectionModule;

  // 依据字段布局独立编写的最小前向模式样例。容器刻意省略有效 bitcode/哈希；使用此补充语义检查前，必须另行执行生产编译着色器边界校验。
  // Minimal forward-written schema fixture, independently authored from field layout.
  // Container omits bitcode/hash validity intentionally: always run the production
  // compiled-shader boundary separately before using this supplemental semantic check.
  function syntheticLibrary({
    keepVarying = false,
    keepPosition = false,
    unusedOverride = {},
    fragmentIds = [0],
    argumentIds = [1, 2, 3, 4, 5],
    mutate = () => {},
  } = {}) {
    const b = [];
    const write = (n, size = 4) => {
      const p = b.length;
      for (let i = 0; i < size; ++i) b.push(Math.floor(n / 2 ** (8 * i)) & 255);
      return p;
    };
    const patch = (p, n, size = 4) => {
      for (let i = 0; i < size; ++i) b[p + i] = Math.floor(n / 2 ** (8 * i)) & 255;
    };
    const pad = () => {
      while (b.length % 4) write(0, 1);
    };
    const table = (fields) => {
      const vs = 4 + fields.length * 2;
      if (vs % 4) write(0, 2);
      const vt = b.length;
      const size =
        4 + fields.reduce((s, x) => s + (x === null ? 0 : Array.isArray(x) ? x.length * 4 : 4), 0);
      write(vs, 2);
      write(size, 2);
      let offset = 4;
      for (const x of fields) {
        write(x === null ? 0 : offset, 2);
        if (x !== null) offset += Array.isArray(x) ? x.length * 4 : 4;
      }
      const root = b.length;
      write(root - vt);
      const slots = fields.map((x) => {
        if (x === null) return null;
        const p = b.length;
        for (const v of Array.isArray(x) ? x : [x]) write(v);
        return p;
      });
      return { root, vt, slots };
    };
    const vector = (pointer, values) => {
      pad();
      const p = b.length;
      patch(pointer, p - pointer);
      write(values.length);
      return values.map((value) => write(value));
    };
    const string = (pointer, text) => {
      pad();
      const p = b.length;
      patch(pointer, p - pointer);
      const textBytes = new TextEncoder().encode(text);
      write(textBytes.length);
      b.push(...textBytes, 0);
      pad();
    };
    const fieldUnused = (name, expected) =>
      Object.hasOwn(unusedOverride, name) ? unusedOverride[name] : expected ? 1 : null;
    write(0);
    b.push(...new TextEncoder().encode("AIRR"));
    const root = table([[0, 4, 0], 0, 0]);
    patch(0, root.root);
    const nodeSlots = vector(root.slots[1], [0, 0, 0, 0, 0, 0]);
    vector(root.slots[2], fragmentIds);
    const structures = [];
    function node(id, kind, fields) {
      const w = table([kind, 0]);
      patch(nodeSlots[id], w.root - nodeSlots[id]);
      const t = table(fields);
      patch(w.slots[1], t.root - w.slots[1]);
      structures.push({ wrapper: w, payload: t });
      return t;
    }
    const fn = node(0, 1, [0, null, 0]);
    string(fn.slots[0], "fragment_input_liveness");
    vector(fn.slots[2], argumentIds);
    for (const [id, name, attr, unused] of [
      [1, "kept", "user(locn7)", fieldUnused("kept", false)],
      [2, "optionalValue", "user(locn11)", fieldUnused("optionalValue", !keepVarying)],
    ]) {
      const t = node(id, 274432, [null, 0, null, null, 2, 0, 0, unused]);
      string(t.slots[1], attr);
      string(t.slots[5], "float4");
      string(t.slots[6], name);
    }
    const pos = node(3, 274434, [null, 1, 3, 0, 0, fieldUnused("position", !keepPosition)]);
    string(pos.slots[3], "float4");
    string(pos.slots[4], "position");
    for (const [id, name, binding] of [
      [4, "bias", 9],
      [5, "untouched", 13],
    ]) {
      const t = node(id, 262144, [
        null,
        16,
        binding,
        1,
        1,
        2,
        null,
        null,
        16,
        16,
        0,
        0,
        fieldUnused(name, false),
      ]);
      string(t.slots[10], "float4");
      string(t.slots[11], name);
    }
    mutate({ b, patch, root, structures, nodeSlots });
    const airr = Uint8Array.from(b);
    b.length = 0;
    for (let i = 0; i < 88; ++i) write(0, 1);
    const tag = (name, payload) => {
      b.push(...new TextEncoder().encode(name));
      if (name !== "ENDT") {
        write(payload.length, 2);
        b.push(...payload);
      }
    };
    const u64 = (n) => {
      const a = [];
      for (let i = 0; i < 8; ++i) a.push(Math.floor(n / 2 ** (8 * i)) & 255);
      return a;
    };
    write(1);
    const record = write(0);
    tag("NAME", [...new TextEncoder().encode("fragment_input_liveness"), 0]);
    tag("TYPE", [1]);
    tag("RFLT", u64(4));
    tag("ENDT");
    patch(record, b.length - record);
    const functionSize = b.length - 88 - 4;
    const rlstTag = b.length;
    tag("RLST", [...u64(0), ...u64(0)]);
    tag("ENDT");
    const pub = b.length;
    tag("ENDT");
    const priv = b.length;
    tag("ENDT");
    const bitcode = b.length;
    write(0);
    const rl = b.length;
    write(1);
    const group = write(0);
    b.push(...new TextEncoder().encode("RBUF"));
    const payloadSize = write(0);
    const payload = b.length;
    while (b.length % 16) write(0, 1);
    const airrStart = b.length;
    b.push(...airr);
    tag("ENDT");
    patch(group, b.length - group);
    patch(payloadSize, b.length - 4 - payload);
    patch(rlstTag + 6, rl, 8);
    patch(rlstTag + 14, b.length - rl, 8);
    for (const [p, n] of [
      [16, b.length],
      [24, 88],
      [32, functionSize],
      [40, pub],
      [48, 4],
      [56, priv],
      [64, 4],
      [72, bitcode],
      [80, 4],
    ])
      patch(p, n, 8);
    b.splice(0, 4, ...new TextEncoder().encode("MTLB"));
    return { bytes: Uint8Array.from(b), airrStart, structures, root };
  }

  for (const keepVarying of [false, true])
    for (const keepPosition of [false, true])
      test(`synthetic optional inputs varying=${keepVarying}, position=${keepPosition}`, () => {
        const { bytes } = syntheticLibrary({ keepVarying, keepPosition });
        const result = checkFragmentInputReflection(bytes, { keepVarying, keepPosition });
        assert.equal(result.arguments.length, 5);
        assert.equal(result.unusedInputs, Number(!keepVarying) + Number(!keepPosition));
      });

  test("rejects an ignore-only implementation that serializes no unused flags", () => {
    const { bytes } = syntheticLibrary({ keepVarying: true, keepPosition: true });
    assert.throws(() => checkFragmentInputReflection(bytes), /unused presence\/value/);
  });
  for (const name of ["optionalValue", "position"]) {
    test(`explicit false ${name} decodes but fails dead assertion`, () => {
      const { bytes } = syntheticLibrary({ unusedOverride: { [name]: 0 } });
      const args = inspectFragmentReflection(bytes).entries[0].fragments[0].arguments;
      assert.deepEqual(args.find((x) => x.name === name).unused.value, false);
      assert.throws(() => checkFragmentInputReflection(bytes), /unused presence\/value/);
    });
    test(`explicit false ${name} fails strict live-absence assertion`, () => {
      const opts = { keepVarying: true, keepPosition: true };
      const { bytes } = syntheticLibrary({ ...opts, unusedOverride: { [name]: 0 } });
      assert.throws(() => checkFragmentInputReflection(bytes, opts), /unused presence\/value/);
    });
  }
  for (const name of ["kept", "bias", "untouched"])
    test(`out-of-scope unused on ${name} rejected`, () => {
      const { bytes } = syntheticLibrary({ unusedOverride: { [name]: 1 } });
      assert.throws(() => checkFragmentInputReflection(bytes), /unused presence\/value/);
    });
  test("noncanonical bool byte rejected", () => {
    const { bytes } = syntheticLibrary({ unusedOverride: { optionalValue: 2 } });
    assert.throws(() => checkFragmentInputReflection(bytes), /noncanonical boolean/);
  });
  test("all truncation points reject without nontermination", () => {
    const { bytes } = syntheticLibrary();
    for (let n = 0; n < bytes.length; ++n)
      assert.throws(() => checkFragmentInputReflection(bytes.subarray(0, n)));
  });
  for (const [name, mutate] of [
    [
      "oversized node vector",
      ({ patch, root, b }) => {
        const p = root.slots[1],
          relative = b[p] + b[p + 1] * 256 + b[p + 2] * 65536 + b[p + 3] * 16777216;
        patch(p + relative, 0xffffffff);
      },
    ],
    ["out-of-bounds wrapper", ({ patch, nodeSlots }) => patch(nodeSlots[0], 0x7fffffff)],
    [
      "out-of-bounds vtable",
      ({ patch, structures }) => patch(structures[2].payload.root, 0x7fffffff),
    ],
    [
      "oversized inline data",
      ({ patch, structures }) => patch(structures[2].payload.vt + 2, 65535, 2),
    ],
    [
      "unknown argument node ID",
      ({ patch, structures, b }) => {
        const p = structures[0].payload.slots[2],
          relative = b[p] + b[p + 1] * 256;
        patch(p + relative + 4, 99999);
      },
    ],
    [
      "wrong argument kind",
      ({ patch, structures }) => patch(structures[2].wrapper.slots[0], 274434),
    ],
    ["wrong buffer binding", ({ patch, structures }) => patch(structures[4].payload.slots[2], 8)],
    [
      "string escapes AIRR",
      ({ patch, structures }) => patch(structures[2].payload.slots[6], 0xffffffff),
    ],
    [
      "bool slot exceeds table",
      ({ patch, structures }) => patch(structures[2].payload.vt + 18, 65535, 2),
    ],
  ])
    test(name, () => {
      const { bytes } = syntheticLibrary({ mutate });
      assert.throws(() => checkFragmentInputReflection(bytes));
    });
  test("Uint8Array byteOffset honored", () => {
    const { bytes } = syntheticLibrary();
    const larger = new Uint8Array(bytes.length + 17);
    larger.set(bytes, 13);
    checkFragmentInputReflection(larger.subarray(13, 13 + bytes.length));
  });

  for (const count of [2, 4096])
    test(`fragment alias amplification rejected before mapping: ${count}`, () => {
      const { bytes } = syntheticLibrary({ fragmentIds: Array(count).fill(0) });
      assert.throws(() => checkFragmentInputReflection(bytes), /vector count limit/);
    });
  for (const count of [6, 4096])
    test(`argument alias amplification rejected before mapping: ${count}`, () => {
      const { bytes } = syntheticLibrary({ argumentIds: Array(count).fill(1) });
      assert.throws(() => checkFragmentInputReflection(bytes), /vector count limit/);
    });
  test("five aliased argument IDs rejected", () => {
    const { bytes } = syntheticLibrary({ argumentIds: [1, 1, 1, 1, 1] });
    assert.throws(() => checkFragmentInputReflection(bytes), /duplicate argument node IDs/);
  });
  test("multiple fixture entries rejected before record traversal", () => {
    const { bytes } = syntheticLibrary();
    new DataView(bytes.buffer).setUint32(88, 64, true);
    assert.throws(() => checkFragmentInputReflection(bytes), /one fixture entry required/);
  });
}

// air-header-operations
{
  // 验证 air-header-operations 的约定与边界；依据 ./fixtures/air/fixture.mjs、./check-buffer-reflection.mjs 及本文件的测试断言。
  // Verify air-header-operations contracts and boundaries against ./fixtures/air/fixture.mjs、./check-buffer-reflection.mjs and this file's assertions.
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { createHash } = nodeCryptoModule;
  const { default: test } = nodeTestModule;
  const { sourceReviewOptions, readForkSource, readReviewedSourceLock } = fixtureModule;

  const compiler = new URL("../toolchain/metal-air-compiler/", import.meta.url);
  const nativeFixtures = new Set([
    "control_smoke.cpp",
    "header_smoke.cpp",
    "operations_smoke.h",
    "smoke.cpp",
  ]);
  const read = (path) => {
    if (path.startsWith("../")) return readFile(new URL(path.slice(3), compiler), "utf8");
    if (nativeFixtures.has(path))
      return readFile(new URL(`fixtures/native-api/${path}`, compiler), "utf8");
    return readForkSource(`wasm/api/${path}`);
  };
  const digest = (value) => createHash("sha256").update(value).digest("hex");

  // 仅检查源码约定；有编译器构建时，由自编头文件冒烟用例检查真实前端/IR 行为。
  // These are source-contract checks only. Actual frontend/IR behavior is checked
  // by the authored header smoke cases when a compiler build is available.
  test(
    "scalar sin, cos and ceil use exact AIR labels and explicit fast exports",
    sourceReviewOptions,
    async () => {
      const math = await read("include/metal_math");
      const apiSource = await read("air_compiler.cpp");
      const profileStart = math.indexOf("#if defined(AIR_FAST_MATH)");
      const profileEnd = math.indexOf("#endif", profileStart);
      assert(profileStart >= 0 && profileEnd > profileStart);
      for (const operation of ["sin", "cos", "ceil"]) {
        assert(
          math.includes(
            `float ${operation}(float) __asm__("air.fast_${operation}.f32") __attribute__((const));`,
          ),
        );
        assert(math.includes(`template<class T> T ${operation}(T) = delete;`));
        assert(
          math.includes(
            `AIR_INLINE float ${operation}(float x) { return __air_detail::${operation}(x); }`,
          ),
        );
        const exports = [...math.matchAll(new RegExp(`using fast::${operation};`, "g"))];
        assert.equal(exports.length, 1);
        assert(exports[0].index > profileStart && exports[0].index < profileEnd);
        assert(apiSource.includes(`"air.fast_${operation}.f32"`));
      }
      assert.doesNotMatch(math, /__builtin_(?:sin|cos|ceil)|\bfmod\s*\(/);
    },
  );

  test(
    "bit reversal uses uint AIR ABI and bit-preserving signed wrappers",
    sourceReviewOptions,
    async () => {
      const integer = await read("include/metal_integer");
      const apiSource = await read("air_compiler.cpp");
      assert(integer.includes('uint reverse(uint) __asm__("air.reverse_bits.i32");'));
      assert(integer.includes("template<class T> T reverse_bits(T) = delete;"));
      assert(
        integer.includes("uint reverse_bits(uint x) { return __integer_detail::reverse(x); }"),
      );
      assert(
        integer.includes(
          "int reverse_bits(int x) { return as_type<int>(reverse_bits(as_type<uint>(x))); }",
        ),
      );
      assert.doesNotMatch(integer, /__builtin_bitreverse/);
      assert(apiSource.includes('name=="air.reverse_bits.i32"'));
      const fixtures = await read("operations_smoke.h");
      assert(fixtures.includes('{"short", "ushort", "long", "ulong", "int2", "uint2"}'));
      assert(fixtures.includes("unsupported bit reversal width"));
    },
  );

  test(
    "SIMD all is convergent, boolean, and limited to compute by the API",
    sourceReviewOptions,
    async () => {
      const simd = await read("include/metal_simd");
      const apiSource = await read("air_compiler.cpp");
      assert(simd.includes('bool all(bool) __asm__("air.simd_all") __attribute__((convergent));'));
      assert.match(
        simd,
        /bool simd_all\(bool predicate\)\s*\{\s*return __simd_detail::all\(predicate\);/,
      );
      assert.match(
        apiSource,
        /name=="air\.simd_all"\) && llvm::StringRef\(stage\)!="air\.kernel"\) return false;/,
      );
      assert.match(
        apiSource,
        /return name=="air\.simd_all" && function->getReturnType\(\)->isSpecificBuiltinType\(clang::BuiltinType::Bool\);/,
      );
      assert.match(
        apiSource,
        /hasAttr<clang::ConstAttr>\(\) \|\| function->hasAttr<clang::PureAttr>\(\)/,
      );
      assert.match(apiSource, /invocation->getLangOpts\(\)->ConvergentFunctions = true;/);
    },
  );

  test(
    "assembly trust remains tied to physical header provenance and exact declarations",
    sourceReviewOptions,
    async () => {
      const apiSource = await read("air_compiler.cpp");
      assert.match(apiSource, /function->isVariadic\(\) \|\| function->hasBody\(\)/);
      assert.match(apiSource, /loc\.isInvalid\(\) \|\| loc\.isMacroID\(\)/);
      for (const location of [
        "sm.getSpellingLoc(decl->getLocation())",
        "sm.getExpansionLoc(decl->getLocation())",
        "attr->getLocation()",
      ])
        assert(apiSource.includes(`!trusted(${location})`));
      assert.match(apiSource, /getFileEntryRefForID\(sm\.getFileID\(loc\)\)/);
      assert.match(apiSource, /function->getNumParams\(\) != count/);
      assert.match(apiSource, /for \(const auto \*parameter : function->parameters\(\)\)/);
    },
  );

  test(
    "the guarded standard umbrella exposes atomic and barrier declarations",
    sourceReviewOptions,
    async () => {
      const stdlib = await read("include/metal_stdlib");
      const atomic = await read("include/metal_atomic");
      const apiSource = await read("air_compiler.cpp");
      assert.match(stdlib, /#include <metal_atomic>\s*#endif\s*$/);
      assert(
        stdlib.indexOf("#include <metal_atomic>") > stdlib.indexOf("#include <metal_control>"),
      );
      assert(
        stdlib.indexOf("#define FLOOR_AIR_METAL_STDLIB") <
          stdlib.indexOf("#include <metal_atomic>"),
      );
      assert(
        atomic.indexOf("#define FLOOR_AIR_METAL_ATOMIC") <
          atomic.indexOf("#include <metal_stdlib>"),
      );
      assert(atomic.includes("enum class mem_flags { mem_threadgroup = 2 };"));
      assert(atomic.includes("void threadgroup_barrier(mem_flags flags)"));
      assert(
        apiSource.includes(
          'if (file->getName() == "/include/metal_atomic") return trustedAtomicDeclaration(function, attr);',
        ),
      );
      assert(
        apiSource.includes(
          'if (name=="air.wg.barrier" && (llvm::StringRef(stage)!="air.kernel" || !constant(0,2) || !constant(1,1))) return false;',
        ),
      );
    },
  );

  test("authored executable fixtures are wired into the existing header smoke suite", async () => {
    const smoke = await read("header_smoke.cpp");
    const fixtures = await read("operations_smoke.h");
    assert(smoke.includes('#include "operations_smoke.h"'));
    assert(smoke.includes("failures+=checkMissingOperations();"));
    for (const marker of [
      "exact scalar math AIR signature",
      "exact bit reversal AIR signature",
      "exact SIMD all AIR signature",
      "SIMD all rejected outside compute",
      "SIMD all helper rejected outside compute",
      "counterfeit AIR label rejected",
      "assembly and assembly symbol labels are unsupported",
      "air_result_size(result) == 0",
    ])
      assert(fixtures.includes(marker), marker);
  });

  test(
    "quad tessellation factors use the verified shader half layout",
    sourceReviewOptions,
    async () => {
      const tessellation = await read("include/metal_tessellation");
      const fixtures = await read("operations_smoke.h");
      assert.match(
        tessellation,
        /struct MTLQuadTessellationFactorsHalf\s*\{\s*half edgeTessellationFactor\[4\];\s*half insideTessellationFactor\[2\];\s*\};/,
      );
      for (const assertion of [
        "sizeof(MTLQuadTessellationFactorsHalf)==12",
        "alignof(MTLQuadTessellationFactorsHalf)==2",
        "__builtin_offsetof(MTLQuadTessellationFactorsHalf,edgeTessellationFactor)==0",
        "__builtin_offsetof(MTLQuadTessellationFactorsHalf,insideTessellationFactor)==8",
      ]) {
        assert(tessellation.includes(assertion), assertion);
        assert(fixtures.includes(assertion), assertion);
      }
      assert(fixtures.includes("quad factors retain half-array IR layout"));
      assert(fixtures.includes("quad factors use half stores, not numeric integer conversion"));
    },
  );

  test(
    "the authenticated Metal umbrella enables fp16 during parsing after semantic initialization",
    sourceReviewOptions,
    async () => {
      const apiSource = await read("air_compiler.cpp");
      const stdlib = await read("include/metal_stdlib");
      const fixtures = await read("operations_smoke.h");
      const activation = "#pragma OPENCL EXTENSION cl_khr_fp16 : enable";
      assert.equal(stdlib.split(activation).length - 1, 1);
      assert(stdlib.indexOf(activation) > stdlib.indexOf("#define FLOOR_AIR_METAL_STDLIB"));
      assert(stdlib.indexOf(activation) < stdlib.indexOf("namespace metal"));
      assert(stdlib.indexOf(activation) < stdlib.indexOf("AIR_VECTOR_TYPES(half)"));
      assert.doesNotMatch(stdlib, /#pragma OPENCL EXTENSION (?!cl_khr_fp16\s*: enable)/);
      // CreateASTConsumer 时机过早，Metal 的 C++20 模块读取器随后在 Sema::Initialize 中覆盖 OpenCLFeatures，不能恢复那个钩子。
      // CreateASTConsumer is too early: Metal's C++20 module reader subsequently
      // overwrites OpenCLFeatures during Sema::Initialize. Do not restore that hook.
      assert.doesNotMatch(apiSource, /getOpenCLFeatures\(\)\.enable/);
      assert.doesNotMatch(apiSource, /-cl-ext=|#pragma OPENCL EXTENSION/);
      assert(apiSource.includes('"-I/include", "-include", "metal_stdlib"'));
      assert(fixtures.includes("for (const unsigned profile : {0u, 1u})"));
      for (const marker of [
        "native Metal half semantics in both profiles",
        "half helper preserves native argument and return ABI",
        "scalar and vector half arithmetic stay native",
        "half memory and explicit float conversions remain typed",
      ])
        assert(fixtures.includes(marker), marker);
    },
  );

  test(
    "only reference-established device uint add extends atomics to fragments",
    sourceReviewOptions,
    async () => {
      const apiSource = await read("air_compiler.cpp");
      const trust = await read("atomic_trust.inc");
      const fixtures = await read("operations_smoke.h");
      assert.match(
        apiSource,
        /const bool fragmentAdd=llvm::StringRef\(stage\)=="air\.fragment" &&\s*name=="air\.atomic\.global\.add\.u\.i32";/,
      );
      assert(
        apiSource.includes(
          '(llvm::StringRef(stage)!="air.kernel" && !fragmentAdd) || !constant(2,0)',
        ),
      );
      assert(
        apiSource.includes(
          '!constant(3,name.startswith("air.atomic.global.") ? 2 : 1) || !constant(4,1)',
        ),
      );
      assert(
        trust.includes(
          'const bool global=name=="air.atomic.global.add.u.i32" || name=="air.atomic.global.min.u.i32";',
        ),
      );
      assert(
        trust.includes("function->getNumParams()!=5 || !is(function->getReturnType(),B::UInt)"),
      );
      assert(
        trust.includes(
          "pointee.getAddressSpace()!=(global ? clang::LangAS::opencl_global : clang::LangAS::opencl_local)",
        ),
      );
      for (const marker of [
        "fragment device atomic add",
        "fragment global min remains rejected",
        "fragment local atomics remain rejected",
        "vertex global add remains rejected",
        "fragment barrier remains rejected",
        "fragment atomic flags remain exact",
      ])
        assert(fixtures.includes(marker), marker);
    },
  );

  test("real-WASM CI canary includes all five live operations in a fourth authored fixture", async () => {
    const canary = await read("../ci-compile-canary.mjs");
    const fixture = await read("../fixtures/msl-scalar-operations.metal");
    const cases = [...canary.matchAll(/stage: "(vertex|fragment|kernel)",\s*filename: "([^"]+)"/g)];
    assert.equal(cases.length, 19);
    assert.equal(new Set(cases.map((entry) => entry[1])).size, 3);
    assert(
      cases.some((entry) => entry[1] === "kernel" && entry[2] === "msl-scalar-operations.metal"),
    );
    assert(canary.includes("maxCompilations: cases.length"));
    assert(
      canary.includes("authored-nineteen-fixture-three-stage-compilation-and-container-integrity"),
    );
    assert.match(canary, /adapter\.compile\(source,[\s\S]*?output: "metallib"/);
    assert.match(canary, /inspectCompiledMetalLibrary\(output\.bytes,/);
    assert.match(fixture, /kernel void main0\(constant float \*input/);
    assert.match(fixture, /constant int \*bits/);
    assert.match(fixture, /device MTLQuadTessellationFactorsHalf \*tess_output/);
    assert.match(fixture, /constant half \*half_input/);
    assert.match(
      fixture,
      /static __attribute__\(\(noinline\)\) half canary_half_sum\(half a, half b\)\s*\{\s*return a \+ b;/,
    );
    assert.match(fixture, /#include <metal_stdlib>/);
    assert.doesNotMatch(fixture, /#include <metal_atomic>/);
    for (const statement of [
      "math_output[0] = sin(input[0]);",
      "math_output[1] = cos(input[1]);",
      "math_output[2] = ceil(input[2]);",
      "math_output[3] = float(canary_half_sum(half_input[0], half_input[1]));",
      "integer_output[0] = reverse_bits(bits[0]);",
      "vote_output[0] = simd_all(bits[1] != 0) ? 1u : 0u;",
      "threadgroup_barrier(mem_flags::mem_threadgroup);",
      "tess_output[0].edgeTessellationFactor[i] = half(input[i + 3]);",
      "tess_output[0].insideTessellationFactor[i] = half(input[i + 7]);",
    ])
      assert(fixture.includes(statement), statement);
    assert(
      cases.some(
        (entry) => entry[1] === "fragment" && entry[2] === "msl-fragment-atomic-add.metal",
      ),
    );
    const fragmentFixture = await read("../fixtures/msl-fragment-atomic-add.metal");
    assert.match(
      fragmentFixture,
      /fragment AtomicFragmentResult main0\(device atomic_uint \*counters/,
    );
    assert.match(fragmentFixture, /constant uint \*input/);
    assert(
      fragmentFixture.includes(
        "atomic_fetch_add_explicit(&counters[input[0]], input[1], memory_order_relaxed)",
      ),
    );
    assert(fragmentFixture.includes("float4(previous == input[2] ? 1.0f : 0.0f)"));

    // D2R 自有样例留在本地，发布源码归档另行镜像；不能声称通用编译器的源码归档包含这些探针。
    // D2R-owned fixtures stay local; the release archive is mirrored separately.
    // Do not claim that the generic compiler's source archive contains these probes.
    assert.equal(
      digest(canary),
      "19415766142c77a9443478e74d5413a300397c28a1a0395b2f99d22d3547eeca",
      "the strict 31-call D2R canary remains byte-identical",
    );
  });

  test("real-WASM canaries retain named entries and public compiler regression inputs", async () => {
    const canary = await read("../ci-compile-canary.mjs");
    const cases = [...canary.matchAll(/stage: "(vertex|fragment|kernel)",\s*filename: "([^"]+)"/g)];
    assert.equal(new Set(cases.map((entry) => entry[2])).size, cases.length);
    for (const [stage, filename] of [
      ["fragment", "msl-internal-helper-function-constant.metal"],
      ["kernel", "msl-optimizer-bit-reversal.metal"],
      ["vertex", "msl-patch-entry-name.metal"],
      ["fragment", "constant-vector-pointer-reads.metal"],
      ["fragment", "direct-buffer-vector-pointer.metal"],
      ["fragment", "conditional-distinct-buffer-rows.metal"],
      ["fragment", "loop-carried-buffer-pointer.metal"],
      ["kernel", "msl-stage-record-threadgroup.metal"],
      ["kernel", "msl-stage-record-buffers.metal"],
      ["kernel", "msl-buffer-reflection-mixed.metal"],
      ["kernel", "msl-buffer-reflection-array.metal"],
      ["kernel", "msl-buffer-reflection-float3.metal"],
      ["kernel", "msl-buffer-reflection-nested.metal"],
    ]) {
      assert(
        cases.some((entry) => entry[1] === stage && entry[2] === filename),
        filename,
      );
    }
    for (const entry of [
      "patch_linkage_probe",
      "constant_vector_pointer_reads",
      "direct_buffer_vector_pointer",
      "conditional_distinct_buffer_rows",
      "loop_carried_buffer_pointer",
      "stage_record_threadgroup",
      "stage_record_buffers",
      "reflection_mixed",
      "reflection_array",
      "reflection_float3",
      "reflection_nested",
    ])
      assert(canary.includes(`entry: "${entry}"`), entry);
    assert.match(
      canary,
      /inspectCompiledMetalLibrary\(output\.bytes,\s*\{\s*entry: fixture\.entry \?\? "main0",\s*stage: fixture\.stage,/,
    );
    assert(canary.includes("minimumOS: [18, 0]"));
    assert(canary.includes("languageVersion: [3, 2]"));
    assert(canary.includes("metadataLengthIncludesPrefix: false"));
    assert(canary.includes("output?.bytes.fill(0)"));
    assert(canary.includes("adapter.dispose()"));

    const linkage = await read("../fixtures/msl-internal-helper-function-constant.metal");
    assert(linkage.includes("static __attribute__((noinline)) float add_bias(float value)"));
    assert(linkage.includes("constant bool enable_color [[function_constant(17)]]"));
    assert(linkage.includes("function_constant(enable_color)"));
    assert(linkage.includes("float4(add_bias(values[0]))"));
    const reversal = await read("../fixtures/msl-optimizer-bit-reversal.metal");
    assert(reversal.includes("uint bits = values[0]"));
    for (const width of [16, 8, 4, 2, 1]) {
      assert(reversal.includes(`bits >> ${width}`));
      assert(reversal.includes(`bits << ${width}`));
    }
    assert(reversal.includes("values[0] = bits"));
    assert.doesNotMatch(reversal, /\breverse_bits\s*\(/);

    const patch = await read("../fixtures/msl-patch-entry-name.metal");
    assert(patch.includes("#define PATCH_ENTRY_NAME patch_linkage_probe"));
    assert(patch.includes("[[patch(triangle, PATCH_CONTROL_POINTS)]] vertex"));
    assert(patch.includes("entry_bias(float value)"));
    assert(patch.includes("entry_bias(uint value)"));
    assert(patch.includes("input.points[0].position"));
    assert(patch.includes("entry_bias(coordinate.x)"));

    const pointer = await read("../fixtures/constant-vector-pointer-reads.metal");
    assert(pointer.includes("fragment float4 constant_vector_pointer_reads("));
    assert(pointer.includes("constant VectorStorage* data [[id(0)]]"));
    assert(pointer.includes("reinterpret_cast<constant int4*>(args.data)"));
    assert(pointer.includes("int4 selected = words[row]"));
    assert(pointer.includes("int seed = words[0][lane]"));
    assert(pointer.includes("return float4(float(selected.x ^ seed)"));
    const directPointer = await read("../fixtures/direct-buffer-vector-pointer.metal");
    assert(directPointer.includes("fragment float4 direct_buffer_vector_pointer("));
    assert(directPointer.includes("constant VectorStorage* data [[buffer(0)]]"));
    assert(directPointer.includes("reinterpret_cast<constant int4*>(data)"));
    assert(directPointer.includes("int4 selected = words[row]"));
    assert(directPointer.includes("int seed = words[0][lane]"));
    assert(directPointer.includes("return float4(float(selected.x ^ seed)"));

    // 保留证明 select/PHI 失败的精确公开输入。哈希认证自编样例字节，不代表编译成功，也不是私有数据证明。
    // Keep the exact public inputs that demonstrated select/PHI failures. Hashes
    // authenticate authored fixture bytes, not compiler success or private data.
    for (const [filename, entry, expectedHash] of [
      [
        "conditional-distinct-buffer-rows.metal",
        "conditional_distinct_buffer_rows",
        "92a97acb7f5c670953d634b2a1635da98db32c283cfe98e53312728c81277ef4",
      ],
      [
        "loop-carried-buffer-pointer.metal",
        "loop_carried_buffer_pointer",
        "99e11691adfe5cef525fccfa3d64e5b21d03800f36bb351d1db50e6316247314",
      ],
    ]) {
      const source = await read(`../fixtures/${filename}`);
      assert.equal(digest(source), expectedHash, filename);
      assert(source.includes(`fragment float4 ${entry}(`), filename);
      assert(source.includes("constant VectorStorage* first [[id(0)]]"));
      assert(source.includes("constant VectorStorage* second [[id(1)]]"));
    }

    // 所有 D2R 自编样例路径保持本地可读；这不会执行或导入 WASM 探针。
    // All D2R-authored fixture paths remain local and readable; this does not
    // execute or import the WASM canary.
    for (const entry of cases) {
      assert.match(entry[2], /^[A-Za-z0-9_-]+\.metal$/);
      assert((await read(`../fixtures/${entry[2]}`)).includes("#include <metal_stdlib>"));
    }
  });

  test("real-WASM canary checks emitted reflection and actual float3 storage, not only containers", async () => {
    const canary = await read("../ci-compile-canary.mjs");
    for (const marker of [
      'from "./check-buffer-reflection.mjs"',
      'new URL("./fixtures/buffer-reflection.json", import.meta.url)',
      "maxCompilations: cases.length + reflectionFixtures.length",
      'assert.equal(sourceSha256, reflection.sha256, "reviewed reflection fixture identity")',
      'output: "llvm-text"',
      'new TextDecoder("utf-8", { fatal: true }).decode(irOutput.bytes)',
      "checkReflection(ir, reflection)",
      'if (fixture.entry === "reflection_float3") checkFloat3Storage(ir)',
      "irOutput?.bytes.fill(0)",
      'assert.equal(llvmValidation.length, reflectionFixtures.length, "all reflection probes ran")',
    ])
      assert(canary.includes(marker), marker);
    assert(
      canary.indexOf("checkReflection(ir, reflection)") < canary.indexOf("llvmValidation.push("),
    );
  });

  test("real-WASM canary preserves strict containers and binary flags for four liveness variants", async () => {
    const canary = await read("../ci-compile-canary.mjs");
    for (const marker of [
      'from "./check-fragment-input-liveness.mjs"',
      'from "./check-fragment-input-reflection.mjs"',
      "maxCompilations: cases.length + reflectionFixtures.length + livenessVariants.length * 2",
      'const livenessFilename = "msl-fragment-input-liveness.metal"',
      "0820f23da050fa12a4031a45c09dec58a4394f7179032570c1acd85a20d434d5",
      "#define LIVENESS_KEEP_VARYING ${Number(keepVarying)}\\n#define LIVENESS_KEEP_POSITION ${Number(keepPosition)}\\n${livenessText}",
      'entry: "fragment_input_liveness"',
      "checkFragmentInputLiveness(ir, { keepVarying, keepPosition })",
      "checkFragmentInputReflection(output.bytes, { keepVarying, keepPosition })",
      'validation: "exact-api-input-descriptors-final-argument-uses-and-binary-reflection"',
    ])
      assert(canary.includes(marker), marker);
    const variants = [
      ...canary.matchAll(
        /\{ name: "([^"]+)", keepVarying: (true|false), keepPosition: (true|false) \}/g,
      ),
    ].map((match) => match.slice(1));
    assert.deepEqual(variants, [
      ["dead", "false", "false"],
      ["live", "true", "true"],
      ["varying-dead", "false", "true"],
      ["position-dead", "true", "false"],
    ]);
    assert.equal(19 + 4 + variants.length * 2, 31);
    const loop = canary.slice(
      canary.indexOf("for (const { name, keepVarying, keepPosition } of livenessVariants)"),
    );
    assert.match(
      loop,
      /output: "metallib"[\s\S]*inspectCompiledMetalLibrary\(output.bytes,[\s\S]*entry: "fragment_input_liveness",\s*stage: "fragment",\s*minimumOS: \[18, 0\],\s*languageVersion: \[3, 2\],\s*metadataLengthIncludesPrefix: false/,
    );
    assert.match(
      loop,
      /output: "llvm-text"[\s\S]*checkFragmentInputLiveness\(ir, \{ keepVarying, keepPosition \}\)/,
    );
    assert(
      loop.indexOf("inspectCompiledMetalLibrary(output.bytes") <
        loop.indexOf("checkFragmentInputReflection(output.bytes"),
    );
    assert(
      loop.indexOf("checkFragmentInputReflection(output.bytes") <
        loop.indexOf("fragmentInputLiveness.push("),
    );
    assert.match(loop, /finally \{\s*output\?\.bytes.fill\(0\);\s*irOutput\?\.bytes.fill\(0\);/);
  });

  test(
    "generic header identity fingerprints all ten authenticated headers in its own domain",
    sourceReviewOptions,
    async () => {
      const cmake = await read("CMakeLists.txt");
      const paths = cmake
        .match(/set\(AIR_HEADER_PATHS\s+([^)]*)\)/)[1]
        .trim()
        .split(/\s+/)
        .sort();
      for (const header of ["metal_math", "metal_integer", "metal_simd"])
        assert(paths.includes(header));
      assert.equal(paths.length, 10);
      let fingerprint = "floor-air-header-bundle-v2\n";
      for (const path of paths) fingerprint += `${path}:${digest(await read(`include/${path}`))}\n`;
      assert.equal(
        digest(fingerprint),
        "694371baef693289d23f75c9b5721227b8290281c513dc81cc6f8f696b3ec7eb",
      );
      const lock = await readReviewedSourceLock();
      assert.deepEqual(
        paths,
        Object.keys(lock.sdkFiles)
          .filter((path) => path.startsWith("api/include/"))
          .map((path) => path.slice(12))
          .sort(),
      );
      assert(cmake.includes('set(AIR_HEADER_FINGERPRINT "floor-air-header-bundle-v2\\n")'));
    },
  );
}

// air-pointer-slot-source
{
  // 验证 air-pointer-slot-source 的约定与边界；依据 ./fixtures/air/fixture.mjs 及本文件的测试断言。
  // Verify air-pointer-slot-source contracts and boundaries against ./fixtures/air/fixture.mjs and this file's assertions.
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { readFile } = promisesModule;
  const { default: test } = nodeTestModule;
  const { sourceReviewOptions, readForkSource } = fixtureModule;

  const root = new URL("../toolchain/metal-air-compiler/", import.meta.url);
  const read = (path) => readFile(new URL(path, root), "utf8");
  const hash = (source) => createHash("sha256").update(source).digest("hex");

  // 仅检查源码身份/覆盖范围，不运行原生 opt、校验器或编译器。
  // Source identity/coverage checks only: no native opt, verifier or compiler is run.
  test(
    "typed pointer-slot correction retains its authenticated fork source boundary",
    sourceReviewOptions,
    async () => {
      const source = await readForkSource("llvm/lib/Transforms/LibFloor/PointerBCFixup.cpp");
      assert.equal(
        hash(source),
        "bd857274c61741b968bbc5c9d7fc9a849b99757fba93290f260031e54ca65c06",
      );
    },
  );

  test(
    "exact inverse merges retain authenticated nominal-root and cycle proofs",
    sourceReviewOptions,
    async () => {
      const source = await readForkSource("llvm/lib/Transforms/LibFloor/PointerBCFixup.cpp");
      assert.equal(
        hash(source),
        "bd857274c61741b968bbc5c9d7fc9a849b99757fba93290f260031e54ca65c06",
      );
      assert.equal(
        hash(await read("ci-compile-canary.mjs")),
        "19415766142c77a9443478e74d5413a300397c28a1a0395b2f99d22d3547eeca",
        "existing 31-call strict real-WASM canary is unchanged",
      );
    },
  );

  test("native slot matrix contains seventeen positive functions and twenty-seven isolated rejections", async () => {
    const matrix = await read("fixtures/metal-constant-pointer-slots.ll");
    assert.equal(hash(matrix), "e72ff040a72f0a8f4bb179809c6e8470e207d9dc2565ed5a4fe054d7fadeaef6");
    assert.equal(
      hash(
        matrix.replace(
          "scalar-to-aggregate inverse pointer cast (select)",
          "scalar or non-aggregate inverse pointer cast (select)",
        ),
      ),
      "6348d48faad7a4e58f0dcab5c5cb61af1f169be8d418898207aa2b7c6dcefaa8",
      "all previous 17/27 fixture bodies and checks except one refined diagnostic are unchanged",
    );
    const split = matrix.split(/^;--- (.+)\n/gm);
    const modules = new Map();
    for (let i = 1; i < split.length; i += 2) modules.set(split[i], split[i + 1]);
    const rejected = [
      "unknown_slot",
      "raw_byte_slot",
      "nonzero_pointer_field",
      "wrong_inner_as",
      "unproved_addrspacecast",
      "wrong_layout",
      "empty_slot_array",
      "volatile_slot",
      "atomic_slot",
      "shifted_reverse",
      "nonexact_reverse",
      "native_escape",
      "native_write",
      "padded_wrapper",
      "inverse_mixed_nominal",
      "inverse_vector_root",
      "inverse_zero_gep",
      "inverse_shifted_gep",
      "inverse_rootless_subcycle",
      "inverse_transitive_native",
      "inverse_null_native",
      "inverse_undef_native",
      "inverse_poison_native",
      "inverse_unknown_arm",
      "inverse_scalar_view",
      "inverse_merged_escape",
      "inverse_merged_write",
    ].map((name) => `${name}.ll`);
    assert.deepEqual([...modules.keys()], ["slots.ll", ...rejected]);
    const functions = [...modules.get("slots.ll").matchAll(/^define .+ @([^ (]+)\(/gm)].map(
      (match) => match[1],
    );
    assert.deepEqual(functions, [
      "direct_slot",
      "folded_record",
      "nested_slot",
      "vector_slot",
      "scalar_slot",
      "exact_reverse_mixed",
      "exact_reverse_native_only",
      "shared_slot_cast",
      "shared_consumed_slot",
      "native_recast_crosslink",
      "inverse_root_roundtrip",
      "inverse_diamond",
      "inverse_slot_select",
      "inverse_loop",
      "inverse_duplicate_edges",
      "inverse_local_nominal_types",
      "inverse_direct_vector_slot",
    ]);
    assert.deepEqual(
      [...matrix.matchAll(/^; POS-LABEL: define floor_fragment .+ @([^ (]+)\(/gm)].map(
        (match) => match[1],
      ),
      functions,
    );
    const negativeRuns = [
      ...matrix.matchAll(
        /^; RUN: not opt .+ %t\/([^ ]+) 2>&1 \| FileCheck %s --check-prefix=([^\n]+)$/gm,
      ),
    ];
    assert.deepEqual(
      negativeRuns.map((match) => match[1]),
      rejected,
    );
    const inverseReasons = {
      "inverse_mixed_nominal.ll": "inverse root nominal type mismatch (bitcast)",
      "inverse_vector_root.ll": "inverse root nominal type mismatch (bitcast)",
      "inverse_zero_gep.ll": "unsupported inverse pointer input (getelementptr)",
      "inverse_shifted_gep.ll": "unsupported inverse pointer input (getelementptr)",
      "inverse_rootless_subcycle.ll": "unanchored inverse pointer cycle (phi)",
      "inverse_transitive_native.ll": "inverse root depends on rewritten native graph (bitcast)",
      "inverse_null_native.ll": "unproved inverse native root (bitcast)",
      "inverse_undef_native.ll": "unproved inverse native root (bitcast)",
      "inverse_poison_native.ll": "unproved inverse native root (bitcast)",
      "inverse_unknown_arm.ll": "unmapped argument or constant pointer root",
      "inverse_scalar_view.ll": "scalar-to-aggregate inverse pointer cast (select)",
      "inverse_merged_escape.ll": "unsupported native-view user (call)",
      "inverse_merged_write.ll": "unsupported native-view user (store)",
    };
    for (const [, name, prefix] of negativeRuns) {
      assert.equal([...modules.get(name).matchAll(/^define /gm)].length, 1);
      assert(modules.get(name).includes(" = select "), name);
      if (Object.hasOwn(inverseReasons, name))
        assert(
          matrix.includes(`; ${prefix}: constant-vector graph: ${inverseReasons[name]}\n`),
          prefix,
        );
      else assert(matrix.includes(`; ${prefix}: invalid pointer bitcast:`), prefix);
    }
    assert(matrix.includes("opt -enable-new-pm=0 -PointerBCFixup -verify -S %t/slots.ll"));
    assert(matrix.includes("--implicit-check-not='= bitcast"));
    assert(matrix.includes("--implicit-check-not='= load"));
    const retained = {
      positives: [...modules.get("slots.ll").matchAll(/^define [^\n]+\n[\s\S]*?^\}/gm)]
        .slice(0, 10)
        .map((match) => match[0]),
      negativeModules: [...modules.values()].slice(1, 15).map((body) => body.trim()),
    };
    assert.equal(
      hash(JSON.stringify(retained)),
      "af5f6bbb56ef497954369ca146b58ef335a6b9eaa291f5ee6a28810cbc5e981e",
      "original 10/14 regression bodies remain unchanged",
    );
  });

  test("demonstrated pointer-slot MSL input is authenticated in the strict real-WASM canary", async () => {
    const filename = "retyped-buffer-pointer-load.metal";
    const source = await read(`fixtures/${filename}`);
    const expected = "57f26a87e4b72aab90afb09caf9f650ddac7d7f025fd29bdfbabce9c46fa536e";
    assert.equal(hash(source), expected);
    assert(source.includes("fragment float4 retyped_buffer_pointer_load("));
    const canary = await read("ci-compile-canary.mjs");
    assert.match(
      canary,
      /stage: "fragment",\s*filename: "retyped-buffer-pointer-load\.metal",\s*entry: "retyped_buffer_pointer_load",\s*sha256: "57f26a87e4b72aab90afb09caf9f650ddac7d7f025fd29bdfbabce9c46fa536e",\s*define: ""/,
    );
    assert(
      canary.includes(
        'assert.equal(sourceSha256, fixture.sha256, "reviewed pointer-slot fixture identity")',
      ),
    );
    assert(
      canary.indexOf("if (fixture.sha256)") < canary.indexOf("output = adapter.compile(source"),
    );
    assert.match(
      canary,
      /inspectCompiledMetalLibrary\(output.bytes,\s*\{\s*entry: fixture.entry \?\? "main0",\s*stage: fixture.stage,\s*minimumOS: \[18, 0\],\s*languageVersion: \[3, 2\],\s*metadataLengthIncludesPrefix: false/,
    );
    assert(
      !canary.includes("metal-constant-pointer-slots.ll"),
      "raw IR is not executed by the MSL API canary",
    );
  });

  test(
    "canonical GEP restoration retains its exact authenticated fork invariants",
    sourceReviewOptions,
    async () => {
      const source = await readForkSource("llvm/lib/Transforms/LibFloor/PointerBCFixup.cpp");
      assert.equal(
        hash(source),
        "bd857274c61741b968bbc5c9d7fc9a849b99757fba93290f260031e54ca65c06",
      );
    },
  );

  test("canonical GEP matrix retains nine symbolic positives and eighteen isolated rejections", async () => {
    const matrix = await read("fixtures/metal-canonical-gep-recasts.ll");
    assert.equal(hash(matrix), "6b3135d88d07acb2f51d8d9b9d7fc3c161a7025d9e0c7afdc86fbf088493e4cc");
    const split = matrix.split(/^;--- (.+)\n/gm);
    const modules = new Map();
    for (let i = 1; i < split.length; i += 2) modules.set(split[i], split[i + 1]);
    const names = [
      "positive.ll",
      "scalar_wrong_width.ll",
      "vector_lane_regroup.ll",
      "vector_to_scalar.ll",
      "scalar_to_vector.ll",
      "scalar_to_record.ll",
      "scalar_to_array.ll",
      "vector_to_record.ll",
      "canonical_write.ll",
      "canonical_escape.ll",
      "canonical_volatile.ll",
      "canonical_atomic.ll",
      "canonical_address_space.ll",
      "canonical_select_input.ll",
      "canonical_unknown_arm.ll",
      "canonical_wrong_root_layout.ll",
      "scalar_to_pointer.ll",
      "vector_to_pointer.ll",
      "vector_to_array.ll",
    ];
    assert.deepEqual([...modules.keys()], names);
    const positives = [
      "canonical_scalar_gep",
      "canonical_vector_gep",
      "canonical_chained_scalar",
      "canonical_native_merges",
      "canonical_loop_alias",
      "canonical_native_gep_crosslink",
      "canonical_shared_inverse",
      "canonical_with_slot_inverse",
      "canonical_integer_storage",
    ];
    assert.deepEqual(
      [...modules.get("positive.ll").matchAll(/^define .+ @([^ (]+)\(/gm)].map((match) => match[1]),
      positives,
    );
    assert.deepEqual(
      [...matrix.matchAll(/^; POS-LABEL: define floor_fragment .+ @([^ (]+)\(/gm)].map(
        (match) => match[1],
      ),
      positives,
    );
    const rejected = [
      ...matrix.matchAll(
        /^; RUN: not opt .+ %t\/([^ ]+) 2>&1 \| FileCheck %s --check-prefix=([^\n]+)$/gm,
      ),
    ];
    assert.deepEqual(
      rejected.map((match) => match[1]),
      names.slice(1),
    );
    for (const [, name, prefix] of rejected) {
      assert.equal([...modules.get(name).matchAll(/^define /gm)].length, 1);
      assert(matrix.includes(`; ${prefix}: constant-vector graph: `), prefix);
    }
    assert(matrix.includes("opt -enable-new-pm=0 -PointerBCFixup -verify -S %t/positive.ll"));
    assert(matrix.includes("--implicit-check-not='= load "));
    assert(matrix.includes("--implicit-check-not='= bitcast "));
    assert(matrix.includes("Native opt/FileCheck is NOT run by current CI"));
    assert(!(await read("ci-compile-canary.mjs")).includes("metal-canonical-gep-recasts.ll"));
  });
}

// air-release-contract
{
  // 验证 air-release-contract 的约定与边界；依据 ../runtime-build/air-release-contract.mjs、./fixtures/air/fixture.mjs 及本文件的测试断言。
  // Verify air-release-contract contracts and boundaries against ../runtime-build/air-release-contract.mjs、./fixtures/air/fixture.mjs and this file's assertions.
  const { default: assert } = strictModule;
  const { Buffer } = nodeBufferModule;
  const { default: test } = nodeTestModule;
  const { readFile } = promisesModule;
  const {
    AIR_RELEASE_CAPS,
    AIR_RELEASE_OUTPUTS,
    airReleaseFileMaximum,
    airReleaseSha256,
    safeAirReleasePath,
    parseAirReleaseJson,
    validateFreshAirReleasePin,
    verifyFreshAirManifest,
    freshAirAssetUrl,
    verifyFreshAirAsset,
    freshAirSourceFingerprint,
    verifyFreshAirBuildInputs,
    verifyFreshAirReceipt,
    verifyFreshAirSourceLock,
    verifyFreshAirNotices,
  } = airReleaseContractModule;
  const {
    createFreshAirReleaseFixture,
    fixtureJson,
    fixtureRecord,
    fixtureSha256,
    fixtureSourceFingerprint,
    fixtureHeaderDigest,
  } = fixtureModule;

  // 解析/哈希固定元数据和合成字节，不运行编译器或 WASM。
  // These tests parse/hash pinned metadata and synthetic bytes. No compiler or WASM runs.
  const checkManifest = (fixture) => verifyFreshAirManifest(fixture.manifestBytes, fixture.pin);
  const checkInputs = (fixture) => verifyFreshAirBuildInputs(fixture.inputsBytes, fixture.manifest);
  const checkReceipt = (fixture) =>
    verifyFreshAirReceipt(fixture.receiptBytes, fixture.manifest, fixture.inputs);
  const checkLock = (fixture) =>
    verifyFreshAirSourceLock(fixture.sourceLockBytes, fixture.manifest, fixture.inputs);

  test("synthetic fresh metadata has deterministic, mutually bound source/API/output identities", () => {
    const fixture = createFreshAirReleaseFixture();
    const repeated = createFreshAirReleaseFixture();
    for (const name of ["manifestBytes", "receiptBytes", "inputsBytes", "sourceLockBytes"])
      assert.deepEqual(fixture[name], repeated[name], name);
    assert.deepEqual(checkManifest(fixture), fixture.manifest);
    assert.deepEqual(checkInputs(fixture), fixture.inputs);
    assert.deepEqual(checkReceipt(fixture), fixture.receipt);
    assert.deepEqual(checkLock(fixture), fixture.sourceLock);
    assert.equal(fixture.pin.commit, "1".repeat(40), "dummy commit, never an actual release pin");
    assert.equal(fixture.manifest.tree, "2".repeat(40));
    assert.equal(fixture.sourceFiles.size, 50);
    assert.equal(Object.keys(fixture.sourceLock.compilerFiles).length, 31);
    assert.equal(Object.keys(fixture.sourceLock.sdkFiles).length, 19);
    assert.equal(
      freshAirSourceFingerprint(fixture.inputs),
      fixtureSourceFingerprint(fixture.inputs),
    );
    assert.equal(fixture.manifest.headerSha256, fixtureHeaderDigest(fixture.sourceLock));
    for (const [path, bytes] of fixture.runtimeFiles)
      assert.deepEqual(fixture.manifest.runtimeFiles[path], fixtureRecord(bytes), path);
    for (const [path, bytes] of fixture.sourceFiles)
      assert.equal(fixture.inputs.sourceInputs[path], fixtureSha256(bytes), path);
    assert.deepEqual(AIR_RELEASE_OUTPUTS, ["air_wasm.mjs", "air_wasm.wasm"]);
  });

  test("exact release pins reject aliases, malformed hashes, missing and extra fields", () => {
    const { pin } = createFreshAirReleaseFixture();
    assert.deepEqual(validateFreshAirReleasePin(pin), pin);
    const cases = [
      { schemaVersion: 2 },
      { repository: "https://github.com/example/project" },
      { repository: "../project" },
      { repository: "example/project/extra" },
      { commit: "main" },
      { commit: "A".repeat(40) },
      { commit: "1".repeat(39) },
      { version: "latest" },
      { version: "0.1.0" },
      { version: "v1.0.0/evil" },
      { manifestSha256: "F".repeat(64) },
      { manifestSha256: "a".repeat(63) },
      { historicalCommit: "3".repeat(40) },
    ];
    for (const change of cases)
      assert.throws(
        () => validateFreshAirReleasePin({ ...pin, ...change }),
        JSON.stringify(change),
      );
    for (const key of Object.keys(pin)) {
      const changed = { ...pin };
      delete changed[key];
      assert.throws(() => validateFreshAirReleasePin(changed), key);
    }
    for (const invalid of [null, [], "pin", 0])
      assert.throws(() => validateFreshAirReleasePin(invalid));
  });

  test("signed JSON rejects duplicate keys including nested/escaped forms and bounded malformed input", () => {
    for (const json of [
      '{"a":1,"a":2}',
      '{"a":1,"\\u0061":2}',
      '{"outer":{"a":1,"a":2}}',
      '[{"a":1,"a":2}]',
      '{"a":[{"nested":1,"nested":2}]}',
      '{"__proto__":{},"__proto__":{}}',
    ])
      assert.throws(
        () => parseAirReleaseJson(Buffer.from(json)),
        /Duplicate AIR release JSON key/,
        json,
      );
    assert.deepEqual(
      parseAirReleaseJson(
        Buffer.from('{"a":{"x":1},"b":{"x":2},"text":"\\\"x\\\":1,\\\"x\\\":2"}'),
      ),
      {
        a: { x: 1 },
        b: { x: 2 },
        text: '"x":1,"x":2',
      },
    );
    for (const bytes of [
      Buffer.alloc(0),
      Buffer.from([0xff]),
      Buffer.from('{"a":'),
      Buffer.from("{}{}"),
    ])
      assert.throws(() => parseAirReleaseJson(bytes));
    assert.throws(() => parseAirReleaseJson(Buffer.from('{"x":1}'), 2), /cap/);
    assert.throws(
      () => parseAirReleaseJson(Buffer.from(`${"[".repeat(64)}0${"]".repeat(64)}`)),
      /nesting exceeds cap/,
    );
    assert.deepEqual(parseAirReleaseJson(Buffer.from("{}"), 2), {});
  });

  test("manifest checks the byte digest before schema and independently binds the exact pin", () => {
    const fixture = createFreshAirReleaseFixture();
    assert.throws(
      () =>
        verifyFreshAirManifest(
          Buffer.concat([fixture.manifestBytes, Buffer.from(" ")]),
          fixture.pin,
        ),
      /digest mismatch/,
    );
    for (const [field, value] of [
      ["repository", "other/fork"],
      ["commit", "3".repeat(40)],
      ["version", "v99.0.0"],
    ]) {
      const changed = createFreshAirReleaseFixture({
        mutateManifest: (report) => {
          report[field] = value;
        },
      });
      assert.throws(() => checkManifest(changed), /pin mismatch/, field);
    }
    for (const [field, value] of [
      ["schemaVersion", 2],
      ["status", "draft"],
      ["kind", "migrated-compiler"],
      ["binaryBuiltFromNewFork", false],
    ]) {
      const changed = createFreshAirReleaseFixture({
        mutateManifest: (report) => {
          report[field] = value;
        },
      });
      assert.throws(() => checkManifest(changed), /unfinished AIR fork release/, field);
    }
  });

  test("manifest rejects wrong source/API/header identity and provenance/output inconsistencies", () => {
    for (const change of [
      { tree: "main" },
      { sourceLockSha256: "bad" },
      { sourceFingerprintSha256: "bad" },
      { apiVersion: 1 },
      { headerBundleVersion: 1 },
      { sourceLanguage: "metal" },
      { headerSha256: "bad" },
      { buildReceiptSha256: "bad" },
    ])
      assert.throws(
        () =>
          checkManifest(
            createFreshAirReleaseFixture({
              mutateManifest: (report) => Object.assign(report, change),
            }),
          ),
        JSON.stringify(change),
      );
    for (const name of ["build-report.json", "source-lock.json", ...AIR_RELEASE_OUTPUTS]) {
      const fixture = createFreshAirReleaseFixture({
        mutateManifest: (report) => {
          report.runtimeFiles[name].sha256 = "a".repeat(64);
        },
      });
      assert.throws(() => checkManifest(fixture), /inventory differs/, name);
    }
  });

  test("runtime inventory has safe bounded paths, required closure and exact output records", () => {
    const valid = createFreshAirReleaseFixture();
    for (const path of [
      "",
      "/abs",
      "../escape",
      "a/../b",
      "a//b",
      "a/./b",
      "a\\b",
      "a\0b",
      "a".repeat(101),
    ]) {
      assert.equal(safeAirReleasePath(path), false, path);
      assert.throws(
        () =>
          checkManifest(
            createFreshAirReleaseFixture({
              mutateManifest: (report) => {
                report.runtimeFiles[path] = fixtureRecord(Buffer.from("x"));
              },
            }),
          ),
        path,
      );
    }
    for (const path of ["air_wasm.mjs", "licenses/llvm/LICENSE.TXT", "include/simd/simd.h"])
      assert.equal(safeAirReleasePath(path), true);
    for (const name of [
      "air_wasm.mjs",
      "air_wasm.wasm",
      "build-report.json",
      "source-lock.json",
      "air_compiler.h",
      "README.txt",
      "CONTENTS.sha256",
      "licenses/llvm/LICENSE.TXT",
      "licenses/clang/LICENSE.TXT",
      "licenses/llvm/include/llvm/Support/LICENSE.TXT",
      "licenses/llvm/include/llvm/Transforms/LibFloor/cfg/LICENSE",
      "licenses/llvm/lib/Transforms/LibFloor/cfg/LICENSE",
      "licenses/llvm/lib/Bitcode/MetalLib/bzip2/LICENSE",
      "licenses/llvm/lib/Bitcode/MetalLib/tar/LICENSE",
      "licenses/GPL-2.0.txt",
      "licenses/ORIGINS.json",
    ]) {
      const fixture = createFreshAirReleaseFixture({
        mutateManifest: (report) => {
          delete report.runtimeFiles[name];
        },
      });
      assert.throws(() => checkManifest(fixture), /Missing AIR runtime member/, name);
    }
    for (const bytes of [
      0,
      -1,
      0.5,
      Number.MAX_SAFE_INTEGER + 1,
      airReleaseFileMaximum("air_wasm.wasm") + 1,
    ]) {
      assert.throws(
        () =>
          checkManifest(
            createFreshAirReleaseFixture({
              mutateManifest: (report) => {
                report.files["air_wasm.wasm"].bytes = bytes;
              },
            }),
          ),
        /output records/,
        String(bytes),
      );
    }
    assert.throws(
      () =>
        checkManifest(
          createFreshAirReleaseFixture({
            mutateManifest: (report) => {
              report.files.extra = report.files["air_wasm.mjs"];
            },
          }),
        ),
      /output records/,
    );
    assert.throws(
      () =>
        checkManifest(
          createFreshAirReleaseFixture({
            mutateManifest: (report) => {
              report.files["air_wasm.mjs"].extra = true;
            },
          }),
        ),
      /output records/,
    );
    assert.throws(
      () =>
        checkManifest(
          createFreshAirReleaseFixture({
            mutateManifest: (report) => {
              for (let i = 0; i < 65; i++)
                report.runtimeFiles[`extra-${i}`] = valid.manifest.files["air_wasm.mjs"];
            },
          }),
        ),
      /runtime inventory/,
    );
  });

  test("asset closure enforces unique safe names, SHA-256 and positive bounded byte sizes", () => {
    const fixture = createFreshAirReleaseFixture();
    for (const [kind, maximum] of Object.entries(AIR_RELEASE_CAPS)) {
      assert.equal(
        freshAirAssetUrl(fixture.manifest, kind),
        `https://github.com/example/synthetic-air-fixture/releases/download/v0.0.0-test/${fixture.manifest.assets[kind].name}`,
      );
      verifyFreshAirAsset(fixture.assetBytes[kind], fixture.manifest, kind);
      assert.throws(
        () => verifyFreshAirAsset(Buffer.from("tampered"), fixture.manifest, kind),
        /asset mismatch/,
      );
      for (const bytes of [0, -1, 0.5, maximum + 1])
        assert.throws(
          () =>
            checkManifest(
              createFreshAirReleaseFixture({
                mutateManifest: (report) => {
                  report.assets[kind].bytes = bytes;
                },
              }),
            ),
          /bounded AIR asset/,
          `${kind}:${bytes}`,
        );
      for (const name of ["../escape", "sub/path.tgz", "bad name", ""])
        assert.throws(
          () =>
            checkManifest(
              createFreshAirReleaseFixture({
                mutateManifest: (report) => {
                  report.assets[kind].name = name;
                },
              }),
            ),
          /bounded AIR asset/,
          name,
        );
      assert.throws(
        () =>
          checkManifest(
            createFreshAirReleaseFixture({
              mutateManifest: (report) => {
                delete report.assets[kind];
              },
            }),
          ),
        /closure/,
      );
    }
    assert.throws(
      () =>
        checkManifest(
          createFreshAirReleaseFixture({
            mutateManifest: (report) => {
              report.assets.runtime.name = report.assets.correspondingSource.name;
            },
          }),
        ),
      /bounded AIR asset/,
    );
    assert.throws(() => freshAirAssetUrl(fixture.manifest, "unknown"), /Unknown/);
    assert.throws(
      () => verifyFreshAirAsset(Buffer.from("x"), fixture.manifest, "unknown"),
      /mismatch/,
    );
  });

  test("source fingerprints are order independent and bind repository, commit, tree, lock and all 50 inputs", () => {
    const { inputs } = createFreshAirReleaseFixture();
    assert.equal(freshAirSourceFingerprint(inputs), inputs.sourceFingerprintSha256);
    assert.equal(
      freshAirSourceFingerprint({
        ...inputs,
        sourceInputs: Object.fromEntries(Object.entries(inputs.sourceInputs).reverse()),
      }),
      inputs.sourceFingerprintSha256,
    );
    for (const [key, value] of [
      ["repository", "other/fork"],
      ["commit", "3".repeat(40)],
      ["tree", "4".repeat(40)],
      ["sourceLockSha256", "a".repeat(64)],
    ])
      assert.notEqual(
        freshAirSourceFingerprint({ ...inputs, [key]: value }),
        inputs.sourceFingerprintSha256,
        key,
      );
    const path = Object.keys(inputs.sourceInputs)[0];
    assert.notEqual(
      freshAirSourceFingerprint({
        ...inputs,
        sourceInputs: { ...inputs.sourceInputs, [path]: "a".repeat(64) },
      }),
      inputs.sourceFingerprintSha256,
    );
    for (const mutate of [
      (map) => {
        delete map[path];
      },
      (map) => {
        map["wasm/unexpected.cpp"] = "a".repeat(64);
      },
      (map) => {
        map[path] = "invalid";
      },
      (map) => {
        map["../escape"] = map[path];
        delete map[path];
      },
      (map) => {
        map["unbounded/root.cpp"] = map[path];
        delete map[path];
      },
    ]) {
      const sourceInputs = { ...inputs.sourceInputs };
      mutate(sourceInputs);
      assert.throws(
        () => freshAirSourceFingerprint({ ...inputs, sourceInputs }),
        /source input closure/,
      );
    }
  });

  test("inputs bind the release identity, digest, fingerprints, commands and tooling inventory", () => {
    const fixture = createFreshAirReleaseFixture();
    assert.throws(
      () =>
        verifyFreshAirBuildInputs(
          Buffer.concat([fixture.inputsBytes, Buffer.from(" ")]),
          fixture.manifest,
        ),
      /asset mismatch/,
    );
    for (const [key, value] of [
      ["schemaVersion", 2],
      ["version", "v4.0.0"],
      ["commit", "3".repeat(40)],
      ["tree", "4".repeat(40)],
      ["sourceFingerprintSha256", "a".repeat(64)],
    ])
      assert.throws(
        () =>
          checkInputs(
            createFreshAirReleaseFixture({
              mutateInputs: (report) => {
                report[key] = value;
              },
            }),
          ),
        key,
      );
    for (const commands of [
      [],
      [""],
      [42],
      [[""]],
      [["x".repeat(8193)]],
      Array.from({ length: 33 }, () => ["x"]),
    ])
      assert.throws(
        () =>
          checkInputs(
            createFreshAirReleaseFixture({
              mutateInputs: (report) => {
                report.commands = commands;
              },
            }),
          ),
        /build commands/,
      );
    for (const tooling of [
      {},
      { "../escape": "a".repeat(64) },
      { "wasm/build.mjs": "bad" },
      Object.fromEntries(Array.from({ length: 17 }, (_, i) => [`file-${i}`, "a".repeat(64)])),
    ])
      assert.throws(
        () =>
          checkInputs(
            createFreshAirReleaseFixture({
              mutateInputs: (report) => {
                report.tooling = tooling;
              },
            }),
          ),
        /tooling inputs/,
      );
    const changed = createFreshAirReleaseFixture({
      mutateInputs: (report) => {
        report.sourceInputs[Object.keys(report.sourceInputs)[0]] = "a".repeat(64);
      },
    });
    assert.throws(() => checkInputs(changed), /fingerprint mismatch/);
  });

  test("receipt and build-input toolchains reject unsupported or incompletely disclosed environments", () => {
    const changes = [
      (tool) => {
        tool.emscriptenVersion = "latest";
      },
      (tool) => {
        tool.emsdkCommit = "a".repeat(40);
      },
      (tool) => {
        tool.nodeMajor = 22;
      },
      (tool) => {
        tool.platform = "Darwin";
      },
      (tool) => {
        tool.byteIdenticalRebuildClaimed = true;
      },
      ...[0, 5, 1.5].map((jobs) => (tool) => {
        tool.jobs = jobs;
      }),
      (tool) => {
        delete tool.versions.emcc;
      },
      (tool) => {
        tool.versions.node = "v22.0.0-test-fixture";
      },
      (tool) => {
        tool.versions.emcc = "emcc (synthetic metadata only) 3.1.0";
      },
      (tool) => {
        tool.versions.emcc = "emcc (synthetic metadata only) 4.0.200";
      },
      (tool) => {
        tool.runner.os = "Windows";
      },
      (tool) => {
        tool.versions.ccache = "";
      },
      (tool) => {
        tool.versions.python = "x".repeat(4097);
      },
      (tool) => {
        delete tool.runner.imageVersion;
      },
      (tool) => {
        tool.runner.arch = 64;
      },
    ];
    for (const mutate of changes) {
      assert.throws(
        () =>
          checkInputs(
            createFreshAirReleaseFixture({ mutateInputs: (report) => mutate(report.buildTool) }),
          ),
        /toolchain|environment disclosure|tool versions\/platform/,
      );
      assert.throws(
        () =>
          checkReceipt(
            createFreshAirReleaseFixture({ mutateReceipt: (report) => mutate(report.buildTool) }),
          ),
        /toolchain|environment disclosure|tool versions\/platform/,
      );
    }
  });

  test("fresh receipts bind digest/source/API/output records and agree with disclosed build inputs", () => {
    const fixture = createFreshAirReleaseFixture();
    assert.throws(
      () =>
        verifyFreshAirReceipt(
          Buffer.concat([fixture.receiptBytes, Buffer.from(" ")]),
          fixture.manifest,
          fixture.inputs,
        ),
      /receipt digest mismatch/,
    );
    for (const [key, value] of [
      ["schemaVersion", 2],
      ["kind", "migrated-compiler"],
      ["repository", "other/fork"],
      ["commit", "3".repeat(40)],
      ["tree", "4".repeat(40)],
      ["sourceLockSha256", "a".repeat(64)],
      ["headerSha256", "b".repeat(64)],
      ["apiVersion", 1],
      ["sourceLanguage", "old-metal"],
    ])
      assert.throws(
        () =>
          checkReceipt(
            createFreshAirReleaseFixture({
              mutateReceipt: (report) => {
                report[key] = value;
              },
            }),
          ),
        key,
      );
    assert.throws(
      () =>
        checkReceipt(
          createFreshAirReleaseFixture({
            mutateReceipt: (report) => {
              report.files["air_wasm.wasm"].sha256 = "a".repeat(64);
            },
          }),
        ),
      /output mismatch/,
    );
    assert.throws(
      () =>
        checkReceipt(
          createFreshAirReleaseFixture({
            mutateReceipt: (report) => {
              report.buildTool.jobs = 1;
            },
          }),
        ),
      /tool mismatch/,
    );
    const differentInputs = structuredClone(fixture.inputs);
    differentInputs.sourceInputs[Object.keys(differentInputs.sourceInputs)[0]] = "a".repeat(64);
    assert.throws(
      () => verifyFreshAirReceipt(fixture.receiptBytes, fixture.manifest, differentInputs),
      /source mismatch/,
    );
  });

  test("receipt verification cannot claim browser, device, GPU or full application execution", () => {
    for (const key of [
      "browserExecutionVerified",
      "deviceExecutionVerified",
      "gpuExecutionVerified",
      "fullApplicationVerified",
    ])
      for (const value of [true, undefined, "false"])
        assert.throws(
          () =>
            checkReceipt(
              createFreshAirReleaseFixture({
                mutateReceipt: (report) => {
                  report.verification[key] = value;
                },
              }),
            ),
          /verification scope/,
          `${key}:${value}`,
        );
    for (const mutate of [
      (probe) => {
        probe.exportsVerified = false;
      },
      (probe) => {
        probe.headerVerified = false;
      },
      (probe) => {
        probe.abiVersion = 1;
      },
      (probe) => {
        probe.headerBundleVersion = 1;
      },
      (probe) => {
        probe.headerSha256 = "a".repeat(64);
      },
      (probe) => {
        probe.smoke.passed = false;
      },
      (probe) => {
        probe.smoke.cases = [];
      },
      (probe) => {
        probe.smoke.cases = Array(33).fill({});
      },
    ])
      assert.throws(
        () =>
          checkReceipt(
            createFreshAirReleaseFixture({
              mutateReceipt: (report) => mutate(report.verification),
            }),
          ),
        /verification scope/,
      );
  });

  test("fresh metadata rejects historical receipt relabeling and unsupported full-game claims", () => {
    for (const [hook, check] of [
      ["mutateManifest", checkManifest],
      ["mutateReceipt", checkReceipt],
      ["mutateInputs", checkInputs],
      ["mutateLock", checkLock],
    ]) {
      for (const claim of [
        { historicalReceiptSha256: "a".repeat(64) },
        { originalBuildReceiptSha256: "a".repeat(64) },
        { migratedFromHistoricalReceipt: true },
        { fullGameCompilationVerified: true },
      ])
        assert.throws(
          () =>
            check(
              createFreshAirReleaseFixture({ [hook]: (report) => Object.assign(report, claim) }),
            ),
          `${hook}: ${Object.keys(claim)[0]}`,
        );
    }
  });

  test("source locks require exact compiler/SDK closure and independently recomputed header digests", () => {
    const fixture = createFreshAirReleaseFixture();
    assert.throws(
      () =>
        verifyFreshAirSourceLock(
          Buffer.concat([fixture.sourceLockBytes, Buffer.from(" ")]),
          fixture.manifest,
          fixture.inputs,
        ),
      /lock digest mismatch/,
    );
    for (const mutate of [
      (lock) => {
        delete lock.compilerFiles[Object.keys(lock.compilerFiles)[0]];
      },
      (lock) => {
        delete lock.sdkFiles[Object.keys(lock.sdkFiles)[0]];
      },
      (lock) => {
        lock.sdkFiles["unexpected"] = "a".repeat(64);
      },
      (lock) => {
        lock.compilerFiles[Object.keys(lock.compilerFiles)[0]] = "bad";
      },
      (lock) => {
        const key = Object.keys(lock.compilerFiles)[0];
        lock.compilerFiles["wasm/compiler.cpp"] = lock.compilerFiles[key];
        delete lock.compilerFiles[key];
      },
    ])
      assert.throws(
        () => checkLock(createFreshAirReleaseFixture({ mutateLock: mutate })),
        /source lock|input closure/,
      );
    const changedInputs = structuredClone(fixture.inputs);
    changedInputs.sourceInputs[Object.keys(changedInputs.sourceInputs)[0]] = "a".repeat(64);
    assert.throws(
      () => verifyFreshAirSourceLock(fixture.sourceLockBytes, fixture.manifest, changedInputs),
      /closure mismatch/,
    );
    assert.throws(
      () =>
        verifyFreshAirSourceLock(
          fixture.sourceLockBytes,
          { ...fixture.manifest, headerSha256: "a".repeat(64) },
          fixture.inputs,
        ),
      /header identity mismatch/,
    );
    assert.throws(
      () =>
        checkLock(
          createFreshAirReleaseFixture({
            mutateLock: (lock) => {
              lock.sdkFiles["api/not-a-header"] = lock.sdkFiles["api/include/metal_atomic"];
              delete lock.sdkFiles["api/include/metal_atomic"];
            },
          }),
        ),
      /header identity mismatch/,
    );
    const digestText =
      "floor-air-header-bundle-v2\n" +
      Object.entries(fixture.sourceLock.sdkFiles)
        .filter(([path]) => path.startsWith("api/include/"))
        .sort(([a], [b]) => a.localeCompare(b))
        .map(([path, digest]) => `${path.slice(12)}:${digest}\n`)
        .join("");
    assert.equal(airReleaseSha256(digestText), fixture.manifest.headerSha256);
  });

  test("producer top-level metadata rejects missing keys and unknown schema extensions", () => {
    for (const [name, hook, check] of [
      ["manifest", "mutateManifest", checkManifest],
      ["receipt", "mutateReceipt", checkReceipt],
      ["inputs", "mutateInputs", checkInputs],
    ]) {
      const fixture = createFreshAirReleaseFixture();
      for (const key of Object.keys(fixture[name]))
        assert.throws(
          () =>
            check(
              createFreshAirReleaseFixture({
                [hook]: (report) => {
                  delete report[key];
                },
              }),
            ),
          `${name} missing ${key}`,
        );
      assert.throws(
        () =>
          check(
            createFreshAirReleaseFixture({
              [hook]: (report) => {
                report.unrecognizedExtension = false;
              },
            }),
          ),
        name,
      );
    }
    const fixture = createFreshAirReleaseFixture();
    for (const key of [...Object.keys(fixture.sourceLock), "unrecognizedExtension"]) {
      const lock = structuredClone(fixture.sourceLock);
      if (key === "unrecognizedExtension") lock[key] = false;
      else delete lock[key];
      const bytes = fixtureJson(lock);
      assert.throws(
        () =>
          verifyFreshAirSourceLock(
            bytes,
            { ...fixture.manifest, sourceLockSha256: fixtureSha256(bytes) },
            fixture.inputs,
          ),
        /source lock/,
        key,
      );
    }
  });

  test("equivalent reordered output records, source inputs and build environments remain valid", () => {
    const reverse = (object) => Object.fromEntries(Object.entries(object).reverse());
    const fixture = createFreshAirReleaseFixture({
      mutateManifest: (report) => {
        for (const name of AIR_RELEASE_OUTPUTS)
          report.runtimeFiles[name] = reverse(report.runtimeFiles[name]);
      },
      mutateInputs: (report) => {
        report.sourceInputs = reverse(report.sourceInputs);
        report.buildTool = reverse(report.buildTool);
        report.buildTool.versions = reverse(report.buildTool.versions);
        report.buildTool.runner = reverse(report.buildTool.runner);
      },
      mutateReceipt: (report) => {
        report.files = reverse(report.files);
        for (const name of AIR_RELEASE_OUTPUTS) report.files[name] = reverse(report.files[name]);
      },
    });
    checkManifest(fixture);
    checkInputs(fixture);
    checkReceipt(fixture);
    checkLock(fixture);
  });

  test("build inputs and receipts require clean sources and exact SDK/archive/command disclosures", () => {
    for (const sourceTreeClean of [false, undefined, "true"])
      for (const [hook, check] of [
        ["mutateInputs", checkInputs],
        ["mutateReceipt", checkReceipt],
      ])
        assert.throws(
          () =>
            check(
              createFreshAirReleaseFixture({
                [hook]: (report) => {
                  report.sourceTreeClean = sourceTreeClean;
                },
              }),
            ),
          /build inputs|build receipt/,
        );
    for (const mutate of [
      (report) => {
        report.sourceDateEpoch = 0;
      },
      (report) => {
        report.sourceDateEpoch = 0.5;
      },
      (report) => {
        report.sourceDateEpoch = Number.MAX_SAFE_INTEGER + 1;
      },
      (report) => {
        report.sourceArchivePrefix = "../escape/";
      },
      (report) => {
        report.sourceArchivePrefix = "air-wasm-v9.0.0-source/";
      },
      (report) => {
        report.sdkSource.repository = "https://example.invalid/sdk";
      },
      (report) => {
        report.sdkSource.commit = "3".repeat(40);
      },
      (report) => {
        report.sdkSource.manifestSha256 = "bad";
      },
      (report) => {
        report.sdkSource.extra = true;
      },
      (report) => {
        delete report.sdkSource;
      },
    ])
      assert.throws(
        () => checkInputs(createFreshAirReleaseFixture({ mutateInputs: mutate })),
        /build inputs/,
      );
    for (const command of [
      [],
      ["node", null],
      Array.from({ length: 33 }, () => "argument"),
      "node build.mjs",
    ])
      assert.throws(
        () =>
          checkInputs(
            createFreshAirReleaseFixture({
              mutateInputs: (report) => {
                report.commands = [command];
              },
            }),
          ),
        /build commands/,
      );
    const fixture = createFreshAirReleaseFixture();
    for (const path of Object.keys(fixture.inputs.tooling))
      assert.throws(
        () =>
          checkInputs(
            createFreshAirReleaseFixture({
              mutateInputs: (report) => {
                delete report.tooling[path];
              },
            }),
          ),
        /tooling inputs/,
        path,
      );
  });

  test("lock build toolchain and upstream identity cannot conflict with build-input disclosure", () => {
    const fixture = createFreshAirReleaseFixture();
    for (const mutate of [
      (lock) => {
        delete lock.buildTool;
      },
      (lock) => {
        delete lock.buildTool.emsdkCommit;
      },
      (lock) => {
        lock.buildTool.emscriptenVersion = "0.0.0";
      },
      (lock) => {
        lock.buildTool.byteIdenticalRebuildClaimed = true;
      },
      (lock) => {
        lock.buildTool.jobs = 2;
      },
      (lock) => {
        lock.upstream.repository = "https://evil.invalid/project";
      },
      (lock) => {
        lock.upstream.commit = "main";
      },
      (lock) => {
        lock.upstream.archiveSha256 = "bad";
      },
      (lock) => {
        lock.upstream.originalBuildReceiptSha256 = "a".repeat(64);
      },
    ]) {
      const lock = structuredClone(fixture.sourceLock);
      mutate(lock);
      const bytes = fixtureJson(lock);
      assert.throws(
        () =>
          verifyFreshAirSourceLock(
            bytes,
            { ...fixture.manifest, sourceLockSha256: fixtureSha256(bytes) },
            fixture.inputs,
          ),
        /source lock/,
      );
    }
  });

  test("all three smoke records have strict shape, bounded outputs and expected rejection diagnostics", () => {
    for (const cases of [[null], [null, null, null], [true, true, true]])
      assert.throws(
        () =>
          checkReceipt(
            createFreshAirReleaseFixture({
              mutateReceipt: (report) => {
                report.verification.smoke.cases = cases;
              },
            }),
          ),
        /verification scope/,
      );
    for (const mutate of [
      (probe) => {
        probe.smoke.cases.reverse();
      },
      (probe) => {
        probe.smoke.cases[0].passed = false;
      },
      (probe) => {
        probe.smoke.cases[0].status = 3;
      },
      (probe) => {
        probe.smoke.cases[0].outputBytes = 4;
      },
      (probe) => {
        probe.smoke.cases[0].outputBytes = 64 * 1024 ** 2 + 1;
      },
      (probe) => {
        probe.smoke.cases[1].outputBytes = 1;
      },
      (probe) => {
        probe.smoke.cases[1].diagnosticBytes = 0;
      },
      (probe) => {
        probe.smoke.cases[2].diagnosticBytes = 64 * 1024 + 1;
      },
      (probe) => {
        probe.smoke.cases[2].diagnosticBytes = 0.5;
      },
      (probe) => {
        probe.smoke.cases[2].fullGameCompilationVerified = true;
      },
      (probe) => {
        probe.fullGameCompilationVerified = true;
      },
      (probe) => {
        probe.smoke.legacyReceipt = true;
      },
    ])
      assert.throws(
        () =>
          checkReceipt(
            createFreshAirReleaseFixture({
              mutateReceipt: (report) => mutate(report.verification),
            }),
          ),
        /verification scope/,
      );
  });

  test("runtime notice origins bind original repository paths and pinned artifact-only GPL text", () => {
    const f = createFreshAirReleaseFixture();
    assert.deepEqual(verifyFreshAirNotices(f.runtimeFiles, f.manifest), f.noticeOrigins);
    for (const mutate of [
      (r) => {
        r.schemaVersion = 2;
      },
      (r) => {
        r.repository = "other/not-the-source";
      },
      (r) => {
        r.commit = "f".repeat(40);
      },
      (r) => {
        r.tree = "e".repeat(40);
      },
      (r) => {
        r.historicalReceiptSha256 = "a".repeat(64);
      },
      (r) => {
        delete r.files["licenses/llvm/LICENSE.TXT"];
      },
      (r) => {
        r.files["licenses/SDK-MIT.txt"] = r.files["licenses/llvm/LICENSE.TXT"];
      },
      (r) => {
        r.files["licenses/llvm/LICENSE.TXT"].repositoryPath = "wasm/licenses/LLVM-LICENSE.txt";
      },
      (r) => {
        r.files["licenses/llvm/LICENSE.TXT"].sha256 = "0".repeat(64);
      },
      (r) => {
        r.files["licenses/GPL-2.0.txt"].sourceUrl = "https://untrusted.invalid/license";
      },
    ]) {
      const candidate = createFreshAirReleaseFixture();
      mutate(candidate.noticeOrigins);
      const bytes = fixtureJson(candidate.noticeOrigins);
      candidate.runtimeFiles.set("licenses/ORIGINS.json", bytes);
      candidate.manifest.runtimeFiles["licenses/ORIGINS.json"] = fixtureRecord(bytes);
      assert.throws(() => verifyFreshAirNotices(candidate.runtimeFiles, candidate.manifest));
    }
    const changed = createFreshAirReleaseFixture(),
      text = Buffer.from("Not the pinned GPL text");
    changed.runtimeFiles.set("licenses/GPL-2.0.txt", text);
    changed.manifest.runtimeFiles["licenses/GPL-2.0.txt"] = fixtureRecord(text);
    Object.assign(changed.noticeOrigins.files["licenses/GPL-2.0.txt"], fixtureRecord(text));
    const originBytes = fixtureJson(changed.noticeOrigins);
    changed.runtimeFiles.set("licenses/ORIGINS.json", originBytes);
    changed.manifest.runtimeFiles["licenses/ORIGINS.json"] = fixtureRecord(originBytes);
    assert.throws(
      () => verifyFreshAirNotices(changed.runtimeFiles, changed.manifest),
      /pinned official source/,
    );
  });

  test("committed release pin selects the authenticated build, not the fork default branch", async () => {
    const bytes = await readFile(
      new URL("../toolchain/metal-air-compiler/release-pin.json", import.meta.url),
    );
    assert.deepEqual(validateFreshAirReleasePin(parseAirReleaseJson(bytes, 4096)), {
      schemaVersion: 1,
      repository: "n3crosis/floor_llvm",
      commit: "1bd8db2f0fefbd13636e51c209ef09292675dff5",
      version: "v0.1.0",
      manifestSha256: "27b7682418911945a5908eaf0276d9a8afc6c2fe2e853119fa540fdfd95a08fc",
    });
  });
}

// air-release-loader
{
  // 验证 air-release-loader 的约定与边界；依据 ../runtime-build/air-release-transport.mjs、../runtime-build/air-release-loader.mjs 及本文件的测试断言。
  // Verify air-release-loader contracts and boundaries against ../runtime-build/air-release-transport.mjs、../runtime-build/air-release-loader.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { Buffer } = nodeBufferModule;
  const { gzipSync } = nodeZlibModule;
  const { mkdtemp, mkdir, writeFile, readFile, readdir, copyFile, rm, stat } = promisesModule;
  const { join } = nodePathModule;
  const { tmpdir } = nodeOsModule;
  const { pathToFileURL } = nodeUrlModule;
  const { downloadAirReleaseBytes } = airReleaseTransportModule;
  const { unpackFreshAirRuntime } = airReleaseLoaderModule;
  const { verifyFreshAirManifest } = airReleaseContractModule;
  const { createFreshAirReleaseFixture, fixtureJson, fixtureRecord, fixtureSha256 } = fixtureModule;
  const official = "https://github.com/test-only/not-a-release/releases/download/v0.1.0/asset";
  test("bounded release download accepts only official HTTPS redirects", async () => {
    const calls = [];
    const bytes = await downloadAirReleaseBytes(official, 8, async (url, options) => {
      calls.push(String(url));
      assert.equal(options.redirect, "manual");
      assert.equal(options.credentials, "omit");
      return calls.length === 1
        ? new Response(null, {
            status: 302,
            headers: { location: "https://release-assets.githubusercontent.com/test" },
          })
        : new Response("safe", { headers: { "content-length": "4" } });
    });
    assert.equal(bytes.toString(), "safe");
    assert.equal(calls.length, 2);
    for (const target of [
      "https://untrusted.invalid/a",
      "http://github.com/a",
      "https://github.com:444/a",
      "https://user@github.com/a",
      "file:///tmp/a",
    ])
      await assert.rejects(
        downloadAirReleaseBytes(
          official,
          8,
          async () => new Response(null, { status: 302, headers: { location: target } }),
        ),
        /official GitHub/,
      );
  });
  test("download rejects oversized declarations/streams, truncation, HTTP failure and redirect loops", async () => {
    for (const response of [
      () => new Response("small", { headers: { "content-length": "1000" } }),
      () => new Response("oversized"),
      () => new Response("tiny", { headers: { "content-length": "6" } }),
      () => new Response("error", { status: 500 }),
      () => new Response(null, { status: 302, headers: { location: official } }),
    ])
      await assert.rejects(downloadAirReleaseBytes(official, 8, async () => response()));
  });
  function archive(entries, { type = "0", link = "", tail = false, badPadding = false } = {}) {
    const blocks = [];
    for (const [name, data] of entries) {
      const header = Buffer.alloc(512),
        body = Buffer.alloc(Math.ceil(data.length / 512) * 512);
      header.write(name, 0, 100);
      header.write("0000644\0", 100);
      header.write("0000000\0", 108);
      header.write("0000000\0", 116);
      header.write(data.length.toString(8).padStart(11, "0") + "\0", 124);
      header.write("00000000000\0", 136);
      header.fill(32, 148, 156);
      header.write(type, 156);
      header.write(link, 157, 100);
      header.write("ustar\0", 257);
      header.write("00", 263);
      header.write(
        header
          .reduce((sum, byte) => sum + byte, 0)
          .toString(8)
          .padStart(6, "0") + "\0 ",
        148,
      );
      data.copy(body);
      if (badPadding && body.length > data.length) body[data.length] = 1;
      blocks.push(header, body);
    }
    return gzipSync(
      Buffer.concat([...blocks, Buffer.alloc(1024), ...(tail ? [Buffer.from("unsafe")] : [])]),
    );
  }
  function withArchive(options = {}) {
    const f = createFreshAirReleaseFixture(options);
    f.assetBytes.runtime = archive(f.runtimeFiles);
    f.manifest.assets.runtime = {
      name: f.manifest.assets.runtime.name,
      ...fixtureRecord(f.assetBytes.runtime),
    };
    f.manifestBytes = fixtureJson(f.manifest);
    f.pin.manifestSha256 = fixtureSha256(f.manifestBytes);
    return f;
  }
  test("complete inert generic runtime validates exact provenance/header/output closure without execution", () => {
    const f = withArchive();
    const release = verifyFreshAirManifest(f.manifestBytes, f.pin);
    const parsed = unpackFreshAirRuntime(f.assetBytes.runtime, release, f.inputs);
    assert.deepEqual(parsed.receipt, f.receipt);
    assert.deepEqual(parsed.sourceLock, f.sourceLock);
    assert.deepEqual([...parsed.files.keys()].sort(), [...f.runtimeFiles.keys()].sort());
    for (const [name, bytes] of f.runtimeFiles) assert.deepEqual(parsed.files.get(name), bytes);
  });
  test("runtime parser checks compressed digest before decoding", () => {
    const f = withArchive();
    assert.throws(
      () => unpackFreshAirRuntime(Buffer.from("not gzip"), f.manifest, f.inputs),
      /asset mismatch/,
    );
  });
  test("runtime parser rejects traversal, links, duplicates, unlisted entries, padding and trailing data", () => {
    const f = withArchive(),
      entries = [...f.runtimeFiles];
    for (const bytes of [
      archive([["../escape", Buffer.from("x")]]),
      archive([["/escape", Buffer.from("x")]]),
      archive([["a//b", Buffer.from("x")]]),
      archive(entries, { type: "2", link: "../escape" }),
      archive([...entries, entries[0]]),
      archive([...entries, ["unlisted", Buffer.from("x")]]),
      archive(entries.slice(1)),
      archive(entries, { tail: true }),
      archive(entries, { badPadding: true }),
    ]) {
      const release = structuredClone(f.manifest);
      release.assets.runtime = { name: release.assets.runtime.name, ...fixtureRecord(bytes) };
      assert.throws(() => unpackFreshAirRuntime(bytes, release, f.inputs));
    }
  });
  test("runtime parser rejects member substitution even with a correctly pinned outer archive", () => {
    const f = withArchive(),
      replaced = new Map(f.runtimeFiles);
    replaced.set("air_wasm.mjs", Buffer.from("x".repeat(replaced.get("air_wasm.mjs").length)));
    const bytes = archive(replaced),
      release = structuredClone(f.manifest);
    release.assets.runtime = { name: release.assets.runtime.name, ...fixtureRecord(bytes) };
    assert.throws(() => unpackFreshAirRuntime(bytes, release, f.inputs), /member digest mismatch/);
  });
  test("source closure precedes runtime verification and leaves destination bytes untouched", async () => {
    const root = await mkdtemp(join(tmpdir(), "fresh-air-release-")),
      oldFetch = globalThis.fetch;
    try {
      const f = withArchive(),
        web = join(root, "web"),
        compiler = join(web, "toolchain/metal-air-compiler"),
        build = join(web, "runtime-build"),
        cache = join(build, ".build/air-compiler-release", f.pin.manifestSha256);
      for (const directory of [join(compiler, "dist"), cache])
        await mkdir(directory, { recursive: true });
      for (const name of [
        "runtime-build/air-release-contract.mjs",
        "runtime-build/air-release-transport.mjs",
        "runtime-build/air-release-loader.mjs",
        "runtime-build/air-compiler-assets.mjs",
        "air-compiler-runtime-contract.mjs",
        "air-compiler-contract.mjs",
        "shader-diagnostics.mjs",
      ])
        await copyFile(new URL(`../${name}`, import.meta.url), join(web, name));
      await writeFile(join(compiler, "release-pin.json"), fixtureJson(f.pin));
      await writeFile(join(cache, "manifest.json"), f.manifestBytes);
      await writeFile(join(cache, f.manifest.assets.buildInputs.name), f.inputsBytes);
      await writeFile(join(cache, f.manifest.assets.runtime.name), f.assetBytes.runtime);
      const sentinel = join(compiler, "dist/untouched"),
        anchor = join(build, ".build/air-compiler-receipt.sha256");
      await writeFile(sentinel, "existing compiler bytes");
      await writeFile(anchor, "existing receipt trust anchor\n");
      const before = {
        data: await readFile(sentinel, "utf8"),
        anchor: await readFile(anchor, "utf8"),
        names: await readdir(join(compiler, "dist")),
        mtime: (await stat(sentinel)).mtimeMs,
      };
      const { loadFreshAirRelease } = await import(
        pathToFileURL(join(build, "air-release-loader.mjs"))
      );
      const { importAirCompilerAssets, verifyAirCompilerAssets } = await import(
        pathToFileURL(join(build, "air-compiler-assets.mjs"))
      );
      const input = join(root, "inert-input");
      await mkdir(input);
      const calls = [];
      globalThis.fetch = async (url) => {
        calls.push(String(url));
        throw new Error("OFFLINE source prerequisite missing");
      };
      await rm(join(compiler, "release-pin.json"));
      await assert.rejects(
        importAirCompilerAssets({
          directory: input,
          expectedDigest: f.manifest.buildReceiptSha256,
        }),
        /Fresh AIR release is not pinned/,
      );
      assert.equal(calls.length, 0, "missing pin must not download or install");
      await writeFile(join(compiler, "release-pin.json"), fixtureJson(f.pin));
      await assert.rejects(loadFreshAirRelease({ download: true }), /OFFLINE source prerequisite/);
      assert.deepEqual(calls, [
        `https://github.com/${f.pin.repository}/releases/download/${f.pin.version}/${f.manifest.assets.correspondingSource.name}`,
      ]);
      await assert.rejects(
        importAirCompilerAssets({
          directory: input,
          expectedDigest: f.manifest.buildReceiptSha256,
        }),
        /OFFLINE source prerequisite/,
      );
      const sourcePath = join(cache, f.manifest.assets.correspondingSource.name);
      await writeFile(sourcePath, Buffer.alloc(f.assetBytes.correspondingSource.length, 1));
      await assert.rejects(
        loadFreshAirRelease({ download: true }),
        /asset mismatch: correspondingSource/,
      );
      assert.equal(calls.length, 2, "corrupt source must not silently redownload");
      await assert.rejects(
        importAirCompilerAssets({
          directory: input,
          expectedDigest: f.manifest.buildReceiptSha256,
        }),
        /asset mismatch: correspondingSource/,
      );
      await writeFile(sourcePath, f.assetBytes.correspondingSource);
      const result = await loadFreshAirRelease();
      assert.deepEqual(result.receipt, f.receipt);
      assert.deepEqual(
        {
          data: await readFile(sentinel, "utf8"),
          anchor: await readFile(anchor, "utf8"),
          names: await readdir(join(compiler, "dist")),
          mtime: (await stat(sentinel)).mtimeMs,
        },
        before,
      );
      for (const [name, bytes] of f.runtimeFiles) {
        const local = ["air_wasm.mjs", "air_wasm.wasm"].includes(name) ? `d2r_${name}` : name;
        const path = join(input, local);
        await mkdir(join(path, ".."), { recursive: true });
        await writeFile(path, bytes);
      }
      const installed = await importAirCompilerAssets({
        directory: input,
        expectedDigest: f.manifest.buildReceiptSha256,
      });
      assert.deepEqual(installed, f.receipt);
      assert.deepEqual(await verifyAirCompilerAssets(), f.receipt);
      assert.deepEqual(
        await readFile(join(compiler, "dist/build-report.json")),
        f.receiptBytes,
        "raw generic receipt must never be rewritten",
      );
      for (const name of ["air_wasm.mjs", "air_wasm.wasm"])
        assert.deepEqual(
          await readFile(join(compiler, "dist", `d2r_${name}`)),
          f.runtimeFiles.get(name),
          "aliases preserve exact bytes",
        );
      await assert.rejects(stat(join(compiler, "dist/air_wasm.wasm")), { code: "ENOENT" });
    } finally {
      globalThis.fetch = oldFetch;
      await rm(root, { recursive: true, force: true });
    }
  });

  test("fork source transport pins exact commit and bounds inert bytes without redirects", async () => {
    const { downloadAirForkSourceBytes } =
      await import("../runtime-build/air-release-transport.mjs");
    const url = `https://raw.githubusercontent.com/test-only/not-a-release/${"a".repeat(40)}/wasm/api/air_compiler.h`;
    assert.equal(
      (
        await downloadAirForkSourceBytes(url, 8, async (_url, options) => {
          assert.equal(options.redirect, "error");
          assert.equal(options.credentials, "omit");
          return new Response("source");
        })
      ).toString(),
      "source",
    );
    for (const target of [
      url.replace("a".repeat(40), "main"),
      url.replace("raw.githubusercontent.com", "untrusted.invalid"),
      url.replace("https:", "http:"),
      url + "?auth=untrusted",
      url + "#fragment",
    ]) {
      let called = false;
      await assert.rejects(
        downloadAirForkSourceBytes(target, 8, async () => {
          called = true;
          return new Response("source");
        }),
      );
      assert.equal(called, false);
    }
    for (const response of [
      () => new Response("too-long-source"),
      () => new Response("x", { headers: { "content-length": "9" } }),
      () => new Response("x", { headers: { "content-length": "2" } }),
      () => new Response(null, { status: 302, headers: { location: url } }),
    ])
      await assert.rejects(downloadAirForkSourceBytes(url, 8, async () => response()));
  });
}

// air-scalar-i1-source
{
  // 验证 air-scalar-i1-source 的约定与边界；依据 ./fixtures/air/fixture.mjs 及本文件的测试断言。
  // Verify air-scalar-i1-source contracts and boundaries against ./fixtures/air/fixture.mjs and this file's assertions.
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { readFile } = promisesModule;
  const { default: test } = nodeTestModule;
  const { sourceReviewOptions, readForkSource } = fixtureModule;

  const root = new URL("../toolchain/metal-air-compiler/", import.meta.url);
  const read = (path) => readFile(new URL(path, root), "utf8");
  const hash = (data) => createHash("sha256").update(data).digest("hex");

  // 仅断言源码，不运行编译器、LLVM 解析/校验器或生成的着色器。
  // Source-only assertions: no compiler, LLVM parser/verifier or generated shader runs.
  test(
    "scalar i1 conversion hardening retains its exact authenticated fork source boundary",
    sourceReviewOptions,
    async () => {
      const source = await readForkSource("llvm/lib/Transforms/LibFloor/MetalFinal.cpp");
      assert.equal(
        hash(source),
        "b7f8c3d17df3fddea4e87cd8a6fc2ac6ed0d53531714f9403b73ab72f3c39973",
      );
      assert.equal(
        hash(await readForkSource("wasm/api/air_compiler.cpp")),
        "5d06207b69e57435934bec8e4e67e71c014d6c890fe2ff625e96cc1d4dc7a448",
      );
      assert.equal(
        hash(await read("ci-compile-canary.mjs")),
        "19415766142c77a9443478e74d5413a300397c28a1a0395b2f99d22d3547eeca",
      );
    },
  );

  test("release CI retains strict canary, failure evidence and complete source-artifact safeguards", async () => {
    const workflow = await readFile(
      new URL("../../.github/workflows/verify.yml", import.meta.url),
      "utf8",
    );
    const steps = workflow.split(/^ {6}- /m).slice(1);
    const step = (id) => {
      const matches = steps.filter((text) => text.includes(`\n        id: ${id}\n`));
      assert.equal(matches.length, 1, `exactly one ${id} step`);
      return matches[0];
    };
    const build = await readFile(
      new URL("../runtime-build/build-air-compiler.mjs", import.meta.url),
      "utf8",
    );
    assert(build.includes("compileCanary: await verifyAirCompilerCanary()"));
    assert(build.includes("await prepareAirCompilerAssets()"));
    assert(build.includes("await importPinnedAirCompiler()"));
    assert(
      build.indexOf("await prepareAirCompilerAssets()") <
        build.indexOf("compileCanary: await verifyAirCompilerCanary()"),
    );
    assert(
      build.indexOf("await importPinnedAirCompiler()") <
        build.indexOf("compileCanary: await verifyAirCompilerCanary()"),
    );
    const runtime = step("runtime");
    assert(runtime.includes("node web/runtime-build/build-air-compiler.mjs"));
    assert(
      runtime.indexOf("node web/runtime-build/build-air-compiler.mjs") <
        runtime.indexOf("air_compiler_verified=true"),
    );
    assert.doesNotMatch(runtime, /continue-on-error:\s*true/);
    const failureVerify = step("air_failure_verify");
    assert(failureVerify.includes("failure() && steps.runtime.outcome == 'failure'"));
    assert(failureVerify.includes("air-compiler-assets.mjs verify --source-fingerprint"));
    const failureSource = step("air_failure_source");
    assert(failureSource.includes("!cancelled() && steps.air_failure_verify.outcome == 'success'"));
    assert(failureSource.includes("prepare-air-compiler-source.mjs --output"));
    const failureUpload = step("air_failure_source_upload");
    assert(failureUpload.includes("!cancelled() && steps.air_failure_source.outcome == 'success'"));
    assert(failureUpload.includes("include-hidden-files: true"));
    assert(failureUpload.includes("if-no-files-found: error"));
    const bundle = step("air_failure_bundle");
    assert(bundle.includes("!cancelled() && steps.air_failure_source_upload.outcome == 'success'"));
    assert(bundle.includes("steps.air_failure_source_upload.outputs.artifact-id"));
    assert(bundle.includes("steps.air_failure_source.outputs.source_report_sha256"));
    assert(bundle.includes("prepare-air-compiler-diagnostic.mjs"));
    const diagnostic = steps.find((text) =>
      text.includes("name: air-compiler-diagnostic-unvalidated"),
    );
    assert(diagnostic?.includes("!cancelled() && steps.air_failure_bundle.outcome == 'success'"));
    assert(diagnostic.includes("if-no-files-found: error"));
    const completeSource = steps.find((text) => text.includes("name: air-compiler-source\n"));
    assert(completeSource?.includes("include-hidden-files: true"));
    assert(completeSource.includes("if-no-files-found: error"));
    assert.doesNotMatch(completeSource, /failure\(\)|always\(\)|!cancelled\(\)/);
    assert(
      workflow.includes(
        'prepare-air-compiler-source.mjs --output "$RUNNER_TEMP/air-compiler-source-artifact"',
      ),
    );
    const preview = steps.find((text) => text.includes("name: web-preview\n"));
    assert(preview?.includes("if-no-files-found: error"));
    assert.doesNotMatch(preview, /failure\(\)|always\(\)|!cancelled\(\)/);
    assert(
      workflow.indexOf("name: air-compiler-source\n") < workflow.indexOf("name: web-preview\n"),
    );
    assert(
      workflow.indexOf("node web/runtime-build/stage-preview.mjs") <
        workflow.indexOf("name: web-preview\n"),
    );
  });

  test("25 scalar i1 cases are frozen authored symbolic shapes, not compiler output", async () => {
    const files = {
      "fixtures/scalar-i1-conversions.ll":
        "38a92cd67dd7522aeab39b8af191a4e02253d193c097b27c8d4cfdb80f06e4e5",
      "fixtures/scalar-i1-conversions.expected.ll":
        "1c562583db1b9180d54706f8f67db369fba1eb1000ea6459e15f1671a1eac21a",
      "fixtures/scalar-i1-conversions.json":
        "06a74555c99c67bf8093f8f440909653343abc36bfb15f11bdbd18767f1f6613",
    };
    const sources = {};
    for (const [path, expected] of Object.entries(files)) {
      sources[path] = await read(path);
      assert.equal(hash(sources[path]), expected, path);
    }
    const input = sources["fixtures/scalar-i1-conversions.ll"];
    const expected = sources["fixtures/scalar-i1-conversions.expected.ll"];
    const cases = JSON.parse(sources["fixtures/scalar-i1-conversions.json"]);
    assert.equal(cases.kind, "hand-authored-symbolic-shapes");
    assert.equal(cases.compilerExecuted, false);
    assert.equal(cases.llvmVerified, false);
    const names = [
      "signed_half",
      "signed_float",
      "signed_double",
      "signed_true",
      "signed_false",
      "signed_poison",
      "signed_undef",
      "unsigned_half",
      "unsigned_float",
      "unsigned_double",
      "unsigned_true",
      "unsigned_false",
      "unsigned_poison",
      "unsigned_undef",
      "frozen_input",
      "adjacent_debug_multiuse",
      "vector_signed",
      "vector_unsigned",
      "vector_one_signed",
      "signed_i8",
      "unsigned_i64",
      "unsupported_bfloat",
      "unsupported_fp128",
      "reverse_signed",
      "reverse_unsigned",
    ];
    assert.deepEqual(
      cases.cases.map((entry) => entry.name),
      names,
    );
    for (const source of [input, expected]) {
      assert.deepEqual(
        [...source.matchAll(/^define .+ @([^ (]+)\(/gm)].map((match) => match[1]),
        names,
      );
      assert.doesNotMatch(source, /^; RUN:/m);
      assert(source.includes("Hand-authored symbolic fixtures, not captured compiler output"));
    }
    for (const type of ["half", "float", "double"])
      for (const sign of ["-", ""])
        assert(
          expected.includes(`select i1 %b, ${type} ${sign}1.000000e+00, ${type} 0.000000e+00`),
        );
    assert(expected.includes("ret float poison"));
    assert(expected.includes("freeze i1"));
    assert(!(await read("ci-compile-canary.mjs")).includes("scalar-i1-conversions"));
  });
}

// air-compiler-diagnostic
{
  const diagnosticBase = new URL("../runtime-build/", import.meta.url);
  // 验证 air-compiler-diagnostic 的约定与边界；依据 ./prepare-air-compiler-diagnostic.mjs 及本文件的测试断言。
  // Verify air-compiler-diagnostic contracts and boundaries against ./prepare-air-compiler-diagnostic.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { Buffer } = nodeBufferModule;
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { readFile } = promisesModule;
  const { validateDiagnosticSource } = diagnosticSourceModule;

  const sha = (bytes) => createHash("sha256").update(bytes).digest("hex");
  function fixture() {
    const compiler = {
      sourceFingerprintSha256: "a".repeat(64),
      headerSha256: "b".repeat(64),
      sourceLanguage: "floor-metal-3.2",
    };
    const receipt = Buffer.from(JSON.stringify(compiler));
    const source = {
      version: 1,
      sourceRevision: "c".repeat(40),
      sourceFingerprintSha256: compiler.sourceFingerprintSha256,
      headerSha256: compiler.headerSha256,
      sourceLanguage: compiler.sourceLanguage,
      reconstructedPatchedSourceVerified: true,
      llvmCompiledByThisStep: false,
      files: [{ path: "compiler-build-report.json", bytes: receipt.length, sha256: sha(receipt) }],
    };
    const sourceBytes = Buffer.from(JSON.stringify(source));
    return {
      compiler,
      receipt,
      source,
      sourceBytes,
      expected: {
        sourceReportSha256: sha(sourceBytes),
        sourceFingerprintSha256: compiler.sourceFingerprintSha256,
        sourceRevision: source.sourceRevision,
      },
    };
  }

  test("diagnostic metadata binds complete source and exact receipt without execution", () => {
    const value = fixture();
    assert.deepEqual(
      validateDiagnosticSource(value.compiler, value.receipt, value.sourceBytes, value.expected),
      value.source,
    );
  });

  for (const field of ["sourceReportSha256", "sourceFingerprintSha256", "sourceRevision"]) {
    test(`rejects independent ${field} drift`, () => {
      const value = fixture();
      value.expected[field] = "0".repeat(field === "sourceRevision" ? 40 : 64);
      assert.throws(() =>
        validateDiagnosticSource(value.compiler, value.receipt, value.sourceBytes, value.expected),
      );
    });
  }

  for (const field of [
    "version",
    "headerSha256",
    "sourceLanguage",
    "reconstructedPatchedSourceVerified",
    "llvmCompiledByThisStep",
  ]) {
    test(`rejects source ${field} drift even with a matching report hash`, () => {
      const value = fixture();
      value.source[field] = null;
      value.sourceBytes = Buffer.from(JSON.stringify(value.source));
      value.expected.sourceReportSha256 = sha(value.sourceBytes);
      assert.throws(() =>
        validateDiagnosticSource(value.compiler, value.receipt, value.sourceBytes, value.expected),
      );
    });
  }

  test("rejects a source archive bound to different compiler receipt bytes", () => {
    const value = fixture();
    value.source.files[0].sha256 = "d".repeat(64);
    value.sourceBytes = Buffer.from(JSON.stringify(value.source));
    value.expected.sourceReportSha256 = sha(value.sourceBytes);
    assert.throws(() =>
      validateDiagnosticSource(value.compiler, value.receipt, value.sourceBytes, value.expected),
    );
  });

  test("retention stays failure-only and keeps source upload before diagnostic binary", async () => {
    const workflow = await readFile(
      new URL("../../.github/workflows/verify.yml", diagnosticBase),
      "utf8",
    );
    assert.match(workflow, /failure\(\) && steps\.runtime\.outcome == 'failure'/);
    assert.match(workflow, /actions\/cache\/save@55cc8345863c7cc4c66a329aec7e433d2d1c52a9/);
    assert.doesNotMatch(workflow, /save-always:|continue-on-error:/);
    assert.ok(
      workflow.indexOf("name: air-compiler-diagnostic-source") <
        workflow.indexOf("name: air-compiler-diagnostic-unvalidated"),
    );
    assert.match(workflow, /steps\.air_failure_source_upload\.outcome == 'success'/);
    const build = await readFile(new URL("build-air-compiler.mjs", diagnosticBase), "utf8");
    assert.match(build, /compileCanary: await verifyAirCompilerCanary\(\)/);
  });

  test("fresh diagnostic source requires exact fork, lock and complete SDK-inclusive source mirror", async () => {
    const { createFreshAirReleaseFixture, fixtureJson, fixtureRecord } =
      await import("./fixtures/air/fixture.mjs");
    const f = createFreshAirReleaseFixture(),
      sourceName = `air-wasm-${f.pin.version}-source.tar.gz`;
    const source = {
      version: 2,
      kind: "pinned-release-source-mirror",
      sourceRevision: "c".repeat(40),
      sourceFingerprintSha256: f.receipt.sourceFingerprintSha256,
      headerSha256: f.receipt.headerSha256,
      sourceLanguage: f.receipt.sourceLanguage,
      completeReleaseSourceVerified: true,
      installedSdkRuntimeSourcesIncluded: true,
      llvmCompiledByThisStep: false,
      binaryBuiltFromNewFork: true,
      release: f.pin,
      correspondingSource: { name: sourceName, ...fixtureRecord(f.correspondingSourceBytes) },
      files: [
        { path: "compiler-build-report.json", ...fixtureRecord(f.receiptBytes) },
        { path: "source-lock.json", ...fixtureRecord(f.sourceLockBytes) },
        { path: sourceName, ...fixtureRecord(f.correspondingSourceBytes) },
      ],
    };
    function check(value) {
      const bytes = fixtureJson(value);
      return validateDiagnosticSource(f.receipt, f.receiptBytes, bytes, {
        sourceReportSha256: sha(bytes),
        sourceFingerprintSha256: f.receipt.sourceFingerprintSha256,
        sourceRevision: source.sourceRevision,
        releaseManifestSha256: f.pin.manifestSha256,
      });
    }
    assert.deepEqual(check(source), source);
    const bytes = fixtureJson(source);
    for (const releaseManifestSha256 of [undefined, "0".repeat(64), "bad"])
      assert.throws(() =>
        validateDiagnosticSource(f.receipt, f.receiptBytes, bytes, {
          sourceReportSha256: sha(bytes),
          sourceFingerprintSha256: f.receipt.sourceFingerprintSha256,
          sourceRevision: source.sourceRevision,
          releaseManifestSha256,
        }),
      );
    for (const mutate of [
      (v) => (v.version = 1),
      (v) => (v.kind = "historical"),
      (v) => (v.completeReleaseSourceVerified = false),
      (v) => (v.installedSdkRuntimeSourcesIncluded = false),
      (v) => (v.binaryBuiltFromNewFork = false),
      (v) => (v.release.commit = "0".repeat(40)),
      (v) => (v.release.repository = "wrong/repo"),
      (v) => (v.release.version = "v9.9.9"),
      (v) => (v.release.manifestSha256 = "bad"),
      (v) => (v.release.manifestSha256 = "0".repeat(64)),
      (v) => (v.files[1].sha256 = "0".repeat(64)),
      (v) => (v.correspondingSource.sha256 = "0".repeat(64)),
      (v) => (v.correspondingSource.name = "partial-source.zip"),
    ]) {
      const invalid = structuredClone(source);
      mutate(invalid);
      assert.throws(() => check(invalid));
    }
  });
}

// 保证自编数值用例的打包不改变输入；不执行或声称验证实际编译器。
// Validate authored numeric fixture packaging, without executing or claiming compiler validation.
{
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { mkdtemp, readFile, rm } = promisesModule;
  const { join, basename } = nodePathModule;
  const { tmpdir } = nodeOsModule;
  const { createHash } = nodeCryptoModule;
  test("numeric fixture bundle materializes all original filenames and exact pinned source bytes", async () => {
    const { numericFixtures, materializeNumericFixture } =
      await import("../toolchain/metal-air-compiler/fixtures/numeric-cases.mjs");
    assert.deepEqual(Object.keys(numericFixtures), [
      "math-scalar",
      "math-vector",
      "integer-preservation",
      "nan-select",
    ]);
    const output = await mkdtemp(join(tmpdir(), "air-numeric-fixtures-"));
    try {
      for (const [name, fixture] of Object.entries(numericFixtures)) {
        const path = materializeNumericFixture(name, output);
        assert.equal(basename(path), `${name}.metal`);
        const bytes = await readFile(path);
        assert.equal(bytes.toString("utf8"), fixture.source);
        assert.equal(createHash("sha256").update(bytes).digest("hex"), fixture.sha256);
      }
      for (const invalid of ["../escape", "toString", "constructor", "missing"])
        assert.throws(() => materializeNumericFixture(invalid, output), /Unknown numeric fixture/);
    } finally {
      await rm(output, { recursive: true, force: true });
    }
  });
}
