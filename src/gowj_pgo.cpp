// gowj - PGO training support (compiled in always, active only in GOWJ_PGO=gen builds).
//
// The title ends with "hard-exiting process", which skips atexit, so the LLVM profile runtime would
// never write its counters. This thread writes them every 20 s (the file name comes from
// LLVM_PROFILE_FILE; %m merges successive writes), so a normal play session leaves a usable
// .profraw behind, at most 20 s short of the end.

#include "gowj_pgo.h"

#ifdef GOWJ_PGO_GEN
#include <windows.h>

#include <thread>

#include <rex/logging.h>

extern "C" int __llvm_profile_write_file(void);

namespace rex::glue {

void InstallPgoFlusher() {
  static bool started = false;
  if (started) return;
  started = true;
  std::thread([] {
    for (int i = 0;; ++i) {
      Sleep(20000);
      const int rc = __llvm_profile_write_file();
      if (i < 3 || i % 15 == 0) {
        REXLOG_INFO("PGO profile flush #{} rc={}", i + 1, rc);
      }
    }
  }).detach();
  REXLOG_WARN("PGO TRAINING BUILD: instrumented guest code (slower than the normal build); "
              "profile written every 20 s to LLVM_PROFILE_FILE");
}

}  // namespace rex::glue
#else
namespace rex::glue {
void InstallPgoFlusher() {}
}  // namespace rex::glue
#endif
