// LSFG frame-generation capture.
//
// Copies each presented frame into an AHardwareBuffer and hands it to
// liblsfg-android.so, which interpolates and presents real + generated frames to
// its own overlay surface. The emulator keeps driving its swapchain underneath.
//
// The library is dlopen'd by soname from the app's linker namespace (see
// native.cpp): loading it from the Vulkan driver's namespace instead would create
// a second mapping with its own statics, and pushed frames would vanish into a
// context nothing presents.

#pragma once

#include <array>
#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_common.h"

struct AHardwareBuffer;

namespace Vulkan {

class Instance;

class LsfgCapture {
public:
    explicit LsfgCapture(const Instance& instance);
    ~LsfgCapture();

    bool IsActive() const {
        return active;
    }

    /// Records a blit of `src` (already in eTransferSrcOptimal) into the next
    /// AHardwareBuffer slot. Must be followed by MarkSubmitted with the fence
    /// that guards the submission this was recorded into.
    void RecordCapture(vk::CommandBuffer cmdbuf, vk::Image src, u32 src_w, u32 src_h);

    /// Associates the slot recorded by the last RecordCapture with `fence`, and
    /// pushes any earlier slot whose fence has already signalled. Never blocks.
    void MarkSubmittedAndDrain(vk::Fence fence);

private:
    struct Slot {
        AHardwareBuffer* ahb{};
        vk::Image image;
        vk::DeviceMemory memory;
        vk::Fence fence{};   ///< guards the blit that wrote this slot
        bool pending{};      ///< recorded, not yet pushed
    };

    bool CreateSlot(Slot& slot);

    const Instance& instance;
    static constexpr size_t NUM_SLOTS = 3;
    std::array<Slot, NUM_SLOTS> slots;
    size_t next_slot{};
    size_t last_recorded{SIZE_MAX};
    u32 width{};
    u32 height{};
    bool active{};
    u64 pushed{};
};

/// Live frame-generation rates for the on-screen overlay. Returns false until the
/// first window has been measured.
bool LsfgGetStats(double& real_fps, double& out_fps);

} // namespace Vulkan
