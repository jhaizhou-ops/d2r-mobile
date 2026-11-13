// 依赖注入的原图配方入口；只调用固定像素、PNG 和字体后端，不启动 Python。 | Injected original-art recipe entry; only pinned pixel/PNG/font backends, never Python.
import { generateDefaultFrame } from "./default-frame.mjs";
import { generateNativeArt } from "./native-art.mjs";
import { generateMapGoal } from "./map-goal.mjs";
import { object, requireValue } from "./util.mjs";
export const ART_RECIPE_OPERATIONS = Object.freeze([
  "image-default-frame",
  "native-art",
  "map-goal",
]);
export const isArtRecipeOperation = (operation) => ART_RECIPE_OPERATIONS.includes(operation);
const sha256 = async (bytes) =>
  Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)), (n) =>
    n.toString(16).padStart(2, "0"),
  ).join("");
export function createArtRecipes({
  codec,
  kernels,
  defaultState,
  font,
  md5,
  sha256: digest = sha256,
}) {
  requireValue(
    codec?.zlibVersion === "1.3.1" && typeof codec.encodeRgba === "function",
    "ART_RECIPE_PNG_BACKEND",
  );
  const dependencies = { codec, kernels, defaultState, font, md5, sha256: digest };
  return async function execute(operation, payload, options = {}) {
    requireValue(isArtRecipeOperation(operation), "Unsupported art recipe operation");
    requireValue(object(options), "ART_RECIPE_OPTIONS");
    switch (operation) {
      case "image-default-frame":
        requireValue(
          typeof kernels?.crop === "function" && typeof defaultState === "function",
          "ART_RECIPE_PIXEL_BACKEND",
        );
        return generateDefaultFrame(payload, options, dependencies);
      case "native-art":
        requireValue(
          typeof md5 === "function" && typeof digest === "function",
          "ART_RECIPE_HASH_BACKEND",
        );
        return generateNativeArt(payload, options, dependencies);
      case "map-goal":
        requireValue(
          ["crop", "channel", "point", "putAlpha"].every(
            (name) => typeof kernels?.[name] === "function",
          ),
          "ART_RECIPE_PIXEL_BACKEND",
        );
        requireValue(
          ["open", "measure", "draw", "close"].every((name) => typeof font?.[name] === "function"),
          "ART_RECIPE_FONT_BACKEND",
        );
        return generateMapGoal(payload, options, dependencies);
      default:
        throw new Error("Unsupported art recipe operation");
    }
  };
}
