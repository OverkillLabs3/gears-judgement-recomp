// gowj - opt-in whole-process profiler (see gowj_profiler.cpp). GOWJ_PROFILE=1 enables it.

#pragma once

#include <cstddef>
#include <cstdint>

namespace rex::glue {

void InstallProfiler();
// Per frame (kind 0 present, 1 guest refresh); writes GOWJ_FRAMELOG rows. Cheap no-op when off.
void FrameLogNote(size_t kind, uint64_t interval_us);

}  // namespace rex::glue
