#pragma once
/* Pure eligibility policy. Aspect bits match Vulkan COLOR=1/DEPTH=2/STENCIL=4.
 * A load clear is allowed only when it replaces the same explicit clear at the
 * start of a new pass, over the whole selected attachment. Other aspects LOAD.
 */
static inline unsigned xbg_load_clear_aspects(int new_pass, int partial_mask,
    unsigned attachment_aspects, unsigned cleared_aspects,
    unsigned pass_w, unsigned pass_h, unsigned attachment_w, unsigned attachment_h,
    int rect_x, int rect_y, unsigned rect_w, unsigned rect_h) {
    if (!new_pass || partial_mask || !pass_w || !pass_h ||
        pass_w != attachment_w || pass_h != attachment_h ||
        rect_x || rect_y || rect_w != pass_w || rect_h != pass_h) return 0;
    if (!cleared_aspects || (cleared_aspects & ~7u) ||
        (cleared_aspects & ~attachment_aspects) ||
        ((cleared_aspects & 1u) && (cleared_aspects & 6u))) return 0;
    return cleared_aspects;
}
