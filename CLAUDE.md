# CLAUDE.md

Guidance for Claude Code (claude.ai/code) in this repository.

## What this is

A community private server for the discontinued game **Dreadnought** (UE4 4.13.1, Steam App 835860): run the **unmodified game client** against an emulated backend; the only thing a player installs is the replacement launcher (`dn-launcher/`).

**The line is drawn at the client, not at the binary.**

- **Client — keep changes minimal.** Players install it; stock. The launcher replacement signs in, trusts our certificate, redirects hostnames — that is the intended extent. Anything beyond it needs real justification and is the last option, not the first.
- **Battle server — modding is allowed.** Same exe run headless by an operator, never a player; `battle-server-mod/` is ours and may grow.

Why this split (2026-08-14, replacing a narrower rule): the battle server is missing whole categories of state because it performs no login — e.g. the orbit gate reads `AYPlayerReplicationInfo::m_highestFleetUnlocked` (`+0x948`), real backend data the host can't have. The old rule forbade writing that byte as an "orbit flag"; it isn't one.

Guidance that remains (as judgment, not gate):
- Prefer **supplying data the engine would have had** over **fabricating a value that satisfies a check**. Registering precast loadouts the host never received = first kind; writing an arbitrary tier to pass a comparison = second (the second produced a pawn that could shoot but not move — `AGENT-CHAT C32.4`).
- Keep host mods **optional at runtime** (`battle-server-mod/README.md`) so a match can still be diagnosed with them off.

A Go workspace (`go.work`, Go 1.24+) of independent modules under `github.com/darkace1998/Dreadnought-Revival-project/<service>`.

## Start here

Five skills carry what is expensive to re-derive — load the relevant one BEFORE working, not after a failed attempt:

| Skill | Use it when |
| --- | --- |
| `dreadnought-stack` | starting a session: what runs where, build/deploy/restart, log paths, operator test cycle |
| `dreadnought-mmog-responses` | adding/debugging any `YA_*` response; the client "ignores" a payload |
| `dreadnought-rva` | "where does the client do X?"; verifying an RVA before trusting it |
| `dreadnought-hooks` | adding a MinHook target in `battle-server-mod/` |
| `dreadnought-verify` | before claiming a fix works |

`AGENT-CHAT.md` is the running log (S## ours, C## client-side project). Read the last few entries first — it records what was tried, what worked, and what was **retracted**, often the more useful half.

## The one rule: never invent data

Everything the server sends must be traceable to the client's own data tables (`data/`) or the client binary. Invented-but-plausible values have repeatedly cost days — they look right and get built on. If a value cannot be determined, mark it `// GUESS:` with what evidence is missing. Debugging order: read the client's log → find the string in the binary → read the parser → only then hypothesize about the payload. Full working rules in `CONTRIBUTING.md` — read it before touching anything protocol-related.

## Current state (2026-10-05)

Working end to end, verified against a live client: login, hangar, inventory/purchases, tech tree (research+buy), fleet/loadout data, matchmaking, spawn, ship selection, orbit teleport, a full proving-ground match (bots, physics, own fitted loadout on any tier, victory screen), daily contracts.

**The recurring pattern:** the battle server is the client exe, and the original backend fed it from a separate SERVER build not in it — each gap is server-side code the host lacks, filled by `battle-server-mod` or mmogbrain, never by patching the client (player loadouts via `?DNPID=` travel URL + loopback `/battle/loadout`; victory screen via unsolicited `ClientSetTopPlayerMatchStats`; hangar-entry quest-cycle recursion fixed by a real `ContractTable` payload, not a client change).

Open problems and detail live in `issues.md` + GitHub issues (#67 scores, #66/#68/#71 placeholder values — operator-approved, do not chase exact values, statistics overflow, etc.). Recent work: see `AGENT-CHAT.md` tail and `RESUME.md`.

## Commands

```bash
bash scripts/setup.sh                # build all into run/ (+ certs/secrets on first run)
bash scripts/start-services.sh       # stack up, logs in run/<service>.log
bash scripts/stop-services.sh
curl http://127.0.0.1:8081/health    # health checks on 8081-8085
cd battle-server-mod && bash build-linux.sh   # host mod DLL, clang cross-build (smoke + deploy: dreadnought-stack skill)
```

### Tests and lint — per-module, never workspace root

`go test ./...` from the repo root fails ("directory prefix . does not contain modules"). Run inside each module:

```bash
cd mmogbrain && go test ./...           # and: go vet ./...; golangci-lint (clean not expected; don't add to backlog)
cd mmogbrain && go test -run TestPayloadSizesVerify ./...
cd dn-dedicated && GOWORK=off go build ./... && GOWORK=off go test ./...
```

`TestPayloadSizesVerify` pins exact byte sizes of every mmogbrain response — a deliberate tripwire. When a change moves a size, don't just update the number: add a comment line explaining what grew/shrank and why. Tests encode client behaviour, so a failing test may itself be wrong (it has twice faithfully defended a disproved theory) — decide which side before "fixing", and write down why if you change the assertion.

## Tooling — reach for `dn` before writing shell

`dn` (repo `scripts/dn`, symlinked to `~/.local/bin/dn`) wraps the shell
incantations this project repeats. Prefer it over hand-rolled commands: output
is bounded and identical every run, so it costs the same context no matter which
model is driving. `DREADGAME_EXE` is exported in `~/.bashrc`, so the RVA scripts
need no setup line.

```bash
dn env                        # resolved paths; is the exe present?
dn status                     # services + health + pids + log sizes
dn logs --list                # log targets -> real paths (traps encoded below)
dn logs mmog --last 40        # per-request trail (repo-root mmogbrain.log)
dn logs client --grep crash --last 30
dn grep 'ACCESS_VIOLATION'    # every service log, counted + sampled
dn build mmogbrain            # per-module go test + vet
dn restart                    # stop->start->health->live DN_ switches from /proc
dn rva disasm 0x2A6CED0       # RVA toolkit with DREADGAME_EXE preset
dn sql "select * from users limit 5" --db auth
dn deploy                     # build changed services into run/, skip identical, restart
dn headless LABEL [--steps F] # unattended client run -> run/cycles/<id>/ bundle + VERDICT
dn live start LABEL           # interactive client: click/key/hold/shot/wait/settle/log, stop
dn cycles                     # recent bundles; dn headless-stop frees the display + sweeps switch files a killed run left
```

**Testing a client flow: drive it live, replay it headless.** `dn live` keeps
one client running between calls; each action prints a screenshot path plus the
notable new client-log lines, so you look, decide, act — no start-to-close
reruns. `dn live stop` writes `steps.txt`, which `dn headless --steps` replays
unattended (`scripts/headless-steps/pg-battle.txt`: hangar -> Proving Grounds
-> spawn -> flight, ~5 min). A test that needs its own match uses
`--fresh` (a running battle swallows the join; a stale one is cleared, one with
anyone online refuses), `--no-npcs` (no Onslaught creeps, no fill bots — the
host switch files exist only for the run), `--switch-off NAME` (moves the
operator's `dn_host_NAME.txt` aside for one run, e.g. `no_onslaught_creeps`;
restored on exit, by the watchdog, or by `dn headless-stop`), `--pin MODE:MAP`, and
`--companion SH` for `dn cmd` on log cues (`ons-boss-meteor.sh`; it gets `DN_CYCLE_DIR` and `DN_CLIENT_DISPLAY`, so `ons-meteor.sh` records `meteor.mp4` into the bundle). With
`--steps` the time cap is estimated from the file. Display/audio/crash traps are in the
`dreadnought-verify` skill. Headless runs are ours; the operator still gates
every real-client test cycle.

Two habits it encodes, both learned the hard way: `run/client.log` is ~70 MB and
must never be dumped unbounded, and a `DN_` switch confirmed in `secrets.env`
proves nothing — `dn restart` reads `/proc/<pid>/environ` instead.

## Architecture

```
[Unmodified Windows client]  (hosts-file redirect + our CA trusted)
  → HTTPS :443  → gateway (TLS termination, routes by Host header)
      ├─ profile-api.*   → auth-server   :8081  (registration, login, JWT — auth.db)
      ├─ legacyapi.*     → legacy-api    :8082  (profiles, inventory, match history — legacy.db)
      └─ masterserver.*  → master-server :8084  (server registry, browser — master.db)
  → HTTPS :65443 → mmogbrain "gateway" socket  (Greybox web-services: catalog, inventory, session)
  → TLS   :48843 → mmogbrain Firmament socket  (social/presence + binary YMmog protocol)
  → UDP :7777-7877 → DreadGame-Win64-Shipping.exe  (one Wine process per match)
```

Naming trap: the **`gateway` service** is the reverse proxy on :443; **`GATEWAY_ADDR` (:65443)** is a socket *inside mmogbrain* — two different things.

There is no dedicated-server build; the battle server is the ordinary client exe launched headless (`"<map>?listen" -server -nullrhi -unattended`; no `-dedicatedserver` switch exists in the binary). `admin-cli/`, `dn-launcher/` (Windows, cross-compiled), `shared/` (db, logging, middleware, `dreadgameconfig` loader).

### mmogbrain — the complex one

Implements the proprietary binary YMmog protocol on the Firmament TLS socket. Must-knows:

- Use the encoding helpers (`appendMmogStringField`, …), never raw bytes.
- Player PIDs on the binary protocol are UUIDs **with hyphens stripped**.
- Firmament auth success is deliberately delayed until MMOG `YA_PlayerGet` confirms player data is ready (real client race; pending-auth maps).
- `PLAYERS_PER_MATCH` defaults to 1 — every player gets a private match and can never meet another. Use 2+ for PvP; PvP modes have no bots (`StartCombat` fires only under Training Match).
- Debug switches (`DN_TECHTREE_LIMIT`, `DN_ANSWER_DAILY_CONTRACTS`, `DN_NO_DEFER_PLAYER_FLEETS`, `DATA_DIR`) documented in README.

### Conventions shared by all services

Handlers wired in `main.go` with gorilla/mux; SQLite via `shared/db` (WAL, `MaxOpenConns=1`, sequential migrations); JWT HS256 (`sub`, `username`, `realm="dreadnought.pc-us"`, `aud="dreadnought"|"launcher"`; `JWT_SECRET` identical everywhere; middleware injects `X-User-ID`/`X-Username`; admin checks `X-Admin-Key`); config via `getenv(key, fallback)` only, runtime values in gitignored `run/secrets.env` (template `scripts/secrets.env.example`); logrus JSON + `promhttp` on `/metrics`; Greybox errors via `writeGreyboxError` (`code <= -32000`, else the launcher shows generic "service unavailable").

### Protocol invariants (verified — full detail in the mmog-responses skill)

- **Category law:** top byte of an item id = its `ItemIDTable` CategoryID; several client gates admit ids only by this byte.
- **Container length excludes its own length field but includes the 6-byte terminator** — off-by-4 makes a container swallow the next sibling.
- **Arrays (tag 0x0d) lose child names**; name-looked-up lists go as objects with children `"0"`, `"1"`, …
- Binary mmog parser compares field names **case-insensitively**; JSON catalog lookups do **not** (`Name` and `name` both required, different values).
- `<DNT>[[NotFound]]` = a **missing field**, not a failed localization.
- **Size ceiling is the client's 32768-byte receive ring** (legal per the frame's 16-bit size field, fatal anyway — oversized frames stop the client dead, they don't degrade). ~26KB proven safe for `YA_GetTechTree`.
- **A reply name is not always the request name** (`YA_Tune` → `YA_TuneReturn`; five names carry the suffix, rest correlate by request id). Check the dispatcher before renaming anything.
- `YA_GetTechTree`/`TechTrees` and `YA_TuneReturn`/`packed` carry everything in one zlib blob and ignore other fields; a hand-built blob document must call `protocol.AppendRootEnd` (`BuildResponseFrame` does it for you — easy to forget without, everything inside resolves to nothing at once).
- **Once the client accepts a response it TRUSTS it.** Empty tuning tables are worse than nothing; echo cooked data tables verbatim (`otsTableRowsJSON`), keyed by blueprint asset name.

### TLS

`certs/server.crt` serves the gateway; the Firmament socket uses `certs/firmament.crt`, issuer spoofed to "Amazon RSA 2048 M01" for CA pinning. `scripts/gen-certs.sh` builds both. `certs/` gitignored — never trust any CA from repo git history.

## Comments and commits

- Comments carry reverse-engineering findings that exist nowhere else — keep them dense, cite binary function addresses, and when you disprove a comment, correct it in place rather than deleting it.
- Lowercase scoped subjects: `techtree: ClassId must be an item id, not the EYShipClass ordinal`. Protocol changes: the body records the evidence (function, offset, client log before/after) and states plainly what was **not** verified against a running client.
- **Local commits only — never push to GitHub.** Operator tests every client cycle.

## Reference docs

- `CONTRIBUTING.md` — working rules, protocol invariants, debugging method (read first)
- `AGENTS.md` — agent-oriented gotchas, current test status
- `docs/client-data-reference.md` — extracted item/ship id maps and naming rules
- `docs/client-data-validation.md` — audit of every id the server emits
- README — setup, configuration variables, troubleshooting