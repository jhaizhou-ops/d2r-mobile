// 独立开发探针：只执行包内静态代码及公开 API，不装载游戏、不修改保留寄存器。
#include "platform_checks.h"
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <new>
#include <sstream>
#include <string>

namespace {
std::atomic<bool> busy{false};
bool Now(int64_t &ns)
{
    timespec t{};
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return false;
    ns = int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
    return true;
}
// 保留实际函数调用，防止优化器将判据直接常量折叠。
__attribute__((noinline)) uint64_t StaticRoundTrip(uint64_t x) { return x * x + 3; }
struct TlsShared {
    pthread_key_t key{};
    std::atomic<unsigned> ready{0};
};
struct TlsThread {
    TlsShared *shared;
    unsigned token;
    int setResult = -1;
    bool isolated = false;
    bool rendezvous = false;
};
void *TlsWorker(void *opaque)
{
    auto &t = *static_cast<TlsThread *>(opaque);
    t.setResult = pthread_setspecific(t.shared->key, &t.token);
    t.shared->ready.fetch_add(1);
    int64_t start = 0, now = 0;
    if (!Now(start)) return nullptr;
    // 同时存活且都已写入后再读，避免串行线程也被误报为隔离通过。
    for (unsigned attempt = 0; attempt < 100000; ++attempt) {
        if (t.shared->ready.load() == 2) { t.rendezvous = true; break; }
        if (!Now(now) || now - start > 500000000) break;
        sched_yield();
    }
    t.isolated = t.setResult == 0 && t.rendezvous;
    for (unsigned i = 0; i < 1024 && t.isolated; ++i) {
        t.isolated = pthread_getspecific(t.shared->key) == &t.token;
        sched_yield();
    }
    return nullptr;
}
void CheckCpu(std::ostream &out)
{
    volatile uint64_t input = 7;
    const uint64_t actual = StaticRoundTrip(input);
    out << "static_call=" << (actual == 52 ? "PASS" : "FAIL") << " input=7 expected=52 actual=" << actual << '\n';
    int64_t first = 0, last = 0, current = 0;
    bool ok = Now(first);
    last = first;
    unsigned samples = 0;
    for (; samples < 1024 && ok; ++samples) {
        ok = Now(current) && current >= last;
        last = current;
    }
    out << "monotonic=" << (ok && last > first ? "PASS" : "FAIL")
        << " samples=" << samples << " elapsedNs=" << last - first << '\n';
    TlsShared shared;
    const int keyResult = pthread_key_create(&shared.key, nullptr);
    if (keyResult != 0) { out << "pthread_tls=FAIL keyCreate=" << keyResult << '\n'; return; }
    unsigned parentToken = 99;
    const int parentSet = pthread_setspecific(shared.key, &parentToken);
    std::array<TlsThread, 2> args{{{&shared, 11}, {&shared, 22}}};
    std::array<pthread_t, 2> threads{};
    std::array<int, 2> created{};
    for (unsigned i = 0; i < 2; ++i) created[i] = pthread_create(&threads[i], nullptr, TlsWorker, &args[i]);
    bool passed = parentSet == 0;
    for (unsigned i = 0; i < 2; ++i) {
        // 每个工作线程只有有界循环、无互斥锁/条件等待；join 后才释放栈和 key。
        const int joined = created[i] == 0 ? pthread_join(threads[i], nullptr) : -1;
        passed = passed && created[i] == 0 && joined == 0 && args[i].isolated;
        out << "tls_thread=" << i << " create=" << created[i] << " join=" << joined
            << " set=" << args[i].setResult << " rendezvous=" << args[i].rendezvous
            << " isolated=" << args[i].isolated << '\n';
    }
    const bool parentPreserved = pthread_getspecific(shared.key) == &parentToken;
    const int deleted = pthread_key_delete(shared.key);
    passed = passed && parentPreserved && deleted == 0;
    out << "pthread_tls=" << (passed ? "PASS" : "FAIL") << " parentPreserved=" << parentPreserved
        << " keyDelete=" << deleted << " readsPerThread=1024\n";
}
void CheckVulkan(std::ostream &out)
{
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "XblPlatformChecks";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&create, nullptr, &instance);
    out << "vulkan_instance=" << result << '\n';
    if (result != VK_SUCCESS) { out << "vulkan_enumeration=FAIL rendering=NOT_EXECUTED\n"; return; }
    // 固定容量：最多 8 个设备、每设备 512 个扩展，不重试变化中的列表。
    std::array<VkPhysicalDevice, 8> devices{};
    uint32_t count = devices.size();
    result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
    out << "vulkan_devices result=" << result << " count=" << count << '\n';
    bool complete = result == VK_SUCCESS && count > 0;
    if (result == VK_SUCCESS || result == VK_INCOMPLETE) {
        for (uint32_t i = 0; i < count && i < devices.size(); ++i) {
            VkPhysicalDeviceProperties props{};
            VkPhysicalDeviceFeatures features{};
            vkGetPhysicalDeviceProperties(devices[i], &props);
            vkGetPhysicalDeviceFeatures(devices[i], &features);
            out << "device=" << i << " name=" << props.deviceName << " vendor=" << props.vendorID
                << " deviceID=" << props.deviceID << " driver=" << props.driverVersion
                << " api=" << VK_VERSION_MAJOR(props.apiVersion) << '.' << VK_VERSION_MINOR(props.apiVersion)
                << '.' << VK_VERSION_PATCH(props.apiVersion) << " textureCompressionBC=" << features.textureCompressionBC << '\n';
            std::array<VkExtensionProperties, 512> extensions{};
            uint32_t extCount = extensions.size();
            VkResult extResult = vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &extCount, extensions.data());
            complete = complete && extResult == VK_SUCCESS;
            out << "extensions result=" << extResult << " count=" << extCount << '\n';
            if (extResult == VK_SUCCESS || extResult == VK_INCOMPLETE) {
                for (uint32_t e = 0; e < extCount && e < extensions.size(); ++e)
                    out << "extension=" << extensions[e].extensionName << " spec=" << extensions[e].specVersion << '\n';
            }
            // Vulkan 中 BC1 RGB 到 BC7 SRGB 是连续的 16 个核心格式。
            for (int f = VK_FORMAT_BC1_RGB_UNORM_BLOCK; f <= VK_FORMAT_BC7_SRGB_BLOCK; ++f) {
                VkFormatProperties format{};
                vkGetPhysicalDeviceFormatProperties(devices[i], static_cast<VkFormat>(f), &format);
                out << "bc_format=" << f << " linear=" << format.linearTilingFeatures
                    << " optimal=" << format.optimalTilingFeatures << " buffer=" << format.bufferFeatures
                    << " sampledOptimal=" << ((format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0)
                    << " storageOptimal=" << ((format.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0) << '\n';
            }
        }
    }
    vkDestroyInstance(instance, nullptr);
    out << "vulkan_enumeration=" << (complete ? "COMPLETE" : "FAIL_OR_TRUNCATED")
        << " rendering=NOT_EXECUTED upload=NOT_EXECUTED readback=NOT_EXECUTED\n";
}
struct Work {
    napi_async_work handle{};
    napi_deferred deferred{};
    std::string report;
};
void Execute(napi_env, void *data)
{
    auto &work = *static_cast<Work *>(data);
    try {
        std::ostringstream out;
        out << "probeVersion=1 hostState=not_integrated\n";
        CheckCpu(out);
        CheckVulkan(out);
        work.report = out.str();
    } catch (...) { work.report = "probe=FAIL native_exception\n"; }
}
void Complete(napi_env env, napi_status status, void *data)
{
    std::unique_ptr<Work> work(static_cast<Work *>(data));
    napi_value value{};
    if (status == napi_ok && napi_create_string_utf8(env, work->report.c_str(), work->report.size(), &value) == napi_ok) {
        napi_resolve_deferred(env, work->deferred, value);
    } else {
        napi_value message{};
        if (napi_create_string_utf8(env, "Native probe completion failed", NAPI_AUTO_LENGTH, &message) == napi_ok &&
            napi_create_error(env, nullptr, message, &value) == napi_ok) napi_reject_deferred(env, work->deferred, value);
    }
    napi_delete_async_work(env, work->handle);
    busy.store(false);
}
}
napi_value RunPlatformChecks(napi_env env, napi_callback_info)
{
    if (busy.exchange(true)) { napi_throw_error(env, nullptr, "A platform probe is already running"); return nullptr; }
    std::unique_ptr<Work> work(new (std::nothrow) Work);
    napi_value promise{}, name{};
    if (!work || napi_create_string_utf8(env, "XblPlatformChecks", NAPI_AUTO_LENGTH, &name) != napi_ok ||
        napi_create_promise(env, &work->deferred, &promise) != napi_ok ||
        napi_create_async_work(env, nullptr, name, Execute, Complete, work.get(), &work->handle) != napi_ok ||
        napi_queue_async_work(env, work->handle) != napi_ok) {
        if (work && work->handle) napi_delete_async_work(env, work->handle);
        busy.store(false);
        napi_throw_error(env, nullptr, "Could not queue native platform probe");
        return nullptr;
    }
    work.release();
    return promise;
}
