#!/usr/bin/env python3
"""Batch cross-references with a cached index: one call, many targets.

    dn rva xrefs m_isActive OnSetUpCapturePoints 0x3E10880 ...

Each argument is a string literal (UTF-16LE and ASCII, every copy in the file)
or a hex RVA (a global / function). Prints every RIP-relative reference
(lea/mov/cmp/call/jmp rel32) with the enclosing function entry (.pdata primary).

Why: strxref.py re-indexes the 48 MB .text on every run (~40 s), and lookups
came one string per call. The index here is built once per exe build
(~/.cache/dn-rva/, keyed by size+mtime) and reused.
"""
import os, pickle, re, struct, sys
from collections import defaultdict
from pe import PE

def build_index(p):
    d = p.data
    lo, hi = p.text_off, p.text_end
    idx = defaultdict(list)
    # [REX] opcode modrm(rip) disp32: lea/mov/cmp/test-ish forms
    rip = re.compile(rb'[\x48\x4c\x49\x4d]?[\x8d\x8b\x89\x3b\x39\x80\x83\xc7\xff\x0f\x38\x88\x8a\x85\x81\xc6]'
                     rb'[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', re.S)
    seg = d[lo:hi]
    for m in rip.finditer(seg):
        o = m.end()
        if o + 4 > len(seg):
            continue
        disp = struct.unpack_from('<i', seg, o)[0]
        nxt = o + 4
        op = seg[m.start():m.start() + 2]
        if op[-1:] in (b'\x80', b'\x83', b'\xc6') or (len(op) == 2 and op[1:2] in (b'\x80', b'\x83', b'\xc6')):
            nxt += 1  # imm8 follows
        elif op[-1:] in (b'\xc7', b'\x81') or (len(op) == 2 and op[1:2] in (b'\xc7', b'\x81')):
            nxt += 4  # imm32
        t = p.text_off2rva(lo + nxt) + disp
        idx[t].append(p.text_off2rva(lo + m.start()))
    # call/jmp rel32
    for m in re.finditer(rb'[\xe8\xe9]', seg):
        o = m.start()
        if o + 5 > len(seg):
            continue
        t = p.text_off2rva(lo + o + 5) + struct.unpack_from('<i', seg, o + 1)[0]
        if 0 < t < 0x6000000 and p.is_entry(t):
            idx[t].append(p.text_off2rva(lo + o))
    return dict(idx)

def load_index(p, path):
    st = os.stat(path)
    cache = os.path.expanduser(f'~/.cache/dn-rva/xref-{st.st_size}-{int(st.st_mtime)}.pkl')
    if os.path.exists(cache):
        with open(cache, 'rb') as f:
            return pickle.load(f)
    print('building xref index (once per exe build)...', file=sys.stderr)
    idx = build_index(p)
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    with open(cache, 'wb') as f:
        pickle.dump(idx, f)
    return idx

def literal_rvas(p, s):
    out = []
    for enc, tag in ((s.encode('utf-16le') + b'\0\0', 'u16'), (s.encode() + b'\0', 'ascii')):
        for m in re.finditer(re.escape(enc), p.data):
            o = m.start()
            if tag == 'u16' and o % 2:
                continue
            # whole literal only: preceded by a terminator
            prev = p.data[o - 2:o] if tag == 'u16' else p.data[o - 1:o]
            if prev.strip(b'\0'):
                continue
            try:
                out.append((p.off2rva(o), tag))
            except Exception:
                pass
    return out

def func_of(p, rva):
    e = p.enclosing(rva)
    if not e:
        return '?(no unwind)'
    pr = p.primary(e[0])
    return hex(pr[0]) if pr else hex(e[0])

def main(args):
    if not args:
        print(__doc__); return
    path = os.environ.get('DREADGAME_EXE')
    p = PE(path) if path else PE()
    idx = load_index(p, path or p.path)
    for a in args:
        if re.fullmatch(r'0x[0-9a-fA-F]+', a):
            targets = [(int(a, 16), 'rva')]
        else:
            targets = literal_rvas(p, a)
            if not targets:
                print(f'== {a}: literal not found'); continue
        for t, tag in targets:
            refs = idx.get(t, [])
            print(f'== {a} [{tag} {t:#x}] {len(refs)} ref(s)')
            for r in refs[:40]:
                print(f'   {r:#x} in func {func_of(p, r)}')

if __name__ == '__main__':
    main(sys.argv[1:])
