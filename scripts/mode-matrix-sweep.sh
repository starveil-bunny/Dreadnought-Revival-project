#!/usr/bin/env bash
# One headless match per mode, each running every fun chat command
# (headless-steps/mode-matrix.sh). Bundles: run/cycles/<ts>-matrix-<mode>/.
# Usage: [MATRIX_CMDS="/boss 1;/nuke"] scripts/mode-matrix-sweep.sh [mode...]   (default: all below)
# Each match ends when the command script does (~3 min boot + ~10 s per command).
modes=("$@")
[ ${#modes[@]} -eq 0 ] && modes=(Onslaught PodTDM TDM TER TE PVE Havoc)  # matchmaker mode names (/match maps pod->PodTDM, conquest->TER)
for m in "${modes[@]}"; do
  echo "=== $(date +%T) $m"
  dn headless "matrix-$m" --fresh --pin "$m" --long --companion scripts/headless-steps/mode-matrix.sh --end-with-companion \
    --steps scripts/headless-steps/mode-matrix.txt 2>&1 | tail -4
done
echo "=== $(date +%T) done"
