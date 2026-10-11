// gowj - ReXGlue Recompiled Project
//
// Access-violation attributor.
//
// A guest-side crash in a recompiled title is invisible to normal host tooling:
// the exe is stripped (no COFF symbol table, no .pdb), so a raw RIP says nothing
// about which PPC function was executing, and the runtime's own log stops dead
// because the faulting thread holds the logger while WER tears the process down.
// Measured 09:25: the level-start path exited 0xC0000005 with an empty
// FATAL list, which the autoresolve loop correctly classified as "manual triage"
// and then could not act on.
//
// This handler resolves RIP - and every host-stack value that lands inside a
// recompiled function - against the codegen-emitted PPCFuncMappings table, so the
// log carries the guest function names of the whole translated call chain. It
// continues the search afterwards: it attributes crashes, it does not change them.

#pragma once

namespace rex::glue {

void InstallCrashWatch();

}  // namespace rex::glue
