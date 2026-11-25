# shadcn/ui source provenance

Components retrieved directly from the official shadcn/ui registry on 2026-10-20
and 2026-10-23.
These are upstream component sources, not lookalike reimplementations.
Source adaptations are `from "cn"` → `from "@/lib/utils"` for the standard local
clsx/tailwind-merge helper, and registry-local imports → `@/components/ui/`.
No alternate UI library or hand-written replacement primitives are used.
The existing Input uses secondary/accent theme utilities for its native file-picker
button, and AccordionContent adds a small inline inset so keyboard focus rings are
not clipped by the upstream overflow-hidden animation container.
已移除未使用的组件与导出；下方来源记录保留作历史归属。
Unused scaffold was removed: `alert.tsx`, `CardAction`, `CardFooter`, and the public
`buttonVariants` export. The registry records below remain as historical attribution.

- `button.tsx`: https://ui.shadcn.com/r/styles/new-york-v4/button.json
  Registry response SHA-256: `4b8d7d848742727973c1b088af8d15f0b628272a1316f2d2fa344e73ddff9e9c`
- `card.tsx`: https://ui.shadcn.com/r/styles/new-york-v4/card.json
  Registry response SHA-256: `c7ebaa49ba2a0613838558f3892c5007523c2dc26d6b36a244145d454a796106`
- `alert.tsx`: https://ui.shadcn.com/r/styles/new-york-v4/alert.json
  Registry response SHA-256: `413c97869ec0e53efa4a37f590833bcf4a7a57255f5a2e909c6bb5bac7b4570d`

Added on 2026-10-23 for the option panels and form controls:

- `accordion.tsx`: https://ui.shadcn.com/r/styles/new-york-v4/accordion.json
  Registry response SHA-256: `93254d43d27259998b10c7bb5261f85bdc44509d46309a5c10a1e5d126a8d279`
- `input.tsx`: https://ui.shadcn.com/r/styles/new-york-v4/input.json
  Registry response SHA-256: `b1fffa12ba72ce30a291749012b47235eda110648b8106725ced22b5b31c8f1b`
- `checkbox.tsx`: https://ui.shadcn.com/r/styles/new-york-v4/checkbox.json
  Registry response SHA-256: `567b649e4cd68c05bfdc58ca00fa04aee38bca291cd6f16ed7776ec06ba3d6bd`
- `native-select.tsx`: https://ui.shadcn.com/r/styles/new-york-v4/native-select.json
  Registry response SHA-256: `950729a758bef332865adc55f25c16667f45347ef850ee7be20b741667f55d8b`
- `field.tsx`: https://ui.shadcn.com/r/styles/new-york-v4/field.json
  Registry response SHA-256: `5524eca8a5aca35b2f02dac696144053ea8d3d02f722676942e4e92bb08efeb2`
- `label.tsx`: https://ui.shadcn.com/r/styles/new-york-v4/label.json
  Registry response SHA-256: `b03d6bc91da205758df87c5403d74bae4d742f1f4ac5ae0b94ad7a29ab6350ea`
- `separator.tsx`: https://ui.shadcn.com/r/styles/new-york-v4/separator.json
  Registry response SHA-256: `4242a9fe839cae9083acff1280b7cccfda10e97bbc83283aed12d2d12bebdc6a`

`lucide-react` 1.61.0 supplies the official component icon imports. It is the only
new package; the existing React, Radix UI, CVA and Tailwind setup is retained.
The build copies this component license and the installed Lucide ISC/Feather MIT
notices verbatim into `dist/third-party-notices.txt`; the preview manifest includes
its size and digest. This is bundle attribution, not a change to repository licensing.
Application composition uses the components’ `className` API and shadcn theme tokens to keep fields and
triggers compact with a teal accent. Force-mounted accordion form content is hidden in its closed
state so local file selections are not cleared by collapsing a panel.

Setup: https://ui.shadcn.com/docs/installation/vite
License: MIT, https://github.com/shadcn-ui/ui/blob/main/LICENSE.md

The copied components also receive standard Oxfmt formatting under the workspace
configuration. The hashes above identify original registry responses, not the
locally formatted files. Formatting does not introduce custom component styles.

## Installation guide

Preparation uses plain, always-visible numbered text, with no extra UI primitive.
The FAQ reuses the existing official shadcn Accordion; it does not hide preparation
or mark manual installation/copying complete. No additional component library is used.

## Overall generation Progress

Added on 2026-10-23 for the overall generation estimate:

- `progress.tsx`: https://ui.shadcn.com/r/styles/new-york-v4/progress.json
  Registry response SHA-256: `ca0c7ae4d88b8039a9ee64653569cf8be6a058070d9f719d2294c9d49d56bcab`
  Official documentation: https://ui.shadcn.com/docs/components/radix/progress
  In addition to the standard `cn` import adaptation, `value` is forwarded to the
  Radix Root so its determinate state and `aria-valuenow` match the indicator.
  This uses the existing Radix dependency, with no additional package.
