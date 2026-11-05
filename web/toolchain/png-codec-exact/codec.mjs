// 基于固定 Pillow 和 CPython zlib ABI 的 PNG、整段及流式压缩适配器。
// PNG, whole-buffer and streaming compression adapters over pinned Pillow and CPython zlib ABIs.
const MAX_PIXELS = 4_194_304;
const MAX_PNG = MAX_PIXELS * 4 + 131_072;
const MAX_ZLIB_INPUT = 128 * 1024 * 1024;
function bytes(value, cap) {
  if (!(value instanceof Uint8Array) || value.length > cap)
    throw new TypeError("Invalid codec bytes/limit");
}
function geometry(width, height) {
  if (
    !Number.isInteger(width) ||
    !Number.isInteger(height) ||
    width < 1 ||
    height < 1 ||
    width > 8192 ||
    height > 8192 ||
    width * height > MAX_PIXELS
  )
    throw new RangeError("PNG image pixel limit");
}
export async function createPngCodec(wasmBytes) {
  const module =
    wasmBytes instanceof WebAssembly.Module ? wasmBytes : await WebAssembly.compile(wasmBytes);
  if (WebAssembly.Module.imports(module).length)
    throw new Error("Unexpected PNG codec host imports");
  const instance = await WebAssembly.instantiate(module);
  const wasm = instance.exports;
  wasm._initialize?.();
  const write = (value) =>
    new Uint8Array(wasm.memory.buffer, wasm.png_input(), value.length).set(value);
  const result = (length) => {
    if (length < 0) throw new Error(`PNG/zlib codec rejected input (code ${wasm.png_error()})`);
    if (length > MAX_PNG) throw new Error("Codec output cap");
    return new Uint8Array(wasm.memory.buffer, wasm.png_output(), length).slice();
  };
  const readVersion = (ptr) => {
    const raw = new Uint8Array(wasm.memory.buffer, ptr, 16);
    return new TextDecoder().decode(raw.subarray(0, raw.indexOf(0)));
  };
  const version = readVersion(wasm.png_zlib_version());
  const genericVersion = readVersion(wasm.generic_zlib_version());
  if (genericVersion !== "1.2.13") throw new Error("CASC codec zlib version mismatch");
  if (version !== "1.3.1") throw new Error("PNG codec zlib version mismatch");
  let streaming = false;
  const idle = () => {
    if (streaming) throw new Error("Codec stream is active");
  };
  const compressZlibStream = createZlibStream({
    begin() {
      idle();
      if (wasm.zlib_begin(9) !== 0) throw new Error("Cannot initialize zlib compressor");
      streaming = true;
    },
    push(part, finish) {
      write(part);
      return result(wasm.zlib_push(part.length, Number(finish)));
    },
    abort() {
      try {
        wasm.zlib_abort();
      } finally {
        streaming = false;
      }
    },
  });
  return Object.freeze({
    compressZlibStream,
    zlibVersion: version,
    genericZlibVersion: genericVersion,
    encodeRgba(rgba, width, height) {
      idle();
      geometry(width, height);
      bytes(rgba, MAX_PIXELS * 4);
      if (rgba.length !== width * height * 4) throw new TypeError("RGBA byte length mismatch");
      write(rgba);
      return result(wasm.png_encode(width, height));
    },
    decodeRgba(png) {
      idle();
      bytes(png, MAX_PNG);
      write(png);
      const data = result(wasm.png_decode(png.length));
      const width = wasm.png_width(),
        height = wasm.png_height();
      geometry(width, height);
      if (data.length !== width * height * 4) throw new Error("PNG decoded byte length mismatch");
      return { width, height, channels: 4, data };
    },
    compressZlib(raw, options = { level: 9 }) {
      idle();
      bytes(raw, MAX_ZLIB_INPUT);
      if (!options || options.level !== 9 || Object.keys(options).some((k) => k !== "level"))
        throw new TypeError(
          "Generic zlib supports only level 9/default wbits15 memLevel8 strategy0",
        );
      if (wasm.zlib_begin(9) !== 0) throw new Error("Cannot initialize zlib compressor");
      const chunks = [];
      let total = 0;
      try {
        const step = 1024 * 1024;
        for (let offset = 0; offset < raw.length || offset === 0; offset += step) {
          const part = raw.subarray(offset, offset + step);
          write(part);
          const out = result(wasm.zlib_push(part.length, Number(offset + step >= raw.length)));
          total += out.length;
          if (total > MAX_ZLIB_INPUT + 1024 * 1024) throw new RangeError("Zlib output limit");
          chunks.push(out);
        }
      } finally {
        wasm.zlib_abort();
      }
      const all = new Uint8Array(total);
      let offset = 0;
      for (const chunk of chunks) {
        all.set(chunk, offset);
        offset += chunk.length;
      }
      return all;
    },
  });
}

export const ZLIB_STREAM_CHUNK_BYTES = 1024 * 1024;
const MAX_PUSH_OUTPUT = 16 * 1024 * 1024 + 131072;
function outputChunk(bytes, offset) {
  const part = bytes.subarray(offset, offset + ZLIB_STREAM_CHUNK_BYTES);
  // 不可压缩输入可能略微扩张；暂存器限制整个底层缓冲区，而不只是视图。
  // Incompressible input can expand; staging bounds the backing buffer, not just the view.
  return bytes.buffer.byteLength > ZLIB_STREAM_CHUNK_BYTES ? new Uint8Array(part) : part;
}
function budget(value) {
  if (!Number.isSafeInteger(value) || value < 0)
    throw new RangeError("Explicit zlib stream budgets required");
  return value;
}
function check(signal) {
  signal?.throwIfAborted();
}
async function next(iterator, signal) {
  check(signal);
  let remove = () => {};
  try {
    const pending = Promise.resolve().then(() => iterator.next());
    if (!signal) return await pending;
    return await Promise.race([
      pending,
      new Promise((_, reject) => {
        const abort = () => reject(signal.reason);
        signal.addEventListener("abort", abort, { once: true });
        remove = () => signal.removeEventListener("abort", abort);
        if (signal.aborted) abort();
      }),
    ]);
  } finally {
    remove();
  }
}

/**
 * 内部 ABI 适配器：begin 必须独占实例；push 必须完全消费输入并返回自有字节。
 * 全部预算由调用方明确提供；取消时立即释放编解码器，上游关闭尽力执行。
 * Internal ABI adapter: begin must claim the instance; push fully consumes input and returns owned bytes.
 * Callers supply all budgets; cancellation releases the codec immediately and closes upstream best-effort.
 */
export function createZlibStream({ begin, push, abort }) {
  return async function* compressZlibStream(input, options = {}) {
    const { maxInputBytes, maxOutputBytes, signal, level = 9 } = options;
    if (
      level !== 9 ||
      Object.keys(options).some(
        (key) => !["maxInputBytes", "maxOutputBytes", "signal", "level"].includes(key),
      )
    )
      throw new TypeError("Generic zlib stream supports only level 9 and explicit budgets");
    budget(maxInputBytes);
    budget(maxOutputBytes);
    if (signal !== undefined && !(signal instanceof AbortSignal))
      throw new TypeError("Expected AbortSignal");
    if (!input || typeof input[Symbol.asyncIterator] !== "function")
      throw new TypeError("Expected async iterable input");
    check(signal);
    let iterator,
      started = false,
      complete = false,
      failed = false,
      closing,
      inputBytes = 0,
      outputBytes = 0,
      turns = 0;
    const release = () => {
      if (!started) return;
      started = false;
      abort();
    };
    const closeUpstream = () => {
      if (!complete && iterator && !closing) {
        closing = Promise.resolve().then(() => {
          if (typeof iterator.return === "function") return iterator.return();
        });
        // 立即观察拒绝；错误或取消路径不能被上游清理掩盖，也不能等待永不完成的清理。
        // Observe rejection immediately; error/cancel paths must not be masked or blocked by upstream cleanup.
        void closing.catch(() => {});
      }
      return closing;
    };
    // 即使消费者停在 yield，也释放实例并请求一次上游关闭；旧生成器不能释放后来的所有者。
    // Even while paused at yield, release the instance and request upstream close once; never release a later owner.
    const cancel = () => {
      try {
        release();
      } catch {
        // 取消原因优先于释放错误。
        // Preserve the cancellation reason over release errors.
      }
      closeUpstream();
    };
    const output = (part, finish) => {
      check(signal);
      const value = push(part, finish);
      if (!(value instanceof Uint8Array) || value.length > MAX_PUSH_OUTPUT)
        throw new Error("Invalid zlib ABI output");
      outputBytes += value.length;
      if (outputBytes > maxOutputBytes) throw new RangeError("Zlib stream output limit");
      return value;
    };
    try {
      begin();
      started = true;
      signal?.addEventListener("abort", cancel, { once: true });
      check(signal);
      iterator = input[Symbol.asyncIterator]();
      for (;;) {
        const item = await next(iterator, signal);
        check(signal);
        if (!item || typeof item !== "object") throw new TypeError("Invalid async iterator result");
        if (item.done) {
          complete = true;
          break;
        }
        const part = item.value;
        if (
          !(part instanceof Uint8Array) ||
          !(part.buffer instanceof ArrayBuffer) ||
          part.buffer.resizable ||
          part.buffer.byteLength > ZLIB_STREAM_CHUNK_BYTES ||
          part.length > ZLIB_STREAM_CHUNK_BYTES
        )
          throw new TypeError("Invalid bounded zlib input chunk");
        inputBytes += part.length;
        if (inputBytes > maxInputBytes) throw new RangeError("Zlib stream input limit");
        // 空输入不调用 Z_NO_FLUSH，避免上游 zlib 的 Z_BUF_ERROR。
        // Do not issue empty Z_NO_FLUSH calls, which can produce zlib Z_BUF_ERROR.
        if (part.length) {
          const encoded = output(part, false);
          for (let offset = 0; offset < encoded.length; offset += ZLIB_STREAM_CHUNK_BYTES) {
            check(signal);
            yield outputChunk(encoded, offset);
          }
        }
        if (++turns % 32 === 0) await new Promise((resolve) => setTimeout(resolve, 0));
      }
      // 单独的空 Z_FINISH 调用只完成同一条流，不增加中间刷新或拼接。
      // One empty Z_FINISH completes the same stream without intermediate flushes or concatenation.
      const tail = output(new Uint8Array(0), true);
      for (let offset = 0; offset < tail.length; offset += ZLIB_STREAM_CHUNK_BYTES) {
        check(signal);
        yield outputChunk(tail, offset);
      }
      check(signal);
    } catch (error) {
      failed = true;
      throw error;
    } finally {
      signal?.removeEventListener("abort", cancel);
      let releaseError;
      try {
        release();
      } catch (error) {
        releaseError = error;
      }
      const cleanup = closeUpstream();
      // 仅正常消费者 return 等待上游关闭；已知失败立即返回其原始错误。
      // Only normal consumer return awaits upstream close; known failures report their original error immediately.
      if (!failed && !signal?.aborted) {
        // eslint-disable-next-line no-unsafe-finally -- 仅无原始错误时报告释放失败。 | Report release failure only without a primary error.
        if (releaseError) throw releaseError;
        await cleanup;
      }
    }
  };
}
