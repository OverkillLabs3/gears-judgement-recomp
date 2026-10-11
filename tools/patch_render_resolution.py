"""Post-codegen patch: let the HOST choose the guest's render resolution.

WHY THIS EXISTS (all measured, no guessing):

* Gears of War: Judgment allocates its back buffer at 1280x720 no matter what the
  host asks for. `video_mode_width/height` reach the guest (a CLI override shows
  up in the runtime's own `[config]`/`VdQueryVideoMode` path) and the engine still
  creates `tiled 1280x720x1 2D k_8_8_8_8`.
* `draw_resolution_scale_x/y` do NOT add visible pixels: the guest rasterises at
  Nx and then resolves back DOWN into its own fixed-size 720p texture, so the
  monitor only ever receives a 720p image that the host stretches. It also costs
  4-9x the GPU work, which is why "200 fps" felt sluggish.
* The single writer of the engine's two render-size globals is sub_82990A28
  (gowj_recomp.206.cpp). It computes
      height := 720                                  # literal
      width  := clamp(round(aspect * 720.0), 904, 1280)
  and ~40 consumers (the render-target builder, viewports, the present params)
  read those two words back. So the fix is three instructions, not a rewrite.
* ...but it is four, because the same function then second-guesses that decision:
      lwz r11,0(r30)        # r30 == &width global, so this re-reads the store
      cmpwi cr6,r11,1280
      beq   <normal path>
                            # else: a THIRD size global set (base+15872/15876,
                            #  initialised to 1120/688/1344 by sub_82990990 in
                            #  gowj_recomp.120.cpp:17797) is clobbered to
                            #  800x800 plus a flag cleared at base+17496.
  Measured: overriding the width to 2560/3840 takes that fallback, and
  the frame then appears stacked target_height/720 times - the engine is still
  laying its output out against a size that is no longer the render target. So
  the guard is fed the guest's own pre-override width instead of the stored one:
  the override changes the target, never the engine's standard-resolution verdict.

WHAT IT DOES: routes each site through a host hook so config decides:

    ctx.r11.u32 = GowjGuestRenderHeight(720);
    ctx.r6.u32  = GowjGuestRenderHeight(720);          # the %dx%d log argument
    REX_STORE_U32(..., GowjGuestRenderWidth(ctx.r5.u32));
    ctx.r11.u64 = GowjGuestOriginalWidth(REX_LOAD_U32(ctx.r30.u32 + 0));

The hooks live in src/gowj_render_scale.cpp and return the guest's own value when
gowj_render_width/height are 0, so an unpatched-behaviour default is preserved and
the override is provable: the first substitution logs
`gowj RESSCALE guest width 1280 -> 3840` at WARN level.

Every substitution must land exactly once across the whole tree; main() raises
otherwise. A silent partial patch is precisely the failure mode that let an
earlier build claim a resolution it did not have.

Wired into gowj/CMakeLists.txt as gowj_render_scale_patch, chained AFTER
gowj_switchtrap_patch (both mutate the same files, so they must not race). Never
hook generated/rexglue.cmake - codegen re-emits that file from the SDK template.
"""
from __future__ import annotations

import glob
import os
import re
import sys

DECL_MARK = "GOWJ NATIVE RESOLUTION"

# Every host symbol the substitutions below reference. Declared individually so a
# TU that an earlier version of this script patched still gains the new hooks -
# a whole-block "declaration already inserted" check used to skip exactly that,
# and the build then failed on an undeclared identifier at the new site.
HOOKS = ["GowjGuestRenderWidth", "GowjGuestRenderHeight", "GowjGuestOriginalWidth"]

DECL_HEAD = (
    "\n"
    f"// {DECL_MARK} (inserted by tools/patch_render_resolution.py):\n"
    "// the guest hardcodes its render target to 720p; these host hooks let config\n"
    "// choose it. See that script's header for the measurement.\n"
)


def decl_line(name: str) -> str:
    return f'extern "C" uint32_t {name}(uint32_t guest_value);\n'


def ensure_decls(out: str):
    """Declare every hook this TU now calls. Returns (text, added_any)."""
    missing = [n for n in HOOKS if decl_line(n) not in out and f"{n}(" in out]
    if not missing:
        return out, False
    block = "".join(decl_line(n) for n in missing)

    if DECL_MARK in out:
        lines = out.split("\n")
        k = next(i for i, l in enumerate(lines) if DECL_MARK in l)
        while k + 1 < len(lines) and lines[k + 1].startswith("//"):
            k += 1
        while k + 1 < len(lines) and lines[k + 1].startswith('extern "C"'):
            k += 1
        lines[k + 1 : k + 1] = [l for l in block.split("\n") if l]
        return "\n".join(lines), True

    # Declared inline rather than in a shared header so the patch stays
    # self-contained: it survives a full codegen regen of every other TU.
    m = INCLUDE_RE.search(out)
    at = m.end() if m else 0
    return out[:at] + DECL_HEAD + block + out[at:], True

# (label, exact search text, replacement text)
SUBS = [
    (
        "height-store",
        "\t// li r11,720\n\tctx.r11.s64 = 720;\n",
        "\t// li r11,720\n\tctx.r11.u32 = GowjGuestRenderHeight(720);\n",
    ),
    (
        "height-log",
        "\t// li r6,720\n\tctx.r6.s64 = 720;\n",
        "\t// li r6,720\n\tctx.r6.u32 = GowjGuestRenderHeight(720);\n",
    ),
    (
        "width-store",
        "\t// stw r5,-12052(r11)\n\tREX_STORE_U32(ctx.r11.u32 + -12052, ctx.r5.u32);\n",
        "\t// stw r5,-12052(r11)\n\tREX_STORE_U32(ctx.r11.u32 + -12052, GowjGuestRenderWidth(ctx.r5.u32));\n",
    ),
    # r30 is the width global's address (addi r30,r11,-12052 just above the
    # store), so this load reads back exactly what the store wrote. Reading the
    # PRE-override value here keeps the engine's own "is this the standard
    # resolution?" verdict intact.
    (
        "std-res-guard",
        "\t// lwz r11,0(r30)\n"
        "\tctx.r11.u64 = REX_LOAD_U32(ctx.r30.u32 + 0);\n"
        "\t// li r31,0\n"
        "\tctx.r31.s64 = 0;\n"
        "\t// cmpwi cr6,r11,1280\n"
        "\tctx.cr6.compare<int32_t>(ctx.r11.s32, 1280, ctx.xer);\n",
        "\t// lwz r11,0(r30)\n"
        "\tctx.r11.u64 = GowjGuestOriginalWidth(REX_LOAD_U32(ctx.r30.u32 + 0));\n"
        "\t// li r31,0\n"
        "\tctx.r31.s64 = 0;\n"
        "\t// cmpwi cr6,r11,1280\n"
        "\tctx.cr6.compare<int32_t>(ctx.r11.s32, 1280, ctx.xer);\n",
    ),
]

INCLUDE_RE = re.compile(r'^#include "[^"]+"\n', re.M)


def patch_file(path: str):
    """Return (sites_applied_now, sites_already_patched) for one TU.

    Substitutions are applied one at a time and each is independent: an anchor
    that is already rewritten simply matches 0 times, so adding a site to SUBS
    still lands on a tree an earlier run patched. (A whole-file "already hooked,
    skip" short-circuit would freeze the patch at whatever sites existed the
    first time it ran.) Declarations are reconciled the same way, on every TU
    that calls a hook, whether or not a substitution landed this run.
    """
    with open(path, "r", encoding="utf-8", newline="") as f:
        src = f.read()

    out = src
    applied = []
    for label, old, new in SUBS:
        c = out.count(old)
        if c == 0:
            continue
        if c != 1:
            raise SystemExit(
                f"patch_render_resolution: {os.path.basename(path)} anchor "
                f"'{label}' occurs {c} times, expected exactly 1. Refusing to "
                f"patch - codegen changed this function's shape; re-read "
                f"sub_82990A28 before continuing."
            )
        out = out.replace(old, new, 1)
        applied.append(label)

    out, _ = ensure_decls(out)
    preexisting = _preexisting(src)
    if out == src:
        return [], preexisting

    # Atomic: a half-written TU compiles into a stale-shaped binary, which is the
    # exact class of bug that made an earlier build ship unpatched code. Only the
    # basename may carry into the temp name - mkstemp joined a prefix containing
    # separators straight onto `dir` and produced a doubled path.
    directory, name = os.path.split(path)
    body, ext = os.path.splitext(name)
    tmp = os.path.join(directory or ".", body + ".resscale.tmp")
    try:
        with open(tmp, "w", encoding="utf-8", newline="") as f:
            f.write(out)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise
    return applied, _preexisting(src)


def _preexisting(src: str):
    """Labels whose replacement text is already in this TU."""
    return [label for label, _, new in SUBS if new in src]


def main(argv):
    if not argv[1:]:
        raise SystemExit("usage: patch_render_resolution.py <generated .cpp> ...")
    # Ninja hands this command the same literal `*.cpp` the switch-trap edge uses:
    # Windows does not glob in the shell, so expand it here.
    paths = []
    for pattern in argv[1:]:
        matched = sorted(glob.glob(pattern))
        paths.extend(matched or [pattern])
    files = 0
    done = {}
    for p in paths:
        try:
            applied, pre = patch_file(p)
        except SystemExit:
            raise
        except OSError as e:
            # The switch-trap patcher is handed a shell-expanded glob; on Windows
            # that arrives literally, so tolerate entries that are not files.
            print(f"patch_render_resolution: skip {p} ({e})")
            continue
        for label in applied + pre:
            done[label] = done.get(label, 0) + 1
        if applied:
            files += 1
            print(f"patch_render_resolution: {os.path.basename(p)} -> "
                  + ", ".join(applied))

    # Every site must exist exactly once in the tree, whether applied now or by an
    # earlier run. 0 means codegen stopped emitting the function; >1 means an
    # anchor matched twice. Both would otherwise ship a binary that claims a
    # resolution it does not have.
    missing = [label for label, _, _ in SUBS if done.get(label, 0) != 1]
    if missing:
        counts = ", ".join(f"{label}={done.get(label, 0)}" for label, _, _ in SUBS)
        raise SystemExit(
            f"patch_render_resolution: site(s) {missing} not applied exactly once "
            f"across {len(paths)} TU(s) ({counts}). Re-read sub_82990A28 in "
            f"generated/default/ before rebuilding."
        )
    if files == 0:
        print("patch_render_resolution: nothing to do (all sites already patched)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
