export interface HostReport {
  hostState: string;
  deviceModel: string;
  deviceType: string;
  osVersion: string;
  deviceAbi: string;
  runtimeApi: number;
  compiledAbi: string;
  compiler: string;
  pointerBits: number;
}
export const getHostReport: () => HostReport;
export const runPlatformChecks: () => Promise<string>;
export const runGpuChecks: () => Promise<string>;
export const runRealShader: () => Promise<string>;
export const runReal2D: (filesDir: string) => Promise<string>;
export const runRuntimeStartup: (filesDir: string) => Promise<string>;
export const runGame: (filesDir: string) => Promise<string>;
export const runtimeProgress: () => string;
export const setGameFocus: (state: number) => void;
export const setGameTouch: (active: boolean, x: number, y: number) => void;
export const getPresentedFrames: () => number;
export const getScaleMode: () => boolean;
export const getTouchHud: () => number[];
export const cloudSync: (saveDir: string, cloudDir: string, backupDir: string, stateFile: string, allowDown: boolean) => number;
export const setTouchControls: (on: boolean) => void;
export const setScaleMode: (fast: boolean) => void;
export const setScale3d: (scale: number) => void;
export const setGameButtons: (mask: number, leftX?: number, leftY?: number, rightX?: number, rightY?: number) => void;
export interface GameKeyboardRequest { id: number; maximum: number; title: string; subtitle: string; guide: string; initial: string; }
export const getGameKeyboard: () => GameKeyboardRequest | null;
export const answerGameKeyboard: (id: number, confirmed: boolean, text: string) => void;
export const presentGpuTest: () => Promise<string>;
export const setInputReceiptBudget: (limit: number) => void;
export const cancelGamePresentation: () => void;

export const cancelGameButtons: () => void;

export const setGameUiTouch: (phase: number, x: number, y: number) => number;
export const sendSurfaceTouch: (phase: number, id: number, x: number, y: number, width: number, height: number) => void;
