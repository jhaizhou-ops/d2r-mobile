// Feature suite: input-extraction. Original test sections retain independent lexical scopes.
import * as nodeTestModule from "node:test";
import * as strictModule from "node:assert/strict";
import * as gameInputModule from "../game-input.mjs";
import * as nspInputModule from "../nsp-input.mjs";
import * as nodeCryptoModule from "node:crypto";
import * as xciInputModule from "../xci-input.mjs";
import * as romfsTreeModule from "../romfs-tree.mjs";
import * as nsoValidationModule from "../nso-validation.mjs";
import * as preflightModule from "../preflight.mjs";
import * as nodeFsModule from "node:fs";
import * as nodeChild_processModule from "node:child_process";
import * as assertIsolatedRunModule from "./helpers/assert-isolated-run.mjs";
import * as extractedInputModule from "../extracted-input.mjs";
import * as extractionWorkerModule from "./helpers/extraction-adapter-fixture.mjs";
import * as extractionJobAdapterModule from "../extraction-job-adapter.mjs";
import * as d2rExpansionPolicyModule from "../d2r-expansion-policy.mjs";
import * as extractionAdapterFixtureModule from "./helpers/extraction-adapter-fixture.mjs";
import * as extractionWorkerClientModule from "../extraction-worker-client.mjs";

// game-input
{
  // 验证 game-input 的约定与边界；依据 ../game-input.mjs、../nsp-input.mjs 及本文件的测试断言。
  // Verify game-input contracts and boundaries against ../game-input.mjs、../nsp-input.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { discoverGameFolder, discoverGameFiles } = gameInputModule;
  const { NSP_INPUT_LIMITS } = nspInputModule;
  const blob = (name, value = "synthetic", relative = `root/${name}`) =>
    Object.assign(new Blob([value]), { name, webkitRelativePath: relative });
  function directory(entries) {
    return {
      kind: "directory",
      async *entries() {
        yield* entries;
      },
    };
  }
  const file = (value, reads = []) => ({
    kind: "file",
    async getFile() {
      reads.push(value.name);
      return value;
    },
  });
  function extracted({
    exefsName = "ExeFS",
    romfsName = "RomFS",
    files = ["main", "sdk"],
    reads = [],
  } = {}) {
    return directory([
      [exefsName, directory(files.map((name) => [name, file(blob(name), reads)]))],
      [
        romfsName,
        directory([
          ["data", directory([["build_manifest.psv", file(blob("build_manifest.psv"), reads)]])],
        ]),
      ],
      [
        "unrelated",
        {
          kind: "directory",
          entries() {
            throw Error("Unrelated directory must not be enumerated");
          },
        },
      ],
      [
        "prod.keys",
        {
          kind: "file",
          getFile() {
            throw Error("Keys must not be opened in extracted mode");
          },
        },
      ],
    ]);
  }
  test("package folder and FileList discovery accept NSP plus XCI and require prod.keys", async () => {
    const root = directory(
      ["base.xci", "update.nsp", "prod.keys"].map((name) => [name, file(blob(name))]),
    );
    const progress = [];
    const found = await discoverGameFolder(root, { onProgress: (event) => progress.push(event) });
    assert.equal(found.inputMode, "packages");
    assert.deepEqual(
      found.nsps.map((f) => f.name),
      ["base.xci", "update.nsp"],
    );
    assert.equal(progress.at(-1).nspCount, 2);
    assert.deepEqual(
      discoverGameFiles(["base.xci", "update.nsp", "prod.keys"].map((name) => blob(name))).nsps.map(
        (f) => f.name,
      ),
      ["base.xci", "update.nsp"],
    );
    await assert.rejects(discoverGameFolder(directory([["base.xci", file(blob("base.xci"))]])), {
      code: "MISSING_INPUT",
    });
    assert.throws(() => discoverGameFiles([blob("base.xci")]), { code: "MISSING_INPUT" });
  });
  test("extracted discovery reads metadata only, includes bounded file references and ignores keys", async () => {
    const reads = [],
      progress = [];
    const found = await discoverGameFolder(extracted({ reads }), {
      onProgress: (event) => progress.push(event),
    });
    assert.equal(found.inputMode, "extracted");
    assert.deepEqual(found.nsps, []);
    assert.deepEqual(
      found.exefs.map((f) => f.path),
      ["main", "sdk"],
    );
    assert.deepEqual(
      found.romfs.map((f) => f.path),
      ["data/build_manifest.psv"],
    );
    assert.equal(progress.at(-1).extractedFileCount, 3);
    assert.equal(reads.length, 3);
    assert.equal(Object.hasOwn(found, "prodKeys"), false);
  });
  test("automatic discovery prefers root packages and never falls back after a package error", async () => {
    const extractedFiles = ["ExeFS/main", "ExeFS/sdk", "RomFS/data/build_manifest.psv"].map(
      (path) => blob(path.split("/").at(-1), "synthetic", `root/${path}`),
    );
    const untouched = {
      kind: "directory",
      entries() {
        throw Error("Extracted directories must not be opened when a package is present");
      },
    };
    for (const name of ["base.NSP", "base.XCI"]) {
      for (const key of [
        undefined,
        blob("prod.keys"),
        blob("prod.keys", new Uint8Array(NSP_INPUT_LIMITS.keyBytes + 1)),
      ]) {
        const files = [blob(name), ...(key ? [key] : [])];
        const root = directory([
          ["ExeFS", untouched],
          ["RomFS", untouched],
          ...files.map((value) => [value.name, file(value)]),
        ]);
        const fallback = [...files, ...extractedFiles];
        if (!key || key.size > NSP_INPUT_LIMITS.keyBytes) {
          const code = key ? "KEY_SIZE" : "MISSING_INPUT";
          await assert.rejects(discoverGameFolder(root), { code });
          assert.throws(() => discoverGameFiles(fallback), { code });
        } else {
          assert.equal((await discoverGameFolder(root)).inputMode, "packages");
          assert.equal(discoverGameFiles(fallback).inputMode, "packages");
        }
      }
    }
    assert.equal(discoverGameFiles(extractedFiles).inputMode, "extracted");
    assert.equal(
      discoverGameFiles([
        ...extractedFiles,
        blob("nested.xci", "synthetic", "root/other/nested.xci"),
      ]).inputMode,
      "extracted",
    );
    await assert.rejects(discoverGameFolder(directory([["game.xcz", file(blob("game.xcz"))]])), {
      code: "EXTRACTED_MISSING_INPUT",
    });
    assert.throws(() => discoverGameFiles([blob("game.xcz")]), {
      code: "EXTRACTED_MISSING_INPUT",
    });
  });
  test("automatic detection enumerates the root once and is bounded before metadata reads", async () => {
    const input = directory(["base.xci", "prod.keys"].map((name) => [name, file(blob(name))]));
    let enumerations = 0;
    const root = {
      kind: "directory",
      entries() {
        assert.equal(++enumerations, 1);
        return input.entries();
      },
    };
    assert.equal((await discoverGameFolder(root)).inputMode, "packages");
    let yielded = 0,
      closed = false;
    const large = {
      kind: "directory",
      async *entries() {
        try {
          for (let i = 0; i < NSP_INPUT_LIMITS.directoryEntries + 2; i++) {
            yielded++;
            yield [`other${i}`, { kind: "directory" }];
          }
        } finally {
          closed = true;
        }
      },
    };
    await assert.rejects(discoverGameFolder(large), { code: "DIRECTORY_LIMIT" });
    assert.equal(yielded, NSP_INPUT_LIMITS.directoryEntries + 1);
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(closed, true);
  });
  test("automatic detection cancels stalled enumeration and observes late failures", async () => {
    const already = new AbortController();
    already.abort();
    await assert.rejects(
      discoverGameFolder(
        {
          kind: "directory",
          entries() {
            throw Error("An already cancelled detection must not start enumeration");
          },
        },
        { signal: already.signal },
      ),
      { name: "AbortError" },
    );
    for (const rejects of [false, true]) {
      const controller = new AbortController(),
        entered = Promise.withResolvers(),
        late = Promise.withResolvers();
      let closed = 0;
      const root = {
        kind: "directory",
        entries() {
          return {
            [Symbol.asyncIterator]() {
              return this;
            },
            next() {
              entered.resolve();
              return late.promise;
            },
            return() {
              closed++;
              return Promise.reject(Error("late cleanup"));
            },
          };
        },
      };
      const pending = discoverGameFolder(root, { signal: controller.signal });
      await entered.promise;
      controller.abort();
      await assert.rejects(pending, { name: "AbortError" });
      assert.equal(closed, 1);
      if (rejects) late.reject(Error("late enumeration"));
      else late.resolve({ done: false, value: ["base.xci", file(blob("base.xci"))] });
      await new Promise((resolve) => setImmediate(resolve));
    }
  });
  test("extracted folder and FileList paths accept native casing and reject missing/ambiguous/unsafe aliases", async () => {
    await discoverGameFolder(extracted({ exefsName: "exefs", romfsName: "romfs" }), {
      inputMode: "extracted",
    });
    await assert.rejects(
      discoverGameFolder(extracted({ files: ["Main", "sdk"] }), { inputMode: "extracted" }),
      { code: "EXTRACTED_MISSING_INPUT" },
    );
    await assert.rejects(
      discoverGameFolder(
        directory([
          ["ExeFS", directory([])],
          ["exefs", directory([])],
        ]),
        { inputMode: "extracted" },
      ),
      { code: "EXTRACTED_NAME" },
    );
    await assert.rejects(
      discoverGameFolder(extracted({ files: ["../main", "sdk"] }), { inputMode: "extracted" }),
      { code: "EXTRACTED_NAME" },
    );
    const selected = ["ExeFS/main", "ExeFS/sdk", "RomFS/data/build_manifest.psv"].map((path) =>
      blob(path.split("/").at(-1), "synthetic", `root/${path}`),
    );
    assert.equal(discoverGameFiles(selected, { inputMode: "extracted" }).romfs.length, 1);
    assert.throws(
      () =>
        discoverGameFiles([...selected, blob("other", "x", "root/romfs/other")], {
          inputMode: "extracted",
        }),
      { code: "EXTRACTED_NAME" },
    );
    assert.throws(() => discoverGameFiles([...selected, selected[0]], { inputMode: "extracted" }), {
      code: "EXTRACTED_NAME",
    });
    assert.throws(
      () =>
        discoverGameFiles([blob("main", "x", "second/ExeFS/main"), ...selected], {
          inputMode: "extracted",
        }),
      { code: "DIRECTORY" },
    );
  });
  test("extracted cancellation does not wait for stalled metadata or enumeration", async () => {
    for (const kind of ["metadata", "enumeration"]) {
      const controller = new AbortController();
      const entered = Promise.withResolvers(),
        late = Promise.withResolvers();
      let closed = false;
      const root =
        kind === "metadata"
          ? directory([
              [
                "ExeFS",
                directory([
                  [
                    "main",
                    {
                      kind: "file",
                      getFile() {
                        entered.resolve();
                        return late.promise;
                      },
                    },
                  ],
                ]),
              ],
            ])
          : {
              kind: "directory",
              entries() {
                return {
                  [Symbol.asyncIterator]() {
                    return this;
                  },
                  next() {
                    entered.resolve();
                    return late.promise;
                  },
                  return() {
                    closed = true;
                    return { done: true };
                  },
                };
              },
            };
      const pending = discoverGameFolder(root, {
        inputMode: "extracted",
        signal: controller.signal,
      });
      await entered.promise;
      controller.abort();
      await assert.rejects(pending, { name: "AbortError" });
      late.reject(new Error("late synthetic error"));
      await new Promise((resolve) => setImmediate(resolve));
      if (kind === "enumeration") assert.equal(closed, true);
    }
  });

  test("recognized separate DLC layouts fail closed rather than imply authenticated absence", async () => {
    for (const name of [
      "DLC",
      "AddOnContent",
      "aoc",
      "Warlock",
      "Reign of the Warlock",
      "game [0100726014353001].nsp",
    ]) {
      const root = extracted();
      const original = root.entries.bind(root);
      root.entries = async function* () {
        yield* original();
        yield [name, directory([])];
      };
      await assert.rejects(discoverGameFolder(root, { inputMode: "extracted" }), {
        code: "EXTRACTED_DLC_UNSUPPORTED",
      });
      const files = [
        "ExeFS/main",
        "ExeFS/sdk",
        "RomFS/data/build_manifest.psv",
        `${name}/main`,
      ].map((path) => blob(path.split("/").at(-1), "synthetic", `root/${path}`));
      assert.throws(() => discoverGameFiles(files, { inputMode: "extracted" }), {
        code: "EXTRACTED_DLC_UNSUPPORTED",
      });
    }
  });

  test("ordinary nested Warlock-named resources do not imply a separate DLC layout", async () => {
    const paths = [
      "ExeFS/main",
      "ExeFS/sdk",
      "RomFS/data/build_manifest.psv",
      "RomFS/data/hd/warlock/0100726014353001.bin",
    ];
    const files = paths.map((path) => blob(path.split("/").at(-1), "synthetic", `root/${path}`));
    assert.equal(discoverGameFiles(files, { inputMode: "extracted" }).romfs.length, 2);
    const root = directory([
      ["ExeFS", directory(["main", "sdk"].map((name) => [name, file(blob(name))]))],
      [
        "RomFS",
        directory([
          [
            "data",
            directory([
              ["build_manifest.psv", file(blob("build_manifest.psv"))],
              ["warlock", directory([["0100726014353001.bin", file(blob("asset"))]])],
            ]),
          ],
        ]),
      ],
    ]);
    assert.equal((await discoverGameFolder(root, { inputMode: "extracted" })).romfs.length, 2);
  });
}

// nsp-input
{
  // 验证 nsp-input 的约定与边界；依据 ../nsp-input.mjs 及本文件的测试断言。
  // Verify nsp-input contracts and boundaries against ../nsp-input.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const {
    discoverNspFolder,
    discoverNspFiles,
    inspectNsp,
    inspectKeyFile,
    normalizeProdKeyBytes,
    preflightNspFolder,
    NSP_INPUT_LIMITS,
  } = nspInputModule;

  const fakeHex = (bytes) => "a5".repeat(bytes);
  for (const lateFailure of [false, true])
    test(`folder cancellation does not wait for pending enumeration (${lateFailure ? "reject" : "resolve"})`, async () => {
      const controller = new AbortController();
      let entered, finish;
      const started = new Promise((resolve) => {
        entered = resolve;
      });
      const pending = new Promise((resolve, reject) => {
        finish = () => (lateFailure ? reject(new Error("late enumeration error")) : resolve());
      });
      let reads = 0;
      let progress = 0;
      let returns = 0;
      let finalized = 0;
      const directory = {
        kind: "directory",
        entries() {
          const iterator = (async function* () {
            try {
              entered();
              await pending;
              yield [
                "game.nsp",
                {
                  kind: "file",
                  getFile() {
                    reads++;
                    return fixture().file;
                  },
                },
              ];
            } finally {
              finalized++;
            }
          })();
          const close = iterator.return.bind(iterator);
          iterator.return = (...args) => {
            returns++;
            return close(...args);
          };
          return iterator;
        },
      };
      const result = discoverNspFolder(directory, {
        signal: controller.signal,
        onProgress() {
          progress++;
        },
      }).then(
        () => "opened",
        (error) => error,
      );
      await started;
      controller.abort();
      await new Promise((resolve) => setImmediate(resolve));
      const waiting = Symbol("waiting");
      const outcome = await Promise.race([result, Promise.resolve(waiting)]);
      const returnsAtCancel = returns;
      const finalizersAtCancel = finalized;
      finish();
      await result;
      await new Promise((resolve) => setImmediate(resolve));
      assert.notEqual(outcome, waiting);
      assert.equal(outcome.name, "AbortError");
      assert.equal(returnsAtCancel, 1);
      assert.equal(finalizersAtCancel, 0, "return queues behind the unfinished next call");
      assert.equal(returns, 1);
      assert.equal(finalized, 1);
      assert.equal(reads, 0);
      assert.equal(progress, 1, "late entries must not update progress");
    });

  test("folder cancellation observes late enumeration and iterator cleanup rejections", async () => {
    const controller = new AbortController();
    let entered, rejectNext, rejectReturn;
    const started = new Promise((resolve) => {
      entered = resolve;
    });
    const next = new Promise((_, reject) => {
      rejectNext = reject;
    });
    const cleanup = new Promise((_, reject) => {
      rejectReturn = reject;
    });
    let returns = 0;
    const directory = {
      kind: "directory",
      entries() {
        return {
          [Symbol.asyncIterator]() {
            return this;
          },
          next() {
            entered();
            return next;
          },
          return() {
            returns++;
            return cleanup;
          },
        };
      },
    };
    const result = discoverNspFolder(directory, { signal: controller.signal }).catch(
      (error) => error,
    );
    await started;
    controller.abort();
    await new Promise((resolve) => setImmediate(resolve));
    const waiting = Symbol("waiting");
    const outcome = await Promise.race([result, Promise.resolve(waiting)]);
    const returnsAtCancel = returns;
    rejectNext(new Error("late enumeration error"));
    rejectReturn(new Error("late cleanup error"));
    await result;
    await new Promise((resolve) => setImmediate(resolve));
    assert.notEqual(outcome, waiting);
    assert.equal(outcome.name, "AbortError");
    assert.equal(returnsAtCancel, 1);
    assert.equal(returns, 1);
  });

  for (const cancelCleanup of [false, true])
    test(`folder validation preserves its error while closing enumeration (${cancelCleanup ? "cancel" : "reject"})`, async () => {
      const controller = new AbortController();
      let entered, rejectReturn;
      const started = new Promise((resolve) => {
        entered = resolve;
      });
      const cleanup = new Promise((_, reject) => {
        rejectReturn = reject;
      });
      let returns = 0;
      const directory = {
        kind: "directory",
        entries() {
          return {
            [Symbol.asyncIterator]() {
              return this;
            },
            next() {
              return Promise.resolve({ done: false, value: ["../unsafe", { kind: "file" }] });
            },
            return() {
              returns++;
              entered();
              return cleanup;
            },
          };
        },
      };
      const result = discoverNspFolder(directory, {
        signal: cancelCleanup ? controller.signal : undefined,
      }).catch((error) => error);
      await started;
      const waiting = Symbol("waiting");
      assert.equal(await Promise.race([result, Promise.resolve(waiting)]), waiting);
      if (cancelCleanup) controller.abort();
      else rejectReturn(new Error("iterator cleanup failed"));
      await new Promise((resolve) => setImmediate(resolve));
      const outcome = await Promise.race([result, Promise.resolve(waiting)]);
      if (cancelCleanup) rejectReturn(new Error("late iterator cleanup failed"));
      await result;
      await new Promise((resolve) => setImmediate(resolve));
      assert.notEqual(outcome, waiting);
      assert.equal(outcome.code, "UNSAFE_NAME");
      assert.equal(returns, 1);
    });

  test("cancellation from initial folder progress does not start enumeration", async () => {
    const controller = new AbortController();
    let enumerations = 0;
    await assert.rejects(
      discoverNspFolder(
        {
          kind: "directory",
          entries() {
            enumerations++;
            return folder([]).entries();
          },
        },
        {
          signal: controller.signal,
          onProgress() {
            controller.abort();
          },
        },
      ),
      { name: "AbortError" },
    );
    assert.equal(enumerations, 0);
  });

  for (const lateFailure of [false, true])
    test(`folder cancellation does not wait for pending metadata (${lateFailure ? "reject" : "resolve"})`, async () => {
      const controller = new AbortController();
      let entered, finish;
      const started = new Promise((resolve) => {
        entered = resolve;
      });
      const pending = new Promise((resolve, reject) => {
        finish = () =>
          lateFailure ? reject(new Error("late metadata error")) : resolve(keys()[0]);
      });
      let laterReads = 0;
      const result = discoverNspFolder(
        folder([
          [
            "prod.keys",
            {
              kind: "file",
              getFile() {
                entered();
                return pending;
              },
            },
          ],
          [
            "game.nsp",
            {
              kind: "file",
              getFile() {
                laterReads++;
                return fixture().file;
              },
            },
          ],
        ]),
        { signal: controller.signal },
      ).then(
        () => "opened",
        (error) => error,
      );
      await started;
      controller.abort();
      await new Promise((resolve) => setImmediate(resolve));
      const waiting = Symbol("waiting");
      const outcome = await Promise.race([result, Promise.resolve(waiting)]);
      finish();
      await result;
      await new Promise((resolve) => setImmediate(resolve));
      assert.notEqual(outcome, waiting);
      assert.equal(outcome.name, "AbortError");
      assert.equal(laterReads, 0);
    });
  test("folder progress observers cannot fail discovery, but signal cancellation still applies", async () => {
    for (const asyncFailure of [false, true]) {
      let calls = 0;
      const result = await discoverNspFolder(
        folder([
          ["prod.keys", handle(keys()[0])],
          ["game.nsp", handle(fixture().file)],
        ]),
        {
          onProgress() {
            calls++;
            if (asyncFailure) return Promise.reject(new Error("observer failed"));
            throw new Error("observer failed");
          },
        },
      );
      assert.equal(result.nsps.length, 1);
      assert.ok(calls > 0);
      await new Promise((resolve) => setImmediate(resolve));
    }
    const controller = new AbortController();
    await assert.rejects(
      discoverNspFolder(folder([["game.nsp", handle(fixture().file)]]), {
        signal: controller.signal,
        onProgress() {
          controller.abort();
        },
      }),
      { name: "AbortError" },
    );
  });
  function localFile(name, content, path = `chosen/${name}`) {
    const blob = new Blob([content]);
    return {
      name,
      size: blob.size,
      webkitRelativePath: path,
      slice: (...args) => blob.slice(...args),
    };
  }
  function fixture(entries = [{ name: "content.nca", offset: 0n, size: 4n }], extraSize = null) {
    const strings = new TextEncoder().encode(entries.map((e) => e.name + "\0").join(""));
    const dataOffset = 16 + entries.length * 24 + strings.length;
    const bytes = new Uint8Array(dataOffset);
    bytes.set(new TextEncoder().encode("PFS0"));
    const view = new DataView(bytes.buffer);
    view.setUint32(4, entries.length, true);
    view.setUint32(8, strings.length, true);
    let nameOffset = 0;
    for (let i = 0; i < entries.length; i++) {
      const pos = 16 + i * 24;
      view.setBigUint64(pos, entries[i].offset, true);
      view.setBigUint64(pos + 8, entries[i].size, true);
      view.setUint32(pos + 16, nameOffset, true);
      nameOffset += new TextEncoder().encode(entries[i].name).length + 1;
    }
    bytes.set(strings, 16 + entries.length * 24);
    let size = BigInt(dataOffset);
    for (const entry of entries)
      if (BigInt(dataOffset) + entry.offset + entry.size > size)
        size = BigInt(dataOffset) + entry.offset + entry.size;
    const reads = [];
    const file = {
      name: "game.nsp",
      webkitRelativePath: "chosen/game.nsp",
      size: Number(extraSize ?? size),
      slice(start, end) {
        reads.push([start, end]);
        assert.ok(end <= bytes.length, "metadata reader must never read the payload");
        return new Blob([bytes.slice(start, end)]);
      },
    };
    return { file, bytes, view, dataOffset, reads };
  }
  function nspBlob() {
    const image = fixture();
    const bytes = new Uint8Array(image.file.size);
    bytes.set(image.bytes);
    return Object.assign(new Blob([bytes]), {
      name: "game.nsp",
      webkitRelativePath: "chosen/game.nsp",
    });
  }
  function folder(entries) {
    return {
      kind: "directory",
      async *entries() {
        yield* entries;
      },
    };
  }
  function handle(file) {
    return { kind: "file", getFile: async () => file };
  }
  function keys() {
    return [
      localFile("prod.keys", `header_key = ${fakeHex(32)}\nmaster_key_00 = ${fakeHex(16)}`),
      localFile("title.keys", `${"12".repeat(16)} = ${fakeHex(16)}`),
    ];
  }
  function code(expected) {
    return (error) => error.code === expected;
  }

  test("root folder discovery is case insensitive and never recurses or reads unrelated files", async () => {
    const [prod, title] = keys();
    const nsp = fixture().file;
    const selection = await discoverNspFolder(
      folder([
        ["PROD.KEYS", handle(prod)],
        ["title.keys", handle(title)],
        ["game.NSP", handle(nsp)],
        [
          "nested",
          {
            kind: "directory",
            entries() {
              throw new Error("no recursion");
            },
          },
        ],
        [
          "unrelated.txt",
          {
            kind: "file",
            getFile() {
              throw new Error("no unrelated reads");
            },
          },
        ],
      ]),
    );
    assert.equal(selection.prodKeys, prod);
    assert.deepEqual(selection.nsps, [nsp]);
  });

  test("discovery accepts NSP and XCI by default, alone or mixed, with required prod.keys", async () => {
    const [prod] = keys();
    for (const names of [["game.NSP"], ["game.XCI"], ["base.xci", "update.nsp"]]) {
      const packages = names.map((name) => localFile(name, "synthetic"));
      const files = [prod, ...packages];
      const progress = [];
      const discovered = await discoverNspFolder(
        folder(files.map((file) => [file.name, handle(file)])),
        { onProgress: (event) => progress.push(event) },
      );
      assert.deepEqual(discovered.nsps, packages);
      assert.equal(discovered.titleKeys, undefined);
      assert.equal(progress.at(-1).nspCount, packages.length);
      assert.deepEqual(discoverNspFiles(files).nsps, packages);
      await assert.rejects(
        discoverNspFolder(folder(packages.map((file) => [file.name, handle(file)]))),
        code("MISSING_INPUT"),
      );
      assert.throws(() => discoverNspFiles(packages), code("MISSING_INPUT"));
    }
  });

  test("discovery excludes unsupported package extensions without opening them", async () => {
    const [prod] = keys();
    const unsupported = [
      "game.nsz",
      "game.xcz",
      "game.ncz",
      "game.xci.00",
      "game.nsp.00",
      "game.zip",
    ];
    const entries = unsupported.map((name) => [
      name,
      {
        kind: "file",
        getFile() {
          throw new Error("Unsupported files must not be opened");
        },
      },
    ]);
    const files = unsupported.map((name) => localFile(name, "synthetic"));
    await assert.rejects(
      discoverNspFolder(folder([[prod.name, handle(prod)], ...entries])),
      code("MISSING_INPUT"),
    );
    assert.throws(() => discoverNspFiles([prod, ...files]), code("MISSING_INPUT"));
    const xci = localFile("base.xci", "synthetic");
    assert.deepEqual(
      (
        await discoverNspFolder(
          folder([[prod.name, handle(prod)], [xci.name, handle(xci)], ...entries]),
        )
      ).nsps,
      [xci],
    );
    assert.deepEqual(discoverNspFiles([prod, xci, ...files]).nsps, [xci]);
  });

  test("discovery rejects missing files, unsafe names, duplicates, and oversized keys", async () => {
    const [prod, title] = keys();
    await assert.rejects(discoverNspFolder(folder([])), code("MISSING_INPUT"));
    for (const name of ["../evil", "bad\\name", "CON", "name.", "x\u0000"]) {
      await assert.rejects(discoverNspFolder(folder([[name, handle(prod)]])), code("UNSAFE_NAME"));
    }
    await assert.rejects(
      discoverNspFolder(
        folder([
          ["prod.keys", handle(prod)],
          ["PROD.KEYS", handle(prod)],
        ]),
      ),
      code("DUPLICATE_NAME"),
    );
    await assert.rejects(
      discoverNspFolder(
        folder([
          ["prod.keys", handle({ ...prod, size: NSP_INPUT_LIMITS.keyBytes + 1 })],
          ["title.keys", handle(title)],
          ["a.nsp", handle(fixture().file)],
        ]),
      ),
      code("KEY_SIZE"),
    );
  });

  test("directory enumeration has a bounded stopping point", async () => {
    let yielded = 0;
    const directory = {
      kind: "directory",
      async *entries() {
        for (let i = 0; i < 100000; i++) {
          yielded++;
          yield [`folder${i}`, { kind: "directory" }];
        }
      },
    };
    await assert.rejects(discoverNspFolder(directory), code("DIRECTORY_LIMIT"));
    assert.equal(yielded, NSP_INPUT_LIMITS.directoryEntries + 1);
  });

  test("FileList fallback requires one common root, ignores nested matches, validates paths", () => {
    const files = [...keys(), fixture().file];
    assert.equal(discoverNspFiles(files).nsps.length, 1);
    assert.throws(
      () =>
        discoverNspFiles(
          files.map((file) => ({ ...file, webkitRelativePath: `chosen/sub/${file.name}` })),
        ),
      code("MISSING_INPUT"),
    );
    assert.throws(
      () => discoverNspFiles([...files, localFile("other.txt", "", "elsewhere/other.txt")]),
      code("DIRECTORY"),
    );
    assert.throws(() => discoverNspFiles([localFile("x", "", "")]), code("DIRECTORY"));
    assert.throws(() => discoverNspFiles([localFile("x", "", "chosen/../x")]), code("DIRECTORY"));
  });

  test("PFS0 index parsing keeps sparse offsets and sizes above 4GiB exact without payload reads", async () => {
    const sparse = fixture([{ name: "huge.nca", offset: 0x100000001n, size: 0x100000002n }]);
    const result = await inspectNsp(sparse.file);
    assert.equal(result.entries[0].offset, BigInt(sparse.dataOffset) + 0x100000001n);
    assert.equal(result.entries[0].size, 0x100000002n);
    assert.deepEqual(sparse.reads, [
      [0, 16],
      [16, sparse.dataOffset],
    ]);
  });

  test("PFS0 validates header and allocation bounds before requesting index", async () => {
    for (const field of [4, 8]) {
      const f = fixture();
      f.view.setUint32(field, 0xffffffff, true);
      await assert.rejects(inspectNsp(f.file), code("PFS0_LIMIT"));
      assert.deepEqual(f.reads, [[0, 16]]);
    }
    const f = fixture();
    f.file.size = f.dataOffset - 1;
    await assert.rejects(inspectNsp(f.file), code("PFS0_TRUNCATED"));
    f.file.size = 15;
    await assert.rejects(inspectNsp(f.file), code("PFS0_HEADER"));
    f.file.size = Number.MAX_SAFE_INTEGER + 1;
    await assert.rejects(inspectNsp(f.file), code("FILE_SIZE"));
  });

  test("PFS0 rejects bad magic, reserved fields and short reads", async () => {
    for (const [offset, expected] of [
      [0, "PFS0_MAGIC"],
      [12, "PFS0_RESERVED"],
      [36, "PFS0_RESERVED"],
    ]) {
      const f = fixture();
      f.view.setUint32(offset, 1, true);
      await assert.rejects(inspectNsp(f.file), code(expected));
    }
    await assert.rejects(
      inspectNsp({ size: 100, slice: () => new Blob([new Uint8Array(2)]) }),
      code("SHORT_READ"),
    );
  });

  test("PFS0 rejects invalid names, string boundaries, UTF8 and case duplicates", async () => {
    for (const name of ["", "../a", "x/y", "x\\y", "name.", "a".repeat(256)]) {
      await assert.rejects(inspectNsp(fixture([{ name, offset: 0n, size: 1n }]).file));
    }
    const f = fixture();
    f.view.setUint32(32, 1, true);
    await assert.rejects(inspectNsp(f.file), code("PFS0_NAME"));
    f.view.setUint32(32, 0xffffffff, true);
    await assert.rejects(inspectNsp(f.file), code("PFS0_NAME"));
    f.view.setUint32(32, 0, true);
    f.bytes[f.bytes.length - 1] = 65;
    await assert.rejects(inspectNsp(f.file), code("PFS0_NAME"));
    f.bytes[f.bytes.length - 1] = 0;
    f.bytes[40] = 255;
    await assert.rejects(inspectNsp(f.file), code("PFS0_NAME"));
    await assert.rejects(
      inspectNsp(
        fixture([
          { name: "a.nca", offset: 0n, size: 1n },
          { name: "A.NCA", offset: 1n, size: 1n },
        ]).file,
      ),
      code("DUPLICATE_NAME"),
    );
  });

  test("PFS0 checks overlap, adjacency, empty entries, and uint64 ranges without wrapping", async () => {
    await assert.rejects(
      inspectNsp(
        fixture([
          { name: "a", offset: 0n, size: 3n },
          { name: "b", offset: 2n, size: 4n },
        ]).file,
      ),
      code("PFS0_OVERLAP"),
    );
    const valid = fixture([
      { name: "b", offset: 3n, size: 4n },
      { name: "a", offset: 0n, size: 3n },
      { name: "empty", offset: 1n, size: 0n },
    ]);
    assert.equal((await inspectNsp(valid.file)).entries.length, 3);
    const range = fixture();
    range.view.setBigUint64(16, 0xffffffffffffffffn, true);
    await assert.rejects(inspectNsp(range.file), code("PFS0_RANGE"));
    range.view.setBigUint64(16, 0n, true);
    range.view.setBigUint64(24, 0xffffffffffffffffn, true);
    await assert.rejects(inspectNsp(range.file), code("PFS0_RANGE"));
  });

  test("key parser supports verified ASCII subset, separators, case, CRLF and metadata only", async () => {
    const result = await inspectKeyFile(
      localFile(
        "prod.keys",
        `# comment\r\n\tHEADER_KEY , ${fakeHex(32).toUpperCase()} \t\r\nmaster_key_00=${fakeHex(16)}\nfuture_key = abcd\n`,
      ),
      "prod",
    );
    assert.deepEqual(result, {
      kind: "prod",
      entries: 3,
      verifiedEntries: 2,
      unknownEntries: 1,
      usableForDecryption: false,
    });
    assert.ok(!JSON.stringify(result).includes("a5"));
    assert.equal((await inspectKeyFile(keys()[1], "title")).verifiedEntries, 1);
    assert.equal((await inspectKeyFile(localFile("title.keys", ""), "title")).entries, 0);
  });

  test("native master KEK source widths are checked before loading keys", async () => {
    for (const name of ["master_kek_source_06", "mariko_master_kek_source_05"]) {
      const valid = await inspectKeyFile(
        localFile("prod.keys", `${name} = ${fakeHex(16)}\n`),
        "prod",
      );
      assert.equal(valid.verifiedEntries, 1);
      const padded = `${name} = ${fakeHex(16)}00\r\n`;
      assert.equal(
        (await inspectKeyFile(localFile("prod.keys", padded), "prod")).verifiedEntries,
        1,
      );
      const owned = new TextEncoder().encode(padded);
      const normalized = normalizeProdKeyBytes(owned);
      assert.equal(new TextDecoder().decode(normalized), `${name} = ${fakeHex(16)}\r\n`);
      assert.deepEqual([...owned.subarray(normalized.length)], [0, 0]);
      await assert.rejects(
        inspectKeyFile(localFile("prod.keys", `${name} = ${fakeHex(17)}\n`), "prod"),
        code("KEY_LINE"),
      );
    }
  });

  test("key parser rejects malformed, odd, wrong-width, duplicate, nonASCII and oversized lines without disclosure", async () => {
    const lines = [
      `header_key = ${fakeHex(16)}`,
      "future = abc",
      "future = gg",
      "future = ",
      `header_key=${fakeHex(32)}\nHEADER_KEY=${fakeHex(32)}`,
      "future = a5 # comment",
      "other = a5\u0000",
      "é = a5",
      "future = " + "ab".repeat(NSP_INPUT_LIMITS.keyLineBytes),
    ];
    for (const text of lines) {
      await assert.rejects(inspectKeyFile(localFile("prod.keys", text), "prod"), (error) => {
        assert.match(error.message, /^Invalid key-file line [12]\.$/u);
        assert.equal(error.code, "KEY_LINE");
        return true;
      });
    }
    await assert.rejects(inspectKeyFile(localFile("prod.keys", "\n"), "prod"), code("KEY_EMPTY"));
    await assert.rejects(
      inspectKeyFile(localFile("title.keys", `wrong=${fakeHex(16)}`), "title"),
      code("KEY_LINE"),
    );
    const id = "12".repeat(16);
    await assert.rejects(
      inspectKeyFile(
        localFile("title.keys", `${id}=${fakeHex(16)}\n${id}=${fakeHex(16)}`),
        "title",
      ),
      code("KEY_LINE"),
    );
  });

  test("key read buffers are cleared best-effort on both success and error", async () => {
    for (const text of [`header_key=${fakeHex(32)}`, "header_key=WRONG"]) {
      const owned = new TextEncoder().encode(text);
      const file = { size: owned.length, slice: () => ({ arrayBuffer: async () => owned.buffer }) };
      try {
        await inspectKeyFile(file, "prod");
      } catch {
        /* 预期无效的样例。 */
        /* Expected invalid fixture. */
      }
      assert.ok(owned.every((byte) => byte === 0));
    }
  });

  test("full folder preflight returns no key contents, handles, or build readiness claim", async () => {
    const result = await preflightNspFolder([...keys(), nspBlob()]);
    assert.equal(result.stage, "metadata-only");
    assert.equal(result.canBuildIpa, false);
    assert.equal(result.packages.length, 1);
    assert.equal(result.prodKeys.verifiedEntries, 2);
    assert.ok(
      !JSON.stringify(result, (_, value) =>
        typeof value === "bigint" ? value.toString() : value,
      ).includes(fakeHex(16)),
    );
  });

  test("folder discovery cancellation stops before enumeration or after pending metadata read", async () => {
    const controller = new AbortController();
    controller.abort();
    await assert.rejects(discoverNspFolder(folder([]), { signal: controller.signal }), {
      name: "AbortError",
    });
    const duringRead = new AbortController();
    const root = folder([
      [
        "prod.keys",
        {
          kind: "file",
          async getFile() {
            duringRead.abort();
            return keys()[0];
          },
        },
      ],
    ]);
    await assert.rejects(discoverNspFolder(root, { signal: duringRead.signal }), {
      name: "AbortError",
    });
  });

  test("FileList fallback detects ambiguous inferred root directory names without rejecting repeated descendants", () => {
    const files = [...keys(), fixture().file];
    assert.equal(
      discoverNspFiles([
        ...files,
        localFile("one", "", "chosen/sub/one"),
        localFile("two", "", "chosen/sub/two"),
      ]).nsps.length,
      1,
    );
    assert.throws(
      () =>
        discoverNspFiles([
          ...files,
          localFile("one", "", "chosen/sub/one"),
          localFile("two", "", "chosen/SUB/two"),
        ]),
      code("DUPLICATE_NAME"),
    );
    assert.throws(
      () =>
        discoverNspFiles([...files, localFile("SUB", ""), localFile("one", "", "chosen/sub/one")]),
      code("DUPLICATE_NAME"),
    );
  });

  test("NSP filename decoder does not discard a leading BOM or allow format controls", async () => {
    for (const name of ["\ufeffcontent.nca", "evil\u202ename.nca"]) {
      await assert.rejects(
        inspectNsp(fixture([{ name, offset: 0n, size: 1n }]).file),
        code("UNSAFE_NAME"),
      );
    }
  });

  test("package count and key-size caps apply without reading contents", async () => {
    const many = keys();
    for (let i = 0; i <= NSP_INPUT_LIMITS.packages; i++)
      many.push(localFile(`${i}.${i % 2 ? "xci" : "nsp"}`, "synthetic"));
    await assert.rejects(
      discoverNspFolder(folder(many.map((file) => [file.name, handle(file)]))),
      code("PACKAGE_LIMIT"),
    );
    assert.throws(() => discoverNspFiles(many), code("PACKAGE_LIMIT"));
    await assert.rejects(
      inspectKeyFile(
        {
          size: NSP_INPUT_LIMITS.keyBytes + 1,
          slice() {
            throw new Error("must not read");
          },
        },
        "prod",
      ),
      code("KEY_SIZE"),
    );
  });

  test("folder discovery and preflight accept absent optional title keys", async () => {
    const [prod] = keys();
    const nsp = fixture().file;
    const inputs = await discoverNspFolder(
      folder([
        ["prod.keys", handle(prod)],
        ["game.nsp", handle(nsp)],
      ]),
    );
    assert.equal(inputs.titleKeys, undefined);
    const result = await preflightNspFolder([prod, nspBlob()]);
    assert.equal(result.titleKeys.entries, 0);
  });

  test("RSA-sized production dump entries do not reject otherwise valid AES keys", async () => {
    const result = await inspectKeyFile(
      localFile(
        "prod.keys",
        `eticket_rsa_keypair = ${fakeHex(528)}\r\nheader_key = ${fakeHex(32)}\nmaster_key_00 = ${fakeHex(16)}\n`,
      ),
      "prod",
    );
    assert.equal(result.entries, 3);
    assert.equal(result.verifiedEntries, 2);
    assert.equal(result.unknownEntries, 1);
    assert.equal(result.usableForDecryption, false);
  });

  test("key-line bound includes exact-length content but excludes CRLF and keeps AES widths strict", async () => {
    const line = "extra = " + "ab".repeat((NSP_INPUT_LIMITS.keyLineBytes - 8) / 2);
    assert.equal(line.length, NSP_INPUT_LIMITS.keyLineBytes);
    for (const ending of ["", "\n", "\r\n"])
      assert.equal(
        (await inspectKeyFile(localFile("prod.keys", line + ending), "prod")).entries,
        1,
      );
    await assert.rejects(
      inspectKeyFile(localFile("prod.keys", line + "ab\n"), "prod"),
      code("KEY_LINE"),
    );
    await assert.rejects(
      inspectKeyFile(localFile("prod.keys", `header_key = ${fakeHex(528)}\n`), "prod"),
      code("KEY_LINE"),
    );
  });
}

// xci-input
{
  // 验证 xci-input 的约定与边界；依据 ../xci-input.mjs、../nsp-input.mjs 及本文件的测试断言。
  // Verify xci-input contracts and boundaries against ../xci-input.mjs、../nsp-input.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { inspectXci, XCI_INPUT_LIMITS, XciInputError } = xciInputModule;
  const { preflightNspFolder } = nspInputModule;

  // 仅使用自编合成 HEAD/HFS0 映像，不含游戏内容、密钥或复制的样例。
  // Authored synthetic HEAD/HFS0 images only. No game content, keys, or copied fixtures.
  const digest = (bytes) => createHash("sha256").update(bytes).digest();
  const payload = (name, length = 32) => ({ name, contents: Buffer.alloc(length, 0x5a) });

  function range(segments, start, length) {
    const bytes = Buffer.alloc(length);
    const end = start + BigInt(length);
    for (const segment of segments) {
      const segmentEnd = segment.offset + BigInt(segment.bytes.length);
      const from = start > segment.offset ? start : segment.offset;
      const to = end < segmentEnd ? end : segmentEnd;
      if (from < to) {
        segment.bytes.copy(
          bytes,
          Number(from - start),
          Number(from - segment.offset),
          Number(to - segment.offset),
        );
      }
    }
    return bytes;
  }

  function hfs(definitions, { padding = 512 } = {}) {
    const strings = Buffer.from(definitions.map((entry) => `${entry.name}\0`).join(""));
    const minimum = 16 + definitions.length * 64 + strings.length;
    const headerSize = Math.ceil(minimum / padding) * padding;
    const bytes = Buffer.alloc(headerSize);
    bytes.write("HFS0");
    bytes.writeUInt32LE(definitions.length, 4);
    bytes.writeUInt32LE(headerSize - 16 - definitions.length * 64, 8);
    strings.copy(bytes, 16 + definitions.length * 64);
    const segments = [{ offset: 0n, bytes }];
    const entries = [];
    let cursor = 0n;
    let stringOffset = 0;
    for (let i = 0; i < definitions.length; i++) {
      const definition = definitions[i];
      const contents = Buffer.isBuffer(definition.contents)
        ? {
            size: BigInt(definition.contents.length),
            segments: [{ offset: 0n, bytes: definition.contents }],
          }
        : definition.contents;
      const offset = definition.offset ?? cursor;
      const size = definition.size ?? contents.size;
      const hashSize = definition.hashSize ?? Number(size < 512n ? size : 512n);
      const position = 16 + i * 64;
      bytes.writeBigUInt64LE(offset, position);
      bytes.writeBigUInt64LE(size, position + 8);
      bytes.writeUInt32LE(stringOffset, position + 16);
      bytes.writeUInt32LE(hashSize, position + 20);
      digest(range(contents.segments, 0n, hashSize)).copy(bytes, position + 32);
      const start = BigInt(headerSize) + offset;
      for (const segment of contents.segments) {
        segments.push({ offset: start + segment.offset, bytes: segment.bytes });
      }
      entries.push({ name: definition.name, offset: start, size, position, contents });
      stringOffset += Buffer.byteLength(definition.name) + 1;
      if (offset + size > cursor) cursor = offset + size;
    }
    return { bytes, segments, entries, headerSize, size: BigInt(headerSize) + cursor };
  }

  function fixture({ partitions, secureMembers, rootOffset = 0xf000n } = {}) {
    const definitions = partitions ?? [
      { name: "secure", contents: hfs(secureMembers ?? [payload("program.nca")]) },
    ];
    const root = hfs(definitions);
    const head = Buffer.alloc(0x200);
    head.write("HEAD", 0x100);
    head.writeBigUInt64LE(rootOffset, 0x130);
    head.writeBigUInt64LE(BigInt(root.headerSize), 0x138);
    digest(root.bytes).copy(head, 0x140);
    const segments = [
      { offset: 0n, bytes: head },
      ...root.segments.map((segment) => ({ ...segment, offset: rootOffset + segment.offset })),
    ];
    const reads = [];
    const file = {
      name: "synthetic.xci",
      size: Number(rootOffset + root.size),
      slice(start, end) {
        assert(Number.isSafeInteger(start) && Number.isSafeInteger(end));
        assert(start >= 0 && start <= end && end <= this.size);
        assert(end - start <= XCI_INPUT_LIMITS.readBytes, "Every read must be bounded");
        reads.push({ start, end });
        return {
          async arrayBuffer() {
            const bytes = range(segments, BigInt(start), end - start);
            return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
          },
        };
      },
    };
    const resealRoot = () => digest(root.bytes).copy(head, 0x140);
    const resealPartitions = () => {
      for (const entry of root.entries) {
        const hashSize = root.bytes.readUInt32LE(entry.position + 20);
        digest(range(segments, rootOffset + entry.offset, hashSize)).copy(
          root.bytes,
          entry.position + 32,
        );
      }
      resealRoot();
    };
    return { file, reads, head, root, rootOffset, segments, resealRoot, resealPartitions };
  }

  const rejects = (image, code) =>
    assert.rejects(inspectXci(image.file), (error) => {
      assert(error instanceof XciInputError);
      assert.equal(error.code, code);
      return true;
    });

  test("global HEAD/HFS0 selects only secure NCA and ticket entries, with absolute BigInt ranges", async () => {
    const secure = hfs([
      payload("program.nca"),
      payload("rights.tik", 24),
      payload("metadata.cnmt.nca", 64),
      payload("ignore.cert"),
    ]);
    const image = fixture({
      partitions: [
        { name: "update", contents: hfs([payload("system.nca")]) },
        { name: "normal", contents: hfs([payload("normal.nca"), payload("normal.tik")]) },
        { name: "secure", contents: secure },
        { name: "logo", contents: hfs([payload("logo.nca")]) },
      ],
    });
    const result = await inspectXci(image.file);
    const secureBase = image.rootOffset + image.root.entries[2].offset;
    assert.deepEqual(result, {
      format: "XCI",
      size: BigInt(image.file.size),
      entries: secure.entries.slice(0, 3).map((entry) => ({
        name: entry.name,
        offset: secureBase + entry.offset,
        size: entry.size,
      })),
    });
    assert(Object.isFrozen(result) && Object.isFrozen(result.entries));
    assert(result.entries.every(Object.isFrozen));
  });

  test("supports empty non-secure partitions and case-insensitive NCA/ticket extensions", async () => {
    const image = fixture({
      partitions: [
        { name: "normal", contents: hfs([], { padding: 1 }) },
        { name: "secure", contents: hfs([payload("PROGRAM.NCA"), payload("RIGHTS.TIK")]) },
      ],
    });
    assert.deepEqual(
      (await inspectXci(image.file)).entries.map((entry) => entry.name),
      ["PROGRAM.NCA", "RIGHTS.TIK"],
    );
  });

  test("accepts an immutable Blob containing only the authored synthetic image", async () => {
    const image = fixture();
    const blob = new Blob([range(image.segments, 0n, image.file.size)]);
    assert.deepEqual(await inspectXci(blob), await inspectXci(image.file));
  });

  test("sparse files above 4 GiB retain exact absolute offsets and do not read whole members", async () => {
    const gap = (1n << 32n) + 1337n;
    const largeMember = (1n << 32n) + 71n;
    const image = fixture({
      rootOffset: (1n << 32n) + 0x200n,
      secureMembers: [
        { ...payload("huge.nca"), offset: gap, size: largeMember, hashSize: 32 },
        { ...payload("tail.tik", 8), offset: gap + largeMember },
      ],
    });
    const result = await inspectXci(image.file);
    const offset = image.rootOffset + BigInt(image.root.headerSize + 512) + gap;
    assert.equal(result.entries[0].offset, offset);
    assert.equal(result.entries[0].size, largeMember);
    assert.equal(result.entries[1].offset, offset + largeMember);
    assert(result.size > 12n * 1024n * 1024n * 1024n);
    assert(image.reads.reduce((sum, read) => sum + read.end - read.start, 0) < 4096);
  });

  test("large metadata indexes and declared hash prefixes are read in 64 KiB chunks", async () => {
    const definitions = Array.from({ length: 1100 }, (_, i) => payload(`data-${i}.nca`, 0));
    definitions[0] = { ...payload("program.nca", 160000), hashSize: 150001 };
    // 允许不相关的空成员，使用空前缀的 SHA-256。
    // Empty irrelevant members are allowed, with the SHA-256 of the empty prefix.
    for (let i = 1; i < definitions.length; i++) definitions[i].name = `data-${i}.cert`;
    const secure = hfs(definitions);
    assert(secure.headerSize > XCI_INPUT_LIMITS.readBytes);
    const image = fixture({ partitions: [{ name: "secure", contents: secure }] });
    const result = await inspectXci(image.file);
    assert.equal(result.entries.length, 1);
    assert(image.reads.some((read) => read.end - read.start === XCI_INPUT_LIMITS.readBytes));
    assert(image.reads.every((read) => read.end - read.start <= 65536));
  });

  test("checks declared prefixes, not trailing bytes, while leaving full NCA integrity to the gate", async () => {
    const content = Buffer.alloc(1000, 0x5a);
    const image = fixture({
      secureMembers: [{ name: "program.nca", contents: content, hashSize: 32 }],
    });
    content[999] ^= 1;
    assert.equal((await inspectXci(image.file)).entries.length, 1);
    content[0] ^= 1;
    await rejects(image, "HFS0_MEMBER_HASH");
  });

  test("rejects truncated or misplaced HEAD magic and invalid file objects", async () => {
    await assert.rejects(inspectXci(new Blob([new Uint8Array(511)])), { code: "XCI_HEADER" });
    for (const file of [null, {}, { size: -1, slice() {} }, { size: 2 ** 53, slice() {} }]) {
      await assert.rejects(inspectXci(file), { code: "FILE_SIZE" });
    }
    const image = fixture();
    image.head.write("HEAD", 0);
    image.head.fill(0, 0x100, 0x104);
    await rejects(image, "XCI_MAGIC");
  });

  for (const [label, offset, headerSize] of [
    ["inside HEAD", 0n, 512n],
    ["unaligned", 0xf001n, 512n],
    ["past file", 1n << 63n, 512n],
    ["empty header", 0xf000n, 0n],
    ["short header", 0xf000n, 15n],
    ["oversized header", 0xf000n, 1n << 63n],
  ]) {
    test(`rejects invalid declared root ${label}`, async () => {
      const image = fixture();
      image.head.writeBigUInt64LE(offset, 0x130);
      image.head.writeBigUInt64LE(headerSize, 0x138);
      await rejects(image, "XCI_ROOT_RANGE");
      assert.equal(image.reads.length, 1);
    });
  }

  test("rejects a root hash mismatch and regional-suffix variants without fallback", async () => {
    const mismatch = fixture();
    mismatch.head[0x140] ^= 1;
    await rejects(mismatch, "XCI_ROOT_HASH");
    const regional = fixture();
    digest(Buffer.concat([regional.root.bytes, Buffer.from([1])])).copy(regional.head, 0x140);
    await rejects(regional, "XCI_ROOT_HASH");
  });

  test("requires the declared root header size to match the complete HFS0 index", async () => {
    const image = fixture();
    image.head.writeBigUInt64LE(16n, 0x138);
    digest(image.root.bytes.subarray(0, 16)).copy(image.head, 0x140);
    await rejects(image, "XCI_ROOT_SIZE");
  });

  test("rejects corrupt partition prefix hashes, including partitions not selected for output", async () => {
    const image = fixture({
      partitions: [
        { name: "normal", contents: hfs([payload("other.nca")]) },
        { name: "secure", contents: hfs([payload("program.nca")]) },
      ],
    });
    image.root.entries[0].contents.bytes[20] ^= 1;
    await rejects(image, "HFS0_PARTITION_HASH");
  });

  test("checks member hashes even in non-secure partitions and ignored secure files", async () => {
    for (const name of ["normal", "secure"]) {
      const normalContent = Buffer.alloc(32, 0xa5);
      const secureContent = Buffer.alloc(32, 0xa5);
      const image = fixture({
        partitions: [
          { name: "normal", contents: hfs([{ name: "ignore.cert", contents: normalContent }]) },
          {
            name: "secure",
            contents: hfs([
              payload("program.nca"),
              { name: "ignore.cert", contents: secureContent },
            ]),
          },
        ],
      });
      (name === "normal" ? normalContent : secureContent)[0] ^= 1;
      await rejects(image, "HFS0_MEMBER_HASH");
    }
  });

  for (const location of ["root", "partition"]) {
    for (const [label, mutate, code] of [
      ["magic", (bytes) => bytes.write("PFS0"), "HFS0_MAGIC"],
      ["header reserved", (bytes) => bytes.writeUInt32LE(1, 12), "HFS0_RESERVED"],
      ["entry reserved", (bytes) => bytes.writeBigUInt64LE(1n, 16 + 24), "HFS0_RESERVED"],
      ["enormous count", (bytes) => bytes.writeUInt32LE(0xffffffff, 4), "HFS0_LIMIT"],
      ["enormous strings", (bytes) => bytes.writeUInt32LE(0xffffffff, 8), "HFS0_LIMIT"],
      ["outside string offset", (bytes) => bytes.writeUInt32LE(0xffffffff, 16 + 16), "HFS0_NAME"],
      ["interior string offset", (bytes) => bytes.writeUInt32LE(1, 16 + 16), "HFS0_NAME"],
      ["outside member offset", (bytes) => bytes.writeBigUInt64LE(1n << 63n, 16), "HFS0_RANGE"],
      ["outside member size", (bytes) => bytes.writeBigUInt64LE(1n << 63n, 16 + 8), "HFS0_RANGE"],
      ["missing hash prefix", (bytes) => bytes.writeUInt32LE(0, 16 + 20), "HFS0_HASH_RANGE"],
      ["hash beyond member", (bytes) => bytes.writeUInt32LE(1000000, 16 + 20), "HFS0_HASH_RANGE"],
    ]) {
      test(`rejects ${location} HFS0 ${label}`, async () => {
        const image = fixture();
        const bytes = location === "root" ? image.root.bytes : image.root.entries[0].contents.bytes;
        mutate(bytes);
        if (location === "root") image.resealRoot();
        else image.resealPartitions();
        await rejects(image, code);
      });
    }
  }

  test("rejects an index that extends outside its partition even when inside the full file", async () => {
    const image = fixture({
      partitions: [
        { name: "normal", contents: hfs([payload("normal.cert")]) },
        { name: "secure", contents: hfs([payload("program.nca")]) },
      ],
    });
    const normal = image.root.entries[0];
    image.root.bytes.writeBigUInt64LE(16n, normal.position + 8);
    image.root.bytes.writeUInt32LE(16, normal.position + 20);
    image.resealPartitions();
    await rejects(image, "HFS0_TRUNCATED");
  });

  test("rejects a member that extends outside its partition even when inside the full file", async () => {
    const normal = hfs([payload("normal.cert")]);
    const image = fixture({
      partitions: [
        { name: "normal", contents: normal },
        { name: "secure", contents: hfs([payload("program.nca")]) },
      ],
    });
    normal.bytes.writeBigUInt64LE(33n, 16 + 8);
    image.resealPartitions();
    await rejects(image, "HFS0_RANGE");
  });

  test("rejects overlapping root partition and nested member extents", async () => {
    const image = fixture({
      partitions: [
        { name: "normal", contents: hfs([payload("normal.cert")]) },
        { name: "secure", contents: hfs([payload("program.nca")]) },
      ],
    });
    image.root.bytes.writeBigUInt64LE(1n, 16 + 64);
    image.resealRoot();
    await rejects(image, "HFS0_OVERLAP");
    const nested = fixture({ secureMembers: [payload("one.nca"), payload("two.nca")] });
    nested.root.entries[0].contents.bytes.writeBigUInt64LE(1n, 16 + 64);
    nested.resealPartitions();
    await rejects(nested, "HFS0_OVERLAP");
  });

  test("accepts disjoint out-of-order member offsets without truncating the selected ranges", async () => {
    const image = fixture({
      secureMembers: [
        { ...payload("later.nca"), offset: 32n },
        { ...payload("earlier.nca"), offset: 0n },
      ],
    });
    const result = await inspectXci(image.file);
    assert.equal(result.entries[0].offset - result.entries[1].offset, 32n);
  });

  for (const name of [
    "../bad.nca",
    "bad\\file.nca",
    "bad:nca",
    "CON.nca",
    "bad. ",
    "x\u0001.nca",
    "e\u0301.nca",
  ]) {
    test(`rejects unsafe filename ${JSON.stringify(name)}`, async () => {
      await rejects(fixture({ secureMembers: [payload(name)] }), "UNSAFE_NAME");
    });
  }

  test("rejects empty, oversized, unterminated, and invalid UTF-8 filenames", async () => {
    for (const name of ["", "x".repeat(XCI_INPUT_LIMITS.filenameBytes + 1)]) {
      await rejects(fixture({ secureMembers: [payload(name)] }), "HFS0_NAME");
    }
    const unterminated = fixture();
    const index = unterminated.root.entries[0].contents.bytes;
    index.fill(0x61, 16 + 64);
    unterminated.resealPartitions();
    await rejects(unterminated, "HFS0_NAME");
    const utf8 = fixture();
    utf8.root.entries[0].contents.bytes[16 + 64] = 0xff;
    utf8.resealPartitions();
    await rejects(utf8, "HFS0_NAME");
  });

  test("rejects case-folded duplicate members and duplicate partitions", async () => {
    await rejects(
      fixture({ secureMembers: [payload("game.nca"), payload("GAME.NCA")] }),
      "DUPLICATE_NAME",
    );
    await rejects(
      fixture({
        partitions: [
          { name: "secure", contents: hfs([payload("one.nca")]) },
          { name: "secure", contents: hfs([payload("two.nca")]) },
        ],
      }),
      "DUPLICATE_NAME",
    );
  });

  test("rejects missing, unknown, or incorrectly cased secure partition layouts", async () => {
    for (const name of ["normal", "unknown", "Secure"]) {
      await rejects(
        fixture({ partitions: [{ name, contents: hfs([payload("program.nca")]) }] }),
        "XCI_PARTITIONS",
      );
    }
    await rejects(fixture({ partitions: [] }), "HFS0_LIMIT");
    await rejects(
      fixture({
        partitions: ["normal", "update", "secure", "logo", "extra"].map((name) => ({
          name,
          contents: hfs([]),
        })),
      }),
      "HFS0_LIMIT",
    );
  });

  test("rejects secure partitions without an NCA and empty selected members", async () => {
    await rejects(fixture({ secureMembers: [] }), "XCI_CONTENT");
    await rejects(fixture({ secureMembers: [payload("rights.tik")] }), "XCI_CONTENT");
    await rejects(fixture({ secureMembers: [payload("program.nca", 0)] }), "XCI_EMPTY_MEMBER");
    await rejects(
      fixture({ secureMembers: [payload("program.nca"), payload("rights.tik", 0)] }),
      "XCI_EMPTY_MEMBER",
    );
  });

  test("explicitly rejects compressed secure members, including mixed NCA/NCZ inputs", async () => {
    for (const name of ["program.ncz", "PROGRAM.NCZ", "metadata.cnmt.ncz"]) {
      await rejects(
        fixture({ secureMembers: [payload("plain.nca"), payload(name)] }),
        "XCI_COMPRESSED",
      );
    }
  });

  test("rejects oversized root or member hash prefixes before trying an oversized allocation/read", async () => {
    const huge = BigInt(XCI_INPUT_LIMITS.hashPrefixBytes + 1);
    const root = fixture({ secureMembers: [{ ...payload("large.nca"), size: huge * 2n }] });
    root.head.writeBigUInt64LE(huge, 0x138);
    await rejects(root, "HFS0_HASH_LIMIT");
    assert.equal(root.reads.length, 1);
    const member = fixture({ secureMembers: [{ ...payload("large.nca"), size: huge * 2n }] });
    member.root.entries[0].contents.bytes.writeUInt32LE(Number(huge), 16 + 20);
    member.resealPartitions();
    await rejects(member, "HFS0_HASH_LIMIT");
    assert(member.reads.every((read) => read.end - read.start <= 512));
  });

  test("caps aggregate declared hash work, including many individually bounded prefixes", async () => {
    const members = Array.from({ length: 5 }, (_, i) => ({
      ...payload(`data-${i}.nca`),
      size: BigInt(XCI_INPUT_LIMITS.hashPrefixBytes),
    }));
    const image = fixture({ secureMembers: members });
    const index = image.root.entries[0].contents.bytes;
    for (let i = 0; i < members.length; i++) {
      index.writeUInt32LE(XCI_INPUT_LIMITS.hashPrefixBytes, 16 + i * 64 + 20);
    }
    image.resealPartitions();
    await rejects(image, "HFS0_HASH_LIMIT");
    assert(image.reads.every((read) => read.end - read.start <= 512));
  });

  test("caps aggregate index bytes across root and partition before reading the index", async () => {
    const image = fixture({
      secureMembers: [{ ...payload("program.nca"), size: BigInt(XCI_INPUT_LIMITS.indexBytes) }],
    });
    const secure = image.root.entries[0].contents;
    // 分区索引本身虽未超限，但根索引耗尽了剩余预算。
    // The partition index alone would fit the limit; the root index exhausts the remainder.
    secure.bytes.writeUInt32LE(XCI_INPUT_LIMITS.indexBytes - 16 - 64, 8);
    image.resealPartitions();
    await rejects(image, "HFS0_LIMIT");
    assert(image.reads.every((read) => read.end - read.start <= 512));
  });

  test("caps aggregate entry count before allocating a partition table", async () => {
    const image = fixture({
      secureMembers: [{ ...payload("program.nca"), size: BigInt(XCI_INPUT_LIMITS.indexBytes) }],
    });
    image.root.entries[0].contents.bytes.writeUInt32LE(XCI_INPUT_LIMITS.entries, 4);
    image.resealPartitions();
    await rejects(image, "HFS0_LIMIT");
    assert(image.reads.every((read) => read.end - read.start <= 512));
  });

  test("fails closed when the browser SHA-256 operation rejects", async (context) => {
    context.mock.method(globalThis.crypto.subtle, "digest", async () => {
      throw new Error("SHA-256 unavailable");
    });
    await rejects(fixture(), "XCI_CRYPTO");
  });

  test("normalizes short-read and rejected local read failures", async () => {
    const image = fixture();
    image.file.slice = () => ({ arrayBuffer: async () => new ArrayBuffer(1) });
    await rejects(image, "SHORT_READ");
    image.file.slice = () => ({
      async arrayBuffer() {
        throw new Error("local failure");
      },
    });
    await rejects(image, "READ_FAILED");
  });

  test("package preflight dispatches XCI by content and rejects unknown supported-name formats", async () => {
    const image = fixture();
    const file = (name, bytes) =>
      Object.assign(new Blob([bytes]), { name, webkitRelativePath: `chosen/${name}` });
    const prod = file("prod.keys", `header_key = ${"a5".repeat(32)}\n`);
    const bytes = range(image.segments, 0n, image.file.size);
    for (const name of ["game.xci", "game.nsp"]) {
      const result = await preflightNspFolder([prod, file(name, bytes)]);
      assert.equal(result.packages.length, 1);
      assert.equal(result.packages[0].format, "XCI");
      assert.equal(result.packages[0].entries[0].name, "program.nca");
      assert.equal(result.stage, "metadata-only");
      assert.equal(result.canBuildIpa, false);
      await assert.rejects(preflightNspFolder([prod, file(name, new Uint8Array(512))]), {
        code: "PACKAGE_FORMAT",
      });
    }
  });
}

// romfs-tree
{
  // 验证 romfs-tree 的约定与边界；依据 ../romfs-tree.mjs 及本文件的测试断言。
  // Verify romfs-tree contracts and boundaries against ../romfs-tree.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { indexRomfs, ROMFS_LIMITS } = romfsTreeModule;

  const EMPTY = 0xffffffff;
  const encoder = new TextEncoder();
  const align = (n) => Math.ceil(n / 4) * 4;
  function fixture({ dataOffset = 256n, size = 8n, name = "hello.bin", directory = null } = {}) {
    const encoded = encoder.encode(name);
    const dirName = encoder.encode(directory ?? "");
    const dirSize = directory === null ? 24 : 48 + align(dirName.length);
    const dirStart = 84;
    const fileHashStart = dirStart + dirSize;
    const fileStart = fileHashStart + 4;
    const fileSize = 32 + align(encoded.length);
    const bytes = new Uint8Array(fileStart + fileSize);
    const view = new DataView(bytes.buffer);
    const set32 = (p, v) => view.setUint32(p, v, true);
    const set64 = (p, v) => view.setBigUint64(p, BigInt(v), true);
    [80, 80, 4, dirStart, dirSize, fileHashStart, 4, fileStart, fileSize, dataOffset].forEach(
      (v, i) => set64(i * 8, v),
    );
    set32(80, 0);
    set32(fileHashStart, 0);
    set32(dirStart, 0);
    set32(dirStart + 4, EMPTY);
    set32(dirStart + 8, directory === null ? EMPTY : 24);
    set32(dirStart + 12, directory === null ? 0 : EMPTY);
    set32(dirStart + 16, EMPTY);
    if (directory !== null) {
      const p = dirStart + 24;
      set32(p, 0);
      set32(p + 4, EMPTY);
      set32(p + 8, EMPTY);
      set32(p + 12, 0);
      set32(p + 16, EMPTY);
      set32(p + 20, dirName.length);
      bytes.set(dirName, p + 24);
    }
    set32(fileStart, directory === null ? 0 : 24);
    set32(fileStart + 4, EMPTY);
    set64(fileStart + 8, 0);
    set64(fileStart + 16, size);
    set32(fileStart + 24, EMPTY);
    set32(fileStart + 28, encoded.length);
    bytes.set(encoded, fileStart + 32);
    const reads = [];
    const source = {
      size: dataOffset + size,
      async readAt(offset, count) {
        reads.push([offset, count]);
        assert.ok(count <= ROMFS_LIMITS.readBytes);
        if (offset < BigInt(bytes.length))
          return bytes.slice(Number(offset), Number(offset) + count);
        assert.ok(offset >= dataOffset, "no metadata gap/hash table scanning");
        return new Uint8Array(count).fill(0x5a);
      },
    };
    return {
      source,
      bytes,
      view,
      set32,
      set64,
      reads,
      dirStart,
      fileStart,
      fileHashStart,
      dataOffset,
    };
  }

  test("indexes nested synthetic RomFS without reading payload; lazy bounded reads retain BigInt offsets", async () => {
    const f = fixture({ directory: "rom", dataOffset: 2n ** 54n, size: 2n ** 40n });
    const index = await indexRomfs(f.source);
    assert.deepEqual(index.directories, ["", "rom"]);
    assert.equal(index.files.length, 1);
    assert.ok(f.reads.every(([offset]) => offset < 256n));
    const member = index.getFile("rom/hello.bin");
    assert.equal(member.size, 2n ** 40n);
    assert.ok(Object.isFrozen(index) && Object.isFrozen(index.files) && Object.isFrozen(member));
    assert.deepEqual(await member.readAt(2n ** 39n, 3), new Uint8Array([90, 90, 90]));
    assert.deepEqual(f.reads.at(-1), [2n ** 54n + 2n ** 39n, 3]);
    await assert.rejects(member.readAt(0n, 65537), /read limit/);
    await assert.rejects(member.readAt(member.size, 1), /out of bounds/);
    await assert.rejects(member.readAt(-1n, 1), /out of bounds/);
    await assert.rejects(member.readAt(0, 1), /read limit/);
    assert.equal((await member.readAt(member.size, 0)).length, 0);
  });

  test("indexes RomFS metadata stored after file data and rejects overlap", async () => {
    const original = fixture({ dataOffset: 512n, size: 8n });
    const metadataStart = 520;
    const bytes = new Uint8Array(metadataStart + original.bytes.length - 80);
    bytes.set(original.bytes.subarray(0, 80));
    bytes.fill(0x5a, 512, metadataStart);
    bytes.set(original.bytes.subarray(80), metadataStart);
    const view = new DataView(bytes.buffer);
    for (const at of [8, 24, 40, 56])
      view.setBigUint64(at, view.getBigUint64(at, true) + BigInt(metadataStart - 80), true);
    const source = {
      size: BigInt(bytes.length),
      readAt: async (offset, count) => bytes.slice(Number(offset), Number(offset) + count),
    };
    const index = await indexRomfs(source);
    assert.equal(index.files.length, 1);
    assert.deepEqual(await index.files[0].readAt(0n, 8), new Uint8Array(8).fill(0x5a));
    view.setBigUint64(metadataStart + original.fileStart - 80 + 16, 12n, true);
    await assert.rejects(indexRomfs(source), /file data and metadata overlap/);
  });

  test("rejects malformed header/table sizes before bulk reads", async () => {
    for (const change of [
      (f) => f.set64(0, 79),
      (f) => f.set64(24, 85),
      (f) => f.set64(56, 84),
      (f) => f.set64(72, 85),
      (f) => {
        f.set64(32, ROMFS_LIMITS.metadataBytes);
        f.set64(72, 2n ** 40n);
        f.source.size = 2n ** 41n;
      },
    ]) {
      const f = fixture();
      change(f);
      await assert.rejects(indexRomfs(f.source), /Invalid RomFS/);
      assert.ok(f.reads.length <= 1);
    }
    await assert.rejects(indexRomfs({ size: 80, readAt() {} }), /reader contract/);
    const f = fixture();
    f.source.readAt = async () => new Uint8Array(0);
    await assert.rejects(indexRomfs(f.source), /short read/);
  });

  test("rejects cycles, bad parents, invalid links and unreachable records", async () => {
    for (const change of [
      (f) => f.set32(f.dirStart + 8, 0),
      (f) => f.set32(f.dirStart + 4, 0),
      (f) => f.set32(f.dirStart + 8, 3),
      (f) => f.set32(f.dirStart + 8, 24),
      (f) => f.set32(f.fileStart + 4, 0),
      (f) => f.set32(f.fileStart, 24),
      (f) => f.set32(f.fileStart + 4, 4),
      (f) => f.set32(f.dirStart + 12, EMPTY),
    ]) {
      const f = fixture();
      change(f);
      await assert.rejects(indexRomfs(f.source), /Invalid RomFS/);
    }
  });

  test("rejects unsafe names, oversized names and invalid UTF-8", async () => {
    for (const name of [
      "",
      ".",
      "..",
      "../x",
      "a/b",
      "a\\b",
      "C:x",
      "nul.txt",
      "x ",
      "x.",
      "x\0y",
      "e\u0301",
    ]) {
      await assert.rejects(indexRomfs(fixture({ name }).source), /Invalid RomFS/);
    }
    const f = fixture();
    f.bytes[f.fileStart + 32] = 0xff;
    await assert.rejects(indexRomfs(f.source), /UTF-8/);
    const large = fixture();
    large.set32(large.fileStart + 28, 256);
    await assert.rejects(indexRomfs(large.source), /name limit/);
  });

  test("rejects out-of-image data and metadata/name extents", async () => {
    const f = fixture();
    f.set64(f.fileStart + 16, 100);
    await assert.rejects(indexRomfs(f.source), /file data out of bounds/);
    const n = fixture();
    n.set32(n.fileStart + 28, 255);
    await assert.rejects(indexRomfs(n.source), /record name out of bounds/);
  });

  test("checks cancellation before/after asynchronous reads and on lazy reads", async () => {
    const controller = new AbortController();
    controller.abort(new Error("cancelled"));
    const f = fixture();
    await assert.rejects(indexRomfs(f.source, { signal: controller.signal }), /cancelled/);
    assert.equal(f.reads.length, 0);
    const next = new AbortController();
    const original = f.source.readAt;
    f.source.readAt = async (...args) => {
      const bytes = await original(...args);
      next.abort(new Error("during read"));
      return bytes;
    };
    await assert.rejects(indexRomfs(f.source, { signal: next.signal }), /during read/);
    const index = await indexRomfs(fixture().source);
    await assert.rejects(index.files[0].readAt(0n, 1, { signal: controller.signal }), /cancelled/);
    const lifetime = new AbortController();
    const lifetimeIndex = await indexRomfs(fixture().source, { signal: lifetime.signal });
    lifetime.abort(new Error("closed"));
    await assert.rejects(lifetimeIndex.files[0].readAt(0n, 1), /closed/);
  });

  for (const bufferSubclass of [false, true])
    test(`copies reusable reader ${bufferSubclass ? "Buffer" : "Uint8Array"} without aliasing prior results`, async () => {
      const f = fixture();
      const original = f.source.readAt;
      const scratch = bufferSubclass ? Buffer.from([1, 2, 3]) : new Uint8Array([1, 2, 3]);
      f.source.readAt = (offset, count) =>
        offset >= f.dataOffset ? scratch : original(offset, count);
      const index = await indexRomfs(f.source);
      const result = await index.files[0].readAt(0n, 3);
      scratch.fill(0);
      assert.deepEqual(result, new Uint8Array([1, 2, 3]));
    });

  function multipleFiles({ secondName = "other.bin", secondOffset = 8n } = {}) {
    const f = fixture({ dataOffset: 512n, size: 16n });
    const firstSize = f.bytes.length - f.fileStart;
    const name = encoder.encode(secondName);
    const bytes = new Uint8Array(f.bytes.length + 32 + align(name.length));
    bytes.set(f.bytes);
    const view = new DataView(bytes.buffer);
    view.setBigUint64(64, BigInt(bytes.length - f.fileStart), true);
    view.setUint32(f.fileStart + 4, firstSize, true);
    view.setBigUint64(f.fileStart + 16, 8n, true);
    const second = f.bytes.length;
    view.setUint32(second + 4, EMPTY, true);
    view.setBigUint64(second + 8, secondOffset, true);
    view.setBigUint64(second + 16, 8n, true);
    view.setUint32(second + 24, EMPTY, true);
    view.setUint32(second + 28, name.length, true);
    bytes.set(name, second + 32);
    return {
      size: 528n,
      readAt: async (offset, count) => bytes.slice(Number(offset), Number(offset) + count),
    };
  }

  test("rejects duplicate/case-colliding paths and overlapping nonempty payloads", async () => {
    const normal = await indexRomfs(multipleFiles());
    assert.equal(normal.files.length, 2);
    for (const secondName of ["hello.bin", "HELLO.BIN"])
      await assert.rejects(indexRomfs(multipleFiles({ secondName })), /duplicate path/);
    await assert.rejects(indexRomfs(multipleFiles({ secondOffset: 7n })), /file data overlap/);
    // 根级同名文件和目录共用一个命名空间。
    // Root-level file and directory with the same name share one namespace.
    const f = fixture({ directory: "hello.bin", name: "hello.bin" });
    f.set32(f.dirStart + 12, 0);
    f.set32(f.dirStart + 24 + 12, EMPTY);
    f.set32(f.fileStart, 0);
    await assert.rejects(indexRomfs(f.source), /duplicate path/);
  });

  function directoryChain(depth, nameLength = 1) {
    const recordSize = 24 + align(nameLength);
    const tableSize = 24 + recordSize * depth;
    const dirStart = 84,
      hashStart = dirStart + tableSize,
      end = hashStart + 4;
    const bytes = new Uint8Array(end),
      view = new DataView(bytes.buffer);
    [80, 80, 4, dirStart, tableSize, hashStart, 4, end, 0, end].forEach((v, i) =>
      view.setBigUint64(i * 8, BigInt(v), true),
    );
    for (let i = 0; i <= depth; i++) {
      const offset = i === 0 ? 0 : 24 + (i - 1) * recordSize;
      const parent = i <= 1 ? 0 : 24 + (i - 2) * recordSize;
      const start = dirStart + offset;
      view.setUint32(start, parent, true);
      view.setUint32(start + 4, EMPTY, true);
      view.setUint32(start + 8, i === depth ? EMPTY : i === 0 ? 24 : offset + recordSize, true);
      view.setUint32(start + 12, EMPTY, true);
      view.setUint32(start + 16, EMPTY, true);
      view.setUint32(start + 20, i === 0 ? 0 : nameLength, true);
      if (i) bytes.fill(0x61, start + 24, start + 24 + nameLength);
    }
    return {
      size: BigInt(end),
      readAt: async (offset, count) => bytes.slice(Number(offset), Number(offset) + count),
    };
  }

  test("rejects deep and overlong paths without recursive traversal", async () => {
    assert.equal((await indexRomfs(directoryChain(64))).directories.length, 65);
    await assert.rejects(indexRomfs(directoryChain(65)), /depth limit/);
    await assert.rejects(indexRomfs(directoryChain(17, 255)), /path budget/);
  });

  // 所有四个元数据表均不得与非空文件相交；零长度范围仍不占用空间。
  // All four metadata tables reject nonempty file intersections; empty extents still occupy no space.
  test("file extents reject each metadata table, but allow empty tables and empty files", async () => {
    for (const tableAt of [8, 24, 40, 56]) {
      const f = fixture({ dataOffset: 80n, size: 1n });
      f.source.size = 1024n;
      const tableStart = f.view.getBigUint64(tableAt, true);
      f.set64(f.fileStart + 8, tableStart - 80n);
      await assert.rejects(indexRomfs(f.source), /file data and metadata overlap/);
      f.set64(f.fileStart + 16, 0n);
      assert.equal((await indexRomfs(f.source)).files[0].size, 0n);
    }
    const f = fixture({ dataOffset: 256n });
    f.set64(8, 260n);
    f.set64(16, 0n);
    f.set64(40, 260n);
    f.set64(48, 0n);
    assert.equal((await indexRomfs(f.source)).files.length, 1);
  });
}

// nso-validation
{
  // 验证 nso-validation 的约定与边界；依据 ../nso-validation.mjs、../preflight.mjs 及本文件的测试断言。
  // Verify nso-validation contracts and boundaries against ../nso-validation.mjs、../preflight.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { decodeLz4Block, validateNsoSegments } = nsoValidationModule;
  const { MAX_READ_BYTES } = preflightModule;

  const hex = (value) => Uint8Array.from(Buffer.from(value, "hex"));
  const text = (value) => new TextEncoder().encode(value);
  const abcHash = hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const emptyHash = hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  function fixture(flags = 0x38, content = [text("abc"), text("abc"), text("abc")]) {
    const stored = content.map((bytes, i) =>
      flags & (1 << i) ? Uint8Array.of(bytes.length << 4, ...bytes) : bytes,
    );
    const bytes = new Uint8Array(0x100 + stored.reduce((n, s) => n + s.length, 0));
    bytes.set(text("NSO0"));
    const view = new DataView(bytes.buffer);
    view.setUint32(0x0c, flags, true);
    let offset = 0x100;
    for (let i = 0; i < 3; i++) {
      view.setUint32(0x10 + i * 16, offset, true);
      view.setUint32(0x14 + i * 16, i * 0x1000, true);
      view.setUint32(0x18 + i * 16, content[i].length, true);
      view.setUint32(0x60 + i * 4, stored[i].length, true);
      bytes.set(content[i].length ? abcHash : emptyHash, 0xa0 + i * 32);
      bytes.set(stored[i], offset);
      offset += stored[i].length;
    }
    const reads = [];
    return {
      bytes,
      view,
      reads,
      source: {
        size: bytes.length,
        read: async (offset, length) => {
          reads.push([offset, length]);
          return bytes.slice(offset, offset + length);
        },
      },
    };
  }

  test("independent liblz4 1.10.0 vectors decode overlapping offsets 1 and 3", async () => {
    // 由系统 liblz4 的 LZ4_compress_default 生成，不是本实现生成。
    // Produced with system liblz4 LZ4_compress_default, not this implementation.
    assert.deepEqual(await decodeLz4Block(hex("1f61010007506161616161"), 32), text("a".repeat(32)));
    assert.deepEqual(
      await decodeLz4Block(hex("3f61626303003f506263616263"), 90),
      text("abc".repeat(30)),
    );
  });

  test("LZ4 specification literal length extension examples and empty block", async () => {
    for (const [size, prefix] of [
      [15, [0xf0, 0]],
      [48, [0xf0, 33]],
      [280, [0xf0, 255, 10]],
    ]) {
      const literal = Uint8Array.from({ length: size }, (_, i) => i & 255);
      assert.deepEqual(await decodeLz4Block(Uint8Array.of(...prefix, ...literal), size), literal);
    }
    assert.deepEqual(await decodeLz4Block(Uint8Array.of(0), 0), new Uint8Array());
  });

  test("LZ4 rejects malformed, truncated, overflow and missing terminal sequences", async () => {
    for (const [input, size] of [
      [[], 0],
      [[0xf0], 15],
      [[0x30, 97], 3],
      [[0x10, 97, 0, 0], 5],
      [[0x10, 97, 2, 0], 5],
      [[0x10, 97, 1], 5],
      [[0x1f, 97, 1, 0], 30],
      [[0x10, 97, 1, 0], 5],
      [[0x10, 97, 1, 0, 0], 5],
      [[0x20, 97, 98], 1],
      [[0x10, 97], 2],
      [[0x1f, 97, 1, 0, 255], 8],
    ]) {
      await assert.rejects(decodeLz4Block(Uint8Array.from(input), size));
    }
    await assert.rejects(
      decodeLz4Block(Uint8Array.of(0), 100, { limits: { maxSegmentBytes: 99 } }),
      /budget/,
    );
  });

  test("all legacy compression and hash flag combinations are independent", async () => {
    for (let flags = 0; flags < 64; flags++) {
      const f = fixture(flags);
      const result = await validateNsoSegments(f.source);
      assert.equal(result.supportedGameBuild, false);
      assert.equal(result.versionVerified, false);
      assert.equal(result.allSegmentsHashed, (flags & 0x38) === 0x38);
      result.segments.forEach((s, i) => {
        assert.deepEqual(s.bytes, text("abc"));
        assert.equal(s.hashRequired, Boolean(flags & (1 << (i + 3))));
        assert.equal(s.integrity, s.hashRequired ? "verified" : "not-requested");
      });
    }
  });

  test("all three required hashes reject tampering; unrequested hashes are not verified", async () => {
    for (let i = 0; i < 3; i++) {
      const f = fixture();
      f.bytes[0xa0 + i * 32] ^= 1;
      await assert.rejects(validateNsoSegments(f.source), /SHA-256 mismatch/);
    }
    const f = fixture(0);
    f.bytes[0xa0] ^= 1;
    assert.equal((await validateNsoSegments(f.source)).segments[0].integrity, "not-requested");
    const empty = fixture(0x3f, [new Uint8Array(), new Uint8Array(), new Uint8Array()]);
    assert.equal((await validateNsoSegments(empty.source)).allSegmentsHashed, true);
  });

  test("unknown module ID never becomes supported, even with valid hashes", async () => {
    const f = fixture();
    f.bytes.fill(0xab, 0x40, 0x60);
    const r = await validateNsoSegments(f.source);
    assert.equal(r.moduleId, "ab".repeat(32));
    assert.equal(r.supportedGameBuild, false);
  });

  test("budgets and malformed metadata reject before payload reads", async () => {
    for (const limits of [
      { maxSegmentBytes: 2 },
      { maxTotalDecodedBytes: 8 },
      { maxWorkingBytes: 100 },
      { maxImageBytes: 4096 },
    ]) {
      const f = fixture();
      await assert.rejects(validateNsoSegments(f.source, { limits }), /budget/);
      assert.deepEqual(f.reads, [[0, 256]]);
    }
    for (const [offset, value] of [
      [0x0c, 0x40],
      [0x04, 1],
      [0x20, 0x100],
      [0x24, 0],
      [0x18, 0xffffffff],
    ]) {
      const f = fixture();
      f.view.setUint32(offset, value, true);
      await assert.rejects(validateNsoSegments(f.source));
      assert.deepEqual(f.reads, [[0, 256]]);
    }
    await assert.rejects(
      validateNsoSegments(fixture().source, { limits: { chunkBytes: MAX_READ_BYTES + 1 } }),
      /Chunk/,
    );
    await assert.rejects(
      validateNsoSegments(fixture().source, { limits: { maxSegmentBytes: Infinity } }),
      /Invalid/,
    );
  });

  test("BSS is bounded and cannot overlap another segment", async () => {
    const f = fixture();
    f.view.setUint32(0x34, 0x100, true);
    f.view.setUint32(0x3c, 0x2000, true);
    await assert.rejects(validateNsoSegments(f.source), /BSS overlaps/);
  });

  test("payload reads are chunked and short reads rejected", async () => {
    const f = fixture();
    await validateNsoSegments(f.source, { limits: { chunkBytes: 2 } });
    assert.ok(f.reads.slice(1).every(([, length]) => length <= 2));
    const bad = fixture();
    bad.source.read = async () => new Uint8Array(1);
    await assert.rejects(validateNsoSegments(bad.source), /Short or invalid/);
  });

  test("cancellation before reads and during LZ4 chunks", async () => {
    const controller = new AbortController();
    controller.abort();
    const f = fixture();
    await assert.rejects(validateNsoSegments(f.source, { signal: controller.signal }), {
      name: "AbortError",
    });
    assert.equal(f.reads.length, 0);
    const active = new AbortController();
    const promise = decodeLz4Block(hex("1f61010007506161616161"), 32, {
      signal: active.signal,
      limits: { chunkBytes: 1 },
    });
    setTimeout(() => active.abort(), 0);
    await assert.rejects(promise, { name: "AbortError" });
  });

  test("cancellation during payload reads is honored", async () => {
    const f = fixture(),
      controller = new AbortController();
    const read = f.source.read;
    f.source.read = async (offset, length) => {
      const bytes = await read(offset, length);
      if (offset >= 256) controller.abort();
      return bytes;
    };
    await assert.rejects(validateNsoSegments(f.source, { signal: controller.signal }), {
      name: "AbortError",
    });
  });

  test("cancellation during WebCrypto rejects without waiting for digest", async (t) => {
    const controller = new AbortController();
    t.mock.method(globalThis.crypto.subtle, "digest", () => {
      setTimeout(() => controller.abort(), 0);
      return new Promise(() => {});
    });
    await assert.rejects(validateNsoSegments(fixture().source, { signal: controller.signal }), {
      name: "AbortError",
    });
  });

  test("compressed declared expansion is budgeted before payload allocation/read", async () => {
    const f = fixture(0x3f);
    f.view.setUint32(0x18, 0x10000000, true);
    f.view.setUint32(0x24, 0x20000000, true);
    f.view.setUint32(0x34, 0x30000000, true);
    await assert.rejects(validateNsoSegments(f.source), /budget/);
    assert.deepEqual(f.reads, [[0, 256]]);
  });

  test("many short LZ4 sequences yield by accumulated work, not per sequence", async (t) => {
    // 一千个字面量 a、偏移 1、长度四字节的匹配，随后十二个尾部字面量；刻意使用短序列的有效手写块，未使用编码器。
    // 1,000 literal-a / offset-1 / four-byte matches, then 12 final literals.
    // A valid handcrafted block with deliberately short sequences, no encoder.
    const sequences = 1000;
    const input = new Uint8Array(sequences * 4 + 13);
    for (let i = 0; i < sequences; i++) input.set([0x10, 0x61, 1, 0], i * 4);
    input.set([0xc0, ...new Uint8Array(12).fill(0x61)], sequences * 4);
    const originalTimer = globalThis.setTimeout;
    let timerCalls = 0;
    t.mock.method(globalThis, "setTimeout", (...args) => {
      timerCalls++;
      return originalTimer(...args);
    });
    const result = await decodeLz4Block(input, sequences * 5 + 12, { limits: { chunkBytes: 256 } });
    assert.deepEqual(result, new Uint8Array(sequences * 5 + 12).fill(0x61));
    assert.ok(timerCalls > 0, "large decoding must yield for cancellation");
    assert.ok(
      timerCalls <= Math.ceil((input.length + result.length) / 256),
      `Unexpected per-sequence timers: ${timerCalls}`,
    );
  });
}

// preflight
{
  // 验证 preflight 的约定与边界；依据 ../preflight.mjs 及本文件的测试断言。
  // Verify preflight contracts and boundaries against ../preflight.mjs and this file's assertions.
  const { readFileSync } = nodeFsModule;
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const {
    safeRelativePath,
    indexFiles,
    readRange,
    inspectNso,
    fileSource,
    MAX_READ_BYTES,
    REQUIRED_NSO_MODULES,
    ZIP_PATH_LIMITS,
  } = preflightModule;

  function fixture() {
    const bytes = new Uint8Array(0x130),
      v = new DataView(bytes.buffer);
    bytes.set([0x4e, 0x53, 0x4f, 0x30]);
    for (let i = 0; i < 3; i++) {
      v.setUint32(0x10 + i * 16, 0x100 + i * 16, true);
      v.setUint32(0x14 + i * 16, i * 0x1000, true);
      v.setUint32(0x18 + i * 16, 16, true);
    }
    return bytes;
  }
  const source = (bytes) => ({ size: bytes.length, read: async (o, n) => bytes.slice(o, o + n) });
  test("accepts safe paths and rejects cross-platform dangerous paths", () => {
    assert.equal(safeRelativePath("ExeFS/main"), "ExeFS/main");
    for (const p of [
      "",
      "/root",
      "../x",
      "a/../x",
      "a//x",
      "C:/x",
      "a\\b",
      "a\0b",
      "x.",
      "x ",
      "CON",
      "aux.txt",
      "e\u0301",
    ]) {
      assert.throws(() => safeRelativePath(p), /Unsafe/);
    }
  });
  test("indexes metadata without reading bytes and rejects duplicates", () => {
    assert.equal(indexFiles([{ path: "RomFS/data/archive.data", size: 6 * 1024 ** 3 }]).size, 1);
    assert.throws(
      () =>
        indexFiles([
          { path: "a", size: 1 },
          { path: "A", size: 2 },
        ]),
      /Conflicting/,
    );
    assert.throws(
      () =>
        indexFiles([
          { path: "a", size: 1 },
          { path: "a/b", size: 2 },
        ]),
      /conflict/,
    );
    assert.throws(() => indexFiles([{ path: "a", size: NaN }]), /size/);
  });
  test("implicit directories retain one case spelling regardless of entry order", () => {
    for (const paths of [
      ["Resources/a", "resources/b"],
      ["Resources/nested/a", "Resources/Nested/b"],
      ["Éléments/a", "éléments/b"],
    ]) {
      for (const order of [paths, [...paths].reverse()])
        assert.throws(() => indexFiles(order.map((path) => ({ path, size: 0 }))), /conflict/i);
    }
    const files = ["Resources/a", "Resources/b", "Resources/Nested/a", "Resources/Nested/b"].map(
      (path) => ({ path, size: 0 }),
    );
    assert.deepEqual(
      [...indexFiles(files).keys()],
      files.map(({ path }) => path),
    );
    for (const paths of [
      ["Resources/a", "resources"],
      ["Resources", "resources/a"],
    ])
      assert.throws(() => indexFiles(paths.map((path) => ({ path, size: 0 }))), /conflict/i);
  });
  test("optional path-index budgets bound depth and all distinct files and directories", () => {
    const limits = { maxPathDepth: 3, maxPathNodes: 4 };
    const entries = (paths) => paths.map((path) => ({ path, size: 0 }));
    assert.equal(indexFiles(entries(["a/b/c", "a/b/d"]), limits).size, 2);
    assert.throws(() => indexFiles(entries(["a/b/c", "a/b/d", "e"]), limits), /node limit/);
    assert.throws(() => indexFiles(entries(["a/b/c/d"]), limits), /depth limit/);
    assert.throws(
      () => indexFiles(entries(["x/".repeat(2000) + "file"]), ZIP_PATH_LIMITS),
      /depth limit/,
    );
    assert.equal(indexFiles(entries(["x/".repeat(64) + "file"])).size, 1);
    for (const key of ["maxPathDepth", "maxPathNodes"])
      for (const value of [-1, 1.5, NaN, -Infinity, Number.MAX_SAFE_INTEGER + 1, "32", null])
        assert.throws(() => indexFiles([], { [key]: value }), /Invalid path index limit/);
    assert.equal(indexFiles([], { maxPathDepth: 0, maxPathNodes: 0 }).size, 0);
  });
  test("NSO reads only 256 bytes even for virtual multi-GB input", async () => {
    const calls = [];
    const result = await inspectNso({
      size: 6 * 1024 ** 3,
      read: async (o, n) => {
        calls.push([o, n]);
        return fixture().slice(o, o + n);
      },
    });
    assert.deepEqual(calls, [[0, 256]]);
    assert.equal(result.segments.length, 3);
    assert.equal(result.versionVerified, false);
  });
  test("rejects malformed, truncated, out-of-bounds and overlapping NSO", async () => {
    await assert.rejects(inspectNso(source(new Uint8Array(12))), /range/);
    await assert.rejects(inspectNso(source(new Uint8Array(256))), /NSO0/);
    for (const [offset, value] of [
      [4, 1],
      [12, 128],
      [16, 0xfffffff0],
      [0x20, 0x100],
      [0x24, 0],
    ]) {
      const b = fixture();
      new DataView(b.buffer).setUint32(offset, value, true);
      await assert.rejects(inspectNso(source(b)));
    }
  });
  test("read budgets, short reads and cancellation are enforced", async () => {
    await assert.rejects(
      readRange(
        {
          size: 1e9,
          read: () => {
            throw Error("must not read");
          },
        },
        0,
        MAX_READ_BYTES + 1,
      ),
      /range/,
    );
    await assert.rejects(
      readRange({ size: 10, read: async () => new Uint8Array(1) }, 0, 2),
      /Short/,
    );
    const c = new AbortController();
    c.abort();
    await assert.rejects(readRange(source(fixture()), 0, 1, c.signal), { name: "AbortError" });
    const d = new AbortController();
    await assert.rejects(
      readRange(
        {
          size: 1,
          read: async () => {
            d.abort();
            return new Uint8Array(1);
          },
        },
        0,
        1,
        d.signal,
      ),
      { name: "AbortError" },
    );
  });
  test("Blob adapter returns exact slices", async () => {
    assert.deepEqual(
      await readRange(fileSource(new Blob([new Uint8Array([1, 2, 3])])), 1, 2),
      new Uint8Array([2, 3]),
    );
  });

  test("required executable modules match native IPA packaging contract", () => {
    const python = readFileSync(new URL("../../dm/tools/d2rm_pack.py", import.meta.url), "utf8");
    const match = python.match(/^MODS = (\[[^\n]+\])/m);
    assert.ok(match, "native MODS declaration changed; review contract");
    assert.deepEqual(REQUIRED_NSO_MODULES, JSON.parse(match[1].replaceAll("'", '"')));
    assert.equal(Object.isFrozen(REQUIRED_NSO_MODULES), true);
  });
}

// extracted-input
{
  // 验证 extracted-input 的约定与边界；依据 ./helpers/assert-isolated-run.mjs、../extracted-input.mjs 及本文件的测试断言。
  // Verify extracted-input contracts and boundaries against ./helpers/assert-isolated-run.mjs、../extracted-input.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { spawnSync } = nodeChild_processModule;
  const { assertIsolatedRun } = assertIsolatedRunModule;
  const { openExtractedInput } = extractedInputModule;
  test("real release gate rejects authored synthetic bytes and does not accept caller hashes", async () => {
    const input = {
      inputMode: "extracted",
      exefs: [
        { path: "main", file: new Blob(["synthetic main"]) },
        { path: "sdk", file: new Blob(["synthetic sdk"]) },
      ],
      romfs: [{ path: "data/build_manifest.psv", file: new Blob(["header\n"]) }],
      programReleaseId: "switch-3.3.94012-touch-run-v4",
      verified: true,
    };
    await assert.rejects(openExtractedInput(input), { code: "PROGRAM_UNSUPPORTED_VERSION" });
    await assert.rejects(openExtractedInput(input, { contentMode: "warlock" }), {
      code: "EXTRACTED_DLC_UNSUPPORTED",
    });
  });
  test("extracted route uses isolated synthetic trusted catalog to exercise provenance and lifecycle", () => {
    const env = { ...process.env };
    delete env.NODE_TEST_CONTEXT;
    assertIsolatedRun(
      spawnSync(
        process.execPath,
        [
          "--experimental-test-module-mocks",
          "--test",
          "--test-reporter=tap",
          new URL("./fixtures/extracted-input.mock.mjs", import.meta.url).pathname,
        ],
        { env, encoding: "utf8", timeout: 30000, maxBuffer: 1024 * 1024 },
      ),
    );
  });
}

// extraction-job-adapter
{
  // 验证 extraction-job-adapter 的约定与边界；依据 ./helpers/extraction-adapter-fixture.mjs、../extraction-job-adapter.mjs 及本文件的测试断言。
  // Verify extraction-job-adapter contracts and boundaries against ./helpers/extraction-adapter-fixture.mjs、../extraction-job-adapter.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { MockWorker } = extractionWorkerModule;
  const { default: assert } = strictModule;
  const {
    createExtractionJobAdapter,
    readAuthenticatedProgramModules,
    readAuthenticatedAssets,
    readInputContentPolicy,
  } = extractionJobAdapterModule;
  const { D2R_WARLOCK_ID: warlock, resolveD2rExpansionSelection } = d2rExpansionPolicyModule;
  const { fixture, open, selection, inspectedFixture } = extractionAdapterFixtureModule;
  test("stages main and sdk before token transition and returns only plaintext capabilities", async () => {
    const f = fixture();
    const session = await open(f);
    const first = await f.adapter.inspectContents({ session });
    first.records[0].version = 99;
    assert.equal((await f.adapter.inspectContents({ session })).records[0].version, 0n);
    const extracted = await f.adapter.extractSelected({ session, selection });
    assert.deepEqual(Object.keys(extracted).sort(), ["logicalAssets", "programModules", "romfs"]);
    assert.deepEqual(Object.keys(extracted.programModules), ["main", "sdk"]);
    assert.equal(extracted.programModules.main.length, 70000);
    assert.equal(extracted.programModules.sdk.length, 8);
    assert.equal(extracted.programModules.sdk[0], 2);
    const ops = f.worker.requests.map((r) => r.operation);
    assert.deepEqual(ops.slice(0, 6), [
      "initialize",
      "select",
      "readExefs",
      "readExefs",
      "readExefs",
      "openRomfs",
    ]);
    assert.ok(
      f.worker.requests
        .filter((r) => r.operation.startsWith("read"))
        .every((r) => r.payload.count <= 65536),
    );
    assert.deepEqual(extracted.logicalAssets.findUniquePathSuffixes([]), []);
    assert.throws(
      () => extracted.logicalAssets.findUniquePathSuffixes(["/missing.sprite"]),
      /EXTRACTION_ASSET_MISSING/,
    );
    const member = extracted.romfs.getFile("data/build_manifest.psv");
    assert.equal((await member.readAt(0n, 1))[0], 83);
    await session.dispose();
    assert.equal(f.disposed, true);
    assert.equal(f.worker.terminated, true);
    await assert.rejects(member.readAt(0n, 0), /EXTRACTION_DISPOSED/);
    assert.throws(() => extracted.logicalAssets.findUniquePathSuffixes([]), /EXTRACTION_DISPOSED/);
    await assert.rejects(extracted.logicalAssets.read("anything"), /EXTRACTION_DISPOSED/);
    assert.equal(
      extracted.programModules.main[0],
      1,
      "session disposal does not destroy staged generator inputs",
    );
    await f.adapter.cleanup();
    assert.ok(extracted.programModules.main.every((x) => x === 0));
    assert.ok(extracted.programModules.sdk.every((x) => x === 0));
  });
  test("removed content-hash option is absent from initialization and selection", async () => {
    const inspected = inspectedFixture();
    inspected.inventory = inspected.inventory.map((entry) => ({
      ...entry,
      sha256: null,
      hashVerified: false,
    }));
    const selected = resolveD2rExpansionSelection(inspected.records, inspected.inventory, {});
    const f = fixture({ inspected, selected, verifyRomfs: false });
    const session = await open(f);
    const extracted = await f.adapter.extractSelected({ session, selection: selected });
    assert.equal(extracted.romfs.files.length, 1);
    assert.equal(f.worker.requests[0].payload.skipContentHashes, undefined);
    assert.equal(f.worker.requests[1].payload.options.skipContentHashes, undefined);
    assert.equal(
      f.worker.requests.find((r) => r.operation === "openRomfs").payload.verifyRomfs,
      false,
    );
    await session.dispose();
    await f.adapter.cleanup();
  });
  test("rejects mismatched selection, forged DLC, missing sdk, aggregate budgets before RomFS", async () => {
    for (const config of [
      { selected: { ...selection, effectiveVersion: 2 } },
      { entries: [{ index: 0, name: "main", size: 1n }] },
      { maxProgramBytes: 70007 },
    ]) {
      const f = fixture(config),
        session = await open(f);
      await assert.rejects(
        f.adapter.extractSelected({ session, selection }),
        /^ExtractionClientError: EXTRACTION_FAILED$/,
      );
      assert.equal(f.worker.terminated, true);
      assert.ok(!f.worker.requests.some((r) => r.operation === "openRomfs"));
      assert.ok(!f.worker.requests.some((r) => r.operation === "readExefs"));
    }
    const f = fixture(),
      session = await open(f);
    await assert.rejects(
      f.adapter.extractSelected({ session, selection: { ...selection, dlc: [{}] } }),
      /EXTRACTION_FAILED/,
    );
    assert.ok(!f.worker.requests.some((r) => r.operation === "select"));
  });
  test("terminal read failure, cancellation, reuse and failed initialization clean up", async () => {
    const f = fixture({ failRead: true }),
      session = await open(f);
    await assert.rejects(
      f.adapter.extractSelected({ session, selection }),
      /^ExtractionClientError: EXTRACTION_FAILED$/,
    );
    assert.equal(f.worker.terminated, true);
    await f.adapter.cleanup();
    await assert.rejects(open(f), /EXTRACTION_SINGLE_USE/);
    const g = fixture(),
      controller = new AbortController(),
      s = await open(g, controller.signal);
    controller.abort("PRIVATE-CANARY");
    assert.equal(g.worker.terminated, true);
    await assert.rejects(g.adapter.inspectContents({ session: s }), /EXTRACTION_DISPOSED/);
    const worker = new MockWorker(() => {
      throw new Error("PRIVATE-CANARY");
    });
    const adapter = createExtractionJobAdapter({ createWorker: () => worker });
    await assert.rejects(
      adapter.openSecretSession({ inputs: {} }),
      /^ExtractionClientError: EXTRACTION_FAILED$/,
    );
    assert.equal(worker.terminated, true);
  });

  test("per-read cancellation terminates an in-flight bounded RomFS read", async () => {
    const f = fixture(),
      session = await open(f);
    const extracted = await f.adapter.extractSelected({ session, selection });
    const member = extracted.romfs.getFile("data/build_manifest.psv");
    f.worker.handler = undefined; // 不返回的原生请求。 | A native request that does not return.
    const controller = new AbortController();
    const pending = member.readAt(0n, 1, { signal: controller.signal });
    controller.abort("PRIVATE-CANARY");
    await assert.rejects(pending, /^ExtractionClientError: EXTRACTION_DISPOSED$/);
    assert.equal(f.worker.terminated, true);
    await f.adapter.cleanup();
  });

  test("program provenance binds exact extracted identity, selection and live session", async () => {
    const f = fixture(),
      session = await open(f);
    const extracted = await f.adapter.extractSelected({ session, selection });
    const assets = readAuthenticatedAssets(extracted, selection);
    assert.deepEqual(readInputContentPolicy(extracted, selection), {
      contentMode: "warlock",
      inputProvenance: "authenticated-package",
      dlcMetadata: "authenticated",
    });
    assert.throws(
      () =>
        readInputContentPolicy(extracted, {
          ...selection,
          inputKind: "extracted",
          dlcMetadata: "not-authenticated",
        }),
      /PROVENANCE/,
    );
    assert.equal(assets.romfs, extracted.romfs);
    assert.equal(assets.logicalAssets, extracted.logicalAssets);
    assert.ok(Object.isFrozen(assets.romfs.files));
    assert.ok(assets.romfs.files.every(Object.isFrozen));
    assert.deepEqual(Object.keys(assets.romfs.files[0]).sort(), ["path", "size"]);
    assert.throws(
      () => readAuthenticatedAssets({ ...extracted }, selection),
      /PROVENANCE_REJECTED/,
    );
    assert.throws(
      () => readAuthenticatedAssets(extracted, { ...selection, effectiveVersion: 999 }),
      /PROVENANCE_REJECTED/,
    );
    assert.equal(readAuthenticatedProgramModules(extracted, selection), extracted.programModules);
    assert.equal(
      readAuthenticatedProgramModules(extracted, structuredClone(selection)),
      extracted.programModules,
    );
    for (const candidate of [{ ...extracted }, { programModules: extracted.programModules }, null])
      assert.throws(
        () => readAuthenticatedProgramModules(candidate, selection),
        /EXTRACTION_PROVENANCE_REJECTED/,
      );
    assert.throws(
      () => readAuthenticatedProgramModules(extracted, { ...selection, effectiveVersion: 999 }),
      /EXTRACTION_PROVENANCE_REJECTED/,
    );
    const controller = new AbortController();
    controller.abort("PRIVATE-CANARY");
    assert.throws(
      () => readAuthenticatedProgramModules(extracted, selection, { signal: controller.signal }),
      /^ExtractionClientError: EXTRACTION_CANCELLED$/,
    );
    await session.dispose();
    assert.throws(() => readAuthenticatedAssets(extracted, selection), /PROVENANCE_REJECTED/);
    assert.throws(
      () => readAuthenticatedProgramModules(extracted, selection),
      /EXTRACTION_PROVENANCE_REJECTED/,
    );
    await f.adapter.cleanup();
  });

  test("requires retained known expansion evidence, not caller supplied DLC claims", async () => {
    for (const mutate of [
      (x) => x.records.pop(),
      (x) => x.records.splice(1, 1),
      (x) => {
        x.records[2].titleId++;
      },
      (x) => {
        x.inventory[2].size++;
      },
      (x) => {
        x.records[2].contents[0].type = 1;
      },
      (x) => {
        x.records[2].contents[0].idOffset = 1;
      },
    ]) {
      const inspected = inspectedFixture();
      mutate(inspected);
      const f = fixture({ inspected }),
        session = await open(f);
      await assert.rejects(
        f.adapter.extractSelected({ session, selection }),
        /^ExtractionClientError: EXTRACTION_FAILED$/,
      );
      assert.ok(!f.worker.requests.some((r) => r.operation === "select"));
      assert.equal(f.worker.terminated, true);
    }
    const f = fixture(),
      session = await open(f);
    await assert.rejects(
      f.adapter.extractSelected({ session, selection: { ...selection, dlc: [] } }),
      /EXTRACTION_FAILED/,
    );
    assert.ok(!f.worker.requests.some((r) => r.operation === "select"));
  });

  test("forwards explicit DLC version and retains exact evidence in provenance", async () => {
    const inspected = inspectedFixture();
    inspected.records.push({ ...inspected.records[2], version: 1n });
    const dlcVersions = { [warlock.toString(16).padStart(16, "0")]: 1n };
    const chosen = resolveD2rExpansionSelection(inspected.records, inspected.inventory, {
      dlcVersions,
    });
    const f = fixture({ inspected, selected: chosen }),
      session = await open(f);
    const extracted = await f.adapter.extractSelected({ session, selection: chosen });
    assert.deepEqual(f.worker.requests.find((r) => r.operation === "select").payload.options, {
      contentMode: "warlock",
      baseVersion: 0n,
      patchVersion: 65536n,
      dlcVersions,
    });
    assert.equal(readAuthenticatedProgramModules(extracted, chosen), extracted.programModules);
    assert.throws(
      () => readAuthenticatedProgramModules(extracted, { ...chosen, dlc: [] }),
      /EXTRACTION_PROVENANCE_REJECTED/,
    );
    assert.equal(chosen.requiredContents.length, 3);
    assert.equal("owned" in chosen, false);
    await f.adapter.cleanup();
  });

  test("safe initialization diagnosis survives adapter cleanup", async () => {
    let worker;
    worker = new MockWorker((request) => {
      worker.emit({ id: request.id, ok: false, error: "PROD_KEYS_UNREADABLE" });
    });
    const adapter = createExtractionJobAdapter({ createWorker: () => worker });
    await assert.rejects(
      adapter.openSecretSession({ inputs: {} }),
      /^ExtractionClientError: PROD_KEYS_UNREADABLE$/,
    );
    assert.equal(worker.terminated, true);
  });

  test("authenticated base and update without DLC work in both permitted content modes", async () => {
    for (const contentMode of ["base-update-only", "optional-warlock"]) {
      const inspected = inspectedFixture();
      inspected.records = inspected.records.slice(0, 2);
      inspected.inventory = inspected.inventory.slice(0, 2);
      const chosen = resolveD2rExpansionSelection(inspected.records, inspected.inventory, {
        contentMode,
      });
      const f = fixture({ inspected, selected: chosen, contentMode });
      const session = await open(f);
      const extracted = await f.adapter.extractSelected({ session, selection: chosen });
      assert.equal(chosen.dlc.length, 0);
      assert.equal(readAuthenticatedProgramModules(extracted, chosen), extracted.programModules);
      assert.equal(
        f.worker.requests.find((r) => r.operation === "select").payload.options.contentMode,
        contentMode,
      );
      await f.adapter.cleanup();
    }
  });

  for (const revocation of ["cancel", "dispose"])
    test(`fulfilled private RomFS reads are wiped after ${revocation} before delivery`, async () => {
      const f = fixture();
      const session = await open(f);
      const extracted = await f.adapter.extractSelected({ session, selection });
      const file = extracted.romfs.getFile("data/build_manifest.psv");
      const controller = new AbortController();
      const emit = f.worker.emit.bind(f.worker);
      let delivered, disposing;
      f.worker.emit = (message) => {
        if (message.result instanceof Uint8Array) {
          delivered = message.result;
          emit(message);
          if (revocation === "cancel") controller.abort();
          else disposing = session.dispose();
        } else emit(message);
      };
      try {
        await assert.rejects(
          file.readAt(0n, 1, { signal: controller.signal }),
          /EXTRACTION_DISPOSED|Abort/,
        );
        assert.ok(delivered instanceof Uint8Array);
        assert.deepEqual([...delivered], [0]);
        await disposing;
      } finally {
        await f.adapter.cleanup();
      }
    });

  test("cleanup after initialization fulfillment cannot publish a disposed session", async () => {
    const f = fixture();
    const emit = f.worker.emit.bind(f.worker);
    let cleaning;
    f.worker.emit = (message) => {
      emit(message);
      if (message.ok && message.result?.records) cleaning = f.adapter.cleanup();
    };
    try {
      await assert.rejects(open(f), /EXTRACTION_FAILED/);
      await cleaning;
      assert.equal(f.worker.terminated, true);
    } finally {
      await f.adapter.cleanup();
    }
  });

  for (const kind of ["throw", "reject"])
    test(`local extraction phase diagnostics isolate observers that ${kind}`, async () => {
      const f = fixture();
      const session = await open(f);
      let phases = 0;
      try {
        const extracted = await f.adapter.extractSelected({
          session,
          selection,
          onDiagnostic() {
            phases++;
            if (kind === "throw") throw new Error("PRIVATE-OBSERVER");
            return Promise.reject(new Error("PRIVATE-OBSERVER"));
          },
        });
        assert.ok(phases >= 4);
        assert.equal(extracted.programModules.main[0], 1);
        await new Promise((resolve) => setImmediate(resolve));
      } finally {
        await f.adapter.cleanup();
      }
    });

  test("isolating phase diagnostics does not swallow cancellation through the actual signal", async () => {
    const f = fixture();
    const session = await open(f);
    const controller = new AbortController();
    try {
      await assert.rejects(
        f.adapter.extractSelected({
          session,
          selection,
          signal: controller.signal,
          onDiagnostic() {
            controller.abort();
            throw new Error("PRIVATE-OBSERVER");
          },
        }),
        /EXTRACTION_CANCELLED/,
      );
      assert.equal(f.worker.terminated, true);
      assert.equal(
        f.worker.requests.some((request) => request.operation === "readExefs"),
        false,
      );
    } finally {
      await f.adapter.cleanup();
    }
  });
}

// extraction-worker-client
{
  // 验证 extraction-worker-client 的约定与边界；依据 ../extraction-worker-client.mjs、./helpers/extraction-adapter-fixture.mjs 及本文件的测试断言。
  // Verify extraction-worker-client contracts and boundaries against ../extraction-worker-client.mjs、./helpers/extraction-adapter-fixture.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createExtractionWorkerClient } = extractionWorkerClientModule;

  const { MockWorker } = extractionWorkerModule;
  const make = (options = {}) => {
    const worker = new MockWorker();
    return {
      worker,
      client: createExtractionWorkerClient({ createWorker: () => worker, ...options }),
    };
  };
  const read = (client, options) =>
    client.readRomfs({ token: "generation", offset: 0n, count: 2 }, options);
  test("serializes bounded requests and receives isolated transferred arrays", async () => {
    const { worker, client } = make({ maxQueue: 1 });
    const first = read(client),
      second = read(client);
    await assert.rejects(read(client), /EXTRACTION_QUEUE_LIMIT/);
    assert.equal(worker.requests.length, 1);
    const bytes = new Uint8Array([1, 2]);
    worker.emit({ id: 1, ok: true, result: structuredClone(bytes, { transfer: [bytes.buffer] }) });
    assert.equal(bytes.byteLength, 0);
    assert.deepEqual(await first, new Uint8Array([1, 2]));
    assert.equal(worker.requests.length, 2);
    worker.emit({ id: 2, ok: true, result: new Uint8Array([3, 4]) });
    assert.deepEqual(await second, new Uint8Array([3, 4]));
    client.close();
    assert.equal(worker.terminated, true);
    assert.equal(worker.listeners.size, 0);
  });
  test("abort terminates active and queued requests without propagating abort reason", async () => {
    const { worker, client } = make();
    const controller = new AbortController();
    const pending = [read(client), read(client, { signal: controller.signal })];
    controller.abort("SECRET-CANARY");
    for (const promise of pending)
      await assert.rejects(promise, /^ExtractionClientError: EXTRACTION_CANCELLED$/);
    assert.equal(worker.terminated, true);
    await assert.rejects(read(client), /EXTRACTION_DISPOSED/);
  });
  test("timeout is terminal for the entire queue", async () => {
    const { worker, client } = make({ timeoutMs: 10 });
    const results = await Promise.allSettled([read(client), read(client)]);
    assert.ok(
      results.every((r) => r.status === "rejected" && r.reason.code === "EXTRACTION_TIMEOUT"),
    );
    assert.equal(worker.terminated, true);
  });
  test("stale ids, malformed shapes, foreign errors, short reads and generation reuse fail closed", async () => {
    for (const response of [
      { id: 9, ok: true, result: new Uint8Array(2) },
      { id: 1, ok: true, result: new Uint8Array(2), extra: "canary" },
      { id: 1, ok: false, error: "SECRET-CANARY" },
      { id: 1, ok: true, result: new Uint8Array(1) },
      { id: 1, ok: true, result: new Uint8Array(4).subarray(1, 3) },
    ]) {
      const { worker, client } = make();
      const promise = read(client);
      worker.emit(response);
      await assert.rejects(promise, /^ExtractionClientError: EXTRACTION_PROTOCOL_FAILED$/);
      assert.equal(worker.terminated, true);
    }
    const { worker, client } = make();
    const promise = client.openRomfs("same");
    worker.emit({ id: 1, ok: true, result: { token: "same", size: 80n } });
    await assert.rejects(promise, /EXTRACTION_PROTOCOL_FAILED/);
  });
  test("fixed worker failure and crash expose no native detail", async () => {
    for (const crash of [false, true]) {
      const { worker, client } = make();
      const promise = read(client);
      if (crash) worker.listeners.get("error")({ message: "SECRET-CANARY" });
      else worker.emit({ id: 1, ok: false, error: "EXTRACTION_FAILED" });
      await assert.rejects(promise, crash ? /EXTRACTION_WORKER_FAILED/ : /EXTRACTION_FAILED/);
      assert.equal(worker.terminated, true);
    }
  });
  test("fixed API strips unknown properties and rejects out of bounds reads before send", async () => {
    const { worker, client } = make();
    assert.equal(client.execute, undefined);
    for (const payload of [
      { token: "t", offset: 0n, count: 65537 },
      { token: "t", offset: -1n, count: 1 },
    ])
      await assert.rejects(client.readRomfs(payload), /EXTRACTION_READ_BOUNDS/);
    assert.equal(worker.requests.length, 0);
    const pending = client.dispose();
    worker.emit({ id: 1, ok: true, result: { disposed: true } });
    await pending;
    await client.dispose();
    assert.equal(worker.requests.length, 1);
  });

  test("large verification operations survive the old deadline but expire at their bounded total deadline", async (t) => {
    t.mock.timers.enable({ apis: ["setTimeout"] });
    for (const operation of ["initialize", "openRomfs"]) {
      const { worker, client } = make();
      const pending =
        operation === "initialize"
          ? client.initialize({ nsps: [], prodKeys: new Blob(), titleKeys: new Blob() })
          : client.openRomfs("exefs");
      const checked = assert.rejects(pending, /^ExtractionClientError: EXTRACTION_TIMEOUT$/);
      t.mock.timers.tick(120000);
      assert.equal(worker.terminated, false);
      t.mock.timers.tick(2 * 60 * 60 * 1000 - 120001);
      assert.equal(worker.terminated, false);
      t.mock.timers.tick(1);
      await checked;
      assert.equal(worker.terminated, true);
      assert.equal(worker.listeners.size, 0);
    }
  });

  test("short reads and disposal retain short deadlines while select retains two minutes", async (t) => {
    t.mock.timers.enable({ apis: ["setTimeout"] });
    for (const operation of ["readRomfs", "readExefs", "dispose", "select"]) {
      const { worker, client } = make();
      const pending =
        operation === "readRomfs"
          ? read(client)
          : operation === "readExefs"
            ? client.readExefs({ token: "t", index: 0, offset: 0n, count: 1 })
            : operation === "dispose"
              ? client.dispose()
              : client.select({});
      const checked = assert.rejects(pending, /EXTRACTION_TIMEOUT/);
      const deadline = operation === "select" ? 120000 : 30000;
      t.mock.timers.tick(deadline - 1);
      assert.equal(worker.terminated, false);
      t.mock.timers.tick(1);
      await checked;
      assert.equal(worker.terminated, true);
    }
  });

  test("trusted per-operation overrides are snapshotted and include queued time", async (t) => {
    t.mock.timers.enable({ apis: ["setTimeout"] });
    const operationTimeoutMs = { openRomfs: 1000000, readRomfs: 100 };
    const { worker, client } = make({ operationTimeoutMs });
    operationTimeoutMs.readRomfs = 1000000; // 不能延长已配置的租约。 | Cannot prolong already configured leases.
    const opening = client.openRomfs("exefs"),
      queued = read(client);
    const checked = Promise.all([
      assert.rejects(opening, /EXTRACTION_TIMEOUT/),
      assert.rejects(queued, /EXTRACTION_TIMEOUT/),
    ]);
    t.mock.timers.tick(99);
    assert.equal(worker.terminated, false);
    assert.equal(worker.requests.length, 1);
    t.mock.timers.tick(1);
    await checked;
    assert.equal(worker.terminated, true);
    assert.equal(
      worker.requests.length,
      1,
      "queued read never reaches worker after its total deadline",
    );
  });

  test("long-operation cancellation remains immediate and cannot be renewed by late worker replies", async (t) => {
    t.mock.timers.enable({ apis: ["setTimeout"] });
    const { worker, client } = make({ operationTimeoutMs: { openRomfs: 6 * 60 * 60 * 1000 } });
    const controller = new AbortController();
    const pending = client.openRomfs("exefs", { signal: controller.signal });
    const checked = assert.rejects(pending, /^ExtractionClientError: EXTRACTION_CANCELLED$/);
    t.mock.timers.tick(120001);
    assert.equal(worker.terminated, false);
    controller.abort("PRIVATE-CANARY");
    assert.equal(worker.terminated, true);
    await checked;
    worker.emit({ id: 1, ok: true, result: { token: "romfs", size: 100n } });
    t.mock.timers.tick(6 * 60 * 60 * 1000);
    await assert.rejects(read(client), /EXTRACTION_DISPOSED/);
  });

  test("operation deadlines cannot be disabled, unbounded or applied to arbitrary RPC", () => {
    let created = 0;
    const createWorker = () => {
      created++;
      return new MockWorker();
    };
    for (const operationTimeoutMs of [
      null,
      [],
      { execute: 1 },
      { initialize: 0 },
      { initialize: Infinity },
      { openRomfs: 6 * 60 * 60 * 1000 + 1 },
      { readRomfs: 120001 },
      { dispose: -1 },
      { select: undefined },
      { readExefs: 1.5 },
    ])
      assert.throws(
        () => createExtractionWorkerClient({ createWorker, operationTimeoutMs }),
        /EXTRACTION_CLIENT_OPTIONS/,
      );
    assert.equal(created, 0, "invalid budgets reject before opening isolated runtime");
  });

  test("successful slow verification clears its timer before later bounded reads", async (t) => {
    t.mock.timers.enable({ apis: ["setTimeout"] });
    const { worker, client } = make();
    const pending = client.openRomfs("exefs");
    t.mock.timers.tick(600001);
    worker.emit({ id: 1, ok: true, result: { token: "romfs", size: 100n } });
    assert.deepEqual(await pending, { token: "romfs", size: 100n });
    t.mock.timers.tick(2 * 60 * 60 * 1000);
    assert.equal(worker.terminated, false, "successful request has no residual long timer");
    const bytes = read(client);
    t.mock.timers.tick(29999);
    worker.emit({ id: 2, ok: true, result: new Uint8Array([1, 2]) });
    assert.deepEqual(await bytes, new Uint8Array([1, 2]));
    client.close();
  });

  test("allowlisted initialization progress and diagnoses expose no arbitrary worker text", async () => {
    const { worker, client } = make();
    const events = [];
    const pending = client.initialize(
      { nsps: [], prodKeys: new Blob(), titleKeys: new Blob() },
      {
        onProgress: (event) => events.push(event),
      },
    );
    const progress = { stage: "content-hash", completed: 0, total: 3, bytes: 1234 };
    worker.emit({ id: 1, progress });
    assert.deepEqual(events, [progress]);
    assert.equal(worker.terminated, false);
    worker.emit({ id: 1, ok: false, error: "PROD_KEYS_MALFORMED" });
    await assert.rejects(pending, /^ExtractionClientError: PROD_KEYS_MALFORMED$/);
    assert.equal(worker.terminated, true);
  });

  test("RomFS verification progress reaches the caller without completing the request", async () => {
    const { worker, client } = make();
    const events = [];
    const pending = client.openRomfs("exefs", { onProgress: (event) => events.push(event) });
    const progress = { stage: "romfs-base", completed: 50, total: 100, bytes: 4294967296 };
    worker.emit({ id: 1, progress });
    assert.deepEqual(events, [progress]);
    assert.equal(worker.terminated, false);
    worker.emit({ id: 1, ok: true, result: { token: "romfs", size: 100n } });
    assert.deepEqual(await pending, { token: "romfs", size: 100n });
    client.close();
  });

  test("ExeFS open progress reaches selection without closing its worker", async () => {
    const { worker, client } = make();
    const events = [];
    const pending = client.select({}, { onProgress: (event) => events.push(event) });
    const progress = { stage: "exefs-open", completed: 0, total: 0, bytes: 0 };
    worker.emit({ id: 1, progress });
    assert.deepEqual(events, [progress]);
    assert.equal(worker.terminated, false);
    worker.emit({ id: 1, ok: true, result: { selection: {}, token: "exefs", entries: [] } });
    assert.deepEqual(await pending, { selection: {}, token: "exefs", entries: [] });
    client.close();
  });

  // 进度观察者的异步结果不改变成功、原始失败或现有截止时间。
  // Async progress observers cannot change success, original failure, or existing deadlines.
  test("rejected async extraction progress is observed without replacing worker outcomes", async () => {
    for (const failed of [false, true]) {
      const { worker, client } = make();
      const pending = client.openRomfs("exefs", {
        onProgress: async () => {
          throw new Error("synthetic observer failure");
        },
      });
      const checked = failed
        ? assert.rejects(pending, /^ExtractionClientError: PROD_KEYS_MALFORMED$/)
        : pending;
      worker.emit({
        id: 1,
        progress: { stage: "romfs-base", completed: 50, total: 100, bytes: 64 },
      });
      if (failed) worker.emit({ id: 1, ok: false, error: "PROD_KEYS_MALFORMED" });
      else worker.emit({ id: 1, ok: true, result: { token: "romfs", size: 80n } });
      const result = await checked;
      if (!failed) assert.equal(result.token, "romfs");
      await new Promise((resolve) => setImmediate(resolve));
      client.close();
      assert.equal(worker.terminated, true);
      assert.equal(worker.listeners.size, 0);
    }
  });
  test("unsettled extraction observer does not renew the existing deadline", async (t) => {
    t.mock.timers.enable({ apis: ["setTimeout"] });
    const { worker, client } = make({ operationTimeoutMs: { openRomfs: 20 } });
    const pending = client.openRomfs("exefs", { onProgress: () => new Promise(() => {}) });
    const checked = assert.rejects(pending, /^ExtractionClientError: EXTRACTION_TIMEOUT$/);
    t.mock.timers.tick(19);
    worker.emit({ id: 1, progress: { stage: "romfs-base", completed: 50, total: 100, bytes: 64 } });
    t.mock.timers.tick(1);
    await checked;
    assert.equal(worker.terminated, true);
  });

  test("aborting from an async progress observer still cancels active and queued extraction", async () => {
    const { worker, client } = make();
    const controller = new AbortController();
    const pending = client.openRomfs("exefs", {
      signal: controller.signal,
      async onProgress() {
        controller.abort("Synthetic private abort reason");
        throw new Error("Synthetic observer failure");
      },
    });
    const checked = Promise.all([
      assert.rejects(pending, /^ExtractionClientError: EXTRACTION_CANCELLED$/),
      assert.rejects(read(client), /^ExtractionClientError: EXTRACTION_CANCELLED$/),
    ]);
    worker.emit({ id: 1, progress: { stage: "romfs-base", completed: 50, total: 100, bytes: 64 } });
    await checked;
    assert.equal(worker.terminated, true);
    assert.equal(worker.listeners.size, 0);
    assert.equal(worker.requests.length, 1);
    await new Promise((resolve) => setImmediate(resolve));
  });
}
