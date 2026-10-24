// Supported source revisions are the reviewed v2 release and current upstream recipe.
export const RELEASE_OVERLAY_REVISION = "b811b90172f4eb40ec1bc89d903308e0c5ab88d0";
export const UPSTREAM_OVERLAY_REVISION = "e3756f8942b40eba6c851e881828a5b1bda17fd6";
export function requireOverlayRevision(revision) {
  if (![RELEASE_OVERLAY_REVISION, UPSTREAM_OVERLAY_REVISION].includes(revision))
    throw new Error("OVERLAY_RECIPE_REVISION");
  return true;
}
