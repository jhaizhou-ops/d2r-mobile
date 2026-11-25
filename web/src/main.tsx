// 挂载浏览器打包界面的 React 入口；依据 react-dom/client、./App 的模块与调用约定。
// Mount the React entry point for the browser packager using the module and calling contracts in react-dom/client、./App.
import { createRoot } from "react-dom/client";
import App from "./App";
import "./index.css";

// 运行时初始化只封闭文档一次，不在 StrictMode 中重放。
// Runtime initialization seals the document once; do not replay it in StrictMode.
createRoot(document.getElementById("root")!).render(<App />);
