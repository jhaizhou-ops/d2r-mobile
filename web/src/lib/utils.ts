// 合并条件类名并解决 Tailwind 类名冲突；依据 clsx、tailwind-merge 的模块与调用约定。
// Merge conditional class names and resolve Tailwind class conflicts using the module and calling contracts in clsx、tailwind-merge.
import { clsx, type ClassValue } from "clsx";
import { twMerge } from "tailwind-merge";
export function cn(...inputs: ClassValue[]) {
  return twMerge(clsx(inputs));
}
