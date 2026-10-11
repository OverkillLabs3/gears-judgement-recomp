#pragma once
// Project-local frame trace shared by the D3D12 backend.
//   Create a file named "gowj_trace.now" in the working directory while the game runs: the next
//   few frames log every resolve (RESOLVE) and swap (SWAP) with their parameters, then it stops.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace rex::graphics::d3d12 {
inline std::atomic<uint32_t> gowj_trace_frame{0};
inline std::atomic<int> gowj_trace_remaining{0};

// Called once per swap. Returns true if this frame is being traced.
inline bool GowjTraceBeginFrame() {
  uint32_t f = gowj_trace_frame.fetch_add(1);
  if (gowj_trace_remaining.load() <= 0 && (f % 30) == 0) {
    std::error_code ec;
    if (std::filesystem::exists("gowj_trace.now", ec)) {
      std::filesystem::remove("gowj_trace.now", ec);
      gowj_trace_remaining.store(3);
    }
  }
  return gowj_trace_remaining.load() > 0;
}
inline bool GowjTraceActive() { return gowj_trace_remaining.load() > 0; }
inline void GowjTraceEndFrame() {
  if (gowj_trace_remaining.load() > 0) gowj_trace_remaining.fetch_sub(1);
}
}  // namespace rex::graphics::d3d12
