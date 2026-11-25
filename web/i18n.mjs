// 浏览器文字桥接：优先使用已选择的语言，未选择时跟随浏览器，存储不可用时保留页面内选择。
// Browser text bridge: explicit preference first, browser default otherwise, with in-memory fallback.
import { catalog } from "./i18n-catalog.mjs";

const LANGUAGE_PREFERENCE_KEY = "d2r-ui-language";
const isSupportedLanguage = (value) => value === "en" || value === "zh-CN";

/**
 * 小型外部状态存储，仅持久保存用户明确选择且受支持的界面语言。
 * A small external store: only an explicit, supported UI-language choice is persisted. */
export function createLanguagePreference(environment = globalThis) {
  let initialized = false;
  let preference;
  const listeners = new Set();
  function getLanguage() {
    if (!initialized) {
      initialized = true;
      try {
        const stored = environment.localStorage?.getItem(LANGUAGE_PREFERENCE_KEY);
        if (isSupportedLanguage(stored)) preference = stored;
      } catch {
        // 存储被拒绝不影响浏览器默认语言或内存中的选择。
        // Denied storage does not prevent browser defaults or an in-memory choice.
      }
    }
    return (
      preference ?? (/^zh(?:-|$)/i.test(environment.navigator?.language ?? "") ? "zh-CN" : "en")
    );
  }
  function setLanguage(value) {
    if (!isSupportedLanguage(value)) return false;
    const previous = getLanguage();
    preference = value;
    try {
      environment.localStorage?.setItem(LANGUAGE_PREFERENCE_KEY, value);
    } catch {
      // 即使无法持久化，也保留本页面内的选择。
      // Keep the choice for this page even when persistence is unavailable.
    }
    if (value !== previous) for (const listener of listeners) listener();
    return true;
  }
  function subscribe(listener) {
    listeners.add(listener);
    return () => listeners.delete(listener);
  }
  return Object.freeze({ getLanguage, setLanguage, subscribe });
}

const languagePreference = createLanguagePreference();
export const browserLanguage = languagePreference.getLanguage;
export const setBrowserLanguage = languagePreference.setLanguage;
export const subscribeLanguage = languagePreference.subscribe;

// 仅替换表中有序的 %s/%d 和 %%；参数永远不会被再次解析为格式串。
// Replace only ordered %s/%d and %% from the table; arguments are never reparsed as format strings.
export function t(key, ...values) {
  const row = catalog[key];
  if (!row) throw new Error(`Unknown browser text key: ${key}`);
  let index = 0;
  return row[browserLanguage() === "zh-CN" ? 1 : 0].replace(/%[sd%]/g, (token) =>
    token === "%%" ? "%" : String(values[index++] ?? ""),
  );
}
