// 仅为原型，从 dm/gfx/tools/dmsh_xlate/Program.cs 改编；一致性门槛见 README。
// Spike only: adapted from dm/gfx/tools/dmsh_xlate/Program.cs. See README for parity gate.
using Ryujinx.Common.Memory;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;

using System.Runtime.InteropServices.JavaScript;
using System.Runtime.Versioning;

[SupportedOSPlatform("browser")]
public static partial class ShaderBridge
{
    const int TexBase = 0x168, SampBase = 0x568;   // NVN 分离贴图 / 采样器句柄在 c[2] 的字节基址（每槽 8 字节；dm/gfx/tools/maxwell_refs.py 全量实测） | byte base of NVN separate texture / sampler handles in c[2] (8 bytes per slot; full measurement in dm/gfx/tools/maxwell_refs.py)

    sealed class Accessor : IGpuAccessor
    {
        readonly byte[] _code; readonly JsonObject _job; int _binding = 1; public readonly List<string> Notes = new();
        readonly byte[] _c1;
        public Accessor(byte[] code, JsonObject job) { _code = code; _job = job; _c1 = job["c1"] is JsonNode h ? Convert.FromHexString((string)h) : Array.Empty<byte>(); }
        // c1 常数段（着色器包控制段前 0x100 字节）：间接跳转 BRX 的跳转表在这里（LDC c[1]）；不提供时翻译器解不出跳转目标、控制流出错（粒子着色器死循环）
        // c1 constant segment (the 0x100 bytes before the control segment in the shader package): the jump table of indirect jump BRX lives here (LDC c[1]); without it the translator cannot resolve jump targets and control flow goes wrong (particle shaders loop forever)
        public uint ConstantBuffer1Read(int offset) { if (offset >= 0 && offset + 4 <= _c1.Length) return BitConverter.ToUInt32(_c1, offset); Notes.Add($"c1 读越界 +{offset:X}（c1 长 {_c1.Length}）"); return 0; }
        // 全部资源放进第 0 组，各占不同绑定号（spirv-cross 出成一个参数缓冲）；常量段号 index 的缓冲由翻译器给 binding = index+1 的惯例不依赖
        // All resources go into set 0, each with a distinct binding number (spirv-cross emits them as one argument buffer); the translator's convention of giving the buffer of constant segment number index binding = index+1 is not relied on
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateImageBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => MemoryMarshal.Cast<byte, ulong>(new ReadOnlySpan<byte>(_code)[(int)address..]);
        public int QuerySamplerArrayLengthFromPool() => 32;
        public int QueryTextureArrayLengthFromBuffer(int slot) => 32;
        public int QueryTextureArrayLengthFromPool() => 32;

        string Get(string table, int slot) => _job[table] is JsonObject t && t[slot.ToString()] is JsonNode n ? (n is JsonObject o ? (string)o["type"] : (string)n) : null;
        int TexSlot(int handle, int cbufSlot)
        {
            (int off0, _, _) = TextureHandle.UnpackOffsets(handle);
            int bytes = off0 * 4;
            return (cbufSlot < 0 || (cbufSlot & 0xffff) == 2) && bytes >= TexBase && bytes < SampBase && (bytes - TexBase) % 8 == 0 ? (bytes - TexBase) / 8 : -1;
        }
        public SamplerType QuerySamplerType(int handle, int cbufSlot = -1)
        {
            int slot = TexSlot(handle, cbufSlot);
            string t = slot >= 0 ? Get("tex", slot) ?? Get("img", slot) : null;
            if (t == null) { Notes.Add($"贴图句柄 {handle:X}（常量段 {cbufSlot}，槽 {slot}）无 PC 类型，按 2D"); return SamplerType.Texture2D; }
            return t switch
            {
                "1d" => SamplerType.Texture1D,
                "1darray" => SamplerType.Texture1D | SamplerType.Array,
                "buffer" => SamplerType.TextureBuffer,
                "2d" => SamplerType.Texture2D,
                "2darray" => SamplerType.Texture2D | SamplerType.Array,
                "2dms" => SamplerType.Texture2D | SamplerType.Multisample,
                "2dmsarray" => SamplerType.Texture2D | SamplerType.Multisample | SamplerType.Array,
                "3d" => SamplerType.Texture3D,
                "cube" => SamplerType.TextureCube,
                "cubearray" => SamplerType.TextureCube | SamplerType.Array,
                _ => SamplerType.Texture2D,
            };
        }
        public TextureFormat QueryTextureFormat(int handle, int cbufSlot = -1)
        {
            int slot = TexSlot(handle, cbufSlot);
            string f = _job["img"] is JsonObject t && slot >= 0 && t[slot.ToString()] is JsonObject o ? (string)o["fmt"] : null;
            if (f == null) { Notes.Add($"图像句柄 {handle:X} 槽 {slot} 无 PC 格式，按 R8G8B8A8Unorm"); return TextureFormat.R8G8B8A8Unorm; }
            return Enum.TryParse(f, true, out TextureFormat tf) ? tf : TextureFormat.R8G8B8A8Unorm;
        }
        int Local(int i) => _job["local"] is JsonArray a && a.Count > i ? (int)a[i] : 1;
        public int QueryComputeLocalSizeX() => Local(0);
        public int QueryComputeLocalSizeY() => Local(1);
        public int QueryComputeLocalSizeZ() => Local(2);
        public int QueryComputeSharedMemorySize() => _job["shared"] is JsonNode n ? (int)n : 0;   // 配对 PC 的 groupshared 声明之和 | sum of the paired PC shader's groupshared declarations
        static AttributeType Attr(string s) => s switch { "sint" => AttributeType.Sint, "uint" => AttributeType.Uint, _ => AttributeType.Float };
        public GpuGraphicsState QueryGraphicsState()
        {
            Array32<AttributeType> attr = default; Array8<AttributeType> fout = default;
            if (_job["attr"] is JsonObject a) foreach (var kv in a) attr[int.Parse(kv.Key)] = Attr((string)kv.Value);
            if (_job["fout"] is JsonObject f) foreach (var kv in f) fout[int.Parse(kv.Key)] = Attr((string)kv.Value);
            return new GpuGraphicsState(false, InputTopology.Triangles, false, TessPatchType.Triangles, TessSpacing.EqualSpacing,
                false, false, false, false /* 深度 0..1 | depth 0..1 */, false, 1f, AlphaTestOp.Always, 0f, attr, false, fout, false,
                false /* 不做 Y 取反 | no Y flip */, true /* 原点左上 | origin upper-left */, false);
        }
        public bool QueryHostSupportsTextureShadowLod() => true;
        // 作业 reduced = true：放宽精度——翻译器只给强制精确的运算打 NoContraction（缺省全部打：保持 Maxwell 分开舍入的逐位结果，代价是 MSL 里大量禁优化的 spvFMul 等）
        // Job reduced = true: relaxed precision - the translator marks NoContraction only on operations that must be exact (the default marks all: keeps the bit-exact results of Maxwell's separate rounding, at the cost of many optimization-disabled spvFMul etc. in the MSL)
        public bool QueryHostReducedPrecision() => _job["reduced"] is JsonNode n && (bool)n;
    }

    [JSExport]
    public static string Translate(string jobJson, byte[] code)
    {
        if (code == null || code.Length < 16 || code.Length > 1024 * 1024 || code.Length % 8 != 0)
            throw new ArgumentException("Shader must be 16 bytes to 1 MiB and 8-byte aligned.");
        if (jobJson == null || jobJson.Length > 65536)
            throw new ArgumentException("Job metadata must be at most 64 KiB.");
        var job = JsonNode.Parse(jobJson)?.AsObject() ?? throw new ArgumentException("Expected job object.");
        string key = (string)job["key"] ?? "synthetic";
        string stage = (string)job["stage"];
        if (stage is not ("cs" or "vs" or "ps" or "hs" or "ds"))
            throw new ArgumentException("Unsupported shader stage; geometry is not integrated.");
        if ((stage is "hs" or "ds") &&
            (job["allowUnverifiedTessellation"] is not JsonValue opt || !opt.TryGetValue<bool>(out var enabled) || !enabled))
            throw new ArgumentException("Tessellation requires explicit unverified-source-port opt-in.");
        if (stage != "cs")
        {
            // 图形输入以完整 0x50 字节 SPH 开始，不能让 ShaderHeader 的零阶段回退静默重新解释任务。
            // Graphics input begins with the complete 0x50-byte SPH. Do not allow
            // ShaderHeader's stage-zero fallback to silently reinterpret a job.
            if (code.Length < 0x60)
                throw new ArgumentException("Graphics input requires a 0x50-byte SPH plus instructions.");
            int headerStage = (int)((BitConverter.ToUInt32(code, 0) >> 10) & 15);
            if (headerStage != (stage switch { "vs" => 1, "hs" => 2, "ds" => 3, _ => 5 }))
                throw new ArgumentException("Declared graphics stage does not match SPH stage bits 10..13.");
            // 原始翻译器从这个精确 SPH 字段取得 hs OutputVertices；缺失则拒绝，绝不从无关阶段猜测补丁数量。
            // Original translator derives hs OutputVertices from this exact SPH field.
            // Reject absent data, never guess a patch count from an unrelated stage.
            if (stage == "hs" && (BitConverter.ToUInt32(code, 8) >> 24) == 0)
                throw new ArgumentException("Hull SPH requires nonzero threads per input primitive.");
            foreach (var (table, count) in new[] { ("attr", 32), ("fout", 8) })
            {
                if (job[table] is not JsonObject types)
                    throw new ArgumentException("Graphics jobs require explicit attr and fout type tables.");
                foreach (var item in types)
                {
                    if (!int.TryParse(item.Key, out int slot) || slot < 0 || slot >= count || slot.ToString(System.Globalization.CultureInfo.InvariantCulture) != item.Key ||
                        item.Value is not JsonValue value || !value.TryGetValue<string>(out var type) ||
                        type is not ("float" or "sint" or "uint"))
                        throw new ArgumentException("Invalid graphics attribute/output component type or slot.");
                }
            }
        }
        var acc = new Accessor(code, job);
        // 作业 debug = false：关 DebugMode（DebugMode 会跳过 AST 优化器，产物满是临时变量；不改语义）
        // Job debug = false: turn off DebugMode (DebugMode skips the AST optimizer and the output is full of temporaries; semantics unchanged)
        var flags = (job["debug"] is JsonNode dbg && !(bool)dbg ? 0 : TranslationFlags.DebugMode) | ((string)job["stage"] == "cs" ? TranslationFlags.Compute : 0);
        var ctx = Translator.CreateContext(0, acc, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags));
        ShaderProgram p = ctx.Translate();

        var res = new JsonObject
        {
            ["key"] = key,
            ["stage"] = (string)job["stage"],
            ["notes"] = new JsonArray(acc.Notes.ConvertAll(n => (JsonNode)n).ToArray()),
            ["cbufs"] = new JsonArray(),
            ["sbufs"] = new JsonArray(),
            ["textures"] = new JsonArray(),
            ["images"] = new JsonArray(),
            ["uses_frag_coord"] = p.Info.UsesFragCoord,
            ["frag_out_map"] = p.Info.FragmentOutputMap,
        };
        foreach (var b in p.Info.CBuffers) res["cbufs"].AsArray().Add(new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot });
        foreach (var b in p.Info.SBuffers) res["sbufs"].AsArray().Add(new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot, ["cb_slot"] = b.SbCbSlot, ["cb_offset"] = b.SbCbOffset, ["flags"] = (int)b.Flags });
        foreach (var (list, name) in new[] { (p.Info.Textures, "textures"), (p.Info.Images, "images") })
            foreach (var t in list) res[name].AsArray().Add(new JsonObject { ["binding"] = t.Binding, ["type"] = (int)t.Type, ["format"] = t.Format.ToString(), ["cbuf_slot"] = t.CbufSlot, ["handle"] = t.HandleIndex, ["array"] = t.ArrayLength, ["separate"] = t.Separate, ["flags"] = (int)t.Flags });
        return new JsonObject { ["spirvBase64"] = Convert.ToBase64String(p.BinaryCode), ["resources"] = res }.ToJsonString();
    }

    public static void Main() { }
}
