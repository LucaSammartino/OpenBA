// ============================================================================
// tests/unit/ext_sharp_ui_test.cpp -- ext.sharp_ui's pure parts
// ============================================================================
// src/ext/sharp_ui/sharp_math.{h,cpp} (the frame decision, the font variant
// set and pick, the glyph placement, the 2x twin naming) and
// src/engine/ui/SharpDraw.h (the device-pixel snap, the glyph quad), without a
// device.
// ============================================================================
#include "unit.h"
#include "ext/sharp_ui/sharp_math.h"
#include "ui/SharpDraw.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

namespace {

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

}  // namespace

UNIT_TEST(ext_sharp_ui_layout_is_exact) {
    // 1 / k as _uiScale gives retail's (int)(bb * scale) == bb / k exactly on
    // the common sizes (the integer-ratio frames ChooseDraw recognises).
    struct Size { int w, h; };
    for (Size s : { Size{1280, 720}, Size{1920, 1080}, Size{2560, 1440}, Size{3840, 2160},
                    Size{1600, 1200}, Size{1366, 768}, Size{3440, 1440}, Size{5120, 2880} }) {
        for (int k : {1, 2, 3, 4}) {
            if (s.w % k != 0 || s.h % k != 0) continue;
            const float scale = 1.0f / static_cast<float>(k);
            CHECK(SharpUI::LayoutExtent(s.w, scale) == s.w / k);
            CHECK(SharpUI::LayoutExtent(s.h, scale) == s.h / k);
        }
    }
}

UNIT_TEST(ext_sharp_ui_choose_draw) {
    using SharpUI::ChooseDraw;
    SharpUI::DrawChoice d = ChooseDraw(0.5f, 2560, 1440);
    CHECK(d.layoutWidth == 1280 && d.layoutHeight == 720);
    CHECK(d.integer && d.integerRatio == 2 && d.snap && d.point);
    CHECK(Near(d.ratio, 2.0f));

    d = ChooseDraw(1.0f, 1920, 1080);   // 1x: snap, linear (nothing to gain from point)
    CHECK(d.integer && d.integerRatio == 1 && d.snap && !d.point);

    d = ChooseDraw(0.75f, 1920, 1080);  // retail's 75%: ratio 1.333, not integer
    CHECK(!d.integer && d.snap && !d.point && d.integerRatio == 0);

    d = ChooseDraw(1.5f, 1920, 1080);   // retail's 150%: ratio 0.667, minified
    CHECK(!d.integer && !d.point);

    d = ChooseDraw(0.5f, 1365, 767);    // odd back buffer at 2x: 682x383, ratio ~2.0015
    CHECK(d.integer && d.integerRatio == 2 && d.point);

    d = ChooseDraw(0.0f, 1920, 1080);   // degenerate (a never-applied scale): retail's frame
    CHECK(!d.snap && !d.point && !d.integer);
}

UNIT_TEST(ext_sharp_ui_snap) {
    using UI::SnapToPixel;
    // Integer ratio: every integer layout coordinate is already on a pixel.
    for (int v = -3; v < 40; ++v)
        CHECK(Near(SnapToPixel(static_cast<float>(v), 2.0f, 0.5f), static_cast<float>(v)));

    // Ratio 1.333 (retail 75%): the result lands on whole device pixels.
    const float px = 4.0f / 3.0f, inv = 0.75f;
    for (int v = 0; v < 64; ++v) {
        const float s = SnapToPixel(static_cast<float>(v), px, inv);
        const float dev = s * px;
        CHECK(Near(dev, std::floor(dev + 0.5f), 1e-3f));
        CHECK(std::fabs(s - static_cast<float>(v)) <= 0.5f * inv + 1e-4f);   // moved by at most half a pixel
    }

    // Monotonic: a quad never inverts, shared edges stay shared.
    float prev = SnapToPixel(0.0f, px, inv);
    for (int v = 1; v < 64; ++v) {
        const float s = SnapToPixel(static_cast<float>(v), px, inv);
        CHECK(s >= prev);
        prev = s;
    }

    // Odd back buffer at 2x (1365 / 682): the right edge lands on the last pixel.
    const float pxOdd = 1365.0f / 682.0f;
    const float e = SnapToPixel(682.0f, pxOdd, 1.0f / pxOdd);
    CHECK(Near(e * pxOdd, 1365.0f, 1e-2f));
}

UNIT_TEST(ext_sharp_ui_pixel_exact) {
    using UI::AxisIsPixelExact;
    // 2x: a 16-texel icon drawn at its 16 layout units = 32 px: exact.
    CHECK(AxisIsPixelExact(16, 2.0f, 16));
    // A glyph drawn at its size at 3x: exact.
    CHECK(AxisIsPixelExact(7, 3.0f, 7));
    // A 1920-texel backdrop letterboxed into 1280 layout units at 2x (1.333): filtered.
    CHECK(!AxisIsPixelExact(1280, 2.0f, 1920));
    // A 9-slice edge, 8 texels stretched to 203 layout units at 2x (50.75): a fill.
    CHECK(AxisIsPixelExact(203, 2.0f, 8));
    // A sprite drawn at half size at 2x (1 px per texel): exact.
    CHECK(AxisIsPixelExact(8, 2.0f, 16));
    // ... at a third of its size (0.667 px per texel): minified, filtered.
    CHECK(!AxisIsPixelExact(6, 2.0f, 18));
    // Mirrored (x2 < x1) and empty sprites.
    CHECK(AxisIsPixelExact(-16, 2.0f, 16));
    CHECK(AxisIsPixelExact(5, 2.0f, 0));

    UI::SharpDraw s{2.0f, 2.0f, 0.5f, 0.5f, true, nullptr, 0};
    CHECK(UI::BatchQuadIsPixelExact(s, 10, 10, 26, 30, 16, 20));
    CHECK(!UI::BatchQuadIsPixelExact(s, 0, 0, 1280, 720, 1920, 1080));
}

UNIT_TEST(ext_sharp_ui_variant_set) {
    using namespace SharpUI;
    // The game's four fonts (the kit's Font/*.rsc _fontHeight).
    CHECK(VariantLow(14) == 9 && VariantHigh(14) == 19 && VariantStep(14) == 1);
    CHECK(VariantLow(18) == 12 && VariantHigh(18) == 24 && VariantStep(18) == 1);
    CHECK(VariantLow(24) == 16 && VariantHigh(24) == 32 && VariantStep(24) == 1);
    CHECK(VariantLow(48) == 32 && VariantHigh(48) == 64 && VariantStep(48) == 2);
    // tools/ext_fonts.py writes exactly these: 10 + 12 + 16 + 16 = 54 sources.
    CHECK(VariantCount(14) == 10);
    CHECK(VariantCount(18) == 12);
    CHECK(VariantCount(24) == 16);
    CHECK(VariantCount(48) == 16);
    CHECK(!IsVariantHeight(24, 24));        // the base itself is retail's font
    CHECK(!IsVariantHeight(48, 33));        // odd heights are not compiled for 48
    CHECK(IsVariantHeight(48, 34));
    CHECK(!IsVariantHeight(24, 15) && !IsVariantHeight(24, 33));
}

UNIT_TEST(ext_sharp_ui_variant_pick) {
    using SharpUI::PickVariantHeight;
    // Slider 75% at 1080p: 1080 / 810 = 1.3333 px per unit.
    const float r75 = 1080.0f / 810.0f;
    CHECK(PickVariantHeight(24, r75) == 32);
    CHECK(PickVariantHeight(14, r75) == 19);   // 18.67
    CHECK(PickVariantHeight(18, r75) == 24);
    CHECK(PickVariantHeight(48, r75) == 64);
    // Slider 100%: the retail fonts.
    CHECK(PickVariantHeight(24, 1.0f) == 24);
    CHECK(PickVariantHeight(48, 1.0f) == 48);
    // Slider 150% (0.6667): smaller fonts.
    const float r150 = 1080.0f / 1620.0f;
    CHECK(PickVariantHeight(24, r150) == 16);
    CHECK(PickVariantHeight(14, r150) == 9);   // 9.33
    // Slider 110% (0.909): 21.8 -> 22; 48 -> 43.6 -> 44 (even only).
    CHECK(PickVariantHeight(24, 1.0f / 1.1f) == 22);
    CHECK(PickVariantHeight(48, 1.0f / 1.1f) == 44);
    // Within half a pixel of the base: the base.
    CHECK(PickVariantHeight(24, 1.015f) == 24);
    // A tie goes to the smaller height: 14 * 1.25 = 17.5 -> 17.
    CHECK(PickVariantHeight(14, 1.25f) == 17);
    // Outside the compiled range (a 2x frame): retail's font, not a clamp.
    CHECK(PickVariantHeight(24, 2.0f) == 24);
    CHECK(PickVariantHeight(24, 0.5f) == 24);
    CHECK(PickVariantHeight(24, 0.0f) == 24);
    // ...and the swap-for-itself is only for a ratio near 1.
    CHECK(SharpUI::NativePickFitsRatio(24, 1.0f));
    CHECK(SharpUI::NativePickFitsRatio(24, 1.015f));
    CHECK(!SharpUI::NativePickFitsRatio(24, 2.0f));
    CHECK(!SharpUI::NativePickFitsRatio(14, 0.5f));
    CHECK(!SharpUI::NativePickFitsRatio(24, 0.0f));

    char path[64];
    CHECK(SharpUI::VariantFontPath("FontSmall", 19, path, sizeof path));
    CHECK(std::strcmp(path, "Font/SharpFontSmall_19.rsc") == 0);
    CHECK(SharpUI::VariantFontPath("Font", 32, path, sizeof path));
    CHECK(std::strcmp(path, "Font/SharpFont_32.rsc") == 0);
    CHECK(!SharpUI::VariantFontPath("FontMedium", 24, path, 8));
}

UNIT_TEST(ext_sharp_ui_glyph_place) {
    // A base glyph (24 px font, layout units): _preAdvance 1, _offsetY -5 (a
    // descender), sprite 10 x 20. ComputeLineLayout put it at x1 = pen + 1 and
    // y = baseline - (-5) - 20, so with the pen at 100 and the baseline at 50:
    // x1 = 101, y2 = 55.
    // Its 32 px variant (device pixels): _preAdvance 2, _offsetY -7, 13 x 27.
    // Advances: the base row (1, kern 1, post 10) steps the pen 12 units, the
    // variant row (2, kern 1, post 13) 16 pixels.
    const UI::SharpGlyphPlace g =
        SharpUI::MakeGlyphPlace({1, 1, 10, -5}, {2, 1, 13, -7}, 13, 27, false);
    CHECK(g.originX == -1 && g.originY == -5 && g.devX == 2 && g.devY == 7);
    CHECK(g.devW == 13 && g.devH == 27);
    CHECK(g.baseAdvance == 12 && g.devAdvance == 16 && !g.wordBreak);
    CHECK(SharpUI::MakeGlyphPlace({0, 0, 5, 0}, {0, 0, 6, 0}, 1, 1, true).wordBreak);

    // At 1.3333 px per unit (1080p, slider 75%): the pen at 133.33 device px
    // rounds to 133, the baseline 66.67 to 67.
    const float px = 1080.0f / 810.0f, up = 810.0f / 1080.0f;
    const UI::SharpQuad q = UI::PlaceGlyph(g, 101, 55, px, px, up, up);
    // One texel per device pixel: the quad is exactly 13 x 27 device pixels,
    // its corners on whole pixels.
    CHECK(Near((q.x2 - q.x1) * px, 13.0f, 1e-3f));
    CHECK(Near((q.y2 - q.y1) * px, 27.0f, 1e-3f));
    CHECK(Near(q.x1 * px, 135.0f, 1e-3f));    // 133 + 2
    CHECK(Near(q.y2 * px, 74.0f, 1e-3f));     // 67 + 7: the variant's descender
    CHECK(Near(q.y1 * px, 47.0f, 1e-3f));

    // At ratio 1 with the base glyph's own metrics as the "variant", the quad is
    // retail's quad: x1 = 101, y1 = 35, x2 = 111, y2 = 55.
    const UI::SharpGlyphPlace same = SharpUI::MakeGlyphPlace({1, 0, 10, -5}, {1, 0, 10, -5}, 10, 20, false);
    const UI::SharpQuad r = UI::PlaceGlyph(same, 101, 55, 1.0f, 1.0f, 1.0f, 1.0f);
    CHECK(Near(r.x1, 101.0f) && Near(r.y1, 35.0f) && Near(r.x2, 111.0f) && Near(r.y2, 55.0f));
}

// A word drawn from a variant steps by the variant's own advances (SharpRun),
// held within kSharpRunTolerance of retail's fractional pen; a space, a
// baseline change or a pen the layout did not advance to starts a new run.
UNIT_TEST(ext_sharp_ui_word_run) {
    using SharpUI::GlyphMetrics;
    // 'H' of the 24 px Font: (1, kern 1, post 11), 11 wide: the pen steps 13.
    // Its 26 px variant: (1, 1, 12), 12 wide: the pen steps 14 pixels, where
    // retail's scaled step is 13 * 26 / 24 = 14.083.
    const float px = 26.0f / 24.0f;
    const UI::SharpGlyphPlace H = SharpUI::MakeGlyphPlace({1, 1, 11, 0}, {1, 1, 12, 0}, 12, 15, false);
    const UI::SharpGlyphPlace space = SharpUI::MakeGlyphPlace({0, 0, 5, 0}, {0, 0, 5, 0}, 1, 1, true);

    // "HHHHHHHH": the layout's x1 = pen + 1, pens 0, 13, 26, ...
    UI::SharpRun run;
    float last = 0.0f;
    for (int i = 0; i < 8; ++i) {
        const int32_t pen = 13 * i;
        const float dev = UI::SharpRunPen(run, H, pen + 1, 50, 0, px);
        if (i == 0) CHECK(dev == 0.0f);
        else        CHECK(dev - last == 14.0f);                    // the hinted step, every time
        CHECK(std::fabs(dev - static_cast<float>(pen) * px) <= UI::kSharpRunTolerance);
        last = dev;
    }
    // The old per-glyph rounding (the control) stepped 14, 14, 14, 14, 14, 15, 14:
    // round(pen * px) for pens 65 -> 70.42 -> 70 and 78 -> 84.5 -> 85.
    CHECK(UI::SnapDevice(78, 0, px) - UI::SnapDevice(65, 0, px) == 15.0f);

    // An over-wide variant (steps 15 for 14.083) is nudged back a pixel as soon
    // as it is more than the tolerance ahead: 15, 30 -> 29 (1.83 ahead), ...
    const UI::SharpGlyphPlace wide = SharpUI::MakeGlyphPlace({1, 1, 11, 0}, {1, 1, 13, 0}, 12, 15, false);
    UI::SharpRun w;
    CHECK(UI::SharpRunPen(w, wide, 1, 50, 0, px) == 0.0f);
    CHECK(UI::SharpRunPen(w, wide, 14, 50, 0, px) == 15.0f);
    CHECK(UI::SharpRunPen(w, wide, 27, 50, 0, px) == 29.0f);
    for (int i = 3; i < 40; ++i)
        CHECK(std::fabs(UI::SharpRunPen(w, wide, 13 * i + 1, 50, 0, px) - 13.0f * i * px) <=
              UI::kSharpRunTolerance);

    // Breaks: after a space, on another baseline, or at a pen the layout did
    // not step to (a new string), the glyph is snapped on its own.
    UI::SharpRun b;
    UI::SharpRunPen(b, H, 66, 50, 0, px);                               // pen 65 -> 70
    CHECK(UI::SharpRunPen(b, H, 79, 50, 0, px) == 84.0f);              // 70 + 14 (snapped: 85)
    CHECK(UI::SharpRunPen(b, space, 91, 50, 0, px) == UI::SnapDevice(91, 0, px));
    CHECK(UI::SharpRunPen(b, H, 97, 50, 0, px) == UI::SnapDevice(96, 0, px));   // after the space
    CHECK(UI::SharpRunPen(b, H, 110, 51, 0, px) == UI::SnapDevice(109, 0, px)); // baseline moved
    CHECK(UI::SharpRunPen(b, H, 200, 51, 0, px) == UI::SnapDevice(199, 0, px)); // not contiguous
    b.live = false;
    CHECK(UI::SharpRunPen(b, H, 213, 51, 0, px) == UI::SnapDevice(212, 0, px)); // run ended

    // At ratio 1 with the font as its own variant the pen is retail's, exactly.
    const UI::SharpGlyphPlace self = SharpUI::MakeGlyphPlace({1, 1, 11, 0}, {1, 1, 11, 0}, 11, 15, false);
    UI::SharpRun one;
    for (int i = 0; i < 30; ++i)
        CHECK(UI::SharpRunPen(one, self, 13 * i + 1, 50, 0, 1.0f) == 13.0f * i);

    // Against a dialog origin the whole run moves by the origin's own snap.
    UI::SharpRun r0, r1;
    const int32_t o = 37;
    for (int i = 0; i < 12; ++i) {
        const float a = UI::SharpRunPen(r0, H, 13 * i + 1, 50, 0, px);
        const float c = UI::SharpRunPen(r1, H, 13 * i + 1 + o, 50, o, px);
        CHECK(c - a == UI::SnapDevice(o, 0, px));
    }
}

UNIT_TEST(ext_sharp_ui_find_swap) {
    // FindSwap matches by sheet pointer only (never dereferenced here).
    alignas(8) static char storage[2];
    const UI::SpriteSheet* a = reinterpret_cast<const UI::SpriteSheet*>(&storage[0]);  // CAST-OK: test_key
    const UI::SpriteSheet* b = reinterpret_cast<const UI::SpriteSheet*>(&storage[1]);  // CAST-OK: test_key
    const UI::SharpSheetSwap swaps[1] = { { a, b, nullptr, nullptr, 0 } };
    UI::SharpDraw s{1.0f, 1.0f, 1.0f, 1.0f, false, swaps, 1};
    CHECK(UI::FindSwap(s, a) == &swaps[0]);
    CHECK(UI::FindSwap(s, b) == nullptr);
    s.swapCount = 0;
    CHECK(UI::FindSwap(s, a) == nullptr);
}

UNIT_TEST(ext_sharp_ui_twin_path) {
    using SharpUI::Twin2xPath;
    char out[128];
    CHECK(Twin2xPath("Dialog/SpriteSheet.rsc", out, sizeof out));
    CHECK(std::strcmp(out, "Dialog/SpriteSheet2x.rsc") == 0);
    CHECK(Twin2xPath("Dialog\\ExtensionsSprites.rsc:resource", out, sizeof out));
    CHECK(std::strcmp(out, "Dialog\\ExtensionsSprites2x.rsc") == 0);
    CHECK(!Twin2xPath("Dialog/SpriteSheet2x.rsc", out, sizeof out));   // a twin has no twin
    CHECK(!Twin2xPath("Dialog/Extensions.rsc:buttonExtensions", out, sizeof out));
    CHECK(!Twin2xPath("Texture/x.png", out, sizeof out));
    CHECK(!Twin2xPath(nullptr, out, sizeof out));
    CHECK(!Twin2xPath("Dialog/SpriteSheet.rsc", out, 8));
    CHECK(SharpUI::WantTwin2x(1.3333f, 1.3333f));
    CHECK(!SharpUI::WantTwin2x(1.0f, 1.0f));
    CHECK(!SharpUI::WantTwin2x(0.6667f, 0.6667f));
}

// A dialog that moves (a panel following its entity while the camera turns)
// must move as one picture: rounded against its own upper-left, the device-pixel
// gap between any two of its coordinates is the same wherever the dialog sits.
// The control: rounded absolutely (origin 0), the same gaps change by a pixel
// as the dialog moves one layout unit -- the shimmer the user saw at 89 %
// (2560 / 2278 = 1.1238 px per unit).
UNIT_TEST(ext_sharp_ui_snap_moves_whole) {
    const float px = 2560.0f / 2278.0f;
    const int32_t offsets[] = { 3, 7, 11, 16, 23, 40, 101 };
    bool absoluteJitters = false;
    for (int32_t k : offsets) {
        const float gap = UI::SnapDevice(k, 0, px) - UI::SnapDevice(0, 0, px);
        for (int32_t o = 0; o < 600; ++o) {
            CHECK(UI::SnapDevice(o + k, o, px) - UI::SnapDevice(o, o, px) == gap);
            const float abs = UI::SnapDevice(o + k, 0, px) - UI::SnapDevice(o, 0, px);
            if (abs != gap) absoluteJitters = true;
        }
    }
    CHECK(absoluteJitters);   // the control: the old rounding does shimmer

    // Origin 0 is the old absolute rounding, unit for unit.
    for (int32_t v = -50; v < 500; ++v)
        CHECK(UI::SnapToPixel(static_cast<float>(v), px, 1.0f / px) ==
              std::floor(static_cast<float>(v) * px + 0.5f) * (1.0f / px));

    // A glyph placed against its dialog's origin keeps its offset from the
    // dialog's corner as the dialog moves.
    const UI::SharpGlyphPlace g{ -1, 4, 1, -3, 9, 14 };
    const float up = 1.0f / px;
    const UI::SharpQuad q0 = UI::PlaceGlyph(g, 37, 52, px, px, up, up, 0, 0);
    for (int32_t o = 1; o < 300; ++o) {
        const UI::SharpQuad q = UI::PlaceGlyph(g, 37 + o, 52 + o, px, px, up, up, o, o);
        const float shift = UI::SnapDevice(o, 0, px);   // the corner's own move
        CHECK(std::fabs((q.x1 - q0.x1) * px - shift) < 1e-3f);
        CHECK(std::fabs((q.y2 - q0.y2) * px - shift) < 1e-3f);
        CHECK(std::fabs((q.x2 - q.x1) - (q0.x2 - q0.x1)) * px < 1e-3f);
    }
}
