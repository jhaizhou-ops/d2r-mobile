// Win32 Vulkan WSI：真实交换链、按图像分配的呈现信号量、同步 blit 与缩放留边。
// Win32 Vulkan WSI: real swapchain, per-image present semaphores, synchronous blits and aspect-preserving scaling.
#define VK_USE_PLATFORM_WIN32_KHR
#include "vulkan_present.h"
#include "../gfx/vulkan/nvn_vk.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
namespace {
HWND window;
VkInstance instance;
VkPhysicalDevice physical;
VkDevice device;
VkQueue queue;
uint32_t family;
VkSurfaceKHR surface;
VkSwapchainKHR swapchain;
VkFormat swap_format;
VkExtent2D extent;
VkCommandPool pool;
VkCommandBuffer command;
VkSemaphore acquired;
VkFence fence;
std::vector<VkImage> images;
std::vector<VkSemaphore> complete;
std::string shader_directory;
bool dirty;
void log_message(void *, const char *message) { fprintf(stderr, "[dm] %s\n", message); }
[[noreturn]] void fatal(void *, const char *message) { fprintf(stderr, "[dm] FATAL Vulkan: %s\n", message); abort(); }
int has_window(void *) { return IsWindow(window) != FALSE; }
void destroy_swap() {
    for (auto semaphore : complete) vkDestroySemaphore(device, semaphore, nullptr);
    complete.clear(); images.clear();
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
}
void cleanup() {
    if (device) {
        vkDeviceWaitIdle(device);
        destroy_swap();
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (acquired) vkDestroySemaphore(device, acquired, nullptr);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyDevice(device, nullptr);
    }
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
    instance = VK_NULL_HANDLE; device = VK_NULL_HANDLE; surface = VK_NULL_HANDLE;
    fence = VK_NULL_HANDLE; acquired = VK_NULL_HANDLE; pool = VK_NULL_HANDLE;
}
VkResult create_swap() {
    VkSurfaceCapabilitiesKHR capabilities;
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities);
    if (result) return result;
    if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    uint32_t count = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr);
    if (result || !count) return result ? result : VK_ERROR_FORMAT_NOT_SUPPORTED;
    std::vector<VkSurfaceFormatKHR> formats(count);
    if ((result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()))) return result;
    auto format = formats[0];
    if (format.format == VK_FORMAT_UNDEFINED) format.format = VK_FORMAT_B8G8R8A8_UNORM;
    for (auto candidate : formats) if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM || candidate.format == VK_FORMAT_R8G8B8A8_UNORM) { format = candidate; break; }
    VkFormatProperties properties; vkGetPhysicalDeviceFormatProperties(physical, format.format, &properties);
    if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        RECT rect; GetClientRect(window, &rect);
        extent.width = std::clamp(uint32_t(std::max(0L, rect.right)), capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(uint32_t(std::max(0L, rect.bottom)), capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    if (!extent.width || !extent.height) return VK_NOT_READY;
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface; info.minImageCount = std::max(2u, capabilities.minImageCount);
    if (capabilities.maxImageCount) info.minImageCount = std::min(info.minImageCount, capabilities.maxImageCount);
    info.imageFormat = format.format; info.imageColorSpace = format.colorSpace; info.imageExtent = extent; info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT; info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    for (uint32_t bit = 1; bit <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; bit <<= 1)
        if (capabilities.supportedCompositeAlpha & bit) { info.compositeAlpha = VkCompositeAlphaFlagBitsKHR(bit); break; }
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR; info.clipped = VK_TRUE; info.oldSwapchain = swapchain;
    VkSwapchainKHR replacement = VK_NULL_HANDLE;
    result = vkCreateSwapchainKHR(device, &info, nullptr, &replacement);
    destroy_swap();
    if (result) return result;
    swapchain = replacement; swap_format = format.format;
    if ((result = vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr))) return result;
    images.resize(count);
    if ((result = vkGetSwapchainImagesKHR(device, swapchain, &count, images.data()))) return result;
    images.resize(count);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < count; i++) {
        VkSemaphore value;
        if ((result = vkCreateSemaphore(device, &semaphore, nullptr, &value))) return result;
        complete.push_back(value);
    }
    dirty = false;
    return VK_SUCCESS;
}
int present(void *, void *, VkImage source, VkFormat format, uint32_t width, uint32_t height, const int32_t crop[4], int32_t interval) {
    (void)interval;
    VkFormatProperties source_properties; vkGetPhysicalDeviceFormatProperties(physical, format, &source_properties);
    if (!(source_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    for (;;) {
        if (!IsWindow(window)) return VK_ERROR_SURFACE_LOST_KHR;
        RECT client; GetClientRect(window, &client);
        if (IsIconic(window) || client.right <= 0 || client.bottom <= 0) { Sleep(16); continue; }
        if (!swapchain || dirty || extent.width != uint32_t(client.right) || extent.height != uint32_t(client.bottom)) {
            VkResult result = vkQueueWaitIdle(queue); if (result) return result;
            result = create_swap();
            if (result == VK_NOT_READY || result == VK_ERROR_OUT_OF_DATE_KHR) { Sleep(16); continue; }
            if (result) return result;
        }
        uint32_t index;
        VkResult result = vkAcquireNextImageKHR(device, swapchain, 1000000000ull, acquired, VK_NULL_HANDLE, &index);
        if (result == VK_TIMEOUT) continue;
        if (result == VK_ERROR_OUT_OF_DATE_KHR) { dirty = true; continue; }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return result;
        bool suboptimal = result == VK_SUBOPTIMAL_KHR;
        if ((result = vkResetCommandPool(device, pool, 0))) return result;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if ((result = vkBeginCommandBuffer(command, &begin))) return result;
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[index]; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkClearColorValue black{{0, 0, 0, 1}};
        vkCmdClearColorImage(command, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &barrier.subresourceRange);
        barrier.oldLayout = barrier.newLayout; barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        int32_t x = 0, y = 0, w = int32_t(width), h = int32_t(height);
        if (crop && crop[2] > 0 && crop[3] > 0) {
            x = crop[0]; y = crop[1]; w = crop[2]; h = crop[3];
            if (x < 0 || y < 0 || int64_t(x) + w > width || int64_t(y) + h > height) return VK_ERROR_INITIALIZATION_FAILED;
        }
        uint32_t dw = extent.width, dh = uint32_t(uint64_t(dw) * uint32_t(h) / uint32_t(w));
        if (dh > extent.height) { dh = extent.height; dw = uint32_t(uint64_t(dh) * uint32_t(w) / uint32_t(h)); }
        int32_t dx = int32_t((extent.width - dw) / 2), dy = int32_t((extent.height - dh) / 2);
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {x, y, 0}; blit.srcOffsets[1] = {x + w, y + h, 1};
        blit.dstOffsets[0] = {dx, dy, 0}; blit.dstOffsets[1] = {dx + int32_t(dw), dy + int32_t(dh), 1};
        VkFilter filter = source_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        vkCmdBlitImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = 0;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        if ((result = vkEndCommandBuffer(command))) return result;
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &acquired; submit.pWaitDstStageMask = &stage;
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command; submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &complete[index];
        if ((result = vkQueueSubmit(queue, 1, &submit, fence))) return result;
        VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        info.waitSemaphoreCount = 1; info.pWaitSemaphores = &complete[index]; info.swapchainCount = 1; info.pSwapchains = &swapchain; info.pImageIndices = &index;
        VkResult presented = vkQueuePresentKHR(queue, &info);
        if ((result = vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull))) return result;
        if ((result = vkResetFences(device, 1, &fence))) return result;
        // fence 已结束所有源图像读取；呈现信号量按图像复用，等下次取得该图像即可。
        // The fence completes all source-image reads; per-image present semaphores are reused only after reacquiring that image.
        if (presented == VK_ERROR_OUT_OF_DATE_KHR) { dirty = true; continue; }
        if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR) return presented;
        dirty = suboptimal || presented == VK_SUBOPTIMAL_KHR;
        return 0;
    }
}
}
extern "C" int dm_windows_vk_initialize(HWND target, const char *shaders, char *error, size_t size) {
    auto fail = [&](const char *operation, VkResult result) {
        if (error && size) snprintf(error, size, "%s: VkResult %d", operation, int(result));
        cleanup(); return -1;
    };
    if (instance || !IsWindow(target) || !shaders || !*shaders) return fail("Invalid Windows Vulkan initialization", VK_ERROR_INITIALIZATION_FAILED);
    window = target; shader_directory = shaders;
    // The shared engine locates parent-directory markers and caches using '/'.
    std::replace(shader_directory.begin(), shader_directory.end(), '\\', '/');
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName = "D2R Mobile"; app.apiVersion = VK_API_VERSION_1_1;
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &app; create.enabledExtensionCount = 2; create.ppEnabledExtensionNames = extensions;
    VkResult result = vkCreateInstance(&create, nullptr, &instance); if (result) return fail("vkCreateInstance", result);
    VkWin32SurfaceCreateInfoKHR win32{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    win32.hinstance = GetModuleHandleW(nullptr); win32.hwnd = window;
    if ((result = vkCreateWin32SurfaceKHR(instance, &win32, nullptr, &surface))) return fail("vkCreateWin32SurfaceKHR", result);
    uint32_t count = 0;
    if ((result = vkEnumeratePhysicalDevices(instance, &count, nullptr))) return fail("vkEnumeratePhysicalDevices", result);
    if (!count) return fail("No Vulkan device", VK_ERROR_INITIALIZATION_FAILED);
    std::vector<VkPhysicalDevice> candidates(count);
    if ((result = vkEnumeratePhysicalDevices(instance, &count, candidates.data()))) return fail("vkEnumeratePhysicalDevices", result);
    for (auto candidate : candidates) {
        uint32_t n = 0; vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n); vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, families.data());
        for (uint32_t i = 0; i < n; i++) {
            VkBool32 supported = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &supported) != VK_SUCCESS) continue;
            if (supported && (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                physical = candidate; family = i; break;
            }
        }
        if (physical) break;
    }
    if (!physical) return fail("No graphics/compute/present queue", VK_ERROR_FEATURE_NOT_PRESENT);
    VkPhysicalDeviceShaderDrawParametersFeatures draw{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext = &draw;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!draw.shaderDrawParameters) return fail("shaderDrawParameters unavailable", VK_ERROR_FEATURE_NOT_PRESENT);
    if ((result = vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr))) return fail("Device extension count", result);
    std::vector<VkExtensionProperties> available(count);
    if ((result = vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.data()))) return fail("Device extensions", result);
    bool mirror = false;
    for (auto extension : available) if (!strcmp(extension.extensionName, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME)) mirror = true;
    std::vector<const char *> device_extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (mirror) device_extensions.push_back(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
    float priority = 1;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = family; queue_info.queueCount = 1; queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &draw; device_info.pEnabledFeatures = &features.features; device_info.queueCreateInfoCount = 1; device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = uint32_t(device_extensions.size()); device_info.ppEnabledExtensionNames = device_extensions.data();
    if ((result = vkCreateDevice(physical, &device_info, nullptr, &device))) return fail("vkCreateDevice", result);
    vkGetDeviceQueue(device, family, 0, &queue);
    if ((result = create_swap())) return fail("Create Windows swapchain", result);
    VkCommandPoolCreateInfo command_pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    command_pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; command_pool.queueFamilyIndex = family;
    if ((result = vkCreateCommandPool(device, &command_pool, nullptr, &pool))) return fail("Present pool", result);
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = pool; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount = 1;
    if ((result = vkAllocateCommandBuffers(device, &allocation, &command))) return fail("Present command", result);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if ((result = vkCreateSemaphore(device, &semaphore, nullptr, &acquired))) return fail("Acquire semaphore", result);
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if ((result = vkCreateFence(device, &fence_info, nullptr, &fence))) return fail("Present fence", result);
    dm_vk_config config{}; config.size = sizeof config; config.game.size = sizeof config.game;
    config.game.physical = physical; config.game.device = device; config.game.queue = queue; config.game.queue_family = family;
    config.game.enabled_core = features.features; config.game.enabled_shader_draw_parameters = draw.shaderDrawParameters;
    config.game.fence_timeout_ns = 10000000000ull; config.game.shader_directory = shader_directory.c_str();
    config.game.present = present; config.game.log = log_message; config.game.fatal = fatal; config.has_window = has_window;
    config.sampler_mirror_clamp_to_edge = mirror;
    int status = dm_vk_initialize(&config, error, size);
    if (status) cleanup();
    return status;
}
