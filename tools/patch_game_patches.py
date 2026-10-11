"""Post-codegen port of the xenia-canary game patches for Gears of War: Judgment.

Source: xenia-canary/game-patches, patches/4D530A26 - Gears of War Judgment.patch.toml
(title 4D530A26, module hash 8429D0AE190C10DB). Verified that the file
targets this exact image: every patched word below matches the original
instruction at that address in the decrypted PE image (flat image, file
offset == address - 0x82000000) and the generated code's comment there.

A patch in a recompile has to land on the C++ that implements the instruction,
not on bytes. Each site keeps the original behaviour behind a host switch,
`GowjGamePatch(id)` (gowj/src/gowj_game_patches.cpp), so every patch is a cvar
and can be A/B'd without a rebuild:

  id 1 film grain        8243D854 lfs f0,180(r30)   -> lfs f0,0(r31)  (gfx/gowj_film_grain)
  id 2 scaling fix       8236AB04 lfs f2,16(r28)    -> lfs f2,0(r28)  (gfx/gowj_game_fxaa=false)
                         8236AB44 lfs f2,20(r28)    -> lfs f2,0(r28)
  id 3 game FXAA off     824408A8 stw r11,17252(r9) -> li r11,0       (gfx/gowj_game_fxaa=false)
  id 4 motion blur off   824427A8 lwz r11,56(r10)   -> li r11,0       (gfx/gowj_motion_blur=false)
                         827CBD8C lwz r11,128(r10)  -> li r11,0

NOT ported: "Occlusion Query Fix" - its 827DE330 site is `addi r12,r1,-152` here,
which the patch's `li r11,0` cannot sensibly replace; refused rather than guessed.

    python tools/patch_game_patches.py generated/default/*.cpp

Idempotent (marker GOWJ GAMEPATCH). Fails closed if any site's instruction text or
C++ body differs from what is asserted below.
"""
from __future__ import annotations

import glob
import os
import re
import sys

MARK = "GOWJ GAMEPATCH"
DECL = f'extern "C" int GowjGamePatch(int id);  // {MARK}\n'

# (address, function start, expected comment, expected C++ body, patched C++ body, id)
SITES = [
    (0x8243D854, 0x8243D6E0, "lfs f0,180(r30)",
     ["temp.u32 = REX_LOAD_U32(ctx.r30.u32 + 180);", "ctx.f0.f64 = double(temp.f32);"],
     "temp.u32 = REX_LOAD_U32(GowjGamePatch(1) ? ctx.r31.u32 + 0 : ctx.r30.u32 + 180); "
     "ctx.f0.f64 = double(temp.f32);", 1),
    (0x8236AB04, 0x8236AA08, "lfs f2,16(r28)",
     ["temp.u32 = REX_LOAD_U32(ctx.r28.u32 + 16);", "ctx.f2.f64 = double(temp.f32);"],
     "temp.u32 = REX_LOAD_U32(ctx.r28.u32 + (GowjGamePatch(2) ? 0 : 16)); "
     "ctx.f2.f64 = double(temp.f32);", 2),
    (0x8236AB44, 0x8236AA08, "lfs f2,20(r28)",
     ["temp.u32 = REX_LOAD_U32(ctx.r28.u32 + 20);", "ctx.f2.f64 = double(temp.f32);"],
     "temp.u32 = REX_LOAD_U32(ctx.r28.u32 + (GowjGamePatch(2) ? 0 : 20)); "
     "ctx.f2.f64 = double(temp.f32);", 2),
    (0x824408A8, 0x82440860, "stw r11,17252(r9)",
     ["REX_STORE_U32(ctx.r9.u32 + 17252, ctx.r11.u32);"],
     "if (GowjGamePatch(3)) { ctx.r11.s64 = 0; } else { "
     "REX_STORE_U32(ctx.r9.u32 + 17252, ctx.r11.u32); }", 3),
    (0x824427A8, 0x82442750, "lwz r11,56(r10)",
     ["ctx.r11.u64 = REX_LOAD_U32(ctx.r10.u32 + 56);"],
     "ctx.r11.u64 = GowjGamePatch(4) ? 0 : REX_LOAD_U32(ctx.r10.u32 + 56);", 4),
    (0x827CBD8C, 0x827CBD58, "lwz r11,128(r10)",
     ["ctx.r11.u64 = REX_LOAD_U32(ctx.r10.u32 + 128);"],
     "ctx.r11.u64 = GowjGamePatch(4) ? 0 : REX_LOAD_U32(ctx.r10.u32 + 128);", 4),
]

INSN = re.compile(r"^\s*// [a-z]")


def main(argv: list[str]) -> int:
    files: list[str] = []
    for a in argv:
        files.extend(glob.glob(a) if any(c in a for c in "*?[") else [a])
    by_func: dict[int, str] = {}
    head = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)")
    wanted = {s[1] for s in SITES}
    for f in files:
        with open(f, errors="replace") as fh:
            for line in fh:
                m = head.match(line)
                if m and int(m.group(1), 16) in wanted:
                    by_func[int(m.group(1), 16)] = f
    missing = wanted - by_func.keys()
    if missing:
        print(f"patch_game_patches: functions not found: {sorted(hex(x) for x in missing)}")
        return 1
    edits: dict[str, list] = {}
    for site in SITES:
        edits.setdefault(by_func[site[1]], []).append(site)
    total = 0
    for f, sites in edits.items():
        with open(f, errors="replace", newline="") as fh:
            lines = fh.readlines()
        changed = False
        for addr, fstart, want, body, new, pid in sites:
            start = next(i for i, l in enumerate(lines)
                         if l.startswith(f"DEFINE_REX_FUNC(sub_{fstart:08X})"))
            k = (addr - fstart) // 4
            seen = -1
            for j in range(start + 1, len(lines)):
                if lines[j].startswith("DEFINE_REX_FUNC"):
                    raise SystemExit(f"{addr:08X}: ran off sub_{fstart:08X}")
                if INSN.match(lines[j]):
                    seen += 1
                    if seen == k:
                        break
            comment = lines[j].strip()
            if MARK in lines[j + 1]:
                continue  # already patched
            if comment != f"// {want}":
                raise SystemExit(f"{addr:08X}: expected '// {want}', found '{comment}'")
            got = [lines[j + 1 + n].strip() for n in range(len(body))]
            if got != body:
                raise SystemExit(f"{addr:08X}: body mismatch {got}")
            indent = re.match(r"\s*", lines[j + 1]).group(0)
            nl = "\r\n" if lines[j + 1].endswith("\r\n") else "\n"
            lines[j + 1:j + 1 + len(body)] = [f"{indent}{new}  // {MARK} {pid}{nl}"]
            changed = True
            total += 1
        if changed:
            text = "".join(lines)
            if DECL not in text:
                text = DECL + text
            tmp = f + ".gp.tmp"
            with open(tmp, "w", errors="replace", newline="") as fh:
                fh.write(text)
            os.replace(tmp, f)
    print(f"patch_game_patches: {total} site(s) patched")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
