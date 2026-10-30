// 组装程序改写流程的输出文件；依据 ../../../program-release-catalog.mjs、./binary.mjs 的模块与调用约定。
// Assemble output files from the program-rewrite pipeline using the module and calling contracts in ../../../program-release-catalog.mjs、./binary.mjs.
import { PROGRAM_REWRITE_LIMITS } from "../../../program-release-catalog.mjs";
import { check, hash, hexAddress, unhex } from "./binary.mjs";
import { SOURCE_SHA256 } from "./rule-data.mjs";

const utf8 = new TextEncoder();
const line = (values) => `${values.join("\t")}\n`;

/**
 * 新输出映射具有与干净 /result 运行相同的过期清单语义。
 * Fresh output maps have the same stale-manifest semantics as clean /result runs. */
export async function buildOutputs(context, rules, keyIndex, bootlist = null) {
  const {
    image,
    patchOff,
    keyPageOff,
    textEnd,
    roOff,
    dataOff,
    moduleName,
    kinds,
    done,
    windows,
    jumps,
    relocator,
  } = context;
  const files = {};
  const put = (extension, text) => {
    files[`${moduleName}.${extension}`] = utf8.encode(text);
  };
  const patch = relocator.patchBytes();
  check(
    patchOff >= image.length && patchOff + patch.length <= PROGRAM_REWRITE_LIMITS.totalOutputBytes,
    "Rewriter output budget exceeded",
  );
  const output = new Uint8Array(patchOff + patch.length);
  output.set(image);
  output.set(patch, patchOff);
  files[`${moduleName}.patched.bin`] = output;
  files["build_src.sha256"] = utf8.encode(`${SOURCE_SHA256}  patch_nso.py\n`);
  const outputHash = rules.ruleRun || rules.privateAim ? await hash(output) : null;
  if (rules.privateAim)
    put(
      "aim.tsv",
      "# RULESET\tskill-aim-v1-private\n# ORIGINAL_CALL\t0x2faf04\t97fee7fd\n# ORIGINAL_CALLEE\t0x2b4ef8\n# SKILL_CALLER\t0x2b5b80\n# SVC\t0x86\n# INPUT\tactual GetNpad sampled right; move axis unchanged\n" +
        `# IMAGE_SHA256\t${outputHash}\n`,
    );
  if (rules.ruleRun) {
    let text = `# RULESET\t${rules.ruleset}\n# BUILD_ID\t${rules.buildId}\n# ORIGINAL_SHA256\t${rules.originalHash}\n# IMAGE_SHA256\t${outputHash}\n`;
    for (const [off, raw, why] of rules.ruleBlocks)
      text += line([hexAddress(off), raw, await hash(unhex(raw)), why]);
    put("rules.tsv", text);
  }
  let fixups = `# TARGET\t0x0\n# PATCH_OFF\t${hexAddress(patchOff)}\n# KEY_PAGE_OFF\t${hexAddress(keyPageOff)}\n# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t${hexAddress(textEnd)}\n# RO_OFF\t${hexAddress(roOff)}\n# DATA_OFF\t${hexAddress(dataOff)}\n`;
  for (const [key, index] of Object.entries(keyIndex)) fixups += `# KEY_${key}_IDX\t${index}\n`;
  put("fixups.tsv", fixups);
  put(
    "gates.tsv",
    relocator.gates
      .map(([ret, kind, arg, original]) => line([hexAddress(ret), kind, arg, hexAddress(original)]))
      .join(""),
  );
  let sites = "";
  for (const address of [...kinds.keys()].sort((a, b) => a - b))
    if (!done.has(address)) sites += line([hexAddress(address), kinds.get(address)[0]]);
  for (const segment of windows)
    sites += line([hexAddress(segment[0].address), "W", hexAddress(segment.at(-1).address + 4)]);
  for (const [address, [base, count]] of jumps)
    sites += line([hexAddress(address), "J", hexAddress(base), count]);
  put("sites.tsv", sites);
  const header = `# BUILD_ID\t${rules.buildId}\n`;
  put(
    "cuts.tsv",
    header +
      rules.cuts
        .map(([off, old, next, why, scope]) => line([hexAddress(off), old, next, scope, why]))
        .join(""),
  );
  put(
    "params.tsv",
    header +
      rules.params
        .map(([oa, , ob, , register, key, why]) =>
          line([hexAddress(oa), hexAddress(ob), `x${register}`, key, why]),
        )
        .join(""),
  );
  put(
    "obs.tsv",
    header + rules.obs.map(([off, id, old, why]) => line([hexAddress(off), id, old, why])).join(""),
  );
  if (moduleName === "main" && bootlist !== null) {
    check(bootlist instanceof Uint8Array, "Invalid bootlist");
    const firstLine = new TextDecoder("utf-8", { fatal: true }).decode(bootlist).split("\n", 1)[0];
    const fields = firstLine.trim().split(/\s+/u);
    check(
      fields.at(-1) === rules.buildId,
      `bootlist header build ID does not match main ${rules.buildId}`,
    );
    files["bootlist.tsv"] = bootlist.slice();
    files["bootlist.sha256"] = utf8.encode(`${await hash(bootlist)}  bootlist.tsv\n`);
  }
  check(
    Object.values(files).reduce((sum, bytes) => sum + bytes.length, 0) <=
      PROGRAM_REWRITE_LIMITS.totalOutputBytes,
    "Rewriter output budget exceeded",
  );
  return files;
}
