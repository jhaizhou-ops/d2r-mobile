#include "xbg_vulkan.h"
/* Build-only link root. No device or process side effects. */
extern "C" int xbg_link_check(const xbg_vk_info *info,xbg_vk_renderer **out,char *error,size_t bytes) {
    return xbg_vk_create(info,out,error,bytes);
}
