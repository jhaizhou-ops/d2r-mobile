// 校验来宾程序适配器的边界约定；依据 web/toolchain/program-rewrite-spike 的模块与调用约定。
// Verify the guest-program adapter boundary contract using the module and calling contracts in web/toolchain/program-rewrite-spike.
import { Buffer } from "node:buffer";
/* Node/浏览器一致性辅助工具；仅配合本地合成夹具服务器调用。
 * Node/browser parity helper; called only with a local synthetic fixture server. */
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
export async function verifyGuestAdapter(page) {
  const results = await page.evaluate(async () => {
    const { createProgramRewriterAdapter } = await import("/program-rewriter-adapter.mjs");
    const catalog = await (await fetch("/fixtures/trusted-synthetic-catalog.json")).json();
    const results = [];
    let created = 0,
      closed = 0;
    const createWorker = () => {
      created++;
      const worker = new Worker("/worker.mjs", { type: "module" });
      let pending, timer;
      return {
        run(request) {
          return new Promise((resolve, reject) => {
            pending = reject;
            timer = setTimeout(() => reject(new Error("Synthetic worker timeout")), 120000);
            worker.onmessage = ({ data }) => {
              clearTimeout(timer);
              pending = undefined;
              resolve(data);
            };
            worker.onerror = () => {
              clearTimeout(timer);
              pending = undefined;
              reject(new Error("Synthetic worker failure"));
            };
            worker.postMessage({ id: "adapter", ...request });
          });
        },
        close() {
          closed++;
          clearTimeout(timer);
          worker.terminate();
          pending?.(new Error("Synthetic worker closed"));
          pending = undefined;
        },
      };
    };
    for (const version of catalog.versions) {
      const bytes = new Uint8Array(
        await (await fetch(`/fixtures/${version.id}.nso`)).arrayBuffer(),
      );
      const adapter = createProgramRewriterAdapter({
        trustedCatalog: catalog,
        createWorker,
        resolveInput: () => ({ versionId: version.id, bytes }),
      });
      const entries = await adapter.generateProgram({ extracted: {}, selection: {} });
      const output = {};
      for (const entry of entries) {
        const chunks = [];
        for await (const chunk of entry.chunks()) chunks.push(...chunk);
        output[entry.path] = { bytes: chunks, mode: entry.mode, signature: entry.signature };
      }
      await adapter.dispose();
      results.push({ id: version.id, output });
    }
    const expectCode = async (action, expected) => {
      try {
        await action();
      } catch (error) {
        if (error.code === expected) return;
        throw error;
      }
      throw new Error(`Expected ${expected}`);
    };
    await expectCode(
      () =>
        createProgramRewriterAdapter({
          trustedCatalog: { ...catalog, scope: "production" },
          createWorker,
          resolveInput: () => {},
        }),
      "PROGRAM_TRUSTED_CATALOG_REQUIRED",
    );
    const mutated = new Uint8Array(await (await fetch("/fixtures/gates.nso")).arrayBuffer());
    mutated[0x50] ^= 1;
    const invalid = createProgramRewriterAdapter({
      trustedCatalog: catalog,
      createWorker,
      resolveInput: () => ({ versionId: "gates", bytes: mutated }),
    });
    await expectCode(() => invalid.generateProgram(), "PROGRAM_INPUT_DIGEST_MISMATCH");
    await invalid.dispose();
    const validBytes = new Uint8Array(await (await fetch("/fixtures/gates.nso")).arrayBuffer());
    const multiCatalog = await (await fetch("/fixtures/trusted-multi-catalog.json")).json();
    const sdkBytes = new Uint8Array(await (await fetch("/fixtures/sdk-gates.nso")).arrayBuffer());
    const multi = createProgramRewriterAdapter({
      trustedCatalog: multiCatalog,
      createWorker,
      resolveInput: () => ({
        versionId: "main-sdk",
        programModules: { main: validBytes, sdk: sdkBytes },
      }),
    });
    const multiEntries = await multi.generateProgram({ extracted: {}, selection: {} });
    const multiOutput = {};
    for (const entry of multiEntries) {
      const chunks = [];
      for await (const chunk of entry.chunks()) chunks.push(...chunk);
      multiOutput[entry.path] = { bytes: chunks, mode: entry.mode, signature: entry.signature };
    }
    await multi.dispose();
    results.push({ id: "main-sdk", output: multiOutput });
    for (const [programModules, error] of [
      [{ main: validBytes }, "PROGRAM_INVALID_MODULE_SET"],
      [{ main: validBytes, sdk: sdkBytes, extra: sdkBytes }, "PROGRAM_INVALID_MODULE_SET"],
      [{ main: sdkBytes, sdk: validBytes }, "PROGRAM_INPUT_DIGEST_MISMATCH"],
    ]) {
      const rejected = createProgramRewriterAdapter({
        trustedCatalog: multiCatalog,
        createWorker,
        resolveInput: () => ({ versionId: "main-sdk", programModules }),
      });
      await expectCode(() => rejected.generateProgram(), error);
      await rejected.dispose();
    }
    // 注册的路径必须同时使用固定的仓库目录和注入的提取来源验证器；即使已认证的合成字节也无法通过摘要校验。
    // Registered route must use the pinned repository catalog AND an injected
    // extraction provenance verifier. Even authenticated synthetic bytes fail hashes.
    await expectCode(
      () => createProgramRewriterAdapter({ createWorker }),
      "PROGRAM_RUNTIME_REQUIRED",
    );
    const realRejected = createProgramRewriterAdapter({
      createWorker,
      verifyProvenance: () => ({ main: validBytes, sdk: sdkBytes }),
    });
    await expectCode(() => realRejected.generateProgram(), "PROGRAM_UNSUPPORTED_VERSION");
    await realRejected.dispose();
    const provenanceRejected = createProgramRewriterAdapter({
      createWorker,
      verifyProvenance: () => {
        throw new Error("untrusted extraction object");
      },
    });
    await expectCode(() => provenanceRejected.generateProgram(), "PROGRAM_GENERATION_FAILED");
    await provenanceRejected.dispose();
    const tamperedOutput = createProgramRewriterAdapter({
      trustedCatalog: catalog,
      resolveInput: () => ({ versionId: "gates", bytes: validBytes }),
      createWorker: () => {
        const base = createWorker();
        return {
          close: () => base.close(),
          async run(request) {
            const result = await base.run(request);
            result.files["main.patched.bin"][0] ^= 1;
            return result;
          },
        };
      },
    });
    await expectCode(() => tamperedOutput.generateProgram(), "PROGRAM_OUTPUT_DIGEST_MISMATCH");
    await tamperedOutput.dispose();
    const abortController = new AbortController();
    const cancelled = createProgramRewriterAdapter({
      trustedCatalog: catalog,
      createWorker,
      resolveInput: () => new Promise(() => {}),
    });
    const cancelledRun = cancelled.generateProgram({ signal: abortController.signal });
    abortController.abort();
    await expectCode(() => cancelledRun, "PROGRAM_CANCELLED");
    await cancelled.dispose();
    if (created !== catalog.versions.length + 2 || closed !== created)
      throw new Error("Worker lifecycle or early rejection failed");
    return results;
  });
  for (const result of results) {
    const guest = result.output["Frameworks/Guest.framework/Guest"];
    assert.equal(guest.mode, 0o755);
    assert.equal(guest.signature.kind, "ad-hoc");
    assert.deepEqual(
      Buffer.from(guest.bytes),
      await readFile(new URL(`./oracle-guest/${result.id}/Guest.framework/Guest`, import.meta.url)),
    );
    assert.deepEqual(
      Buffer.from(result.output["Frameworks/Guest.framework/Info.plist"].bytes),
      await readFile(
        new URL(`./oracle-guest/${result.id}/Guest.framework/Info.plist`, import.meta.url),
      ),
    );
    for (const suffix of ["fixups", "gates", "cuts"])
      assert.deepEqual(
        Buffer.from(result.output[`dm/main.${suffix}.tsv`].bytes),
        await readFile(new URL(`./oracle/${result.id}/main.${suffix}.tsv`, import.meta.url)),
      );
  }
  const multi = results.find((entry) => entry.id === "main-sdk");
  for (const suffix of ["fixups", "gates"])
    assert.deepEqual(
      Buffer.from(multi.output[`dm/sdk.${suffix}.tsv`].bytes),
      await readFile(new URL(`./oracle/main-sdk/sdk.${suffix}.tsv`, import.meta.url)),
    );
  return {
    syntheticOnly: true,
    multiModuleNativeParity: true,
    registeredFixtureRejected: true,
    provenanceRequired: true,
    missingAndSwappedModulesRejected: true,
    guestNativeParityCases: results.length,
    trustedCatalogRequired: true,
    corruptedWorkerOutputRejected: true,
    inputWaitCancellation: true,
    changedInputRejectedBeforeWorker: true,
  };
}
