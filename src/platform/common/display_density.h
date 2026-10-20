#pragma once
// ============================================================================
// src/platform/common/display_density.h -- UI-scale defaults for a high-density display
// ============================================================================
// NOT RETAIL. The game's UI is laid out in pixel units (GameSettings::uiScale, percent;
// _uiScale = uiScale / 100 is LAYOUT UNITS PER DEVICE PIXEL, so a smaller value is a
// bigger UI). On a window whose back buffer has more pixels than points (a high-density
// window, SDL pixel density > 1) the same settings show the UI physically smaller. These
// are the two questions game code would ask so a platform can answer for that case:
//   * the UI scale a FIRST RUN gets (no "UIScale" saved yet);
//   * the Options > UI scale slider's range.
// Bodies: src/platform/common/display_density.cpp, the identity on every host today (no
// high-density window exists; Platform::Window::PixelDensity is 1.0f). The work that opts
// a Darwin window into high density derives both answers from that density here (at
// density 1 they stay the identity), or moves the bodies to posix / darwin twins.
// D4_RULES C1: namespace `Platform`, free functions, NO OS / SDL header here.
//
// Not called by game code yet: GameSettings::Load (game_settings_persist.cpp) and
// OptionsDialog::OnOpened's SetSliderRange(sliderUIScale, 75, 150) keep retail's values.
// ============================================================================

namespace Platform {
namespace Display {

// The UI scale (percent) to use when no "UIScale" is saved. `defaultPercent` is what the
// game would use otherwise; returned unchanged at density 1.
int FirstRunUiScalePercent(int defaultPercent);

// The UI-scale slider's range (percent). In: retail's 75 / 150. Unchanged at density 1.
void UiScaleSliderRange(float* minPercent, float* maxPercent);

}  // namespace Display
}  // namespace Platform
