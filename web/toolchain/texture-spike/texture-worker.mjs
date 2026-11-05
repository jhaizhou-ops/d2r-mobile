// 在工作线程中验证并执行纹理解码请求；依据 ./dist/texture.mjs、./browser-texture.mjs 的模块与调用约定。
// Validate and execute texture-decoding requests inside a worker using the module and calling contracts in ./dist/texture.mjs、./browser-texture.mjs.
import createModule from "./dist/texture.mjs";
import { createTextureDecoder } from "./browser-texture.mjs";
const decoder = createTextureDecoder(createModule);
// 请求包含 id、operation（layout／decode／decodeV61）、descriptor、source 和 options。
// Request: {id, operation: 'layout'|'decode'|'decodeV61', descriptor, source, options}.
// 响应为成功对象 {id, ok:true, result}，或失败对象 {id, ok:false, error:{name,code,message}}。
// Response: {id, ok:true, result} or {id, ok:false, error:{name,code,message}}.
// 转移 source.buffer 以避免复制大型输入；结果拥有自己的字节。
// Transfer source.buffer to avoid copying large inputs. Results own their bytes.
let queue = Promise.resolve();
self.onmessage = ({ data }) => {
  queue = queue.then(async () => {
    try {
      const api = await decoder;
      let result;
      if (data.operation === "decodeV61") result = api.decodeV61(data.source);
      else if (data.operation === "decode")
        result = api.decode(data.descriptor, data.source, data.options);
      else if (data.operation === "decodePillowBc3")
        result = api.decodePillowBc3(data.descriptor, data.source, data.options);
      else if (data.operation === "layout") result = api.layout(data.descriptor, data.options);
      else throw new Error("Unsupported texture worker operation");
      self.postMessage(
        { id: data.id, ok: true, result },
        result.pixels ? [result.pixels.buffer] : [],
      );
    } catch (error) {
      self.postMessage({
        id: data?.id,
        ok: false,
        error: { name: error.name, code: error.code ?? -1, message: error.message },
      });
    }
  });
};
