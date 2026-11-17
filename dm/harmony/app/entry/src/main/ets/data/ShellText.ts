// 启动页的字串与占位符从生成的中英表读取。
// Read launcher strings and format their placeholders using the generated bilingual table.
import { UI_TEXT_EN, UI_TEXT_ZH } from '../generated/UiText';

export type ShellText = (key: string, ...values: string[]) => string;

export function shellText(lang: string): ShellText {
  const messages = lang.startsWith('zh') ? UI_TEXT_ZH : UI_TEXT_EN;
  return (key: string, ...values: string[]): string => {
    let index = 0;
    return (messages[key] ?? key).replace(/%[ds]/g, () => values[index++] ?? '');
  };
}
