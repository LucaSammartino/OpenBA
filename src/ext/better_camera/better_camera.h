#pragma once
// ============================================================================
// src/ext/better_camera/better_camera.h -- ext.better_camera (class A, default OFF)
// ============================================================================
// NOT RETAIL. docs/extensions/handins/BETTERCAM.md. Two changes to retail's camera, one toggle:
//
//   * A WIDER ZOOM RANGE. Retail clamps the zoom target (the distance the camera settles at) to 20 .. 130
//     (.rdata 0x1401721c4 / 0x140172218, span 110 at 0x14017220c). With the row on the range is 1 .. 200, the
//     range Banished Game Tweaks' exe fix has used since 2026-09 (live-confirmed in the game). The span that
//     normalises the zoom for the pitch blend and the pan speed is recomputed from the new limits (max - min),
//     as the exe fix does, so those two keep their near-to-far shape across the new range.
//
//   * A STEADY TILT. Retail's pitch target is a blend between a NEAR pair of slopes and a FAR pair,
//     weighted by how far out the camera is (normZ, 0 near .. 1 far). The far pair is weaker, so the
//     camera flattens toward top-down as you zoom out -- by design, but it is what players ask to turn off.
//     With the row on the far pair is replaced by the near pair, so the tilt is the same at every zoom.
//     (Banished Game Tweaks' tilt fix redirects the same two constants.)
//
// The pitch's own ceiling is NOT touched: Vector3::FromYawPitch is a fitted approximation that is only valid
// up to about 1.0 rad (the exe-fix research found raising the ceiling flips the camera).
//
// WHERE: PlayerToolManager::UpdateCamera (game/Game/camera_input.cpp), once per frame, the toggle read once at
// the top; with the row off every value is retail's own constant and the pitch expression is retail's, in
// retail's operation order, so the faithful camera is bit-identical.
//
// SAVES: no new state. The zoom target (PlayerToolManager +0x3C) is already serialized; with the row on it can
// hold values outside retail's 20 .. 130. A retail game loads such a save fine and clamps the target back into
// its range at the first zoom action. Turning the row off mid-game clamps the target back at once
// (ClampOnToggleOff), so the faithful camera never runs with an out-of-range target.
// ============================================================================

namespace BetterCamera {

// Retail's limits (the faithful values; camera_input.cpp keeps its own copy of the .rdata constants).
constexpr float kRetailZoomMin = 20.0f;
constexpr float kRetailZoomMax = 130.0f;
constexpr float kRetailZoomSpan = 110.0f;

// ext.better_camera on.
constexpr float kZoomMin = 1.0f;
constexpr float kZoomMax = 200.0f;
constexpr float kZoomSpan = kZoomMax - kZoomMin;

// The pitch target (degrees). `manual` is the player's manual pitch (0..1), `normZ` the normalised zoom.
// steady = false is retail's expression exactly (same terms, same order); steady = true uses the near pair
// for the far term as well, which makes the result independent of normZ.
inline float PitchTarget(float manual, float normZ, float farMan, float farFlat, float nearMan, float nearFlat, bool steady) {
    if (steady) {
        farMan = nearMan;
        farFlat = nearFlat;
    }
    return (manual * farMan  - (1.0f - manual) * farFlat) * normZ +
           (manual * nearMan - (1.0f - manual) * nearFlat) * (1.0f - normZ);
}

// The frame the row turns OFF: a zoom target left outside retail's range is brought back into it.
inline float ClampOnToggleOff(float zoomTarget) {
    if (zoomTarget == 0.0f) return zoomTarget;           // not seeded yet (retail seeds it from the distance)
    if (zoomTarget < kRetailZoomMin) return kRetailZoomMin;
    if (zoomTarget > kRetailZoomMax) return kRetailZoomMax;
    return zoomTarget;
}

}  // namespace BetterCamera
