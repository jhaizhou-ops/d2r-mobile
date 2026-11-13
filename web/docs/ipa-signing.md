# Local IPA signing / 本地 IPA 签名

## English

### Prepare and use an identity

Signing is optional; leave it off for unsigned output. Browser generation needs
no Xcode or command-line tools. Choose a P12/PFX containing **certificate and
private key**, a profile covering the same certificate/team, actual bundle ID
and registered device, and the **P12 export password**, never Apple Account
password. A `.cer` alone cannot sign. Set [custom bundle ID](application-ui.md)
before generation; do not assume `org.d2rmobile.app` is available to your team.

Manual Distribution/Ad Hoc preparation requires an Apple Developer Program team
and Account Holder/Admin access. Manage only authorized certificates/devices;
never upload private keys/P12/passwords to a converter or signing service.
On Mac, follow Apple's [CSR](https://developer.apple.com/help/account/certificates/create-a-certificate-signing-request)
and [Keychain export](https://support.apple.com/guide/keychain-access/import-and-export-keychain-items-kyca35961/mac)
instructions, exporting the certificate with its original private key.
Without a Mac, run trusted local OpenSSL in a private directory with fresh filenames:

```sh
openssl genrsa -aes256 -out distribution.key 2048
openssl req -new -sha256 -key distribution.key -out distribution.csr
# Submit only the CSR to Apple, then substitute the issued certificate filename.
openssl x509 -inform DER -in distribution.cer -out distribution.pem
openssl pkcs12 -export -inkey distribution.key -in distribution.pem -out distribution.p12
```

Use separate strong key/export passwords. References: OpenSSL [genrsa](https://docs.openssl.org/3.5/man1/openssl-genrsa/),
[req](https://docs.openssl.org/3.5/man1/openssl-req/), [x509](https://docs.openssl.org/3.5/man1/openssl-x509/),
[pkcs12](https://docs.openssl.org/3.5/man1/openssl-pkcs12/).
[Feather's guide](https://github.com/claration/feather/wiki/Apple-Developer-Account-Non%E2%80%90Mac)
informed preparation only; its upload route is not recommended.

[Register UDID](https://developer.apple.com/help/account/devices/register-a-single-device)
and explicit App ID; create an [Ad Hoc profile](https://developer.apple.com/help/account/provisioning-profiles/create-an-ad-hoc-provisioning-profile/)
for that ID/certificate/devices, without unrelated capabilities. Regenerate after
changes; renew with the same team/ID. Back up saves; deleting the app to refresh
signing can delete resources/saves. Apple Ad Hoc distribution is not unsupported
certificate-free “ad-hoc signing.”

### Runtime safety and recovery

Await `packager.prepareSigning({signal})` **before credential reads**, then call
`attempt.sign(...)` and always `attempt.close()` in finally. Each attempt owns a
fresh disposable worker; the [factory](../../web/sealed-runtime/ipa-signing-factory.mjs)
retains verified public bytes only, with no retry downloads or old credentials.
Signing is separate from conversion: failure/cancellation preserves the exact
unsigned Blob and completed resources; retry needs fresh credentials, not regeneration.

[Archive storage](../../web/ipa-signing-zip.mjs), [file store](../../web/ipa-signing-fs.mjs)
and [worker](../../web/ipa-signing-worker-core.mjs) own bounded STORE ZIP32 validation,
immutable resources, narrow writable paths and cleanup. No whole-IPA MEMFS copy,
arbitrary native paths/edits/entitlements, persistent credentials or raw native
errors. The profile is embedded in the IPA; P12/password are not. JS strings
cannot be reliably erased. Offline profile checks do not establish Apple trust,
revocation, device eligibility or installability. Unsupported XML/entitlements
fail closed. Application-ID length includes profile prefix and reports a distinct
`SIGNING_APPLICATION_ID_TOO_LONG` error; see [contract](../../web/ipa-signing-contract.mjs).

### Build, licenses and validation

Sources: [pinned lbr77 port](https://github.com/lbr77/zsign-wasm/tree/0fb14887c61716508088e365447ffdedf7a1837d),
[zsign v1.1.3](https://github.com/zhlynn/zsign/releases/tag/v1.1.3),
[pins](../../web/toolchain/zsign-wasm/pins.json). Use Emscripten 4.0.20 and OpenSSL
3.5.9 libcrypto only; retain `no-module`, not `no-legacy`. Preserve zsign/port MIT
licenses and OpenSSL Apache-2.0 license/notice with receipts. Exact ABI/budgets
remain in the [audited build](../../web/toolchain/zsign-wasm/build.sh) and worker.

```sh
bash web/toolchain/zsign-wasm/build.sh
node web/toolchain/zsign-wasm/wasm-smoke.mjs
node --test web/tests/ipa-signing.test.mjs
```

Run from repository root; smoke needs Node 24+. Tests/build receipts do not prove
real credential signing or installation. Install the signed IPA separately, open
once, then copy completed resource folders with actual names as described in the
[installation guide](../../docs/WEB_IPA_PACKAGER.md). Signing/download never marks
device installation or copying complete.

## 简体中文

### 准备与使用身份

签名可选，关闭即可下载未签名 IPA；浏览器生成无需 Xcode 或命令行。选择同时包含**证书和私钥**
的 P12/PFX、覆盖同一证书/团队/实际包标识/已注册设备的描述文件，并输入 **P12 导出密码**，
不是 Apple 账号密码。单独 cer 不能签名；先在定制中设置包标识，不要假定默认 ID 可供自己的团队使用。

手动 Distribution/Ad Hoc 流程需开发者计划团队及 Account Holder/Admin 权限。
只管理授权证书/设备，私钥、P12、密码不得上传第三方转换或签名服务。Mac 按上方 Apple CSR/
钥匙串文档导出证书和原私钥。非 Mac 在私有目录使用可信 OpenSSL 和新文件名执行上方命令，
仅向 Apple 提交 CSR；私钥与导出密码分别设置。Feather 仅提供准备流程参考，不采用其上传路线。

注册 UDID 和明确 App ID，为对应 ID、证书、设备创建 Ad Hoc 描述文件，不启用无关能力。
设备/证书变化后重新生成，续期保持团队和 ID；备份存档，不要为刷新签名删除应用。
Apple Ad Hoc 分发不是无证书的 ad-hoc 签名模式。

### 安全、恢复与维护

必须先等待 prepareSigning，再读取凭据、执行 attempt.sign，并在 finally 调用 attempt.close。
每次独立可销毁线程，工厂只保留验证后的公开字节；重试不下载、不保留旧凭据。签名是独立事务，
失败/取消保留同一未签名 Blob 和完成资源；重新选择凭据即可重试，无需再次转换/导出。

上方归档、文件存储与线程源码负责规范 STORE ZIP32、有界读取、不可变资源、狭窄可写路径和清理。
不可整包复制到 MEMFS、开放任意原生路径/编辑/授权、持久保存凭据或显示原始错误。
描述文件有意嵌入 IPA，P12/密码不嵌入；JS 字符串无法保证擦除。离线检查不证明 Apple 信任、
撤销、设备资格或可安装性；不支持的 XML/授权失败关闭。应用标识长度包含描述文件前缀，超限
独立报告 SIGNING_APPLICATION_ID_TOO_LONG，精确限制见契约源码。

使用上方固定 port、zsign、编译器和 OpenSSL 版本；只链接 libcrypto，保留 no-module，不能改成 no-legacy。
部署保留 zsign/port MIT 许可证、OpenSSL Apache-2.0 许可证/声明和构建凭据，ABI/预算见构建及 worker。
从仓库根目录运行上方三条命令，冒烟需 Node 24+。构建或合成测试不证明真实签名/安装。
按安装指南单独安装签名 IPA、打开一次，再按实际名称复制完成资源；签名/下载不代表设备操作完成。
