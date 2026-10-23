#!/usr/bin/env python3
"""Ghidra pseudo-C for RVAs: dn rva decompile <rva> [<rva> ...] [--asm]

Wraps analyzeHeadless + DumpFuncs.java against one pre-analysed project, so
each call costs only Ghidra start-up (~20-40 s), not a re-analysis. Prints the
decompiled C per function; --asm adds DumpFuncs' disassembly (field offsets
survive there when the decompiler invents locals).

One-time setup (hours for this 48 MB exe; run in the background):
  dn rva decompile --import
Project: $DN_GHIDRA_PROJECT or ~/ghidra_projects/dreadnought (name DreadGame; Ghidra
rejects any path element starting with ".", so not ~/.cache).
Ghidra: $GHIDRA_HOME or /opt/ghidra (Arch package `ghidra`).
"""
import glob
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GHIDRA = os.environ.get("GHIDRA_HOME", "/opt/ghidra")
HEADLESS = os.path.join(GHIDRA, "support", "analyzeHeadless")
PROJ = os.environ.get("DN_GHIDRA_PROJECT", os.path.expanduser("~/ghidra_projects/dreadnought"))
NAME = "DreadGame"
EXE = os.environ.get("DREADGAME_EXE", "")
# Analysis of the full exe needs a big heap; leave room for a running client.
MAXMEM = os.environ.get("DN_GHIDRA_MAXMEM", "6G")


def run(args):
    env = dict(os.environ, MAXMEM=MAXMEM)
    return subprocess.run([HEADLESS, PROJ, NAME] + args, env=env, capture_output=True, text=True)


def main():
    argv = sys.argv[1:]
    if not os.path.exists(HEADLESS):
        sys.exit(f"no Ghidra at {HEADLESS} (sudo pacman -S ghidra, or set GHIDRA_HOME)")
    if "--import" in argv:
        os.makedirs(PROJ, exist_ok=True)
        # No -analysisTimeoutPerFile: its value is seconds and 0 means "time out
        # immediately" (import saved, analysis skipped), not "unlimited".
        if glob.glob(os.path.join(PROJ, NAME + ".gpr")):
            # Already imported: analyse the saved program in place.
            r = run(["-process", os.path.basename(EXE)])
        else:
            r = run(["-import", EXE, "-overwrite"])
        tail = (r.stdout + r.stderr).strip().splitlines()[-15:]
        print("\n".join(tail))
        sys.exit(r.returncode)
    asm = "--asm" in argv
    rvas = [a for a in argv if not a.startswith("--")]
    if not rvas:
        sys.exit(__doc__)
    if not glob.glob(os.path.join(PROJ, NAME + ".gpr")):
        sys.exit(f"no analysed project at {PROJ}; run: dn rva decompile --import")
    with tempfile.TemporaryDirectory() as out:
        os.environ["GHIDRA_DUMP_DIR"] = out
        r = run(["-process", os.path.basename(EXE), "-noanalysis", "-readOnly",
                 "-scriptPath", HERE, "-postScript", "DumpFuncs"] + rvas)
        files = sorted(glob.glob(os.path.join(out, "dump_*.txt")))
        if not files:
            print("\n".join((r.stdout + r.stderr).strip().splitlines()[-20:]))
            sys.exit(1)
        for f in files:
            text = open(f).read()
            if not asm:
                text = text.split("---- DISASSEMBLY ----")[0].rstrip() + "\n"
            print(text)


if __name__ == "__main__":
    main()
