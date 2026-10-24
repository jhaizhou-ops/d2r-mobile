// 按名称读取着色器桥接的嵌入资源；依据 web/toolchain/shader-spike 的模块与调用约定。
// Read embedded shader-bridge resources by name using the module and calling contracts in web/toolchain/shader-spike.
using System;
using System.IO;

namespace Ryujinx.Common
{
    // 这是链接的 Shader 源码唯一使用的成员，不依赖桌面 Common/NuGet。
    // Only member used by the linked Shader sources. No desktop Common/NuGets.
    internal static class EmbeddedResources
    {
        public static string ReadAllText(string filename)
        {
            string name = filename.Replace('/', '.');
            using Stream stream = typeof(EmbeddedResources).Assembly.GetManifestResourceStream(name)
                ?? throw new InvalidOperationException($"Missing embedded shader helper: {name}");
            using var reader = new StreamReader(stream);
            return reader.ReadToEnd();
        }
    }
}
