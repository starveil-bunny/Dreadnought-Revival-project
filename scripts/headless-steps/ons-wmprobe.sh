#!/usr/bin/env bash
# Companion: weapon-manager struct/function layouts from the live player
# ship, then /meteor 12 (herald weapon logs). For the client-visible herald
# fire path (weapon groups replicate, server-only projectiles do not).
L=$(dirname "$DREADGAME_EXE")/dn_host_loadout.log
off=$(stat -c %s "$L"); ino=$(stat -c %i "$L")
since() {
  local o=$off
  if [ "$(stat -c %i "$L")" != "$ino" ] || [ "$(stat -c %s "$L")" -lt "$off" ]; then o=1; fi
  tail -c +"$o" "$L"
}
for _ in $(seq 900); do since | grep -aq 'ship physics: movement component .* remote player' && break; sleep 1; done
since | grep -aq 'ship physics: movement component .* remote player' || { echo "timed out waiting for spawn" >&2; exit 1; }
sleep 40
for p in m_targetInfo m_repTargetInfo m_weaponGroupFireInfos m_weaponGroups m_weaponGroupImp; do
  dn cmd "/host props pawn.m_weaponManager struct:$p"; sleep 2
done
dn cmd "/host props pawn.m_weaponManager m_"; sleep 2
for f in ServerStartFireActiveWeaponGroup ServerStopFireActiveWeaponGroup ServerSetTargetInfo ServerSetActiveTargetInfoReliable ServerActivateWeaponGroup; do
  dn cmd "/host probefn Default__YWeaponManagerComponent $f"; sleep 2
done
sleep 30
echo "== $(date +%T) /meteor 12"; dn cmd "/meteor 12"
sleep 60
echo "== $(date +%T) log"
since | grep -a 'props\|probefn\|  \|meteor\|herald' | tail -250
