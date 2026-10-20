// ============================================================================
// src/platform/common/display_density.cpp -- UI-scale defaults (identity)
// ============================================================================
// The rules are in display_density.h. Every host's window is density 1 today, so both
// answers are the game's own values, unchanged.
// ============================================================================

#include "display_density.h"

namespace Platform {
namespace Display {

int FirstRunUiScalePercent(int defaultPercent) {
    return defaultPercent;
}

void UiScaleSliderRange(float* minPercent, float* maxPercent) {
    (void)minPercent;
    (void)maxPercent;
}

}  // namespace Display
}  // namespace Platform
