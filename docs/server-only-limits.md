# Server-only limits

What the private server can and cannot do with a stock client and a modded
host only. Phase 1 (operator 2026-10-17) is to finish everything doable
server-side; this list is the starting backlog for phase 2, a client mod,
which needs its own update and security story. Each entry says what was
tried and the evidence. "Open" means a server-side route is still untried.

## Cannot be done server-only (verified)

- **In-match chat window.** The stock HUD has none ("HUDWidgetManager doesn't
  support AYHUD::OpenChatAllWindow yet"). Commands mid-match come from hangar
  chat, `dn cmd`, or the launcher battle viewer's chat.
- **Custom profile pictures.** The client only shows its built-in head
  portraits; research closed 2026-10-17.
- **The game's own bot system in Onslaught.** The bot spawner (0x3822F0 ->
  0x374550) resolves the hull through the combat scene manager's dataset
  (scene+0x3D0, lookup 0x2CF110). The Onslaught map's dataset only has the
  Onslaught ships, so player hulls fail every frame for the rest of the match
  ("ShipId=SupportMediumT2, no base pawn in dataAsset", boss5: 55k errors).
  Workaround in use: spawn the AI pawn class directly (see /spawn below).
  Details in the note above ShipIdControllerClass in dn_host_loadout.cpp.
- **Cloaking the meteor heralds.** Neither herald hull has a
  UYCloakingComponent: VH_ONS_AITargetM (skymeteor5) and VH_DreadM_AI_Pawn_T3
  (nuke5). They are parked 8000 uu over the highest ship instead.

## Works, with caveats

- **/meteor damage.** Host-spawned Heavy Nuclear Torpedoes land and kill
  (boss6: full-HP player one-shot ~9 s after the volley, live 2026-10-17 the
  same). Launch height 12000 uu: from 200000 they took ~140 s to fall.
- **/spawn <ShipId>, /boss outside Havoc.** Havoc ShipIds map to the AI pawn
  classes (HVC_VH_<C><W>_Boss_01, VH_<C><W>_AI_Pawn_T<n>) and spawn at enemy
  assault points. Pawns that name no AI controller (the Assault Heavy/Light
  bosses, live 2026-10-17) get the Horde "Hard" behaviour for their class.
  boss7 (headless 2026-10-17): AssaultHeavyBoss01 + Goliath_Hard spawned,
  elite x4 (1.34M HP), visible on the stock client and killed the player
  with Ballistic Cannons V. GUESS: the pairing for the other classes.
  Modules: working (boss19, headless 2026-10-18: 4 "fail ability" errors in
  a 5-minute fight instead of ~650 per slot, projectile volleys with no
  /meteor). The controller's m_fireAbilities slots name T0 ability classes
  (or none) while the pawn carries T5 ones, so every slot resolved to -1.
  FixNpcFireAbilities points each slot at the pawn's matching ability (by
  class name without the tier; GUESS: a classless slot means the pawn
  ability at the same index) and re-applies whenever the game resets them,
  which it does ~40 s after each player kill. Seen live 2026-10-18:
  /boss DreadnoughtHeavyNukerT5 nuked the player, visuals shown.
  The dataset does confirm the Hard controller pairing for
  DreadnoughtHeavyNukerT5.

## Open (server-side routes still to try)

- **/meteor visuals.** The torpedoes hurt but stock clients draw nothing
  (nuke5 screenshots; live 2026-10-17 "killed me, no visuals"). They are
  spawned straight on the host (YBlueprintLibrary.SpawnProjectile) and
  registered with projectile tracking. GUESS: clients only draw projectiles
  they simulate themselves from a replicated weapon fire, so a host-only
  projectile actor is never shown. Next try: fire the herald's own nuke
  weapon through its weapon manager / ability so the fire event replicates.
  If that also stays invisible, this moves to the list above.
