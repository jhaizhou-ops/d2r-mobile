// 用模拟的 WSI / GPU 执行真实呈现回调，验证源图像与信号量生命周期，不创建窗口。
// Run the real presenter against simulated WSI/GPU calls to check source/semaphore lifetime without creating a window.
#define VK_USE_PLATFORM_WIN32_KHR
#include "../vulkan_present.h"
#include "../../gfx/vulkan/nvn_vk.h"
#include <cassert>
#include <algorithm>

static unsigned submissions, presents, waits, resets, idles, acquisitions, semaphore_id = 100;
static uint32_t current_image;
static bool source_pending, acquire_pending, present_pending[2];
static VkResult acquire_result = VK_SUCCESS, present_result = VK_SUCCESS, wait_result = VK_SUCCESS;
static VkSemaphore expected_acquired, expected_complete[2];
static unsigned created_semaphores;
template<class T> static T handle(unsigned value) { return (T)(uintptr_t)value; }
static BOOL WINAPI test_window(HWND) { return TRUE; }
static BOOL WINAPI test_iconic(HWND) { return FALSE; }
static BOOL WINAPI test_rect(HWND, LPRECT rect) { *rect = {0, 0, 1600, 900}; return TRUE; }
static VkResult test_create_instance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *) {
    return VK_ERROR_INITIALIZATION_FAILED;
}
static void test_format(VkPhysicalDevice, VkFormat, VkFormatProperties *properties) {
    *properties = {};
    properties->optimalTilingFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
}
static VkResult test_caps(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *caps) {
    *caps = {}; caps->minImageCount = 2; caps->currentExtent = {1600, 900};
    caps->supportedUsageFlags = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; return VK_SUCCESS;
}
static VkResult test_formats(VkPhysicalDevice, VkSurfaceKHR, uint32_t *count, VkSurfaceFormatKHR *formats) {
    *count = 1; if (formats) *formats = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}; return VK_SUCCESS;
}
static VkResult test_create_swap(VkDevice, const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *, VkSwapchainKHR *swap) {
    assert(info->presentMode == VK_PRESENT_MODE_FIFO_KHR && info->minImageCount == 2);
    created_semaphores = 0;
    *swap = handle<VkSwapchainKHR>(10); return VK_SUCCESS;
}
static void test_destroy_swap(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks *) {
    assert(!source_pending && !present_pending[0] && !present_pending[1]);
}
static VkResult test_images(VkDevice, VkSwapchainKHR, uint32_t *count, VkImage *images) {
    *count = 2; if (images) { images[0] = handle<VkImage>(20); images[1] = handle<VkImage>(21); } return VK_SUCCESS;
}
static VkResult test_create_semaphore(VkDevice, const VkSemaphoreCreateInfo *, const VkAllocationCallbacks *, VkSemaphore *semaphore) {
    assert(created_semaphores < 2);
    *semaphore = handle<VkSemaphore>(semaphore_id++);
    expected_complete[created_semaphores++] = *semaphore; return VK_SUCCESS;
}
static void test_destroy_semaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks *) {
    assert(!present_pending[0] && !present_pending[1]);
}
static VkResult test_acquire(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore semaphore, VkFence, uint32_t *index) {
    assert(!acquire_pending && semaphore == expected_acquired); acquisitions++;
    VkResult result = acquire_result; acquire_result = VK_SUCCESS;
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return result;
    current_image = acquisitions % 2; *index = current_image;
    present_pending[current_image] = false; acquire_pending = true; return result;
}
static VkResult test_reset_pool(VkDevice, VkCommandPool, VkCommandPoolResetFlags) { assert(!source_pending); return VK_SUCCESS; }
static VkResult test_begin(VkCommandBuffer, const VkCommandBufferBeginInfo *) { return VK_SUCCESS; }
static VkResult test_end(VkCommandBuffer) { return VK_SUCCESS; }
static VkResult test_submit(VkQueue, uint32_t count, const VkSubmitInfo *submit, VkFence fence) {
    assert(count == 1 && fence && acquire_pending && !source_pending && !present_pending[current_image]);
    assert(submit->waitSemaphoreCount == 1 && submit->signalSemaphoreCount == 1 && submit->commandBufferCount == 1);
    assert(*submit->pWaitSemaphores == expected_acquired && *submit->pSignalSemaphores == expected_complete[current_image]);
    acquire_pending = false; source_pending = true; submissions++; return VK_SUCCESS;
}
static VkResult test_present(VkQueue, const VkPresentInfoKHR *info) {
    assert(source_pending && info->waitSemaphoreCount == 1 && info->swapchainCount == 1);
    assert(*info->pImageIndices == current_image && !present_pending[current_image]);
    assert(*info->pWaitSemaphores == expected_complete[current_image]);
    present_pending[current_image] = true; presents++;
    VkResult result = present_result; present_result = VK_SUCCESS; return result;
}
static VkResult test_wait(VkDevice, uint32_t count, const VkFence *, VkBool32 all, uint64_t timeout) {
    assert(source_pending && count == 1 && all && timeout); waits++;
    if (wait_result != VK_SUCCESS) return wait_result;
    source_pending = false; return VK_SUCCESS;
}
static VkResult test_reset_fence(VkDevice, uint32_t count, const VkFence *) {
    assert(!source_pending && count == 1); resets++; return VK_SUCCESS;
}
static VkResult test_idle(VkQueue) {
    idles++; source_pending = false; present_pending[0] = present_pending[1] = false; return VK_SUCCESS;
}
static void test_blit(VkCommandBuffer, VkImage source, VkImageLayout from, VkImage, VkImageLayout to,
        uint32_t count, const VkImageBlit *blit, VkFilter) {
    assert(source == handle<VkImage>(50) && from == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
        to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && count == 1);
    assert(blit->srcOffsets[1].x == 1600 && blit->srcOffsets[1].y == 900);
}
#define IsWindow test_window
#define IsIconic test_iconic
#define GetClientRect test_rect
#define vkCreateInstance test_create_instance
#define vkGetPhysicalDeviceFormatProperties test_format
#define vkGetPhysicalDeviceSurfaceCapabilitiesKHR test_caps
#define vkGetPhysicalDeviceSurfaceFormatsKHR test_formats
#define vkCreateSwapchainKHR test_create_swap
#define vkDestroySwapchainKHR test_destroy_swap
#define vkGetSwapchainImagesKHR test_images
#define vkCreateSemaphore test_create_semaphore
#define vkDestroySemaphore test_destroy_semaphore
#define vkAcquireNextImageKHR test_acquire
#define vkResetCommandPool test_reset_pool
#define vkBeginCommandBuffer test_begin
#define vkEndCommandBuffer test_end
#define vkQueueSubmit test_submit
#define vkQueuePresentKHR test_present
#define vkWaitForFences test_wait
#define vkResetFences test_reset_fence
#define vkQueueWaitIdle test_idle
#define vkCmdBlitImage test_blit
#define vkCmdPipelineBarrier(...) ((void)0)
#define vkCmdClearColorImage(...) ((void)0)
#include "../vulkan_present.cpp"
extern "C" int dm_vk_initialize(const dm_vk_config *, char *, size_t) { abort(); }

static int frame() {
    return present(nullptr, nullptr, handle<VkImage>(50), VK_FORMAT_B8G8R8A8_UNORM, 1600, 900, nullptr, 1);
}
int main() {
    char error[256];
    const char *directories[] = {"C:\\Games\\d2\\shaders", "C:/Games/d2/shaders", "C:\\Games/d2\\shaders",
        "\\\\server\\share\\shaders"};
    const char *normalized[] = {"C:/Games/d2/shaders", "C:/Games/d2/shaders", "C:/Games/d2/shaders",
        "//server/share/shaders"};
    for (unsigned i = 0; i < 4; i++) {
        assert(dm_windows_vk_initialize(handle<HWND>(1), directories[i], error, sizeof error) != 0);
        assert(shader_directory == normalized[i] && !instance && !device);
        assert(strstr(error, "vkCreateInstance"));
    }
    window = handle<HWND>(1); device = handle<VkDevice>(2); physical = handle<VkPhysicalDevice>(3);
    queue = handle<VkQueue>(4); surface = handle<VkSurfaceKHR>(5); swapchain = handle<VkSwapchainKHR>(6);
    pool = handle<VkCommandPool>(7); command = handle<VkCommandBuffer>(8);
    acquired = handle<VkSemaphore>(9); fence = handle<VkFence>(10); extent = {1600, 900};
    images = {handle<VkImage>(20), handle<VkImage>(21)};
    complete = {handle<VkSemaphore>(30), handle<VkSemaphore>(31)};
    expected_acquired = acquired; expected_complete[0] = complete[0]; expected_complete[1] = complete[1];
    for (unsigned i = 0; i < 8; i++) {
        assert(!frame() && !source_pending);
        assert(submissions == i + 1 && waits == i + 1 && resets == i + 1 && !idles);
    }
    dirty = true; assert(!frame() && idles == 1);
    acquire_result = VK_ERROR_OUT_OF_DATE_KHR; assert(!frame() && idles == 2);
    unsigned before = acquisitions; acquire_result = VK_TIMEOUT;
    assert(!frame() && acquisitions == before + 2 && idles == 2);
    present_result = VK_SUBOPTIMAL_KHR; assert(!frame() && dirty && idles == 2);
    assert(!frame() && !dirty && idles == 3);
    before = submissions; present_result = VK_ERROR_OUT_OF_DATE_KHR;
    assert(!frame() && submissions == before + 2 && waits == submissions && resets == waits && idles == 4);
    before = resets; wait_result = VK_ERROR_DEVICE_LOST;
    assert(frame() == VK_ERROR_DEVICE_LOST && source_pending && resets == before && idles == 4);
    puts("Windows present: source-use fence retained, no steady-state queue idle, per-image reuse and swapchain recovery passed");
    return 0;
}
