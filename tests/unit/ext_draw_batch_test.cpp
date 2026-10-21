// ============================================================================
// tests/unit/ext_draw_batch_test.cpp -- DRAWBATCH: ext.draw_batch (class A)
// ============================================================================
// src/ext/draw_batch/batch_fill.{h,cpp} on plain buffers, and the registry row:
//   * the fill is the concatenation of the spans' vertex bytes, and each span's indices
//     plus the vertices before it -- so every index names the vertex it named alone;
//   * retail's capacity test, the sic index-vs-vertex-capacity compare included;
//   * a run is cut where it would pass 65536 vertices or fail the capacity test;
//   * the registry row: class A, on by default, off under the faithful profile.
// The DrawItemList copy and the smoke VS rewrite are the exe's (docs/extensions/handins/DRAWBATCH.md).
// ============================================================================
#include "unit.h"
#include "ext/draw_batch/batch_fill.h"
#include "ext/registry.h"
#include "io/settings_file.h"

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using Ext::DrawBatch::FillRun;
using Ext::DrawBatch::kMaxRunVertices;
using Ext::DrawBatch::RetailAccepts;
using Ext::DrawBatch::RunFits;
using Ext::DrawBatch::Span;

using IO::SettingsFile::Entries;

bool EntriesLookup(void* ctx, const char* key, std::string& value) {
    const Entries* entries = static_cast<const Entries*>(ctx);
    for (const IO::SettingsFile::Entry& e : *entries) {
        if (e.key == key) {
            value = e.value;
            return true;
        }
    }
    return false;
}

Ext::State ResolveText(const char* text) {
    Entries entries = IO::SettingsFile::Parse(std::string(text));
    return Ext::Resolve(&EntriesLookup, &entries, nullptr, nullptr);
}

constexpr int32_t kStride = 0x14;

struct Batch {
    std::vector<uint8_t>  vertices;   // count * kStride bytes
    std::vector<uint16_t> indices;
    int32_t Count() const { return static_cast<int32_t>(vertices.size() / kStride); }
};

Batch MakeBatch(std::mt19937& rng, int32_t vertexCount, int32_t triangles) {
    Batch b;
    b.vertices.resize(static_cast<size_t>(vertexCount) * kStride);
    for (uint8_t& v : b.vertices) v = static_cast<uint8_t>(rng());
    std::uniform_int_distribution<int32_t> pick(0, vertexCount - 1);
    for (int32_t t = 0; t < triangles * 3; ++t) b.indices.push_back(static_cast<uint16_t>(pick(rng)));
    return b;
}

std::vector<Span> SpansOf(const std::vector<Batch>& batches) {
    std::vector<Span> spans;
    for (const Batch& b : batches) {
        Span s;
        s.vertices    = b.vertices.data();
        s.vertexCount = b.Count();
        s.indices     = b.indices.data();
        s.indexCount  = static_cast<int32_t>(b.indices.size());
        spans.push_back(s);
    }
    return spans;
}

}  // namespace

UNIT_TEST(ext_draw_batch_fill_is_the_concatenation_rebased) {
    std::mt19937 rng(20261020);
    std::vector<Batch> batches;
    for (int k = 0; k < 9; ++k) batches.push_back(MakeBatch(rng, 4 + k * 3, 2 + k));
    const std::vector<Span> spans = SpansOf(batches);

    size_t vbytes = 0;
    size_t icount = 0;
    for (const Batch& b : batches) {
        vbytes += b.vertices.size();
        icount += b.indices.size();
    }
    std::vector<uint8_t>  vb(vbytes + 8, 0xCD);
    std::vector<uint16_t> ib(icount + 4, 0xCDCD);
    const int64_t written = FillRun(spans.data(), static_cast<int32_t>(spans.size()), kStride, vb.data(), ib.data());
    CHECK_EQ(written, static_cast<int64_t>(icount));

    // Every merged triangle corner reads the same vertex bytes the batch's own draw read.
    size_t voff = 0;
    size_t ioff = 0;
    uint32_t base = 0;
    for (const Batch& b : batches) {
        CHECK(std::memcmp(vb.data() + voff, b.vertices.data(), b.vertices.size()) == 0);
        for (size_t k = 0; k < b.indices.size(); ++k) {
            const uint16_t merged = ib[ioff + k];
            CHECK_EQ(static_cast<uint32_t>(merged), b.indices[k] + base);
            CHECK(std::memcmp(vb.data() + static_cast<size_t>(merged) * kStride,
                              b.vertices.data() + static_cast<size_t>(b.indices[k]) * kStride, kStride) == 0);
        }
        voff += b.vertices.size();
        ioff += b.indices.size();
        base += static_cast<uint32_t>(b.Count());
    }
    // Nothing past the run is touched.
    CHECK_EQ(vb[vbytes], 0xCD);
    CHECK_EQ(ib[icount], 0xCDCD);
}

UNIT_TEST(ext_draw_batch_the_largest_index_is_65535) {
    std::mt19937 rng(7);
    // Two batches whose sum is exactly 65536 vertices: the second's last vertex is index 65535.
    std::vector<Batch> batches;
    batches.push_back(MakeBatch(rng, 65000, 1));
    batches.push_back(MakeBatch(rng, 536, 1));
    batches[1].indices[0] = 535;
    CHECK(RunFits(0x800000, kStride, 0, 0, 65000, 3));
    CHECK(RunFits(0x800000, kStride, 65000, 3, 536, 3));
    CHECK(!RunFits(0x800000, kStride, 65000, 3, 537, 3));   // 65537 vertices: cut
    const std::vector<Span> spans = SpansOf(batches);
    std::vector<uint8_t>  vb(65536 * kStride);
    std::vector<uint16_t> ib(6);
    FillRun(spans.data(), 2, kStride, vb.data(), ib.data());
    CHECK_EQ(ib[3], 65535);
}

UNIT_TEST(ext_draw_batch_retail_capacity_test) {
    // reject capacity <= vertices * stride (strict: equal is rejected)
    CHECK(RetailAccepts(1000, 49, 0x14, 0));
    CHECK(!RetailAccepts(1000, 50, 0x14, 0));
    // reject indices * 2 >= the VERTEX capacity (sic), whatever the index buffer's size
    CHECK(RetailAccepts(1000, 1, 0x14, 499));
    CHECK(!RetailAccepts(1000, 1, 0x14, 500));
    // a run is cut where the merged request fails, even when each item alone passes
    CHECK(RunFits(1000, 0x14, 0, 0, 30, 300));
    CHECK(!RunFits(1000, 0x14, 30, 300, 30, 0));    // 60 * 0x14 = 1200 bytes
    CHECK(!RunFits(1000, 0x14, 10, 300, 10, 200));  // 500 indices * 2 >= 1000
    CHECK(RunFits(1000, 0x14, 10, 300, 10, 199));
}

UNIT_TEST(ext_draw_batch_split_runs_cover_every_item_once) {
    // The greedy cut the DrawItemList copy uses: walk the items, start a new run when RunFits says no.
    std::mt19937 rng(42);
    std::uniform_int_distribution<int32_t> verts(1, 9000);
    const int32_t capacity = 0x800000;   // the 8 MB dynamic vertex buffer
    int64_t runV = 0;
    int64_t runI = 0;
    int64_t totalV = 0;
    int64_t seenV = 0;
    int runs = 1;
    for (int k = 0; k < 400; ++k) {
        const int32_t v = verts(rng);
        const int32_t idx = v * 3;
        totalV += v;
        if (!RunFits(capacity, kStride, runV, runI, v, idx)) {
            CHECK(runV > 0);
            seenV += runV;
            runV = 0;
            runI = 0;
            ++runs;
            CHECK(RunFits(capacity, kStride, 0, 0, v, idx));
        }
        runV += v;
        runI += idx;
        CHECK(runV <= kMaxRunVertices);
    }
    seenV += runV;
    CHECK_EQ(seenV, totalV);
    CHECK(runs > 1);
}

UNIT_TEST(ext_draw_batch_registry_row) {
    const Ext::Entry* e = Ext::FindEntry("draw_batch");
    CHECK(e != nullptr);
    if (!e) return;
    CHECK(e->tier == Ext::Tier::Extension);
    CHECK(e->cls == Ext::Class::A);
    CHECK(e->defaultOn);
    CHECK(Ext::SettingsKey(*e) == "ext.draw_batch");
    CHECK(ResolveText("").ext.draw_batch);                      // class A: on by default
    CHECK(!ResolveText("ext.draw_batch=0\n").ext.draw_batch);
    CHECK(!ResolveText("profile=faithful\n").ext.draw_batch);   // the faithful profile turns it off
}
