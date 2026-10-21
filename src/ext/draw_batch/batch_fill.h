// ============================================================================
// src/ext/draw_batch/batch_fill.h -- ext.draw_batch's buffer fill and run limits (pure)
// ============================================================================
// NOT RETAIL. docs/extensions/handins/DRAWBATCH.md. Standard C++ only: the unit tests
// (tests/unit/ext_draw_batch_test.cpp) link batch_fill.cpp alone.
//
// A merged run is a sequence of consecutive draw items whose faithful draws would each map
// the Context's dynamic vertex and index buffers, copy one item's vertices and indices, and
// issue one DrawIndexed. The merged draw maps once, writes every item's vertices in item
// order and every item's indices offset by the vertices written before it, and draws once.
// The vertices the GPU fetches, in the order it rasterises the triangles, are the same.
//
// The run limits keep every merged draw one that retail's own path would accept and that
// 16-bit indices can address:
//   * retail's two capacity tests of ContextData::BeginInstancedIndexedVertices
//     (src/render/dx11/contextdata.cpp), INCLUDING the sic test of the index byte count
//     against the VERTEX buffer's capacity;
//   * at most 65536 vertices, so the largest rebased index is 65535.
// ============================================================================
#pragma once

#include <cstdint>

namespace Ext {
namespace DrawBatch {

constexpr int64_t kMaxRunVertices = 65536;   // 16-bit indices address 0..65535

// Retail's accept test for one Begin(Instanced)IndexedVertices request, the two compares as
// written there: reject `capacity <= vertices * stride`, reject `indices * 2 >= capacity` (the
// VERTEX buffer's capacity, sic). Widened to 64 bits: every request it is asked about here is
// one retail computes in 32 bits without overflow (at most kMaxRunVertices * 0x34 bytes).
bool RetailAccepts(int32_t vertexCapacityBytes, int64_t vertexCount, int32_t stride, int64_t indexCount);

// True when a run already holding (runVertices, runIndices) can take one more item of
// (vertices, indices) and stay a request retail accepts with at most kMaxRunVertices.
bool RunFits(int32_t vertexCapacityBytes, int32_t stride, int64_t runVertices, int64_t runIndices,
             int64_t vertices, int64_t indices);

// One item's CPU geometry: `vertexCount` records of the run's stride and its 16-bit indices,
// each relative to the item's own first vertex.
struct Span {
    const void*     vertices    = nullptr;
    int32_t         vertexCount = 0;
    const uint16_t* indices     = nullptr;
    int32_t         indexCount  = 0;
};

// Writes the spans in order: the vertex bytes concatenated into `vb`, and each span's indices
// plus the number of vertices written before that span into `ib`. The caller has checked the
// run with RunFits, so every rebased index fits 16 bits. Returns the indices written.
int64_t FillRun(const Span* spans, int32_t count, int32_t stride, uint8_t* vb, uint16_t* ib);

}  // namespace DrawBatch
}  // namespace Ext
