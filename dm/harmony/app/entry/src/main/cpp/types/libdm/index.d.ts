// libdm.so（dm/harmony/native/napi_ohos.c）的 ArkTS 声明 | ArkTS declarations for libdm.so (dm/harmony/native/napi_ohos.c)
export const start: (files: string, ext: string, src: string, lang: string) => number;
export const focus: (ev: number) => void;
export const refresh: (hz: number) => void;
export const touchEvent: (phase: number, id: number, x: number, y: number) => void;
// 0 right button (value 1 down / 0 up) = B; 1 wheel (value +1 up / -1 down) = right stick briefly
export const mouse: (kind: number, value: number) => void;
// pointer mode: op 0 down 1 drag 2 up 3 hover, window vp; returns 1 when taken (not a touch then)
export const pointer: (op: number, x: number, y: number) => number;
export const touchScreen: (width: number, height: number, left: number, right: number, bottom: number) => void;
export const touchTick: (out: Float32Array) => number;
export const uiText: (key: string) => string;
export const execProbe: () => string;
export const setCallbacks: (showInput: (id: number, title: string, sub: string, guide: string, initial: string, maxChars: number, nameRule: number, numericMax: number) => void,
  bootFailed: (code: number) => void, shareLog?: () => void) => void;
export const textResult: (id: number, accepted: boolean, text: string) => void;
export const presentCount: () => number;

export const touchCancel: () => void;
export const appIdentity: (bundle: string, version: string) => void;

export const experimentFlags: () => number;
export const setExperimentFlags: (flags: number) => number;

export const profileDescriptors: () => number;
