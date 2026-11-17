// 经验证的 3.3.94012 图集使用浏览器编解码器编码后得到这些 PNG 摘要。
// 每个替代版本均绑定到其固定 PNG 摘要；不捆绑任何图像字节。
// The verified 3.3.94012 atlases encode to these PNG hashes with the browser codec.
// Each alternate is tied to its frozen PNG digest; no image bytes are bundled.
const browserDigests = Object.freeze({
  "dm_native_skill_0.png": Object.freeze({
    frozen: "15f751a2499a798509ab4746780414527b3405c8afc78b654e34cdadaf868617",
    browser: "a9e5a63e6eee6cf44cd96e77ad59f6067dc09dcf6d2bcd2bf77200327e6928e9",
  }),
  "dm_native_skill_1.png": Object.freeze({
    frozen: "9e81902dc36aa972136926773b443f954cc09a0051c7f69ae88e1f513298fa09",
    browser: "9b2bf9cad3a8bb494259e7f633846641006f7ad66d27e84053b8d57e8f5a0d57",
  }),
  "dm_native_skill_2.png": Object.freeze({
    frozen: "9a40e7cf2701caf119c17c11a202e473558f316ae7292ef63817b22a06d5bb5a",
    browser: "de7bd638a3277e3a124e02d3688bf2bf8fdd063e18814ac10c7db6a770f37c0c",
  }),
  "dm_native_skill_3.png": Object.freeze({
    frozen: "f7fbe1601f949ca34cf2627e10a5aeab2ad5519b1449d1a993dc09fadec655f8",
    browser: "86fe3aeed77eaa56993f167b23c76b48b2c8d8c6276456acac3a4e0d582fbe8c",
  }),
  "dm_native_skill_4.png": Object.freeze({
    frozen: "ac856222331383adad53ba3c7837f8be974831051ca77c9a89356f5ea0d63461",
    browser: "12cb489c73cf3816d2d6a5cfa791087da02a00e99a5099b959c1c20f1c5d6479",
  }),
  "dm_native_skill_5.png": Object.freeze({
    frozen: "05ccf4e35a5f69a77be07b901b64cdd279d275d8fab8a83de8ecf8920db360bb",
    browser: "a3c1a8d78b4722ebf601ffb2a7ce7cc3491a3241c9026bf365f3fb8d0e7d9e2f",
  }),
  "dm_native_skill_6.png": Object.freeze({
    frozen: "045f65a835a5ab1d52fa079183cbab7d47c16c754f437819b6e6103b3b9d87b6",
    browser: "92570e3296cf5c9d2c2c6b0a47483d3b244a8758899f10e0bec7767043331620",
  }),
  "dm_native_skill_7.png": Object.freeze({
    frozen: "9e588da57e3fbbb85e09920c55a661d2e491501f691bcdf03a0c2c7bef772516",
    browser: "f1eaf9d702047f4b0206743f01844b6e2d475e73e994c47e1c0f532629966156",
  }),
  "dm_native_skill_8.png": Object.freeze({
    frozen: "f549fbf808b865c2a2d10472e6bc236cf75ee71493e535b530b96969ceb5679b",
    browser: "13b4edb1c02c19538214f0aa29482a979ab9d64d426ca3295fceaa50e1ff3aff",
  }),
  "dm_native_circle.png": Object.freeze({
    frozen: "b13b019e1ef715700c604308f949cb9e46d7abca2dd233c3d0a4c2483c6fbbe6",
    browser: "e6eaba87e773e044cb92f06f8222bc7edfac98131934eda591325e217a946d4f",
  }),
  "dm_native_map.png": Object.freeze({
    frozen: "dccd07f3634889a8ea8cfb7a8613d77eb291958812446572f6077caddd1b9252",
    browser: "be53ddb1d1c9252b9c85ac9a7f9616e9fa5d060356b113681649a26539109f61",
  }),
  "dm_native_menu.png": Object.freeze({
    frozen: "f2d2d8605afce8faa10f4401da2dcdd3d61bcf2a9a72e8661143f84cd789fe4b",
    browser: "f99240a186a1c4ae335aeaeeacdd690d10e54c169c60309a94e16cbdea32a4cc",
  }),
});

export function acceptsNativeArtPngDigest(asset, digest) {
  if (typeof digest !== "string" || !/^[a-f0-9]{64}$/.test(digest)) return false;
  if (digest === asset?.decoded_png_sha256) return true;
  const pin = browserDigests[asset?.asset];
  return pin !== undefined && asset.decoded_png_sha256 === pin.frozen && digest === pin.browser;
}
