// NOT RETAIL: reuse the unchanged, platform-independent DXBC transformations.
// Retail inputs and transformed DXBC exist only in memory / the local airconv cache.
#include "tree_passes.h"
#include "ext/better_trees/better_trees.h"
#include "foliage_aa/cutout_rewrite.h"
#include "billboard_shadow/receiver_rewrite.h"
#include "billboard_shadow/edge_dither.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <cstdio>
#include <unistd.h>

namespace VideoMetal::TreePasses {
namespace {
// Metal-only mode 2 tuning (docs/macos/PASSES.md "Foliage mode 2"): the shared kDistanceFillK (0.15) /
// kDistanceFillFloor (0.15) pull pale blue-grey twig texels (low alpha, bluish filtered RGB) into
// coverage on a Mac window (haze). The rewriter's maths is unchanged; only these two constants differ
// from the Windows path, which keeps Ext::BetterTrees::kDistanceFill*.
constexpr float kMetalFillK = 0.08f;
constexpr float kMetalFillFloor = 0.40f;
int mode = 0;
bool msaa = false;
std::unordered_set<const Shader*> coverage;
id<MTLDevice> cachedDevice = nil;
struct Variant { Shader shader; bool changed = false; bool coverage = false; };
std::unordered_map<std::string, Variant> cache;
std::unordered_map<Shader*, Shader*> originals;
}
bool IsCoverage(const Shader* pixel) { return pixel && coverage.count(pixel) != 0; }
BlendGuard::BlendGuard(Shader*& pixel, bool blending) : slot(pixel), saved(pixel) {
    if (blending) {
        auto found = originals.find(pixel);
        if (found != originals.end()) slot = found->second;
    }
}
void BeginFrame() {
    const int next = Ext::BetterTrees::Current().mode;
    const bool nextMsaa = CurrentSamples() > 1;
    if (next != mode || nextMsaa != msaa)
        std::fprintf(stderr, "[MetalPasses] better_trees mode %d (%s)\n", next,
                     nextMsaa ? "multisample: alpha-to-coverage" : "single sample: cutout retained");
    mode = next; msaa = nextMsaa;
}
Shader* Select(Shader* retail, const std::vector<uint8_t>& bytes,
               id<MTLDevice> device, bool vertex) {
    if (!mode || !retail) return retail;
    if (cachedDevice != device) { originals.clear(); cache.clear(); coverage.clear(); cachedDevice = device; }
    // DXBC's embedded 128-bit checksum identifies the whole container. Avoid
    // copying kilobytes of shader tokens at every program bind.
    std::string key(reinterpret_cast<const char*>(bytes.data()), std::min(bytes.size(), size_t(20)));
    key.push_back(char(mode)); key.push_back(char(vertex)); key.push_back(char(msaa));
    auto found = cache.find(key);
    if (found == cache.end()) {
        Variant variant;
        std::vector<uint8_t> output;
        bool changed = false;
        if (vertex) {
            BbsRewrite::Match match;
            BbsRewrite::Analyse(bytes.data(), bytes.size(), &match);
            changed = BbsRewrite::TransformWithParams(match, &output, BbsRewrite::kDefaultParams);
        } else {
            FaaRewrite::Match match;
            FaaRewrite::Analyse(bytes.data(), bytes.size(), &match);
            auto options = FaaRewrite::OptionsFor(mode, kMetalFillK, msaa, kMetalFillFloor);
            changed = FaaRewrite::Transform(match, options, &output);
            variant.coverage = changed && options.coverage;
            if (match.colour) {
                const auto& input = changed ? output : bytes;
                BbsDither::Match dither;
                BbsDither::Analyse(input.data(), input.size(), &dither);
                std::vector<uint8_t> dithered;
                if (BbsDither::Transform(dither, &dithered, BbsRewrite::kDefaultParams)) {
                    output = std::move(dithered); changed = true;
                }
            }
        }
        if (changed) {
            variant.shader = Translate(output, device);
            variant.changed = variant.shader.function != nil;
            if (!variant.changed) variant.coverage = false;
            std::fprintf(stderr, "[MetalPasses] %s rewrite %zu -> %zu: %s\n",
                         vertex ? "billboard receiver" : "cutout/dither", bytes.size(), output.size(),
                         variant.changed ? "loaded" : "FAILED; using retail");
        }
        found = cache.emplace(std::move(key), std::move(variant)).first;
    }
    if (!found->second.changed) return retail;
    auto selected = &found->second.shader;
    if (!vertex) { originals[selected] = retail; if (found->second.coverage) coverage.insert(selected); }
    return selected;
}
}
