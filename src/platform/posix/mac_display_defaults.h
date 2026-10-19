#pragma once
// ============================================================================
// src/platform/posix/mac_display_defaults.h -- UI scale default for Retina
// ============================================================================
// Header only (no link-time dependency, so the unit tests and every target see it).
// docs/macos/UI_SCALE.md has the why.
//
// The engine's UI is laid out in pixel-sized units: _uiScale = settings.uiScale / 100 is
// LAYOUT UNITS PER DEVICE PIXEL, so a SMALLER _uiScale gives a BIGGER UI. At native Retina
// (OPENBA_NATIVE_RETINA, SDL pixel density 2) the back buffer has 2x the device pixels, so
// the same UI looks half its physical size; uiScale 50 (= 100 / density) restores it.
// The Metal backend records the window's pixel density once the window exists.
namespace MacDisplay {

inline float g_pixelDensity = 1.0f;   // 1 everywhere except a high-density Apple window

inline void SetPixelDensity(float d) { g_pixelDensity = (d >= 1.0f) ? d : 1.0f; }
inline float PixelDensity() { return g_pixelDensity; }

// The Options > UI scale default on first run (no saved UIScale): 100 on a 1x window,
// 100 / density (50 on Retina) otherwise.
inline int DefaultUiScalePercent() {
    return static_cast<int>(100.0f / g_pixelDensity + 0.5f);
}

// The slider's range, retail's 75..150 divided by the density (identity at density 1).
inline float SliderMin() { return static_cast<float>(static_cast<int>(75.0f / g_pixelDensity + 0.5f)); }
inline float SliderMax() { return static_cast<float>(static_cast<int>(150.0f / g_pixelDensity + 0.5f)); }

}  // namespace MacDisplay
