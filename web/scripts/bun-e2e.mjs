// 在 Bun 中使用本地输入运行生产打包器；仓库不包含游戏文件或密钥，
// 也不将它们发送到预览服务器。
// Run the production packager against local inputs under Bun. No game files or
// keys are included in the repository or sent to the preview server.
import { createHash } from "node:crypto";
import { mkdtemp, readFile, rm, stat, lstat, open, realpath, writeFile } from "node:fs/promises";
import { basename, join, resolve, sep } from "node:path";
import { tmpdir } from "node:os";
import { pathToFileURL } from "node:url";
import { gunzipSync } from "node:zlib";
import { prepareBrowserPackager } from "../browser-packager.mjs";
import { DirectoryHandle, storageAt } from "./bun-e2e-fs.mjs";
import { NodeCrossWorker } from "./bun-e2e-node-cross.mjs";

function usage() {
  throw new Error(
    "Usage: bun scripts/bun-e2e.mjs --preview-dir DIST --input-dir NSP_FOLDER " +
      "--nsp BASE.nsp --nsp UPDATE.nsp --output FILE.ipa [--export-resources]",
  );
}

function parseArgs(args) {
  const options = { nsps: [], exportResources: false };
  for (let index = 0; index < args.length; index++) {
    const arg = args[index];
    if (arg === "--export-resources") options.exportResources = true;
    else if (["--preview-dir", "--input-dir", "--nsp", "--output"].includes(arg)) {
      const value = args[++index];
      if (!value) usage();
      if (arg === "--nsp") options.nsps.push(value);
      else
        options[{ "--preview-dir": "preview", "--input-dir": "input", "--output": "output" }[arg]] =
          value;
    } else usage();
  }
  if (!options.preview || !options.input || !options.output || !options.nsps.length) usage();
  if (options.nsps.some((name) => basename(name) !== name || !name.endsWith(".nsp"))) usage();
  return options;
}

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

async function verifiedWorkerSource(preview, workerPath) {
  const manifest = JSON.parse(
    await readFile(join(preview, "runtime/runtime-manifest.json"), "utf8"),
  );
  const asset = manifest.assets.find((item) => item.path === workerPath);
  const entry = manifest.transport.entries.find((item) => item.sha256 === asset?.sha256);
  if (!asset || !entry || entry.bytes !== asset.bytes) throw new Error("BUN_E2E_WORKER_MANIFEST");
  const compressed = await readFile(join(preview, "runtime", entry.file));
  const transport = manifest.transport.files.find((item) => item.path === entry.file);
  if (!transport || sha256(compressed) !== transport.sha256)
    throw new Error("BUN_E2E_WORKER_TRANSPORT");
  const decoded = gunzipSync(compressed);
  const source = decoded.subarray(entry.offset, entry.offset + entry.bytes);
  if (source.length !== asset.bytes || sha256(source) !== asset.sha256)
    throw new Error("BUN_E2E_WORKER_HASH");
  return source.toString("utf8");
}

// 真实路径检查阻止预览目录内符号链接越界；只打开已检查的目标。
// Confine real paths so preview symlinks cannot escape; open only the checked target.
export async function resolvePreviewFile(previewRoot, path) {
  const target = await realpath(path);
  if (!target.startsWith(previewRoot + sep) || !(await stat(target)).isFile()) return null;
  return target;
}
async function startPreview(preview) {
  const previewRoot = await realpath(preview);
  return Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    async fetch(request) {
      if (request.method !== "GET") return new Response(null, { status: 405 });
      let pathname;
      try {
        pathname = decodeURIComponent(new URL(request.url).pathname);
      } catch {
        return new Response(null, { status: 400 });
      }
      if (pathname.includes("\\") || pathname.split("/").some((part) => part.startsWith(".")))
        return new Response(null, { status: 404 });
      const path = resolve(preview, pathname.replace(/^\/+/, "") || "index.html");
      if (!path.startsWith(preview + sep)) return new Response(null, { status: 404 });
      try {
        const target = await resolvePreviewFile(previewRoot, path);
        if (!target) return new Response(null, { status: 404 });
        return new Response(Bun.file(target));
      } catch {
        return new Response(null, { status: 404 });
      }
    },
  });
}

async function checkArchive(path) {
  const checked = Bun.spawnSync(["unzip", "-tq", path], { stdout: "pipe", stderr: "pipe" });
  if (checked.exitCode !== 0) throw new Error("BUN_E2E_IPA_ZIP_INVALID");
  const listed = Bun.spawnSync(["unzip", "-Z1", path], { stdout: "pipe", stderr: "pipe" });
  if (
    listed.exitCode !== 0 ||
    !new TextDecoder().decode(listed.stdout).includes("Payload/DmIOS.app/DmIOS")
  )
    throw new Error("BUN_E2E_IPA_PAYLOAD_MISSING");
}

// 各项清理独立执行，保留原失败，不让关闭错误跳过后续释放。
// Run cleanup independently and preserve the primary failure instead of skipping later releases.
export async function cleanupBunE2e(releases, primary) {
  const failures = [];
  for (const release of releases) {
    try {
      await release();
    } catch (error) {
      failures.push(error);
    }
  }
  if (failures.length)
    throw new AggregateError(failures, "BUN_E2E_CLEANUP_FAILED", { cause: primary ?? failures[0] });
}

// 只安装临时全局属性，恢复原描述符（包括原本不存在的属性）。
// Install temporary globals and restore exact descriptors, including absent properties.
export function replaceBunGlobal(target, key, value) {
  const original = Object.getOwnPropertyDescriptor(target, key);
  // 不可配置但可写的数据属性仍可替换值，不能改写其描述符标志。
  // Nonconfigurable writable data properties allow value replacement, not flag changes.
  Object.defineProperty(
    target,
    key,
    original && Object.hasOwn(original, "value")
      ? { ...original, value }
      : { configurable: true, writable: true, enumerable: original?.enumerable ?? false, value },
  );
  return () => {
    if (original) Object.defineProperty(target, key, original);
    else delete target[key];
  };
}
export async function requireUnusedOutput(path) {
  try {
    await lstat(path);
  } catch (error) {
    if (error.code === "ENOENT") return;
    throw error;
  }
  throw new Error("BUN_E2E_OUTPUT_EXISTS");
}
// 排他创建防止覆盖已有文件或跟随目标符号链接；失败仅删除仍属于本次写入的条目。
// Exclusive creation prevents overwrite or destination symlink traversal; remove only our own failed entry.
export async function writeBunOutput(path, blob) {
  const file = await open(path, "wx");
  let primary, identity;
  try {
    identity = await file.stat({ bigint: true });
    await file.writeFile(blob.stream());
    if ((await file.stat()).size !== blob.size) throw new Error("BUN_E2E_IPA_SIZE");
  } catch (error) {
    primary = error;
    throw error;
  } finally {
    try {
      await file.close();
    } finally {
      if (primary) {
        const current = await lstat(path, { bigint: true }).catch(() => null);
        if (identity && current?.dev === identity.dev && current?.ino === identity.ino)
          await rm(path).catch(() => {});
      }
    }
  }
}
let running = false;
export async function run(options) {
  const preview = resolve(options.preview);
  const output = resolve(options.output);
  await requireUnusedOutput(output);
  if (running) throw new Error("BUN_E2E_ALREADY_RUNNING");
  running = true;
  let readerUrl, server, staging, packager, primary;
  const restore = [];
  try {
    const manifestBytes = await readFile(join(preview, "runtime/runtime-manifest.json"));
    const { runtimeManifestSha256 } = await import(
      pathToFileURL(join(preview, "runtime/runtime-integrity.mjs")).href
    );
    if (sha256(manifestBytes) !== runtimeManifestSha256) throw new Error("BUN_E2E_MANIFEST_HASH");
    const readerSource = await verifiedWorkerSource(preview, "sealed-workers/nsp-worker.mjs");
    const crossSource = await verifiedWorkerSource(preview, "sealed-workers/cross-worker.mjs");
    const prefix = await Bun.file(
      new URL("./bun-e2e-file-reader-prefix.mjs", import.meta.url),
    ).text();
    readerUrl = URL.createObjectURL(
      new Blob([prefix, "\n", readerSource], { type: "text/javascript" }),
    );
    server = await startPreview(preview);
    staging = await mkdtemp(join(tmpdir(), "d2r-bun-e2e-"));
    const crossPath = join(staging, "cross-worker.mjs");
    await writeFile(crossPath, crossSource);
    const NativeWorker = globalThis.Worker;
    let workers = 0;
    restore.push(
      replaceBunGlobal(
        globalThis,
        "Worker",
        class {
          constructor(url, workerOptions) {
            workers++;
            if (workers === 5) return new NodeCrossWorker(crossPath);
            return new NativeWorker(workers === 4 ? readerUrl : url, workerOptions);
          }
        },
      ),
    );
    restore.push(replaceBunGlobal(globalThis, "isSecureContext", true));
    restore.push(replaceBunGlobal(globalThis, "location", server.url));
    restore.push(replaceBunGlobal(navigator, "storage", await storageAt(staging)));
    let last = "";
    const onProgress = (event) => {
      const label = `${event.stage ?? event.phase ?? ""}: ${event.label ?? event.state ?? ""}`;
      if (label !== last) console.log(`${new Date().toLocaleString()} ${label}`);
      last = label;
    };
    packager = await prepareBrowserPackager({
      manifestSha256: runtimeManifestSha256,
      onProgress,
    });
    if (!packager.ready || workers !== 6) throw new Error("BUN_E2E_WORKER_ORDER");
    const result = await packager.generate({
      input: new DirectoryHandle(resolve(options.input)),
      selectedNspNames: options.nsps,
      exportResources: options.exportResources,
      onProgress,
    });
    await writeBunOutput(output, result.download.blob);
    const size = (await stat(output)).size;
    if (size !== result.download.blob.size) throw new Error("BUN_E2E_IPA_SIZE");
    await checkArchive(output);
    console.log(`IPA complete: ${output} (${size} bytes)`);
    return { output, size, resources: result.resources };
  } catch (error) {
    primary = error;
    throw error;
  } finally {
    await cleanupBunE2e(
      [
        () => packager?.close(),
        ...restore.reverse(),
        () => {
          if (readerUrl) URL.revokeObjectURL(readerUrl);
        },
        () => server?.stop(),
        () => staging && rm(staging, { recursive: true, force: true }),
        () => {
          running = false;
        },
      ],
      primary,
    );
  }
}

if (import.meta.main) {
  try {
    await run(parseArgs(Bun.argv.slice(2)));
  } catch (error) {
    console.error(error.cause?.stack ?? error.stack ?? String(error));
    if (error.report) console.error(`Report: ${JSON.stringify(error.report)}`);
    process.exitCode = 1;
  }
}
