"""Post-codegen patch: make every statically-unresolved guest branch/call dispatch
instead of aborting.

RexGlue codegen gives up in two places:

1. bctr jump tables it under-counts. It emits
   `default: __builtin_trap(); // Switch case out of range`
   (manual [[entrypoint.switch_tables]] are parsed but never applied in SDK
   0.10.0.9-dev, measured: "Loaded manual switch table" at debug
   level, "discoverBlocks: using manual jump table" never emitted). Every such
   switch is preceded by `mtctr rN` -> `ctx.ctr.u64 = ...` computed from the
   real guest table, so ctx.ctr already holds the correct target for ANY index:
   REX_CALL_INDIRECT_FUNC(ctx.ctr.u32) restores guest semantics, and an unknown
   target becomes a harvestable FATAL log instead of SIGILL.

2. Branch/call targets it never discovered: REX_FATAL("Unresolved branch|call
   from 0xS to 0xT"). REX_FATAL is log+std::abort, i.e. a silent-ish process
   death (0xC0000409) that costs one build+launch cycle per address when hunted
   live. This pass rewrites all of them at once:

     * branch whose 0xT label already exists in the SAME generated function body
       -> `goto loc_T`. This is the common case in this title: the block is a
       real intra-function edge that discovery simply failed to link (measured: 0x824AF5C0 is `loc_824AF5C0:` at gowj_recomp.123.cpp:5400 and
       simultaneously "conditional branch to unknown address" at :8507 of the
       same function). Correct guest semantics, no new function, no cycle.
     * otherwise -> REX_CALL_INDIRECT_FUNC(0xT); return; using the dispatch
       table plus the runtime resolver.

It prints how many of the rewritten dispatch targets are still not registered in the function table.

Run after every codegen pass, before build. Idempotent. Wired into the build via
CMakeLists.txt (the switchtrap step) - do NOT put it in
generated/rexglue.cmake (codegen re-emits that file from the SDK template).
"""
import glob
import json
import re
import sys
from collections import Counter
from pathlib import Path

TRAP = "__builtin_trap(); // Switch case out of range"
REPL = "REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n\t\treturn;"

FATAL_SITE = re.compile(
    r'(if \((?:!\s*)?ctx\.cr\d\.\w+\)) '
    r'REX_FATAL\("Unresolved (branch|call) from 0x([0-9A-Fa-f]{8}) to 0x([0-9A-Fa-f]{8})"\);')
FATAL_BARE = re.compile(
    r'REX_FATAL\("Unresolved (branch|call) from 0x([0-9A-Fa-f]{8}) to 0x([0-9A-Fa-f]{8})"\);(\s*return;)?')
#                  ^ group 4: codegen's dead trailing return, DELETED by being
#                    part of the match. Never re-advance `last` by its length -
#                    doing so skipped the following `len(tail)` characters of the
#                    source and spliced them into the replacement. Measured
#                     on gowj_recomp.175.cpp:27019, which turned
#                    `\tREX_FATAL(...);\n\treturn;\nloc_82D9D4FC:` into
#                    `\tREX_CALL_INDIRECT_FUNC(...); return;D4FC:` - i.e. silent
#                    corruption of the next label, and the actual cause of the
#                    "use of undeclared label" build failure.
# Every DEFINE_REX_FUNC name, not just `sub_<hex>`: codegen also emits bodies for
# guest symbols like `xstart` and the compiler-ABI stubs (`__restfpr_20`,
# `__savevmx_114`). Skipping one leaves its braces invisible to func_extents, so a
# goto/label pair inside it is attributed to no body at all (measured: 17 false
# "unresolvable goto" reports in gowj_recomp.154.cpp, all inside `xstart`).
FUNC_DEF = re.compile(r"DEFINE_REX_FUNC\((\w+)\)")

WS = Path(__file__).resolve().parents[1]


def func_extents(s: str):
    """[(body_open, body_close)] for every DEFINE_REX_FUNC, brace-matched.

    The body, not the [def, next-def) text range: `loc_` labels are per-function
    C++ labels, and a goto may only target one inside the same braces. Assuming
    functions are back-to-back is wrong here because codegen also emits
    `DEFINE_REX_FUNC` prototypes and a dispatch-table block, so a `loc_` line can
    sit between two bodies (measured: a [def,next-def) check approved
    `goto loc_82DAE3D0`, which the compiler rejected as an undeclared label).
    """
    spans = []
    for m in FUNC_DEF.finditer(s):
        i = s.find("{", m.end())
        if i < 0:
            continue
        depth = 0
        j = i
        n = len(s)
        while j < n:
            c = s[j]
            if c == "/" and s.startswith("//", j):
                j = s.find("\n", j)
                if j < 0:
                    break
                continue
            if c == '"':
                j += 1
                while j < n and s[j] != '"':
                    j += 2 if s[j] == "\\" else 1
            elif c == "'":
                j += 1
                while j < n and s[j] != "'":
                    j += 2 if s[j] == "\\" else 1
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        spans.append((i, min(j + 1, n)))
    return spans


def _body_of(spans, pos):
    """Index of the body containing pos, or -1."""
    lo, hi = 0, len(spans) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        a, b = spans[mid]
        if pos < a:
            hi = mid - 1
        elif pos >= b:
            lo = mid + 1
        else:
            return mid
    return -1


LABEL_DEF = re.compile(r"^loc_([0-9A-Fa-f]{8}):", re.M)
GOTO = re.compile(r"goto loc_([0-9A-Fa-f]{8});")


def intra_labels(s: str, spans):
    """{body_index: set of labels declared at column 0 inside that body}."""
    out = {}
    for k, (a, b) in enumerate(spans):
        out[k] = {m.group(1).upper() for m in LABEL_DEF.finditer(s, a, b)}
    return out


def patch_fatal_branches(s: str, fname: str):
    """Rewrite unresolved branch/call fatals; return (source, n_patched, harvest)."""
    harvest = []
    n = 0

    def make_render(spans, labels):
        def render(kind, tgt, cond, pos):
            """Replacement text for one site; also report whether it needs a hint."""
            k = _body_of(spans, pos)
            # An intra-function edge: discovery already emitted the destination
            # block in this very body, so a goto is exactly the guest's `pc = T`.
            if kind == "branch" and k >= 0 and tgt in labels[k]:
                return (f"{cond} goto loc_{tgt};" if cond else f"goto loc_{tgt};"), False
            if cond:
                return f"{cond} {{ REX_CALL_INDIRECT_FUNC(0x{tgt}); return; }}", True
            return f"REX_CALL_INDIRECT_FUNC(0x{tgt}); return;", True
        return render

    # Conditioned sites first; their replacement text matches neither pattern, so
    # the bare sweep below cannot double-patch them. Each pass re-derives the
    # extents from the text it is about to scan, since the first pass rewrites
    # (and the bare pass deletes codegen's now-dead `return;`).
    for pat, groups in ((FATAL_SITE, (2, 3, 4, 1)), (FATAL_BARE, (1, 2, 3, None))):
        spans = func_extents(s)
        labels = intra_labels(s, spans)
        render = make_render(spans, labels)
        out, last = [], 0
        for m in pat.finditer(s):
            kind, src, tgt, cond = (m.group(groups[0]), m.group(groups[1]).upper(),
                                    m.group(groups[2]).upper(),
                                    m.group(groups[3]) if groups[3] else None)
            rep, needs_hint = render(kind, tgt, cond, m.start())
            out.append(s[last:m.start()])
            out.append(rep)
            # codegen's own trailing `return;` is dead after either replacement;
            # the bare pattern captured it, so it is already gone from the output.
            last = m.end()
            n += 1
            if needs_hint:
                harvest.append((tgt, src, kind))
        out.append(s[last:])
        s = "".join(out)

    records = [{"target": f"0x{t}", "site": f"0x{src}", "kind": kind,
                "file": Path(fname).name} for t, src, kind in harvest]
    return s, n, records


def unresolvable_gotos(s: str):
    """goto statements whose label is not in the same brace-matched body.

    A compile error here would otherwise only surface 10 minutes into a build, and
    the earlier [def, next-def) text window let one through, so the patcher now
    proves every goto it emits and fails the build edge instead.
    """
    spans = func_extents(s)
    labels = intra_labels(s, spans)
    bad = []
    for m in GOTO.finditer(s):
        k = _body_of(spans, m.start())
        if k < 0 or m.group(1).upper() not in labels[k]:
            bad.append((s.count("\n", 0, m.start()) + 1, m.group(1).upper()))
    return bad


def main(pattern: str) -> int:
    files = sorted(glob.glob(pattern))
    if not files:
        print(f'no sources matched {pattern}', file=sys.stderr)
        return 1
    total = changed_files = unsafe = fatals = 0
    harvest = []
    pending = []  # (path, text) - written only once every file has verified
    offenders = []
    for fn in files:
        s = open(fn, encoding="utf-8").read()
        orig = s
        if TRAP in s:
            n = s.count(TRAP)
            for m in re.finditer(re.escape(TRAP), s):
                ctx_before = s[max(0, m.start() - 8000):m.start()]
                switch_pos = ctx_before.rfind("switch (")
                mtctr_pos = ctx_before.rfind("ctx.ctr.u64 =")
                if mtctr_pos == -1 or switch_pos == -1 or mtctr_pos > switch_pos:
                    unsafe += 1
            s = re.sub(
                r"(\t+)__builtin_trap\(\); // Switch case out of range",
                lambda m: m.group(1) + REPL,
                s,
            )
            total += n
        if 'REX_FATAL("Unresolved ' in s:
            s, nf, hv = patch_fatal_branches(s, fn)
            fatals += nf
            harvest.extend(hv)
        if s != orig:
            bad = unresolvable_gotos(s)
            if bad:
                # Never ship a tree that cannot compile: a goto whose label is not
                # in the same function is a hard error, and reporting it here costs
                # 9 seconds instead of the ~10 minutes a failed build costs.
                offenders.extend((fn, line, lab) for line, lab in bad)
            # Rewrites replace statements, so the set of column-0 labels can only
            # stay identical. Anything else means the pass spliced source text
            # (the `last = m.end() + len(tail)` bug destroyed the label following
            # a bare fatal's `return;`), and that class of mistake is exactly what
            # this check exists to make impossible rather than what it is for.
            lost = Counter(LABEL_DEF.findall(orig)) - Counter(LABEL_DEF.findall(s))
            if lost:
                offenders.extend((fn, 0, f"{k} x{v} label(s) destroyed")
                                 for k, v in sorted(lost.items()))
            pending.append((fn, s))

    if offenders:
        for fn, line, lab in offenders:
            print(f"ERROR {fn}:{line}: goto loc_{lab} has no label in that body"
                  if line else f"ERROR {fn}: {lab}", file=sys.stderr)
        print(f"not writing {len(pending)} file(s): {len(offenders)} unresolvable goto(s)",
              file=sys.stderr)
        return 2
    for fn, s in pending:
        open(fn, "w", encoding="utf-8", newline="").write(s)
        changed_files += 1

    seen = {}
    for h in harvest:
        seen.setdefault(h["target"], h)
    # An REX_CALL_INDIRECT_FUNC(0xT) is only a problem if nothing is registered
    # at 0xT: the hint added on the previous round turns the same dispatch into a
    # resolved table call, and reporting it again would make static_harvest.py
    # loop forever on an already-closed class (measured, round 1
    # repeated round 0's 4 targets, all four of which had DEFINE_REX_FUNC bodies).
    reg = set()
    for cand in (Path(files[0]).parent / "gowj_register.cpp",
                 Path(files[0]).parent / "gowj_init.cpp"):
        if cand.exists():
            reg |= {"0x" + a.upper() for a in
                    re.findall(r"SetFunction\(0x([0-9A-Fa-f]{8})",
                               cand.read_text(errors="replace"))}
    unmet = {t: h for t, h in seen.items() if t not in reg}
    gotos = fatals - len(harvest)
    print(f"patched {total} traps in {changed_files} files, {unsafe} without verified mtctr; "
          f"{fatals} unresolved-branch fatals rewritten ({gotos} intra-function goto, "
          f"{len(harvest)} indirect-dispatch, {len(unmet)} of {len(seen)} targets still unregistered)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "generated/default/*.cpp"))
