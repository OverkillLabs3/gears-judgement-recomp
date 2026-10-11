// gowj - ReXGlue Recompiled Project
//
// Serialization + instrumentation around the guest's XamInput* entry points.
// See gowj_xam_input_guard.cpp.

#pragma once

namespace rex::glue {

void InstallXamInputGuard();
// Suppresses the system's startup "Disc Read Error" box (no disc drive is mounted by design).
void InstallDiscErrorSilencer();

}  // namespace rex::glue
