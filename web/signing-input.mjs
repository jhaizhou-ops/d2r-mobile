// 凭据仅在本地签名时读取；已读取的独立缓冲区在所有退出路径擦除。
// Read credentials only for local signing; wipe owned read buffers on every exit path.
import { IPA_SIGNING_LIMITS, clearSigningBuffers, signingError } from "./ipa-signing-contract.mjs";
export async function signWithLocalFiles({
  unsigned,
  sign,
  identityFile,
  profileFile,
  password,
  signal,
  onProgress,
}) {
  const buffers = [];
  const check = () => signal?.throwIfAborted();
  const validFile = (file, limit) => file instanceof Blob && file.size > 0 && file.size <= limit;
  try {
    check();
    if (
      !validFile(identityFile, IPA_SIGNING_LIMITS.identityBytes) ||
      !validFile(profileFile, IPA_SIGNING_LIMITS.profileBytes) ||
      typeof password !== "string" ||
      password.length > IPA_SIGNING_LIMITS.passwordCharacters ||
      password.includes("\0")
    )
      throw signingError("SIGNING_INPUT_INVALID");
    const identity = await identityFile.arrayBuffer();
    buffers.push(identity);
    check();
    const profile = await profileFile.arrayBuffer();
    buffers.push(profile);
    check();
    return await sign({
      ipa: unsigned.blob,
      credentials: { kind: "p12", identity, profile, password },
      signal,
      onProgress,
    });
  } finally {
    clearSigningBuffers(buffers);
  }
}
