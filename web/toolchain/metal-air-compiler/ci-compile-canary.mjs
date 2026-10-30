// CI 中仅编译固定的原创着色器，覆盖三个阶段；不读取游戏、密钥或用户文件。
// CI-only authored shaders and bounded LLVM-text regressions, never game, key or user inputs.
import { readFile } from "node:fs/promises";
import { createHash } from "node:crypto";
import assert from "node:assert/strict";
import { createAirCompilerAdapter } from "../../air-compiler-adapter.mjs";
import { inspectCompiledMetalLibrary } from "../../compiled-shader-boundary.mjs";
import { AIR_COMPILER_SOURCE_LANGUAGE } from "../../air-compiler-contract.mjs";
import { checkReflection, checkFloat3Storage } from "./check-buffer-reflection.mjs";
import { checkFragmentInputLiveness } from "./check-fragment-input-liveness.mjs";
import { checkFragmentInputReflection } from "./check-fragment-input-reflection.mjs";

const livenessVariants = Object.freeze([
  { name: "dead", keepVarying: false, keepPosition: false },
  { name: "live", keepVarying: true, keepPosition: true },
  { name: "varying-dead", keepVarying: false, keepPosition: true },
  { name: "position-dead", keepVarying: true, keepPosition: false },
]);

const cases = Object.freeze([
  { stage: "vertex", filename: "msl-vertex.metal", define: "#define SCALE 2.0f\n" },
  { stage: "fragment", filename: "msl-fragment.metal", define: "#define THRESHOLD 0.5f\n" },
  { stage: "kernel", filename: "msl-compute.metal", define: "#define FACTOR 3\n" },
  { stage: "kernel", filename: "msl-scalar-operations.metal", define: "" },
  { stage: "fragment", filename: "msl-fragment-atomic-add.metal", define: "" },
  { stage: "fragment", filename: "msl-internal-helper-function-constant.metal", define: "" },
  { stage: "kernel", filename: "msl-optimizer-bit-reversal.metal", define: "" },
  {
    stage: "vertex",
    filename: "msl-patch-entry-name.metal",
    entry: "patch_linkage_probe",
    define: "",
  },
  {
    stage: "fragment",
    filename: "constant-vector-pointer-reads.metal",
    entry: "constant_vector_pointer_reads",
    define: "",
  },
  {
    stage: "fragment",
    filename: "direct-buffer-vector-pointer.metal",
    entry: "direct_buffer_vector_pointer",
    define: "",
  },
  {
    stage: "fragment",
    filename: "conditional-distinct-buffer-rows.metal",
    entry: "conditional_distinct_buffer_rows",
    define: "",
  },
  {
    stage: "fragment",
    filename: "loop-carried-buffer-pointer.metal",
    entry: "loop_carried_buffer_pointer",
    define: "",
  },
  {
    stage: "fragment",
    filename: "retyped-buffer-pointer-load.metal",
    entry: "retyped_buffer_pointer_load",
    sha256: "57f26a87e4b72aab90afb09caf9f650ddac7d7f025fd29bdfbabce9c46fa536e",
    define: "",
  },
  {
    stage: "kernel",
    filename: "msl-stage-record-threadgroup.metal",
    entry: "stage_record_threadgroup",
    define: "",
  },
  {
    stage: "kernel",
    filename: "msl-stage-record-buffers.metal",
    entry: "stage_record_buffers",
    define: "",
  },
  {
    stage: "kernel",
    filename: "msl-buffer-reflection-mixed.metal",
    entry: "reflection_mixed",
    define: "",
  },
  {
    stage: "kernel",
    filename: "msl-buffer-reflection-array.metal",
    entry: "reflection_array",
    define: "",
  },
  {
    stage: "kernel",
    filename: "msl-buffer-reflection-float3.metal",
    entry: "reflection_float3",
    define: "",
  },
  {
    stage: "kernel",
    filename: "msl-buffer-reflection-nested.metal",
    entry: "reflection_nested",
    define: "",
  },
]);

// The caller authenticates the actual module and passes its independently computed
// fixed-header digest. This probes the real frontend, passes, writer and boundary,
// not Apple acceptance, Chrome execution or full-game compatibility.
export async function runAirCompilerCanary(module, { expectedHeaderSha256 } = {}) {
  const reflectionFixtures = JSON.parse(
    await readFile(new URL("./fixtures/buffer-reflection.json", import.meta.url), "utf8"),
  );
  const adapter = createAirCompilerAdapter(module, {
    expectedHeaderSha256,
    apiVersion: 2,
    maxCompilations: cases.length + reflectionFixtures.length + livenessVariants.length * 2,
  });
  const results = [];
  const llvmValidation = [];
  const fragmentInputLiveness = [];
  try {
    for (const fixture of cases) {
      const text = await readFile(
        new URL(`./fixtures/${fixture.filename}`, import.meta.url),
        "utf8",
      );
      const source = fixture.define + text;
      const sourceSha256 = createHash("sha256").update(source).digest("hex");
      if (fixture.sha256)
        assert.equal(sourceSha256, fixture.sha256, "reviewed pointer-slot fixture identity");
      let output;
      let irOutput;
      try {
        output = adapter.compile(source, {
          iosMajor: 18,
          output: "metallib",
          sourceLanguage: AIR_COMPILER_SOURCE_LANGUAGE,
          mathProfile: "fast-f32",
        });
        await inspectCompiledMetalLibrary(output.bytes, {
          entry: fixture.entry ?? "main0",
          stage: fixture.stage,
          minimumOS: [18, 0],
          languageVersion: [3, 2],
          metadataLengthIncludesPrefix: false,
        });
        const reflection = reflectionFixtures.find(
          (candidate) => candidate.file === `fixtures/${fixture.filename}`,
        );
        if (reflection) {
          assert.equal(sourceSha256, reflection.sha256, "reviewed reflection fixture identity");
          irOutput = adapter.compile(source, {
            iosMajor: 18,
            output: "llvm-text",
            sourceLanguage: AIR_COMPILER_SOURCE_LANGUAGE,
            mathProfile: "fast-f32",
          });
          const ir = new TextDecoder("utf-8", { fatal: true }).decode(irOutput.bytes);
          checkReflection(ir, reflection);
          if (fixture.entry === "reflection_float3") checkFloat3Storage(ir);
          llvmValidation.push({
            fixture: fixture.filename,
            reflection: "exact-buffer-offsets-sizes-alignment-and-bindings",
            ...(fixture.entry === "reflection_float3"
              ? { storage: "80-byte-type-16-byte-float3-stride-and-exact-source-geps" }
              : {}),
          });
        }
        results.push({
          fixture: fixture.filename,
          entry: fixture.entry ?? "main0",
          stage: fixture.stage,
          sourceSha256,
          bytes: output.bytes.byteLength,
        });
      } catch (error) {
        throw new Error(`AIR CI compile canary failed for ${fixture.stage}/${fixture.filename}`, {
          cause: error,
        });
      } finally {
        output?.bytes.fill(0);
        irOutput?.bytes.fill(0);
      }
    }
    assert.equal(llvmValidation.length, reflectionFixtures.length, "all reflection probes ran");
    const livenessFilename = "msl-fragment-input-liveness.metal";
    const livenessText = await readFile(
      new URL(`./fixtures/${livenessFilename}`, import.meta.url),
      "utf8",
    );
    assert.equal(
      createHash("sha256").update(livenessText).digest("hex"),
      "0820f23da050fa12a4031a45c09dec58a4394f7179032570c1acd85a20d434d5",
      "reviewed liveness fixture identity",
    );
    for (const { name, keepVarying, keepPosition } of livenessVariants) {
      const source = `#define LIVENESS_KEEP_VARYING ${Number(keepVarying)}\n#define LIVENESS_KEEP_POSITION ${Number(keepPosition)}\n${livenessText}`;
      let output;
      let irOutput;
      try {
        output = adapter.compile(source, {
          iosMajor: 18,
          output: "metallib",
          sourceLanguage: AIR_COMPILER_SOURCE_LANGUAGE,
          mathProfile: "fast-f32",
        });
        await inspectCompiledMetalLibrary(output.bytes, {
          entry: "fragment_input_liveness",
          stage: "fragment",
          minimumOS: [18, 0],
          languageVersion: [3, 2],
          metadataLengthIncludesPrefix: false,
        });
        irOutput = adapter.compile(source, {
          iosMajor: 18,
          output: "llvm-text",
          sourceLanguage: AIR_COMPILER_SOURCE_LANGUAGE,
          mathProfile: "fast-f32",
        });
        const ir = new TextDecoder("utf-8", { fatal: true }).decode(irOutput.bytes);
        checkFragmentInputLiveness(ir, { keepVarying, keepPosition });
        checkFragmentInputReflection(output.bytes, { keepVarying, keepPosition });
        fragmentInputLiveness.push({
          fixture: livenessFilename,
          variant: name,
          sourceSha256: createHash("sha256").update(source).digest("hex"),
          bytes: output.bytes.byteLength,
          unusedInputs: Number(!keepVarying) + Number(!keepPosition),
          validation: "exact-api-input-descriptors-final-argument-uses-and-binary-reflection",
        });
      } catch (error) {
        throw new Error(`AIR CI fragment input liveness failed for ${name} variant`, {
          cause: error,
        });
      } finally {
        output?.bytes.fill(0);
        irOutput?.bytes.fill(0);
      }
    }
    assert.equal(
      adapter.compilationCount,
      cases.length + reflectionFixtures.length + livenessVariants.length * 2,
    );
    return {
      validation: "authored-nineteen-fixture-three-stage-compilation-and-container-integrity",
      results,
      llvmValidation,
      fragmentInputLiveness,
    };
  } finally {
    adapter.dispose();
  }
}
