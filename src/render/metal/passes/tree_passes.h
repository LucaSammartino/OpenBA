#pragma once
#include "../VideoMetalData.h"
namespace VideoMetal::TreePasses {
// Snapshot the live extension once per command frame, outside draw loops.
void BeginFrame();
// Returns retail on a non-match or conversion failure. Cache lives per device.
Shader* Select(Shader* retail, const std::vector<uint8_t>& bytes,
               id<MTLDevice> device, bool vertex);
// True for a cutout variant that writes coverage in o0.w: it needs alpha-to-coverage and a
// multisampled target, otherwise the caller must run the retail shader.
bool IsCoverage(const Shader* pixel);
// Match DX11: transformed cutout shaders never run with alpha blending.
class BlendGuard {
    Shader*& slot;
    Shader* saved;
public:
    BlendGuard(Shader*& pixel, bool blending);
    ~BlendGuard() { slot = saved; }
};
}
