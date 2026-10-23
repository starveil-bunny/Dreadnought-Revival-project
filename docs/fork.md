# fork

upstream: darkace1998/Dreadnought-Revival-project
server hostnames/IPs in this tree are placeholders (your-server.example.org, 203.0.113.x). set your own:
- `gateway/main.go` cuteFrontHost
- `dn-launcher/servers_windows.go` bunnyServerAddress

## install (windows)

`dn-install.exe`: welcome > game folder > install > finish

- game folder: steam default, else browse. Next disabled until the folder has DreadGame
- `DreadnoughtLauncher.exe` -> `DreadnoughtLauncher.exe.old` (first run only)
- writes `DreadnoughtLauncher.exe` (dn-launcher), `ca.crt`, `dn-launcher.json` if bundled, `dn-uninstall.exe`
- CA -> CurrentUser\Root (certutil -user). no admin. windows confirm prompt.
- steam verify restores the stock launcher. rerun.
- uninstall: `dn-uninstall.exe` or Apps. restores the .old. CA stays: certmgr.msc

linux: `dn-launcher/linux/dn-launcher-linux.sh`

## build installer

```
scripts/gen-certs.sh                       # certs/ca.crt, once
SERVER=host [WEB_PORT=8443] [CONFIG=dn-launcher.json] scripts/build-installer.sh
-> run/dn-install.exe
```

needs makensis, or the NSIS zip under wine (`NSIS_DIR`, default ~/.cache/nsis-dl/nsis-3.10)

## battle server mod

```
battle-server-mod/build-linux.sh
```

off switches: env var or `<name>.txt` beside the server exe
- `DN_HOST_NO_HAVOC_DIRECTOR` / `dn_host_no_havoc_director.txt`

## havoc

- waves, upgrade panel, wave modifiers (shown on the panel before the wave), countdown, ready-up, end screen rewards
- `/difficulty recruit|veteran|legendary`

## launcher

- Fleet page: ships, loadout A/B, saved loadouts, fit, cosmetics, 3D view
- live battles: VIEW
- backgrounds: map loading screens

## admin dashboard

- Bosses: custom bosses (tier/maker/class tree, editor, mesh preview)
- Hunters: hunter pool
- Waves: counts/caps for /swarm /squad /wave /hunters

## chat

`DN_ADMINS=<player id>,...` for admin commands. `/help` lists.

```
/match <havoc|tdm|ffa|pod|turbo|onslaught|conquest|te|pve|pg> [map] | off
/say <text>
/turbo on|off
/npc normal|hard|brutal
/bots 1|2|3|4
/enemy <ShipId>|off|list
/spawn <ShipId>|boss [1-8] [x2-x10]
/boss [<id>|list|ShipId|random|stock] [2-10]
/clone <player|me> [ship] [1-10]
/team <player> <1|2>
/shake 1-10
/flash white|red|pink|black|gold|blue
/slowmo 0.2-1
/kill me|all|<player>
/meteor [1-40] [distance] [team]
/swarm [1-60] [team]
/squad [1-20] [team]
/wave [x2-x10] | skip|1-50
/hunters [1-12] [team]
/fighters [team]
/nuke
/difficulty recruit|veteran|legendary
/props <object> [filter]
/host <raw command>
```

ffa: `/match ffa`. TDM, one team per player, no bots, ends at kill limit.

## dev

- `scripts/dn` helper
- `scripts/cycle.py`, `scripts/live.py`: headless client runs under wine
