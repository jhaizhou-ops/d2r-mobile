// 根据仓库内的解码器构造，并非从任何游戏提取。
// Constructed from the vendored decoder, not extracted from any game.
// Maxwell 控制／调度字，后接无条件 EXIT（PT、CC.T）。
// Maxwell control/scheduling word, followed by unconditional EXIT (PT, CC.T).
export const code = new Uint8Array(16);
new DataView(code.buffer).setBigUint64(8, 0xe30000000007000fn, true);
export const job = Object.freeze({
  key: "synthetic-exit",
  stage: "cs",
  local: [1, 1, 1],
  shared: 0,
  tex: {},
  img: {},
  attr: {},
  fout: {},
  debug: true,
  reduced: false,
});
