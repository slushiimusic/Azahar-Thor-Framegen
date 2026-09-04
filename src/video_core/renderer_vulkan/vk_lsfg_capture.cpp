// AHardwareBuffer import types live behind the Android platform define.
#define VK_USE_PLATFORM_ANDROID_KHR 1

#include <atomic>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <dlfcn.h>
#include <android/hardware_buffer.h>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/vk_lsfg_capture.h"

namespace Vulkan {

namespace {

using PushFrameFn = void (*)(AHardwareBuffer*, int64_t);
using CounterFn = uint64_t (*)();
PushFrameFn g_push_frame = nullptr;
CounterFn g_unique = nullptr;     ///< captures whose content changed

// Rolling rates for the on-screen readout, recomputed roughly twice a second.
std::atomic<double> g_real_fps{0.0};
std::atomic<double> g_out_fps{0.0};
std::atomic<bool> g_running{false};

// pushFrame() blocks when LSFG's internal queue is full. Calling it from the
// emulator's render thread therefore stalls emulation whenever the framegen
// consumer falls behind — measured as 250-580 ms freezes with the GPU only
// 15-41% busy, i.e. not a throughput problem at all. Hand the push to a worker
// with a single-slot mailbox and DROP frames when it is busy: a skipped
// interpolation is invisible, a stalled emulator is not.
struct PushRequest {
    AHardwareBuffer* ahb{};
    void* owner{};  // Slot*, returned to the pool once pushed
};

std::mutex g_push_mutex;
std::condition_variable g_push_cv;
PushRequest g_push_pending{};
bool g_push_has_work = false;
bool g_push_quit = false;
std::thread g_push_thread;
std::atomic<uint64_t> g_push_dropped{0};
std::vector<void*> g_push_done;  // slots the worker has finished with

uint64_t MonotonicNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}
CounterFn g_posted = nullptr;     ///< frames actually posted to the overlay

/// The library is already loaded (and its render loop initialised) by
/// native.cpp; dlopen here only resolves the handle in this translation unit.
bool ResolvePushFrame() {
    if (g_push_frame) {
        return true;
    }
    void* handle = dlopen("liblsfg-android.so", RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        LOG_CRITICAL(Render_Vulkan, "LSFG capture: dlopen failed: {}", dlerror());
        return false;
    }
    g_push_frame = reinterpret_cast<PushFrameFn>(
        dlsym(handle, "_ZN12lsfg_android9pushFrameEP15AHardwareBufferl"));
    if (!g_push_frame) {
        LOG_CRITICAL(Render_Vulkan, "LSFG capture: pushFrame symbol missing");
        return false;
    }
    // Only getPostedFrameCount/getUniqueCaptureCount have C++ exports;
    // getGeneratedFrameCount exists solely as a JNI wrapper. posted/pushed is the
    // ground truth for what actually reaches the overlay anyway.
    g_posted =
        reinterpret_cast<CounterFn>(dlsym(handle, "_ZN12lsfg_android19getPostedFrameCountEv"));
    g_unique = reinterpret_cast<CounterFn>(
        dlsym(handle, "_ZN12lsfg_android21getUniqueCaptureCountEv"));
    LOG_CRITICAL(Render_Vulkan, "LSFG capture: counters posted={} unique={}",
                 g_posted != nullptr, g_unique != nullptr);
    return true;
}

void PushWorker() {
    for (;;) {
        PushRequest req{};
        {
            std::unique_lock lock{g_push_mutex};
            g_push_cv.wait(lock, [] { return g_push_has_work || g_push_quit; });
            if (g_push_quit) {
                return;
            }
            req = g_push_pending;
            g_push_has_work = false;
        }
        if (req.ahb && g_push_frame) {
            g_push_frame(req.ahb, 0);  // may block; that is fine off the render thread
        }
        {
            std::unique_lock lock{g_push_mutex};
            g_push_done.push_back(req.owner);
        }
    }
}

void EnsurePushWorker() {
    if (!g_push_thread.joinable()) {
        g_push_thread = std::thread(PushWorker);
    }
}

/// Capture resolution, from /sdcard/Azahar/lsfg/capture as "WxH". This is what
/// LSFG interpolates at; anything below the output resolution is upscaled to the
/// overlay and looks soft. Default is full 1080p. MUST match the width/height
/// passed to initRenderLoop or LSFG copies 1:1 and crops instead of scaling.
void ReadCaptureSize(uint32_t& w, uint32_t& h) {
    w = 1920;
    h = 1080;
    FILE* f = fopen("/sdcard/Azahar/lsfg/capture", "rb");
    if (!f) {
        return;
    }
    char buf[64]{};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) {
        return;
    }
    unsigned pw = 0;
    unsigned ph = 0;
    if (sscanf(buf, "%ux%u", &pw, &ph) == 2 && pw >= 256 && ph >= 224) {
        w = pw & ~1u;
        h = ph & ~1u;
    }
}

bool LsfgEnabledByMarker() {
    // The Graphics setting is now the source of truth; the marker file remains as
    // an adb-only override for testing. Lossless.dll is a hard requirement -- LSFG's
    // shaders are extracted from it, so arming the capture without it just burns
    // GPU time on frames nothing will ever consume.
    bool have_dll = false;
    for (const char* candidate :
         {"/sdcard/Azahar/lsfg/Lossless.dll", "/sdcard/Roms/Lossless.dll"}) {
        if (FILE* dll = fopen(candidate, "rb")) {
            fclose(dll);
            have_dll = true;
            break;
        }
    }
    if (!have_dll) {
        return false;
    }
    if (Settings::values.frame_generation.GetValue()) {
        return true;
    }
    FILE* f = fopen("/sdcard/Azahar/lsfg/enabled", "rb");
    if (!f) {
        return false;
    }
    fclose(f);
    return true;
}

} // namespace

LsfgCapture::LsfgCapture(const Instance& instance_) : instance{instance_} {
    if (!LsfgEnabledByMarker()) {
        return;
    }
    if (!instance.IsAndroidAhbImportSupported()) {
        LOG_CRITICAL(Render_Vulkan, "LSFG capture: AHB import extension unavailable");
        return;
    }
    if (!ResolvePushFrame()) {
        return;
    }

    ReadCaptureSize(width, height);

    for (auto& slot : slots) {
        if (!CreateSlot(slot)) {
            LOG_CRITICAL(Render_Vulkan, "LSFG capture: slot allocation failed, disabling");
            return;
        }
    }
    EnsurePushWorker();
    active = true;
    LOG_CRITICAL(Render_Vulkan, "LSFG capture: armed at {}x{} with {} slots", width, height,
                 NUM_SLOTS);
}

LsfgCapture::~LsfgCapture() {
    const vk::Device device = instance.GetDevice();
    for (auto& slot : slots) {
        if (slot.image) {
            device.destroyImage(slot.image);
        }
        if (slot.memory) {
            device.freeMemory(slot.memory);
        }
        if (slot.ahb) {
            AHardwareBuffer_release(slot.ahb);
        }
    }
}

bool LsfgCapture::CreateSlot(Slot& slot) {
    const vk::Device device = instance.GetDevice();

    AHardwareBuffer_Desc desc{};
    desc.width = width;
    desc.height = height;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
    if (AHardwareBuffer_allocate(&desc, &slot.ahb) != 0 || !slot.ahb) {
        LOG_CRITICAL(Render_Vulkan, "LSFG capture: AHardwareBuffer_allocate failed");
        return false;
    }

    auto get_props = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
        device.getProcAddr("vkGetAndroidHardwareBufferPropertiesANDROID"));
    if (!get_props) {
        LOG_CRITICAL(Render_Vulkan, "LSFG capture: vkGetAndroidHardwareBufferProperties missing");
        return false;
    }

    VkAndroidHardwareBufferPropertiesANDROID props{};
    props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    if (get_props(device, slot.ahb, &props) != VK_SUCCESS) {
        LOG_CRITICAL(Render_Vulkan, "LSFG capture: AHB properties query failed");
        return false;
    }

    const vk::ExternalMemoryImageCreateInfo external_info{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eAndroidHardwareBufferANDROID,
    };
    const vk::ImageCreateInfo image_info{
        .pNext = &external_info,
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    slot.image = device.createImage(image_info);

    // Dedicated allocation is mandatory for AHB imports.
    const vk::MemoryDedicatedAllocateInfo dedicated{.image = slot.image};
    const vk::ImportAndroidHardwareBufferInfoANDROID import_info{
        .pNext = &dedicated,
        .buffer = slot.ahb,
    };

    u32 memory_type = 0;
    const auto mem_props = instance.GetPhysicalDevice().getMemoryProperties();
    for (u32 i = 0; i < mem_props.memoryTypeCount; i++) {
        if (props.memoryTypeBits & (1u << i)) {
            memory_type = i;
            break;
        }
    }

    const vk::MemoryAllocateInfo alloc_info{
        .pNext = &import_info,
        .allocationSize = props.allocationSize,
        .memoryTypeIndex = memory_type,
    };
    slot.memory = device.allocateMemory(alloc_info);
    device.bindImageMemory(slot.image, slot.memory, 0);
    return true;
}

void LsfgCapture::RecordCapture(vk::CommandBuffer cmdbuf, vk::Image src, u32 src_w, u32 src_h) {
    if (!active) {
        return;
    }
    Slot& slot = slots[next_slot];
    if (slot.pending) {
        // Still waiting to be pushed; skip this frame rather than overwrite it.
        return;
    }

    const vk::ImageSubresourceRange range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };

    const vk::ImageMemoryBarrier to_dst{
        .srcAccessMask = vk::AccessFlagBits::eNone,
        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = slot.image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                           vk::PipelineStageFlagBits::eTransfer,
                           vk::DependencyFlagBits::eByRegion, {}, {}, to_dst);

    const std::array offsets_src{vk::Offset3D{0, 0, 0},
                                 vk::Offset3D{static_cast<s32>(src_w), static_cast<s32>(src_h), 1}};
    const std::array offsets_dst{vk::Offset3D{0, 0, 0},
                                 vk::Offset3D{static_cast<s32>(width), static_cast<s32>(height), 1}};
    const vk::ImageBlit blit{
        .srcSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1},
        .srcOffsets = offsets_src,
        .dstSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1},
        .dstOffsets = offsets_dst,
    };
    cmdbuf.blitImage(src, vk::ImageLayout::eTransferSrcOptimal, slot.image,
                     vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);

    // Release to the foreign queue family: LSFG reads this buffer on its own
    // VkDevice, so ownership has to leave ours or the read is undefined.
    const vk::ImageMemoryBarrier release{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eNone,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .image = slot.image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                           vk::PipelineStageFlagBits::eBottomOfPipe,
                           vk::DependencyFlagBits::eByRegion, {}, {}, release);

    last_recorded = next_slot;
    next_slot = (next_slot + 1) % NUM_SLOTS;
}

void LsfgCapture::MarkSubmittedAndDrain(vk::Fence fence) {
    if (!active) {
        return;
    }
    if (last_recorded != SIZE_MAX) {
        slots[last_recorded].fence = fence;
        slots[last_recorded].pending = true;
        last_recorded = SIZE_MAX;
    }

    // Reclaim slots the worker has finished pushing.
    {
        std::unique_lock lock{g_push_mutex};
        for (void* owner : g_push_done) {
            static_cast<Slot*>(owner)->pending = false;
        }
        g_push_done.clear();
    }

    const vk::Device device = instance.GetDevice();
    for (auto& slot : slots) {
        if (!slot.pending || !slot.fence) {
            continue;
        }
        // Non-blocking: only push once the blit that filled this slot is done.
        if (device.getFenceStatus(slot.fence) != vk::Result::eSuccess) {
            continue;
        }
        // Hand to the worker; drop if it is still busy with the previous frame.
        {
            std::unique_lock lock{g_push_mutex};
            if (g_push_has_work) {
                g_push_dropped.fetch_add(1);
                slot.pending = false;  // recycle; a dropped interpolation is invisible
                continue;
            }
            g_push_pending = PushRequest{slot.ahb, &slot};
            g_push_has_work = true;
        }
        g_push_cv.notify_one();

        // Rolling rates: 'real' is unique captures (the emulator's true output
        // rate, since LSFG dedups static frames) and 'out' is what lands on the
        // overlay. out/real is the multiplier actually being achieved.
        {
            static uint64_t last_ns = 0;
            static uint64_t last_unique = 0;
            static uint64_t last_posted = 0;
            const uint64_t now = MonotonicNs();
            if (last_ns == 0) {
                last_ns = now;
            } else if (now - last_ns >= 500000000ull) {
                const double secs = static_cast<double>(now - last_ns) / 1e9;
                const uint64_t u = g_unique ? g_unique() : 0;
                const uint64_t p = g_posted ? g_posted() : 0;
                g_real_fps.store(static_cast<double>(u - last_unique) / secs);
                g_out_fps.store(static_cast<double>(p - last_posted) / secs);
                g_running.store(true);
                last_unique = u;
                last_posted = p;
                last_ns = now;
            }
        }
        if ((++pushed % 600) == 0) {
            const unsigned long long uniq = g_unique ? g_unique() : 0;
            const unsigned long long post = g_posted ? g_posted() : 0;
            // posted/pushed is the real multiplier actually reaching the screen.
            LOG_CRITICAL(Render_Vulkan,
                         "LSFG: pushed={} unique={} posted={} ratio={:.2f}x dropped={}",
                         pushed, uniq, post,
                         pushed ? static_cast<double>(post) / static_cast<double>(pushed) : 0.0,
                         g_push_dropped.load());
        }
    }
}

bool LsfgGetStats(double& real_fps, double& out_fps) {
    if (!g_running.load()) {
        return false;
    }
    real_fps = g_real_fps.load();
    out_fps = g_out_fps.load();
    return true;
}

} // namespace Vulkan
