// libdm.so（dm/harmony/native/napi_ohos.c）的 ArkTS 声明 | ArkTS declarations for libdm.so (dm/harmony/native/napi_ohos.c)
export const start: (files: string, ext: string, src: string, lang: string) => number;
export const focus: (ev: number) => void;
export const refresh: (hz: number) => void;
export const pad: (buttons: number, lx: number, ly: number, rx: number, ry: number) => void;
export const checkPack: (root: string) => string;
export const importStash: (save: string, source: string, name: string) => number;
export const touchEvent: (phase: number, id: number, x: number, y: number) => void;
export const touchScreen: (width: number, height: number, left: number, right: number, bottom: number) => void;
export const touchTick: (out: Float32Array) => number;
export const uiText: (key: string) => string;
export const execProbe: () => string;
export const setCallbacks: (showInput: (id: number, title: string, sub: string, guide: string, initial: string, maxChars: number, nameRule: number, numericMax: number) => void,
  bootFailed: (code: number) => void) => void;
export const textResult: (id: number, accepted: boolean, text: string) => void;
export const presentCount: () => number;
