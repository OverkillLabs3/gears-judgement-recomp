"""POST_BUILD guard: prove every patched source was actually compiled.

Why: ninja decides what is dirty when the build is PLANNED, before any of the
post-codegen patcher edges run. So when a patcher rewrites a generated .cpp
during a build whose .obj was already up to date, ninja never queues that
compile, links the stale object, and the shipped exe silently lacks the patch.
Measured: patch_raw_look rewrote gowj_recomp.133.cpp at
00:21:49 and gowj.exe linked at 00:21:54 from an object last compiled at the
previous afternoon - a 9-hour-stale TU carrying the entire raw-mouse feature.

The fix is to build twice, which is what scripts/build.ps1 does; this script makes the
first build LOUD instead of quietly wrong. It compares each generated source
against the object CMake names for it and fails if any source is newer.

    python tools/check_objects_fresh.py <gen_dir> <obj_dir>

Exit 0 = every object is at least as new as its source.
"""
from __future__ import annotations

import os
import sys


def main(argv):
    if len(argv) != 3:
        raise SystemExit("usage: check_objects_fresh.py <generated_dir> <obj_dir>")
    gen, objs = argv[1], argv[2]
    if not os.path.isdir(gen):
        raise SystemExit(f"no generated dir {gen}")
    if not os.path.isdir(objs):
        # A different generator layout is not a stale build; say so and pass.
        print(f"check_objects_fresh: no object dir {objs}, skipped")
        return 0

    stale = []
    checked = 0
    for name in sorted(os.listdir(gen)):
        if not name.endswith(".cpp"):
            continue
        src = os.path.join(gen, name)
        obj = os.path.join(objs, name + ".obj")
        if not os.path.exists(obj):
            stale.append((name, "no object at all"))
            continue
        checked += 1
        s, o = os.path.getmtime(src), os.path.getmtime(obj)
        if s > o:
            stale.append((name, f"source {s:.0f} newer than object {o:.0f}"))

    if stale:
        for name, why in stale[:20]:
            print(f"STALE OBJECT: {name}: {why}", file=sys.stderr)
        print(f"check_objects_fresh: {len(stale)} of {checked + len(stale)} generated "
              "sources are NOT in the objects that were just linked. A patcher ran "
              "during this build and ninja had already planned its compiles. "
              "RE-BUILD - the second invocation sees the patched tree.",
              file=sys.stderr)
        return 1
    print(f"check_objects_fresh: {checked} generated objects all fresher than sources")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
