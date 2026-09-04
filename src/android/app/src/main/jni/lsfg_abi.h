// Copied VERBATIM from LSFG-Android lsfg_render_loop.hpp. This is an ABI
// contract with the prebuilt liblsfg-android.so: field order/types must not
// drift or initRenderLoop() reads garbage.
#pragma once
#include <cstdint>
namespace lsfg_android {
struct RenderLoopConfig {
    uint32_t width;
    uint32_t height;
    int multiplier;       // generationCount passed to LSFG: total = capture * multiplier
    float flowScale;      // 0.25 .. 1.0
    bool performance;     // selects LSFG_3_1P vs LSFG_3_1
    bool hdr;
    bool antiArtifacts;
    // Use the precompiled SPIR-V FP16 shader variants (Lossless.dll resource
    // IDs 304..351) instead of the DXBC-translated FP32 set (255..302). The
    // FP16 variants enable OpCapability Float16 and use mixed FP16/FP32 ops.
    // Requires the GPU to support VK_KHR_shader_float16_int8 + shaderFloat16,
    // and requires the FP16 SPIR-V cache to have been populated by the DLL
    // extraction step. The render loop transparently falls back to the FP32
    // path when either prerequisite is missing.
    bool framegenFp16;
    bool npuPostProcessing;
    int npuPreset;        // see NpuPreset: 0 off, 1 sharpen, 2 detail boost, 3 chroma clean, 4 game crisp
    int npuUpscaleFactor; // 1 or 2
    float npuAmount;      // 0.0 .. 1.0 enhance strength
    float npuRadius;      // 0.5 .. 2.0 blur radius for unsharp-mask paths
    float npuThreshold;   // 0.0 .. 1.0 (reserved)
    bool npuFp16;
    // CPU post-process: pure CPU pixel pass, applied after NPU (or in place
    // of it when the user only toggled the CPU category). See CpuPreset.
    bool cpuPostProcessing;
    int cpuPreset;         // 0 off .. 6 cinematic
    float cpuStrength;     // 0.0 .. 1.0
    float cpuSaturation;   // 0.0 .. 1.0 (0.5 is neutral)
    float cpuVibrance;     // 0.0 .. 1.0
    float cpuVignette;     // 0.0 .. 1.0
    bool gpuPostProcessing;
    int gpuStage;          // 0 before LSFG on real frames, 1 after LSFG on final frames
    int gpuMethod;         // see GpuPostProcessingMethod.nativeValue
    float gpuUpscaleFactor;// 1.0 .. 2.0
    float gpuSharpness;    // 0.0 .. 1.0
    float gpuStrength;     // 0.0 .. 1.0
    // Pacing tunables (0/negative values fall back to defaults inside the loop).
    int targetFpsCap;      // 0 = unlimited
    float emaAlpha;        // 0.05 .. 0.5 (default 0.125)
    float outlierRatio;    // 2.0 .. 8.0 (default 4.0)
    float vsyncSlackMs;    // 1.0 .. 5.0 (default 2.0)
    int queueDepth;        // 2 .. 6 (default 4)
};
} // namespace lsfg_android
