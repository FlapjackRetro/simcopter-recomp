#!/usr/bin/env python3
"""SimCopter lift driver: SimCopter.exe -> src/recomp/gen/.

SimCopter.exe (v1.0.1.4, PE timestamp 0x33418C93, 1997-04-01, MSVC, linker
3.10) is one PE image with the C runtime linked in statically and no
protection. smackw32.dll, RAD's Smacker player, is not lifted: the guest loads
it through the native32 bridge like any system DLL. Every catalogued function
is lifted, no closure limit.

    py -3 tools/catalog.py      # work/functions.json
    py -3 run_lift.py

Adapted from the Streets of SimCity recomp's run_lift.py (sp00nznet/
streetsofsimcity, MIT), itself Monster Truck Madness 2's: Streets runs on the
SimCopter engine, and its hand-written software rasterizer has the same
"return to the caller's caller" routines here. Only the chunks whose text
changed are rewritten, so a lifter fix recompiles what it touched. The
generated C is a derivative of the game binary and is never committed;
docs/bringup.md has the notes.
"""
import argparse
import filecmp
import json
import os
import re
import shutil
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
# PCRECOMP picks the toolkit checkout, the same knob CMakeLists.txt has: the
# lifter and the runtime header must come from the same tree.
_TOOLS = os.path.join(os.environ.get('PCRECOMP', os.path.join(_HERE, '..', 'pcrecomp')), 'tools')
sys.path.insert(0, os.path.join(_TOOLS, 'lift'))
sys.path.insert(0, os.path.join(_TOOLS, 'pe'))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32          # noqa: E402
from generate import (EXTENT_REACH, find_splits, true_extent,  # noqa: E402
                      linear_disassemble_function, lift_function_linear, write_chunk)
from lift32 import Lifter                                  # noqa: E402
from pe_analyze import analyze_pe, build_iat_map           # noqa: E402

GAME = os.path.join(_HERE, 'game', 'SIMCOPTER')
EXE = 'SimCopter.exe'
CATALOG = os.path.join(_HERE, 'work', 'functions.json')
OUT = os.path.join(_HERE, 'src', 'recomp', 'gen')
STATS = os.path.join(_HERE, 'work', 'lift_stats.json')

# ---- returns to the caller's caller ------------------------------------------
# The software rasterizer (hand-written asm, as in Streets of SimCity) walks a
# polygon's edges with small routines that, when the last edge is done, end
# `pop eax; ret` (or `pop eax; popal; ret`): they throw away their own return
# address and return straight to whoever called the span routine. The lift
# returns from a C function to its C caller, which would keep drawing past the
# last edge.
#
# So each such `pop` also sets sco_unwind, and every direct call to a routine
# that holds one is followed by `if (sco_unwind) { sco_unwind = 0; return; }`:
# the caller returns at once, as the original's second `ret` did.
#
# In SimCopter.exe every one of them follows `mov [abs32], eax; ret`, so the
# shape matched is `A3 imm32 C3 58 [pop r32 | popal | popfd]... C3` over all
# of .text. A looser `ret; pop r32; ret` matches compiled epilogues and bytes
# inside other instructions here (`mov eax, ebx` is 8B C3). The sites fall in
# two clusters, 0x00475Bxx-0x0047BFxx and 0x004EF6xx-0x004F6xxx: two copies
# of the span code (docs/bringup.md, "the rasterizer").
UNWIND_SET = '{ extern uint32_t sco_unwind; sco_unwind = 1; } /* run_lift.py: returns to the caller\'s caller */'
UNWIND_CHECK = ('{ extern uint32_t sco_unwind; if (sco_unwind) { sco_unwind = 0; return; } } '
                '/* run_lift.py: the callee returned to our caller */')


# ---- fixes: C emitted after one instruction ----------------------------------
# The lift stays faithful; a patch is a line of C after an instruction this
# file names, for what the lift cannot do by itself (a divide the game expects
# to fault, as Streets' 0x004C7993). --original runs the shipping code
# unpatched, so it remains the reference. None are known for SimCopter yet.
PATCHES = {}


def apply_patches(body, patches):
    """Add each patch's C after the lines that lift its instruction. Some
    instructions lift to a block over several lines (idiv's `{ ... }`): the
    patch goes after the block closes. (Tiberian Sun's.)"""
    for va, c in patches.items():
        k = body.find('/* 0x%08X:' % va)
        if k != -1:
            s = body.rfind('\n', 0, k) + 1
            e = body.index('\n', k)
            depth = body[s:e].count('{') - body[s:e].count('}')
            while depth > 0:
                s, e = e + 1, body.index('\n', e + 1)
                depth += body[s:e].count('{') - body[s:e].count('}')
            body = body[:e + 1] + '    ' + c + '\n' + body[e + 1:]
    return body


# Code to compile without optimisation: [lo, hi) ranges whose bodies are
# wrapped in #pragma optimize("", off), for a chunk the optimiser chokes on
# (Streets had one start-up routine that took 40 minutes). None yet.
COLD = []


def unwind_pops(code, cs):
    """Addresses of the `pop eax` in `mov [abs32], eax; ret; pop eax;
    [pop r32 | popal | popfd]...; ret`."""
    out = []
    for i in range(5, len(code) - 2):
        if code[i] != 0xC3 or code[i + 1] != 0x58 or code[i - 5] != 0xA3:
            continue
        j = i + 2
        while j < len(code) and (0x58 <= code[j] <= 0x5F or code[j] in (0x61, 0x9D)):
            j += 1
        if j < len(code) and code[j] == 0xC3:
            out.append(cs + i + 1)
    return out


def _check_unwind_pops():
    """The shapes run_lift relies on, and the ones it must not take."""
    m = b'\xa3\x00\x00\x50\x00'                                   # mov [0x500000], eax
    assert unwind_pops(m + b'\xc3\x58\xc3', 0) == [6]                # ret; pop eax; ret
    assert unwind_pops(m + b'\xc3\x58\x61\xc3', 0) == [6]            # ret; pop eax; popal; ret
    assert unwind_pops(b'\x8b\xc3\x5f\x5e\x5b\xc3', 0) == []        # mov eax, ebx; an epilogue
    assert unwind_pops(b'\x7c\xc3\x5f\x5e\xc3', 0) == []            # a ret, then an epilogue
    assert unwind_pops(m + b'\xc3\x58\x90\xc3', 0) == []            # anything else in between


def after_line(body, k, c):
    """body with c on a line of its own after the line holding offset k."""
    e = body.index('\n', k)
    return body[:e + 1] + '    ' + c + '\n' + body[e + 1:]


def lift(path, catalog, md, out, split):
    info = analyze_pe(path)
    iat = build_iat_map(info)
    cs, ce = info.code_start, info.code_end
    cat = json.load(open(catalog))
    byaddr = {f['address']: f for f in cat['functions'] if cs <= f['address'] < ce}
    print('[*] %s: base=0x%08X code=0x%08X-0x%08X IAT=%d catalog=%d'
          % (os.path.basename(path), info.image_base, cs, ce, len(iat), len(byaddr)))

    text = [s for s in info.sections if s.name == '.text'][0]
    code = open(path, 'rb').read()[text.raw_offset:text.raw_offset + text.raw_size]
    # Known targets become RECOMP_CALL; anything else a decoded `call` names
    # becomes RECOMP_ICALL, which reports at run time instead of failing the build.
    lifter = Lifter(iat_map=iat, lifted=set(byaddr))
    starts = {a for a in byaddr if byaddr[a].get('entry_kind') != 'alias'}
    starts -= find_splits(md, code, cs, ce, starts)
    # The routine holding each unwinding pop: the nearest start at or below it.
    _check_unwind_pops()
    pops = unwind_pops(code, cs)
    ordered = sorted(starts)
    unwinders = {'sub_%08X' % max(a for a in ordered if a <= p) for p in pops}
    call_re = re.compile(r'RECOMP_CALL\((%s)\);' % '|'.join(sorted(unwinders)))
    print('[*]   %d routines return to their caller\'s caller (%d pops)' % (len(unwinders), len(pops)))

    stats = {'lifted': 0, 'errors': 0, 'no_terminator': 0, 'files': 0}
    entries, chunk = [], []
    # A direct branch that leaves a body to an address nothing catalogued is a
    # tail call the catalog missed or a jump into a neighbour's shared code.
    # Either way the target has to be dispatchable, so each round's outside
    # targets become entries of the next.
    todo, added = sorted(byaddr), 0
    while todo:
        outside = set()
        for addr in todo:
            name = 'sub_%08X' % addr
            reached, behind = set(), set()
            end, clean = true_extent(md, code, cs, addr, min(addr + EXTENT_REACH, ce), starts,
                                     reached=reached, behind=behind)
            outside |= {t for t in behind if t not in reached}
            stats['no_terminator'] += not clean
            try:
                lo = min(reached) if reached else addr   # a chunk can sit below the entry
                insns, leaders = (linear_disassemble_function(md, code, cs, lo, end, reached=reached)
                                  if end > addr else ([], None))
                if leaders is not None:
                    leaders.add(addr)
                body = (lift_function_linear(lifter, name, insns, leaders, addr) if insns
                        else 'void %s(void) { }\n' % name)
                for p in pops:
                    k = body.find('/* 0x%08X: pop' % p)
                    if k != -1:
                        body = after_line(body, k, UNWIND_SET)
                for m in reversed(list(call_re.finditer(body))):
                    body = after_line(body, m.start(), UNWIND_CHECK)
                body = apply_patches(body, PATCHES)
                if any(lo <= addr < hi for lo, hi in COLD):
                    body = ('#ifdef _MSC_VER\n#pragma optimize("", off)\n#endif\n' + body.rstrip('\n') +
                            '\n#ifdef _MSC_VER\n#pragma optimize("", on)\n#endif\n')
            except Exception as e:                  # noqa: BLE001 -- counted, not hidden
                body = '/* ERROR %s: %s */\nvoid %s(void) { }\n' % (name, e, name)
                stats['errors'] += 1
            chunk.append((body, addr, name))
            entries.append((addr, name))
            if len(chunk) >= split:
                write_chunk(out, stats['files'], chunk)
                stats['files'] += 1
                chunk = []
        todo = sorted(t for t in outside if cs <= t < ce and t not in byaddr)
        for t in todo:
            byaddr[t] = {'address': t}
        added += len(todo)
    if chunk:
        write_chunk(out, stats['files'], chunk)
        stats['files'] += 1
    stats['lifted'] = len(byaddr)
    print('[*]   %d outside branch targets added' % added)
    return entries, info.image_base + info.entry_point_rva, stats


def sync(new, out):
    """Move new's files over out's where their text differs, drop out's chunks
    the new lift no longer has (a smaller lift must not leave stale ones).
    Returns how many files changed."""
    os.makedirs(out, exist_ok=True)
    names = set(os.listdir(new))
    changed = 0
    for fn in names:
        src, dst = os.path.join(new, fn), os.path.join(out, fn)
        if not (os.path.exists(dst) and filecmp.cmp(src, dst, shallow=False)):
            os.replace(src, dst)
            changed += 1
    for fn in set(os.listdir(out)) - names:
        if fn.startswith('recomp_') and fn.endswith('.c'):
            os.remove(os.path.join(out, fn))
            changed += 1
    shutil.rmtree(new)
    return changed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=GAME, help='folder holding SimCopter.exe')
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--split', type=int, default=400, help='functions per .c file')
    args = ap.parse_args()
    if not os.path.exists(CATALOG):
        sys.exit('no catalog at %s -- run tools/catalog.py first (README, Step by step)' % CATALOG)

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    # The lift goes to a staging folder, and only files whose text changed
    # replace the old ones: the build is 6M lines, and a lifter fix that
    # touches three chunks should recompile three, not all of them.
    final, args.out = args.out, args.out + '.new'
    shutil.rmtree(args.out, ignore_errors=True)
    os.makedirs(args.out)

    t0 = time.time()
    entries, oep, stats = lift(os.path.join(args.game, EXE), CATALOG, md, args.out, args.split)

    with open(os.path.join(args.out, 'recomp_funcs.h'), 'w', newline='\n') as f:
        f.write('/* SimCopter - AUTO-GENERATED by run_lift.py */\n#pragma once\n#include <stdint.h>\n\n')
        for a, n in entries:
            f.write('void %s(void);\n' % n)
    with open(os.path.join(args.out, 'recomp_dispatch.c'), 'w', newline='\n') as f:
        f.write('/* SimCopter - AUTO-GENERATED by run_lift.py */\n'
                '#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n'
                'const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n')
        for a, n in sorted(entries):
            f.write('    { 0x%08Xu, %s },\n' % (a, n))
        f.write('};\nconst uint32_t recomp_dispatch_count = %d;\n'
                'const uint32_t sco_entry_va = 0x%08Xu;\n' % (len(entries), oep))

    stats['lines'] = sum(sum(1 for _ in open(os.path.join(args.out, fn), encoding='utf-8',
                                             errors='replace'))
                         for fn in os.listdir(args.out))
    stats['changed'] = sync(args.out, final)
    json.dump(stats, open(STATS, 'w'), indent=1)
    print('=' * 60)
    print('  lifted %(lifted)d   errors %(errors)d   no terminator %(no_terminator)d' % stats)
    print('  %s lines of C in %d files (%d changed), %.1fs' % (format(stats['lines'], ','), stats['files'],
                                                               stats['changed'], time.time() - t0))
    print('=' * 60)


if __name__ == '__main__':
    main()
