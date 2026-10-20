// ============================================================================
// tests/unit/ext_better_camera_test.cpp -- ext.better_camera
// ============================================================================
// src/ext/better_camera/better_camera.h without the game: the registry row (class A, off by default, the
// profile keys), the pitch blend (row off = retail's expression to the last bit; row on = the same tilt at every
// zoom, equal to retail's NEAR tilt), the wider zoom limits and their normalisation, and the clamp back into
// retail's range when the row is turned off mid-game.
// ============================================================================
#include "unit.h"
#include "ext/better_camera/better_camera.h"
#include "ext/registry.h"
#include "io/settings_file.h"

#include <cmath>
#include <string>

namespace {

using IO::SettingsFile::Entries;

bool EntriesLookup(void* ctx, const char* key, std::string& value) {
    const Entries* entries = static_cast<const Entries*>(ctx);
    for (const IO::SettingsFile::Entry& e : *entries) {
        if (e.key == key) { value = e.value; return true; }
    }
    return false;
}

Ext::State ResolveText(const char* text) {
    Entries entries = IO::SettingsFile::Parse(std::string(text));
    return Ext::Resolve(&EntriesLookup, &entries, nullptr, nullptr);
}

// camera_input.cpp's retail constants (.rdata 0x140172298 / 0x140172200 / 0x140172290 / 0x1401721f0)
constexpr float kFarMan = -65.0f, kFarFlat = 85.0f, kNearMan = -25.0f, kNearFlat = 60.0f;

// Retail's expression, written out here exactly as camera_input.cpp had it before the seam.
float RetailPitch(float manual, float normZ) {
    return (manual * kFarMan  - (1.0f - manual) * kFarFlat) * normZ +
           (manual * kNearMan - (1.0f - manual) * kNearFlat) * (1.0f - normZ);
}

}  // namespace

UNIT_TEST(ext_better_camera_registry_row) {
    const Ext::Entry* e = Ext::FindEntry("better_camera");
    CHECK(e != nullptr);
    if (!e) return;
    CHECK(e->tier == Ext::Tier::Extension);
    CHECK(e->cls == Ext::Class::A);                                   // live toggle, no sim or save effect
    CHECK(!e->defaultOn);                                             // it changes how the world looks: the player's choice
    CHECK(e->displayName != nullptr && *e->displayName != 0 && e->description != nullptr && *e->description != 0);
    CHECK(e->formatVersion == 0);                                     // class A carries no save format
    CHECK(Ext::SettingsKey(*e) == "ext.better_camera");

    CHECK(!ResolveText("").ext.better_camera);                        // absent key = the default (off)
    CHECK(ResolveText("ext.better_camera=1\n").ext.better_camera);
    CHECK(!ResolveText("ext.better_camera=0\n").ext.better_camera);
    CHECK(!ResolveText("profile=faithful\next.better_camera=1\n").ext.better_camera);   // the faithful profile turns it off
    CHECK(ResolveText("profile=all\n").ext.better_camera);
}

UNIT_TEST(ext_better_camera_off_is_retails_pitch_to_the_last_bit) {
    for (int m = 0; m <= 8; ++m) {
        const float manual = m * 0.125f;                              // the manual pitch steps in 0.125
        for (int i = -10; i <= 120; ++i) {
            const float normZ = i * 0.01f;                            // beyond 0..1 on purpose: a stale saved zoom
            const float got = BetterCamera::PitchTarget(manual, normZ, kFarMan, kFarFlat, kNearMan, kNearFlat, false);
            CHECK(got == RetailPitch(manual, normZ));                 // exact, not approximate
        }
    }
}

UNIT_TEST(ext_better_camera_steady_tilt_is_the_same_at_every_zoom) {
    for (int m = 0; m <= 8; ++m) {
        const float manual = m * 0.125f;
        const float nearTilt = manual * kNearMan - (1.0f - manual) * kNearFlat;          // retail's tilt fully zoomed in
        CHECK(std::fabs(RetailPitch(manual, 0.0f) - nearTilt) < 1e-4f);                  // (the near end of retail's blend)
        for (int i = -50; i <= 150; ++i) {
            const float normZ = i * 0.01f;
            const float got = BetterCamera::PitchTarget(manual, normZ, kFarMan, kFarFlat, kNearMan, kNearFlat, true);
            CHECK(std::fabs(got - nearTilt) < 1e-3f);
        }
    }
    // Retail really does flatten: with the row off the zoomed-out tilt differs from the zoomed-in one.
    CHECK(std::fabs(RetailPitch(0.0f, 1.0f) - RetailPitch(0.0f, 0.0f)) > 20.0f);
    CHECK(std::fabs(RetailPitch(1.0f, 1.0f) - RetailPitch(1.0f, 0.0f)) > 20.0f);
}

UNIT_TEST(ext_better_camera_zoom_limits) {
    CHECK(BetterCamera::kRetailZoomMin == 20.0f && BetterCamera::kRetailZoomMax == 130.0f && BetterCamera::kRetailZoomSpan == 110.0f);
    CHECK(BetterCamera::kRetailZoomSpan == BetterCamera::kRetailZoomMax - BetterCamera::kRetailZoomMin);
    CHECK(BetterCamera::kZoomMin == 1.0f && BetterCamera::kZoomMax == 200.0f);
    CHECK(BetterCamera::kZoomSpan == BetterCamera::kZoomMax - BetterCamera::kZoomMin);       // recomputed, as the exe fix does
    CHECK(BetterCamera::kZoomMin < BetterCamera::kRetailZoomMin && BetterCamera::kZoomMax > BetterCamera::kRetailZoomMax);
    // normalised zoom runs 0 .. 1 across the wider range, so the pan speed and (retail) pitch blend keep their shape
    CHECK((BetterCamera::kZoomMin - BetterCamera::kZoomMin) / BetterCamera::kZoomSpan == 0.0f);
    CHECK(std::fabs((BetterCamera::kZoomMax - BetterCamera::kZoomMin) / BetterCamera::kZoomSpan - 1.0f) < 1e-6f);
}

UNIT_TEST(ext_better_camera_clamp_back_when_turned_off) {
    using BetterCamera::ClampOnToggleOff;
    CHECK(ClampOnToggleOff(0.0f) == 0.0f);                            // not seeded yet: left for retail to seed
    CHECK(ClampOnToggleOff(1.0f) == 20.0f);                           // zoomed in past retail's limit
    CHECK(ClampOnToggleOff(19.9f) == 20.0f);
    CHECK(ClampOnToggleOff(20.0f) == 20.0f && ClampOnToggleOff(75.0f) == 75.0f && ClampOnToggleOff(130.0f) == 130.0f);
    CHECK(ClampOnToggleOff(130.1f) == 130.0f);
    CHECK(ClampOnToggleOff(200.0f) == 130.0f);                        // zoomed out past it
}
