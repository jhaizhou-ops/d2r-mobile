"""组合固定上游签名源码与固定分支的浏览器桥接层。
Combine pinned upstream signing with the pinned fork's browser bridge.

这是源码派生适配，不是对分支中不透明二进制文件的重建。
This is a source-derived adaptation, not a rebuild of the fork's opaque binary.
仅修改构建目录中的副本，保留所有原始许可证声明。
Only build-directory copies are modified; all original license notices remain.
"""

from pathlib import Path
import hashlib
import json
import shutil
import sys

here = Path(__file__).resolve().parent
source, port, output = map(Path, sys.argv[1:])
if output.exists():
    raise SystemExit("Patched output already exists; use a fresh build directory")
shutil.copytree(source / "src", output)
for name in ["zsign_export.cpp", "zsign_export.h"]:
    shutil.copyfile(port / "native" / name, output / name)
shutil.copyfile(here / "profile-compatibility.inc", output / "profile-compatibility.inc")


def replace(path, old, new):
    file = output / path
    text = file.read_text()
    if text.count(old) != 1:
        raise SystemExit(f"Unexpected pinned source shape: {path}")
    file.write_text(text.replace(old, new))


sha = output / "common/sha.cpp"
text = sha.read_text()
start = text.index("bool ZSHA::SHAFile(")
end = text.index("bool ZSHA::SHABase64(", start)
text = text[:start] + (here / "stream-sha.inc").read_text() + "\n" + text[end:]
text = text.replace(
    "#include <openssl/sha.h>",
    "#include <openssl/sha.h>\n#include <openssl/evp.h>\n#include <openssl/crypto.h>",
)
sha.write_text(text)
# v1.1.3 已传播哈希错误；若此约束变化则拒绝构建。
# v1.1.3 already propagates hash errors; fail closed if that invariant changes.
if "if (!SHAFile(szFile, strSHA1, strSHA256))" not in text:
    raise SystemExit("Missing upstream resource-hash error propagation")
if "if (!ZSHA::SHABase64File(" not in (output / "bundle.cpp").read_text():
    raise SystemExit("Missing upstream bundle-hash error propagation")
fs = output / "common/fs.cpp"
text = fs.read_text()
start = text.index("void* ZFile::MapFile(")
end = text.index("bool ZFile::WriteFile(", start)
# 数据结构和基于 fopen 的映射设计源于固定的 lbr77 分支，增加读写检查和分配上限。
# The structure and fopen-backed mapping design are derived from the pinned
# lbr77 port, with explicit read/write error checks and a bounded allocation.
struct = """// D2R 浏览器对 lbr77 可移植文件映射的适配。
// D2R browser adaptation of lbr77's portable file mapping.
struct ZFileMappedInfo { string path; size_t offset; size_t size; bool ro; };
static map<void*, ZFileMappedInfo> s_mapFilesNoMmap;

"""
fs.write_text(text[:start] + struct + (here / "bounded-map.inc").read_text() + "\n" + text[end:])
replace(
    "zsign_export.cpp",
    "#define ZSIGN_EXPORT EMSCRIPTEN_KEEPALIVE",
    "#define ZSIGN_EXPORT",
)
replace(
    "zsign_export.cpp",
    '#define ZSIGN_VERSION "0.7"',
    '#define ZSIGN_VERSION "1.1.3-d2r-browser"',
)
replace(
    "zsign_export.cpp",
    '#include "zsign_export.h"',
    '#include "zsign_export.h"\n#include <openssl/provider.h>\n#include <openssl/rand.h>\n#include <openssl/crypto.h>',
)
# 保持分支 ABI，同时适配上游新增的动态库移除参数。
# Keep fork ABI; adapt to upstream's additional dylib-removal argument.
replace(
    "zsign_export.cpp",
    "\t\tarrDylibFiles,\n\t\tbForceSign,",
    "\t\tarrDylibFiles,\n\t\tvector<string>(),\n\t\tbForceSign,",
)
replace(
    "zsign_export.cpp",
    "\tstring inputFolder = SafePath(input_folder);",
    """    // D2R：若静态密码提供程序或浏览器随机源不可用，则拒绝签名。
    // D2R: fail closed if the static providers or browser entropy are unavailable.
    // no-module 将两个提供程序保留在 libcrypto 中，不使用动态模块或网络回退。
    // no-module builds keep both providers in libcrypto; no DSO/network fallback.
    ZLog::SetLogLever(ZLog::E_NONE);
    if (!OSSL_PROVIDER_load(NULL, "default") || !OSSL_PROVIDER_load(NULL, "legacy") ||
        RAND_status() != 1) return -205;
\tstring inputFolder = SafePath(input_folder);""",
)
# Init 完成后清零桥接层持有的密码副本，失败路径也必须清零。
# Wipe the bridge-owned password copy after Init, including the failure path.
old = """\tif (!zsa.Init(certFile, pkeyFile, provFile, entitlementsFile, passwd, bAdhoc, bSHA256Only, false)) {
\t\treturn -203;
\t}"""
new = """    bool initialized = zsa.Init(certFile, pkeyFile, provFile, entitlementsFile, passwd, bAdhoc, bSHA256Only, false);
    if (!passwd.empty()) OPENSSL_cleanse(&passwd[0], passwd.size());
    if (!initialized) return -203;"""
replace("zsign_export.cpp", old, new)
# 兼容性检查仅修改派生授权，不修改用户的原始描述文件或应用标识。
# Compatibility checks change derived entitlements, never the original profile or bundle ID.
replace(
    "openssl.h",
    "\tbool GenerateCMS(const string& strCDHashData,",
    "\tbool ValidateProfileForBundle(const string& bundleId, bool* applicationIdentifierTooLong = nullptr);\n\n\tbool GenerateCMS(const string& strCDHashData,",
)
with (output / "openssl.cpp").open("a") as file:
    file.write('\n#include "profile-compatibility.inc"\n')
replace(
    "zsign_export.cpp",
    "    if (!initialized) return -203;",
    """    if (!initialized) return -203;
    // 只读取固定根目录的原始标识；限制元数据大小并检查读取和关闭错误。
    // Read the unchanged identifier at the fixed root, bounding metadata and checking I/O.
    if (inputFolder != "/work/Payload/App.app" || !bundleId.empty() ||
        !bundleVersion.empty() || !displayName.empty() || !entitlementsFile.empty()) return -206;
    FILE* info = fopen("/work/Payload/App.app/Info.plist", "rb");
    if (!info) return -206;
    string infoData;
    char infoChunk[4096];
    bool infoValid = true;
    while (true) {
        size_t count = fread(infoChunk, 1, sizeof(infoChunk), info);
        if (infoData.size() + count > 1024 * 1024) { infoValid = false; break; }
        infoData.append(infoChunk, count);
        if (count < sizeof(infoChunk)) {
            infoValid = feof(info) && !ferror(info);
            break;
        }
    }
    if (fclose(info) != 0) infoValid = false;
    jvalue infoPlist;
    bool applicationIdentifierTooLong = false;
    if (!infoValid || infoData.empty() || !infoPlist.read_plist(infoData) ||
        !infoPlist.is_object() || !infoPlist["CFBundleIdentifier"].is_string() ||
        !zsa.ValidateProfileForBundle(infoPlist["CFBundleIdentifier"].as_string(),
                                      &applicationIdentifierTooLong))
        return applicationIdentifierTooLong ? -207 : -206;""",
)
# wasm32 的 int64_t 不一定是 long；使用工具链提供的格式宏。
# wasm32 int64_t is not necessarily long; use the toolchain's format macro.
replace(
    "common/json.cpp",
    "#ifdef __APPLE__\n#define PRId64",
    "#ifdef __EMSCRIPTEN__\n#include <inttypes.h>\n#elif defined(__APPLE__)\n#define PRId64",
)
replace(
    "common/util.cpp",
    "#ifdef _WIN32\n#define PRId64",
    "#ifdef __EMSCRIPTEN__\n#include <inttypes.h>\n#elif defined(_WIN32)\n#define PRId64",
)
# 显式转换日期值，确保可变参数类型与 PRId64 完全一致。
# Cast dates explicitly so the variadic argument type matches PRId64 exactly.
replace(
    "common/json.cpp",
    '#ifdef _WIN32\n\t\t::snprintf(buf, 256, "%lld", m_value.v_date);',
    '#ifdef __EMSCRIPTEN__\n\t\t::snprintf(buf, 256, "%" PRId64, static_cast<int64_t>(m_value.v_date));\n#elif defined(_WIN32)\n\t\t::snprintf(buf, 256, "%lld", m_value.v_date);',
)
# 在派生副本中修复 XML 字符串往返；不修改固定上游源码或二进制 plist 解析器。
# Fix XML string round-trips in the derived copy, not the pinned upstream or binary plist reader.
plist = output / "common/json.cpp"
text = plist.read_text()
start = text.index("bool jpreader::_decode_string(")
end = text.index("bool jpreader::_add_error(", start)
text = (
    text[:start]
    + r"""bool jpreader::_decode_string(ptoken& token, string& strdec)
{
    strdec.clear();
    strdec.reserve(static_cast<size_t>(token.pend - token.pbegin));
    const char* cursor = token.pbegin;
    while (cursor != token.pend) {
        unsigned char ch = static_cast<unsigned char>(*cursor++);
        if (ch != '&') {
            // 数据和日期保留既有的格式空白处理；键与字符串保留内容。
            // Keep existing data/date formatting whitespace handling; preserve key/string content.
            if (token.type != ptoken::E_PTOKEN_KEY && token.type != ptoken::E_PTOKEN_STRING &&
                (ch == '\t' || ch == '\r' || ch == '\n')) continue;
            if (ch < 0x20 && ch != '\t' && ch != '\r' && ch != '\n')
                return _add_error("Invalid XML character", token.pbegin);
            // XML 原始换行规范化；字符引用中的换行不受此规则影响。
            // Normalize literal XML line endings, not character-reference line endings.
            if (ch == '\r') {
                if (cursor != token.pend && *cursor == '\n') ++cursor;
                ch = '\n';
            }
            strdec.push_back(static_cast<char>(ch));
            continue;
        }
        const char* start = cursor;
        while (cursor != token.pend && *cursor != ';') ++cursor;
        if (cursor == token.pend || *cursor != ';')
            return _add_error("Invalid XML entity", token.pbegin);
        string entity(start, cursor++);
        uint32_t code = 0;
        if (entity == "amp") code = '&';
        else if (entity == "lt") code = '<';
        else if (entity == "gt") code = '>';
        else if (entity == "quot") code = '"';
        else if (entity == "apos") code = '\'';
        else {
            size_t at = 1;
            uint32_t base = 10;
            if (entity.empty() || entity[0] != '#')
                return _add_error("Unknown XML entity", token.pbegin);
            if (at < entity.size() && entity[at] == 'x') { base = 16; ++at; }
            if (at == entity.size()) return _add_error("Empty XML character reference", token.pbegin);
            for (; at < entity.size(); ++at) {
                unsigned char digit = static_cast<unsigned char>(entity[at]);
                uint32_t value = digit >= '0' && digit <= '9' ? digit - '0' :
                    digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 :
                    digit >= 'A' && digit <= 'F' ? digit - 'A' + 10 : 16;
                if (value >= base || code > (0x10ffffU - value) / base)
                    return _add_error("Invalid XML character reference", token.pbegin);
                code = code * base + value;
            }
        }
        if (!(code == 9 || code == 10 || code == 13 ||
              (code >= 0x20 && code <= 0xd7ff) ||
              (code >= 0xe000 && code <= 0xfffd) || (code >= 0x10000 && code <= 0x10ffff)))
            return _add_error("Invalid XML character reference", token.pbegin);
        // 仅解码一次；例如 &amp;lt; 必须保留为字面量 &lt;。
        // Decode once only: for example, &amp;lt; remains the literal text &lt;.
        if (code < 0x80) strdec.push_back(static_cast<char>(code));
        else {
            if (code < 0x800) strdec.push_back(static_cast<char>(0xc0 | (code >> 6)));
            else {
                if (code < 0x10000) strdec.push_back(static_cast<char>(0xe0 | (code >> 12)));
                else {
                    strdec.push_back(static_cast<char>(0xf0 | (code >> 18)));
                    strdec.push_back(static_cast<char>(0x80 | ((code >> 12) & 63)));
                }
                strdec.push_back(static_cast<char>(0x80 | ((code >> 6) & 63)));
            }
            strdec.push_back(static_cast<char>(0x80 | (code & 63)));
        }
    }
    return true;
}

"""
    + text[end:]
)
# 数据、日期和字符串调用方必须传播实体解析错误。
# Data, date and string callers must propagate entity parsing failures.
old = "\t\t_decode_string(token, strval);"
if text.count(old) != 3:
    raise SystemExit("Unexpected pinned plist string callers")
text = text.replace(old, "\t\tif (!_decode_string(token, strval)) return false;")
plist.write_text(text)
# 解码后的结束符和回车必须重新转义，保持有效 XML 与字符引用语义。
# Re-escape decoded terminators and carriage returns to preserve XML validity and reference semantics.
replace(
    "common/json.cpp",
    '\t//_string_replace(str, ">", "&gt;");\t//optional',
    '\t_string_replace(str, ">", "&gt;");\n\t_string_replace(str, "\\r", "&#13;");',
)
report = {
    "patches": {
        p.name: hashlib.sha256(p.read_bytes()).hexdigest()
        for p in [
            here / "prepare-source.py",
            here / "stream-sha.inc",
            here / "bounded-map.inc",
            here / "profile-compatibility.inc",
        ]
    },
    "modifiedFiles": [
        "common/sha.cpp",
        "common/fs.cpp",
        "zsign_export.cpp",
        "common/json.cpp",
        "common/util.cpp",
        "openssl.cpp",
        "openssl.h",
    ],
    "portedFiles": ["zsign_export.cpp", "zsign_export.h"],
}
(output.parent / "patch-report.json").write_text(json.dumps(report, indent=2) + "\n")
