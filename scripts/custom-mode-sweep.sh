#!/usr/bin/env bash
# One headless match per custom mode (mmogbrain/custom_modes.go), pinned with
# {"mode":<base>,"custom":<id>}; prints the match's "custom:" host lines
# (kill tallies folded into a count) and any faults.
# Usage: [SECS=300] scripts/custom-mode-sweep.sh [id...]   (default: all)
L=$(dirname "${DREADGAME_EXE:-$HOME/.local/share/Steam/steamapps/common/Dreadnought/DreadGame/DreadGame/Binaries/Win64/x}")/dn_host_loadout.log
ids=("$@")
[ ${#ids[@]} -eq 0 ] && ids=(ffa ffa_pve teams3 teams4 secret lifedrain meteors fake_havoc)
for id in "${ids[@]}"; do
  base=TDM; [ "$id" = fake_havoc ] && base=Onslaught
  echo "=== $(date +%T) $id ($base)"
  off=$(stat -c %s "$L" 2>/dev/null || echo 0)
  dn headless "cm-$id" --fresh --pin "{\"mode\":\"$base\",\"custom\":\"$id\"}" \
    --steps scripts/headless-steps/spawn-fight.txt --secs "${SECS:-300}" 2>&1 | grep -E "VERDICT"
  # the host may start a fresh log for the match: read it whole if it shrank
  [ "$(stat -c %s "$L")" -lt "$off" ] && off=0
  tail -c +"$((off + 1))" "$L" | tr -d '\0' > /tmp/cm-sweep-$id.log
  grep -cE "custom: side [0-9]+ has" /tmp/cm-sweep-$id.log | sed 's/^/  kill tallies: /'
  grep -E "custom:|fault" /tmp/cm-sweep-$id.log | grep -vE "has [0-9]+ kill" | cut -c32-170 | sort | uniq -c | sort -rn | head -12
done
echo "=== $(date +%T) done"
