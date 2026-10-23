"""UFunction name -> native implementation RVA, from the reflection tables.

The build's static-init code registers every native `UFUNCTION` through one
helper, in a fixed shape (first hit: exe+0x627419):

    lea r8,  [rip+X]     ; X  = the implementation address
    lea rdx, [rip+Y]     ; Y  = the function name (plain ASCII C string)
    call thunk           ; registers (class, name, impl, flags)

so scanning for `lea r8,[rip+..]` immediately followed by `lea rdx,[rip+..]`
recovers the whole table -- 5,443 pairs in this binary.

This is the fastest way to answer "is there a native entry for this Blueprint
name?", and the *absence* of a name is just as useful: the AI-fire survey found
`StartFire`, `SetFireAtActor`, `TriggerAbilityOne..Four` missing => those legs
are Blueprint graphs with no native entry point, so they can only be reached
through `ProcessEvent` by name.

    python nativefns.py            # summary + /tmp/werx/nativefns.json
    python nativefns.py Name ...   # print RVAs for specific names

Caveats, both real:
  - The value is the address the registrar was handed. It is usually the
    function itself, but the build also emits thin exec stubs of the form
    `mov rax,[rdx+0x20]; ...; jmp IMPL` (frame-advance + tail call) -- follow a
    leading `jmp` before treating the value as the implementation.
  - Names are recovered from the string the LEA points at. UE sometimes
    references a literal from inside it; the walk-back to the string's first
    character handles the normal case, but a name that comes out truncated
    ("astFiredWeapon") means the reference was mid-literal -- search the
    substring instead.

Run it with `-I` on a machine where the current directory is untrusted; it
imports only `pe.py` from its own directory.
"""
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE  # noqa: E402

OUT = "/tmp/werx/nativefns.json"


def off_of(rva):
    o = pe.rva2off(rva)
    return o if o is not None and 0 <= o < len(data) else None


def name_at(rva):
    o = off_of(rva)
    if o is None:
        return None
    # UE may reference the literal from inside it; walk back to its first char.
    s = o
    while s > 0 and 32 <= data[s - 1] < 127 and o - s < 96:
        s -= 1
    b = data[s:s + 160]
    m = re.match(rb"[ -~]{3,90}", b)
    if m and b"\\" not in m.group(0):
        s = m.group(0).decode()
    else:
        m = re.match(rb"(?:[\x20-\x7e]\x00){3,80}", b)
        if not m:
            return None
        s = m.group(0).decode("utf-16le")
    return s if re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", s) else None


pe = PE()
data = pe.data
seen = {}
n_sites = 0
o = pe.text_off
end = pe.text_end
while o < end - 16:
    if data[o] == 0x4C and data[o + 1] == 0x8D and data[o + 2] == 0x05:
        n_sites += 1
        # A rip-relative displacement is in address space: add it to the RVA of
        # the *next instruction*, never to a file offset (the .text and .rdata
        # file/address deltas differ, so file-offset arithmetic lands 0xA00 off
        # and silently reads a neighbouring name).
        impl = pe.off2rva(o + 7) + struct.unpack_from("<i", data, o + 3)[0]
        j = o + 7
        for _ in range(10):
            if data[j] == 0x48 and data[j + 1] == 0x8D and data[j + 2] == 0x15:
                nm = name_at(pe.off2rva(j + 7) + struct.unpack_from("<i", data, j + 3)[0])
                if nm and impl is not None:
                    seen.setdefault(nm, impl)
                break
            j += 1
        o += 7
        continue
    o += 1

if len(sys.argv) > 1:
    for k in sys.argv[1:]:
        print(f"{k:36} -> {hex(seen[k]) if k in seen else '(no native entry)'}")
else:
    print(f"lea-r8 sites {n_sites}, names recovered {len(seen)}")
    print("sample:", [(k, hex(v)) for k, v in list(seen.items())[:5]])
    try:
        json.dump(seen, open(OUT, "w"))
        print("wrote", OUT)
    except OSError as e:
        print("could not write", OUT, e)
