// 独立单次签名事务：凭据不进入归档，完成前清除工作内存。
// Independent one-shot signing transaction: credentials never enter the archive; clear working memory before completion.
import {
  IPA_SIGNING_LIMITS,
  signingFields,
  signingError,
  validSigningBlob,
  validateSigningCredentials,
  clearSigningBuffers,
} from "./ipa-signing-contract.mjs";

export const SIGNING_NATIVE_PATHS = Object.freeze({
  app: "/work/Payload/App.app",
  identity: "/credentials/identity.p12",
  profile: "/credentials/profile.mobileprovision",
});

/** 仅调用原生应用包接口；无命令行、任意选项、注入、账号或网络操作。
 * Native bundle API only. No CLI, arbitrary flags, injection, account, or network operation. */
export function invokeSigningNative(native, password) {
  const strings = [
    SIGNING_NATIVE_PATHS.app,
    "",
    SIGNING_NATIVE_PATHS.identity,
    SIGNING_NATIVE_PATHS.profile,
    password,
    "",
    "",
    "",
    "",
  ];
  const allocations = [];
  try {
    for (const value of strings) {
      const bytes = new TextEncoder().encode(value);
      let pointer;
      try {
        pointer = native._malloc(bytes.length + 1);
        if (
          !Number.isSafeInteger(pointer) ||
          pointer <= 0 ||
          pointer + bytes.length + 1 > native.HEAPU8.length
        )
          throw signingError("SIGNING_SIZE_LIMIT");
        allocations.push({ pointer, length: bytes.length + 1 });
        native.HEAPU8.set(bytes, pointer);
        native.HEAPU8[pointer + bytes.length] = 0;
      } finally {
        bytes.fill(0);
      }
    }
    const code = native._zsign_sign_bundle(
      ...allocations.map((item) => item.pointer),
      0,
      1,
      1,
      0,
      0,
    );
    if (code !== 0)
      throw signingError(
        code === -207
          ? "SIGNING_APPLICATION_ID_TOO_LONG"
          : code === -206
            ? "SIGNING_PROFILE_MISMATCH"
            : code === -203
              ? "SIGNING_IDENTITY_INVALID"
              : "SIGNING_FAILED",
      );
  } finally {
    for (const { pointer, length } of allocations) {
      try {
        native.HEAPU8.fill(0, pointer, pointer + length);
      } finally {
        native._free(pointer);
      }
    }
    strings.fill("");
  }
}

/** 所有依赖均由构建固定，绝不由消息选择。
 * All dependencies are build-owned imports, never chosen by messages. */
export function createIpaSigningWorkerHandler({
  createNative,
  inspectArchive,
  mountArchive,
  createArchive,
  postMessage,
  closeWorker,
}) {
  let state = "new",
    native;
  function clearNative(value) {
    if (!value) return;
    for (const path of [SIGNING_NATIVE_PATHS.identity, SIGNING_NATIVE_PATHS.profile]) {
      try {
        const node = value.FS.lookupPath(path).node;
        node.contents?.fill?.(0);
        value.FS.unlink(path);
      } catch {
        /* 整个堆仍会清零。 | The entire heap is still cleared. */
      }
    }
    try {
      value.HEAPU8.fill(0);
    } catch {
      /* 终止线程释放剩余内存。 | Worker termination releases remaining memory. */
    }
  }
  function replyFailure(id, code) {
    postMessage({ id, ok: false, error: { code: signingError(code).message } });
  }
  function terminate() {
    state = "closed";
    closeWorker();
  }
  const current = () => {
    if (state !== "signing") throw signingError("SIGNING_CLOSED");
  };
  const progress = (stage, completed) => {
    current();
    postMessage({ id: 1, type: "progress", stage, completed, total: 1 });
  };
  return async function handle(data) {
    if (state === "closed") {
      clearSigningBuffers([data?.credentials?.identity, data?.credentials?.profile]);
      return;
    }
    if (data?.type === "initialize" && state === "new") {
      state = "initializing";
      let created;
      try {
        if (
          !signingFields(data, ["id", "type", "assets"]) ||
          data.id !== 0 ||
          !Array.isArray(data.assets) ||
          data.assets.length !== 1 ||
          !Array.isArray(data.assets[0]) ||
          data.assets[0].length !== 2 ||
          data.assets[0][0] !== "zsign.wasm" ||
          !(data.assets[0][1] instanceof ArrayBuffer) ||
          data.assets[0][1].resizable ||
          data.assets[0][1].byteLength < 8 ||
          data.assets[0][1].byteLength > IPA_SIGNING_LIMITS.wasmBytes
        )
          throw signingError("SIGNING_INITIALIZATION_FAILED");
        created = await createNative(data.assets[0][1]);
        if (state !== "initializing") {
          clearNative(created);
          return;
        }
        if (
          !created?.FS ||
          !(created.HEAPU8 instanceof Uint8Array) ||
          typeof created._malloc !== "function" ||
          typeof created._free !== "function" ||
          typeof created._zsign_sign_bundle !== "function"
        )
          throw signingError("SIGNING_INITIALIZATION_FAILED");
        native = created;
        state = "ready";
        postMessage({ id: 0, ok: true, result: { initialized: true } });
      } catch {
        clearNative(created);
        if (state !== "closed") {
          replyFailure(0, "SIGNING_INITIALIZATION_FAILED");
          terminate();
        }
      }
      return;
    }
    if (
      state !== "ready" ||
      !signingFields(data, ["id", "type", "ipa", "credentials"]) ||
      data.id !== 1 ||
      data.type !== "sign"
    ) {
      clearSigningBuffers([data?.credentials?.identity, data?.credentials?.profile]);
      // 已在运行的事务自己清理；关闭状态阻止等待中的继续执行。
      // An active transaction owns its cleanup; closed state prevents awaited work continuing.
      if (state !== "signing" && state !== "initializing") {
        clearNative(native);
        native = undefined;
      }
      replyFailure(data?.type === "initialize" ? 0 : 1, "SIGNING_PROTOCOL_INVALID");
      terminate();
      return;
    }
    state = "signing";
    const credentials = data.credentials;
    let mounted, output, failure;
    try {
      if (!validSigningBlob(data.ipa) || !validateSigningCredentials(credentials))
        throw signingError("SIGNING_INPUT_INVALID");
      progress("inspect", 0);
      const archive = await inspectArchive(data.ipa);
      current();
      progress("inspect", 1);
      mounted = mountArchive(native, archive);
      native.FS.mkdirTree("/credentials");
      native.FS.writeFile(SIGNING_NATIVE_PATHS.identity, new Uint8Array(credentials.identity), {
        canOwn: false,
      });
      native.FS.writeFile(SIGNING_NATIVE_PATHS.profile, new Uint8Array(credentials.profile), {
        canOwn: false,
      });
      clearSigningBuffers([credentials.identity, credentials.profile]);
      progress("sign", 0);
      invokeSigningNative(native, credentials.password);
      credentials.password = "";
      progress("sign", 1);
      const entries = mounted.collectEntries();
      const required = [
        archive.appRoot + "embedded.mobileprovision",
        archive.appRoot + "_CodeSignature/CodeResources",
      ];
      if (!required.every((path) => entries.some((entry) => entry.path === path && entry.size > 0)))
        throw signingError("SIGNING_OUTPUT_INVALID");
      // 条目已是独立 Blob 快照；打包前释放可变覆盖层并擦除原生凭据和堆。
      // Entries are independent Blob snapshots; release the mutable overlay and wipe native credentials/heap before packaging.
      mounted.dispose();
      mounted = undefined;
      clearNative(native);
      native = undefined;
      progress("package", 0);
      output = await createArchive(entries);
      current();
      if (!validSigningBlob(output)) throw signingError("SIGNING_OUTPUT_INVALID");
      progress("package", 1);
      progress("verify", 1);
    } catch (error) {
      failure = signingError(error?.message).message;
    } finally {
      clearSigningBuffers([credentials?.identity, credentials?.profile]);
      if (credentials && typeof credentials === "object") {
        try {
          credentials.password = "";
        } catch {
          /* 消息可能已冻结。 | A test message may be frozen. */
        }
      }
      try {
        mounted?.dispose();
      } catch {
        // 清理失败仍须阻止成功，但不能掩盖已有的兼容性错误。
        // Cleanup failure still prevents success without hiding an earlier compatibility error.
        failure ??= "SIGNING_FAILED";
      }
      clearNative(native);
      native = undefined;
    }
    if (state === "closed") return;
    if (failure) replyFailure(1, failure);
    else postMessage({ id: 1, ok: true, result: { blob: output } });
    terminate();
  };
}
