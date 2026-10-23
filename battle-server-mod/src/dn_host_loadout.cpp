// dn_host_loadout.cpp -- the host-side loadout fix, and nothing else.
//
// WHY THIS EXISTS
//
// UYLoadoutManagerComponent is populated by exactly one path:
// InitializeFromPlayerData, which reads the YMmogbrain module at +0x3898, which
// requires a login the battle server never performs. LoadInstallingLadouts
// would install exactly the four T1 mediums the client offers, and it sits
// behind the same gate; there is no third caller. So on a host the manager is
// empty, every UYLoadoutManagerComponent::FindLoadoutByID misses, and
// AYGameMode::SpawnDefaultPawn refuses to create a pawn -- the player sits on
// the orbit camera watching the planet for the whole match.
//
// This hooks FindLoadoutByID and, ON A MISS ONLY, registers the four cooked
// precast loadout assets with the manager and re-runs the engine's own lookup.
// A lookup that already succeeded is never touched. It supplies data the engine
// was designed to have and then gets out of the way, so it stays correct if a
// real backend ever fills the manager.
//
// It is deliberately NOT a general-purpose mod. In particular it does not force
// PlayerController+0x948 (EYOrbitReadyState) -- the engine computes that value
// correctly from the fleet slot count, and writing to it would be lying to a
// gate rather than filling a hole. See battle-server-mod/README.md.
//
// VERIFIED ADDRESSES (DreadGame-Win64-Shipping.exe, the 2018 retail build)
//
// Every RVA below was confirmed against the PE exception directory (.pdata) as
// a real RUNTIME_FUNCTION entry, not a chained cold chunk and not the middle of
// an instruction:
//
//   0x340340-0x3404D3  UYLoadoutManagerComponent::FindLoadoutByID(mgr, id, warn)
//                      `id` is an FName; the loop compares the 8 bytes at
//                      loadout+0xB0.
//   0x3382F0-0x338330  AddLoadout(mgr, loadout, uint8 type) -- ADD ONLY.
//                      NOT 0x337450: that is AddAndActivateLoadout, whose tail
//                      calls 0x337050 unconditionally, so registering four
//                      loadouts in a row through it leaves the LAST one active.
//                      The array ends with Support, which is why every hull a
//                      player picked used to spawn as a Cerberus.
//                      Type 2 matters: the validity gate at 0x33C680 rejects
//                      loadouts recorded as type 4.
//   0xD78110-0xD789B6  StaticLoadObject/StaticLoadClass, 7 arguments.
//
// And two data offsets, from the generated SDK for this build:
//
//   0x3F63A70          GObjects (FUObjectArray)
//   0x3E069D0          GNames   (TStaticIndirectArrayThreadSafeRead)
//
// GNames is used for log text only. If its layout were wrong the worst outcome
// is an unnamed line in the log; no decision depends on it.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <math.h>
#pragma comment(lib, "ws2_32.lib")

#include <share.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "MinHook.h"
#include "precast_paths.h"
#include "npc_abilities.h"

// ---------------------------------------------------------------------------
// Logging
//
// S14.4 asked for lines that are distinguishable from engine output on both the
// install and the fire. Everything here carries the [dn-host-loadout] tag and
// goes to stdout (which dn-dedicated captures) as well as a file beside the
// executable, so a host log tells you whether the hook was even present.
// ---------------------------------------------------------------------------

static FILE *g_logFile = nullptr;

static void LogOpen() {
  char path[MAX_PATH];
  if (!GetModuleFileNameA(NULL, path, MAX_PATH))
    return;
  char *slash = strrchr(path, '\\');
  if (!slash)
    return;
  strcpy_s(slash + 1, sizeof(path) - (slash + 1 - path), "dn_host_loadout.log");
  // _fsopen, not fopen: the CRT's fopen takes an exclusive lock on Windows, so
  // the file cannot even be COPIED while the host is running, and the whole
  // point of this log is that an operator can read it during a match.
  // _SH_DENYNO, not _SH_DENYWR (changed 2026-10-17): with DENYWR the first host
  // locks every later one out -- a second concurrent match, or any host after
  // a wineserver that kept a dead host's handle open, logged nothing to the
  // file (escort5 ran with no file log behind a stale wineserver's handle).
  // Lines from concurrent hosts interleave; each is a whole fprintf+fflush.
  g_logFile = _fsopen(path, "a", _SH_DENYNO);
}

// Wall-clock stamp on every line. C29 cost us a day: the log recorded the four
// precast resolutions with no times, so an 88-second hole sat in the middle of
// it unnoticed and we published a wrong cause built on top of it. A line that
// cannot be placed in time cannot be used as evidence.
static void LogStamp(char *out, size_t n) {
  SYSTEMTIME st;
  GetLocalTime(&st);
  _snprintf_s(out, n, _TRUNCATE, "%02u:%02u:%02u.%03u", st.wHour, st.wMinute,
              st.wSecond, st.wMilliseconds);
}

static void Logf(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
  va_end(ap);

  char ts[32];
  LogStamp(ts, sizeof(ts));

  printf("[dn-host-loadout] %s %s\n", ts, buf);
  fflush(stdout);
  if (g_logFile) {
    fprintf(g_logFile, "[dn-host-loadout] %s %s\n", ts, buf);
    fflush(g_logFile);
  }
}

// Milliseconds, for attributing cost to a phase rather than guessing at it.
static double NowMs() {
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

// ---------------------------------------------------------------------------
// Memory safety
//
// This runs inside a live game process with no symbols. Every pointer that came
// from the game is checked before it is followed.
// ---------------------------------------------------------------------------

// VirtualQuery cost meter. Under Wine 11 every call is NtQueryVirtualMemory
// taking virtual_mutex inside two sigprocmask syscalls; Glacier TDM
// 2026-10-17, 14 bots: 75% of the game thread's samples were that pair (perf:
// ntdll.so get_basic_memory_info, the cmp $0x2f buffer-size check) and frame
// ticks fell from 59/s to 13/s. Counted per where-slot (0 elsewhere, 1-4
// FrameTick phases, 5 GameModeTimer sections) for the heartbeat.
static volatile long g_vqN[8], g_vqWhere = 0;
static double g_vqMs[8], g_vqT = 0;

// Region cache, on only for one FrameTick on the game thread (the game thread
// is inside our code then, so its own frees cannot interleave). Remembers
// readable committed regions [lo,hi), 64 slots direct-mapped by 64 KB block.
// GUESS: another engine thread decommitting a region mid-tick is rare enough;
// a stale hit would fault inside the phase's __try and switch that phase off.
// g_vqHits counts lookups the cache answered (no syscall).
static DWORD g_vqCacheTid = 0;
static struct { uintptr_t lo, hi; } g_vqC[64];
static volatile long g_vqHits = 0;
static void VqCacheOn(bool on) {
  memset(g_vqC, 0, sizeof g_vqC);
  g_vqCacheTid = on ? GetCurrentThreadId() : 0;
}

static bool IsReadable(const void *p, size_t n) {
  if (!p)
    return false;
  const uintptr_t a = (uintptr_t)p;
  const bool cache = g_vqCacheTid && GetCurrentThreadId() == g_vqCacheTid;
  if (cache) {
    auto &e = g_vqC[(a >> 16) & 63];
    if (a >= e.lo && a + n <= e.hi && a + n >= a) {
      ++g_vqHits;
      return true;
    }
  }
  ++g_vqN[g_vqWhere & 7];
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi)))
    return false;
  if (mbi.State != MEM_COMMIT)
    return false;
  const DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
  if (mbi.Protect & bad)
    return false;
  const uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
  if (cache) // every slot the region covers: heap regions span many blocks
    for (uintptr_t lo = (uintptr_t)mbi.BaseAddress, blk = (a >> 16) - 31 > (lo >> 16) ? (a >> 16) - 31 : lo >> 16,
                   end = (a >> 16) + 32 < ((regionEnd - 1) >> 16) ? (a >> 16) + 32 : (regionEnd - 1) >> 16;
         blk <= end; ++blk)
      g_vqC[blk & 63].lo = lo, g_vqC[blk & 63].hi = regionEnd;
  return a + n <= regionEnd;
}

// ---------------------------------------------------------------------------
// Just enough UE4 to find four objects
// ---------------------------------------------------------------------------

static uintptr_t g_base = 0;

#define RVA_FIND_LOADOUT_BY_ID 0x340340
#define RVA_ADD_LOADOUT 0x3382F0
#define RVA_STATIC_LOAD_CLASS 0xD78110

// CORRECTED 2026-09-24: the second argument is the PAWN, not the PlayerState
// (the null branch logs "Trying to teleport into level a null YPawn!", and the
// caller passes an IsA-checked element of an array of pawns). This function is
// no longer hooked; see the DISPROVED note above OFF_HIGHEST_FLEET and
// HookGetNetMode. The original analysis follows, kept as a record.
//
// AYOrbitTransitionManager::TeleportPlayerIntoLevel(this, AYPlayerReplicationInfo*)
//
// The function that reads the fleet-tier gate. Verified 2026-08-15 by
// disassembly rather than inference:
//
//   0x1403d92b6  test rdx, rdx
//   0x1403d92b9  jne  0x1403d9303          ; null PRI -> a different message
//   0x1403d9303  cmp  byte ptr [rdx+0x948], 0
//   0x1403d930a  jne  0x1403d9393          ; -> teleport proceeds
//                ; fallthrough logs, at 0x1403d9360:
//                ; "Trying to teleport into level player %s that is not in orbit!"
//
// The format string has TWO identical .rdata copies (0x142edaf90 and
// 0x142edb0a0) and exactly ONE xref, to the SECOND -- the wrong-copy trap in
// CONTRIBUTING.md. Scanning .text for RIP-relative LEAs found it.
//
// rdx is AYPlayerReplicationInfo: +0x948 is m_highestFleetUnlocked in
// dread-sdk's DreadGame_Classes.h:1920, and nothing else in the SDK dump puts
// an EYFleetType there. Only rcx/rdx are read, so the two-argument passthrough
// below is complete -- [rsp+0xC0] at 0x1403d9393 is a STORE into the caller's
// home space, not a stack argument.
// UNetDriver::GetNetMode -- 0x1A5CF60-0x1A5CF8D (.pdata entry, 45 bytes):
//   if (this->IsServer())  return 1 + (GIsClient != 0);   // 1 Dedicated, 2 Listen
//   else                   return 3;                       // Client
// AActor::GetNetMode (0x1726F50) tail-jumps here with the actor's net driver.
// See HookGetNetMode.
#define RVA_NETDRIVER_GET_NET_MODE 0x1A5CF60
#define NM_DEDICATED_SERVER 1
#define NM_LISTEN_SERVER 2

// AYGameMode_Multiplayer's once-a-second timer, 0x36A080-0x36A539 (.pdata
// entry; vtable slot 0x8E0 of every multiplayer mode). Reads only rcx, so the
// one-argument passthrough is complete. See HookGameModeTimer.
#define RVA_GAMEMODE_MP_TIMER 0x36A080
#define OFF_GM_GAMESTATE 0x458        // AGameModeBase -> AGameState*
#define OFF_GM_ENABLE_SPAWN_AI 0x961  // AYGameMode_Multiplayer::m_enableSpawnAI
#define OFF_GS_GAME_MODE_TYPE 0x500   // AYGameState::m_gameModeType
#define YGMT_BOOTCAMP 18              // EYGameModeType -- the proving ground
// Team Elimination. Observed on live hosts (2026-09-29): a ?game=TE host logs
// "type 5", a TDM host "type 3". TE starts with m_enableSpawnAI 0; forcing it
// on for the whole match refilled both teams every time a bot died, so TE
// "just played like a deathmatch" (operator). TE's ROUND flow is not in this
// exe at all -- ClientPreRoundStart / ClientNewRoundStarts / ClientPostRoundEnd
// / ClientRespawnTeamElimination have FName globals (0x3E10260, 0x3E10218,
// 0x3E10268, 0x3E10298) referenced ONLY by their initializers, like
// ClientSetTopPlayerMatchStats: the server build ran rounds. What the mod does
// for TE (operator's call: "bots can be enabled but they should not respawn"):
// spawner ON until both teams are filled, then OFF, so a destroyed bot stays
// destroyed. See the timer below.
#define YGMT_TEAM_ELIMINATION 5
#ifndef OFF_GM_AI_TARGET_T1
#define OFF_GM_AI_TARGET_T1 0x980
#endif

// UYVehicleMovementComp: 0x5C4EB0-0x5C511E (.pdata entry), one argument (this).
// The only writer of +0x489. See HookVehicleViewCull.
#define RVA_VEHICLE_VIEW_CULL_SETUP 0x5C4EB0

// AYPlayerController::ClientSetPlayerRestrictions_Implementation,
// 0x5743B0-0x5744A0 (.pdata entry; referenced from 4 controller vtables, run by
// the exec thunk 0x746B40). 18 arguments: this, 16 bytes, 1 pointer (read at
// 0x5743BA-0x5744A0). See HookClientSetPlayerRestrictions.
#define RVA_CLIENT_SET_PLAYER_RESTRICTIONS_IMPL 0x5743B0
#define OFF_VMC_LOCALLY_CONTROLLED 0x488  // = owner->IsLocallyControlled()
#define OFF_VMC_VIEW_CULLED 0x489         // "cull my physics by the local camera"
#define OFF_GOBJECTS 0x3F63A70
#define OFF_GNAMES 0x3E069D0

struct FNameMin {
  int32_t ComparisonIndex;
  int32_t Number;
};

struct UObjectMin {
  void *VfTable;        // 0x00
  int32_t Flags;        // 0x08
  int32_t InternalIndex;// 0x0C
  UObjectMin *Class;    // 0x10
  FNameMin Name;        // 0x18
  UObjectMin *Outer;    // 0x20
};

struct FUObjectItemMin {
  UObjectMin *Object;
  int32_t Flags;
  int32_t ClusterIndex;
  int32_t SerialNumber;
  int32_t Pad;
};

struct FUObjectArrayMin {
  FUObjectItemMin *Objects;
  int32_t MaxElements;
  int32_t NumElements;
};

static FUObjectArrayMin *GObjects() {
  return (FUObjectArrayMin *)(g_base + OFF_GOBJECTS);
}

// Name text, for the log only. Mirrors the layout the generated SDK uses for
// this build: a pointer to a chunk table, chunks of 16384 FNameEntry pointers,
// and an FNameEntry whose ANSI text starts at +0x10 (int32 Index, 4 bytes of
// padding, FNameEntry* HashNext). Bit 0 of Index means the entry is wide.
static const char *NameText(const FNameMin &name) {
  const int32_t kElementsPerChunk = 16384;
  int32_t idx = name.ComparisonIndex;
  if (idx < 0)
    return nullptr;

  void ***chunkTablePtr = (void ***)(g_base + OFF_GNAMES);
  if (!IsReadable(chunkTablePtr, sizeof(void *)))
    return nullptr;
  void **chunkTable = (void **)*chunkTablePtr;
  if (!IsReadable(chunkTable + (idx / kElementsPerChunk), sizeof(void *)))
    return nullptr;
  void **chunk = (void **)chunkTable[idx / kElementsPerChunk];
  if (!IsReadable(chunk + (idx % kElementsPerChunk), sizeof(void *)))
    return nullptr;
  uint8_t *entry = (uint8_t *)chunk[idx % kElementsPerChunk];
  if (!IsReadable(entry, 0x20))
    return nullptr;
  if (*(int32_t *)entry & 0x1) // wide -- we only ever compare ASCII asset names
    return nullptr;
  return (const char *)(entry + 0x10);
}

// The class of a UClass is UClass, and the UClass class object is its own
// Class. That fixed point identifies it without needing GNames to be right,
// which is why it is used instead of a name lookup: StaticLoadClass takes this
// pointer, so a wrong answer here would be a wrong call rather than a wrong log
// line.
static UObjectMin *g_uclassClass = nullptr;

static UObjectMin *ResolveUClassClass() {
  if (g_uclassClass)
    return g_uclassClass;

  FUObjectArrayMin *arr = GObjects();
  if (!IsReadable(arr, sizeof(*arr)))
    return nullptr;
  int32_t count = arr->NumElements;
  if (count <= 0 || !IsReadable(arr->Objects, sizeof(FUObjectItemMin)))
    return nullptr;

  for (int32_t i = 0; i < count && i < 4096; ++i) {
    FUObjectItemMin *item = &arr->Objects[i];
    if (!IsReadable(item, sizeof(*item)))
      continue;
    UObjectMin *o = item->Object;
    if (!IsReadable(o, sizeof(*o)))
      continue;
    UObjectMin *c = o->Class;
    if (!IsReadable(c, sizeof(*c)))
      continue;
    UObjectMin *cc = c->Class;
    if (!IsReadable(cc, sizeof(*cc)))
      continue;
    if (cc->Class == cc) { // fixed point: this is UClass
      g_uclassClass = cc;
      const char *n = NameText(cc->Name);
      Logf("resolved UClass class object at %p (%s)", cc, n ? n : "<unnamed>");
      return g_uclassClass;
    }
  }
  Logf("could not resolve the UClass class object; giving up on this pass");
  return nullptr;
}

// Pin an object and its Outer chain into the GC root set, so an asset we loaded
// outside the engine's own reference graph is not collected between the load
// and the spawn. RootSet is bit 30; PendingKill (29) and Unreachable (28) are
// cleared.
static void PinToRootSet(UObjectMin *obj) {
  FUObjectArrayMin *arr = GObjects();
  if (!IsReadable(arr, sizeof(*arr)))
    return;
  for (UObjectMin *cur = obj; cur && IsReadable(cur, sizeof(*cur));
       cur = cur->Outer) {
    int32_t idx = cur->InternalIndex;
    if (idx < 0 || idx >= arr->NumElements)
      break;
    FUObjectItemMin *item = &arr->Objects[idx];
    if (!IsReadable(item, sizeof(*item)) || item->Object != cur)
      break;
    InterlockedOr((volatile LONG *)&item->Flags, (LONG)(1 << 30));
    InterlockedAnd((volatile LONG *)&item->Flags,
                   (LONG)~((1 << 28) | (1 << 29)));
  }
}

typedef void *(*tStaticLoadClass)(void *ObjectClass, void *InOuter,
                                  const wchar_t *InName,
                                  const wchar_t *Filename, int LoadFlags,
                                  void *Sandbox, bool bDoNotReconcile);

// ---------------------------------------------------------------------------
// The four precast loadouts
//
// These are the same four T1 mediums LoadInstallingLadouts would have installed,
// which is the set the client's ship-select screen offers. All four are
// registered, not just the first: an earlier version stopped at the first
// success and every player got the Assault Medium whatever they picked, because
// the other three ids kept missing.
// ---------------------------------------------------------------------------

static const wchar_t *kPrecastPaths[] = {
    L"/Game/Generic/Loadouts/Precast/T1/VH_AssaultMedium_T1_PrecastLoadout_BP."
    L"VH_AssaultMedium_T1_PrecastLoadout_BP_C",
    L"/Game/Generic/Loadouts/Precast/T1/"
    L"VH_DreadnoughtMedium_T1_PrecastLoadout_BP."
    L"VH_DreadnoughtMedium_T1_PrecastLoadout_BP_C",
    L"/Game/Generic/Loadouts/Precast/T1/VH_SniperMedium_T1_PrecastLoadout_BP."
    L"VH_SniperMedium_T1_PrecastLoadout_BP_C",
    L"/Game/Generic/Loadouts/Precast/T1/VH_SupportMedium_T1_PrecastLoadout_BP."
    L"VH_SupportMedium_T1_PrecastLoadout_BP_C",
};

static const char *kPrecastLabels[] = {
    "VH_AssaultMedium_T1",
    "VH_DreadnoughtMedium_T1",
    "VH_SniperMedium_T1",
    "VH_SupportMedium_T1",
};

static const int kPrecastCount =
    (int)(sizeof(kPrecastPaths) / sizeof(kPrecastPaths[0]));

static UObjectMin *g_precastCDO[4] = {};
static int g_precastResolved = 0;
static bool g_precastAttempted = false;

// StaticLoadClass returns the loaded UClass. Its class default object is the
// object whose Class is that UClass and which is not the UClass itself -- at
// resolve time, before any pawn spawns, the CDO is the only such object.
//
// Matching on the pointer rather than on the CDO's name is deliberate: the
// short name "Default__..._C" cannot be matched against UObject::GetFullName,
// which is what an earlier version tried, and it made all four report "could not
// resolve" before we knew whether the load itself had worked.
// ONE pass over GObjects for all four classes, and no VirtualQuery inside it.
//
// This function used to be called once per precast and used IsReadable -- i.e. a
// VirtualQuery syscall -- twice per object, over an array that holds millions of
// entries once a map is loaded. Four scans x ~2M objects x 2 syscalls is roughly
// 8 million syscalls, and it measured ~21.6 seconds per precast: the whole of
// C29's 90-second game-thread stall, misattributed at the time to
// StaticLoadClass, which the log could not distinguish because it had no
// timestamps.
//
// Per-object SEH replaces the per-object syscall. On x64 __try is table-driven
// and costs nothing when nothing faults, so a bad pointer still cannot take the
// host down -- it just skips that entry instead of aborting the scan.
static int FindCDOsForClasses(UObjectMin **classes, UObjectMin **cdosOut,
                              int count) {
  for (int i = 0; i < count; ++i)
    cdosOut[i] = nullptr;

  FUObjectArrayMin *arr = GObjects();
  if (!IsReadable(arr, sizeof(*arr)))
    return 0;

  int32_t num = arr->NumElements;
  FUObjectItemMin *items = arr->Objects;
  if (num <= 0 || !IsReadable(items, sizeof(FUObjectItemMin)))
    return 0;

  int found = 0;
  for (int32_t i = 0; i < num && found < count; ++i) {
    UObjectMin *o = nullptr;
    UObjectMin *ocls = nullptr;
    __try {
      o = items[i].Object;
      if (o)
        ocls = o->Class;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      continue;
    }
    if (!o || !ocls)
      continue;

    for (int c = 0; c < count; ++c) {
      if (cdosOut[c] || !classes[c] || o == classes[c] || ocls != classes[c])
        continue;
      // Prefer the class default object explicitly rather than relying on it
      // being the only instance. That held while this ran before any pawn
      // spawned; it stops holding the moment resolution moves earlier or later,
      // and a live instance would be silently registered in its place.
      const char *n = nullptr;
      __try {
        n = NameText(o->Name);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        n = nullptr;
      }
      if (n && strncmp(n, "Default__", 9) != 0)
        continue;
      cdosOut[c] = o;
      ++found;
      break;
    }
  }
  return found;
}

static void ResolvePrecastLoadouts() {
  if (g_precastAttempted)
    return;
  g_precastAttempted = true;

  UObjectMin *uclassClass = ResolveUClassClass();
  if (!uclassClass) {
    g_precastAttempted = false; // try again on the next miss
    return;
  }

  tStaticLoadClass StaticLoadClass =
      (tStaticLoadClass)(g_base + RVA_STATIC_LOAD_CLASS);

  // Phase 1: load the four classes. Timed individually, because C29 blamed this
  // call for the whole stall without ever measuring it.
  UObjectMin *classes[4] = {};
  double loadTotal = 0.0;
  for (int i = 0; i < kPrecastCount; ++i) {
    double t0 = NowMs();
    UObjectMin *cls = nullptr;
    __try {
      cls = (UObjectMin *)StaticLoadClass(uclassClass, nullptr,
                                          kPrecastPaths[i], nullptr, 0, nullptr,
                                          false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      cls = nullptr;
    }
    double dt = NowMs() - t0;
    loadTotal += dt;

    if (!IsReadable(cls, sizeof(UObjectMin))) {
      Logf("precast %s: StaticLoadClass returned nothing (%.0f ms)",
           kPrecastLabels[i], dt);
      continue;
    }
    PinToRootSet(cls);
    classes[i] = cls;
    Logf("precast %s: class=%p (StaticLoadClass %.0f ms)", kPrecastLabels[i],
         cls, dt);
  }

  // Phase 2: one scan of GObjects for all four default objects.
  UObjectMin *cdos[4] = {};
  double t0 = NowMs();
  int found = FindCDOsForClasses(classes, cdos, kPrecastCount);
  double scanMs = NowMs() - t0;

  for (int i = 0; i < kPrecastCount; ++i) {
    if (!classes[i])
      continue;
    if (!cdos[i]) {
      Logf("precast %s: class loaded at %p but no default object found",
           kPrecastLabels[i], classes[i]);
      continue;
    }
    PinToRootSet(cdos[i]);
    g_precastCDO[g_precastResolved++] = cdos[i];
    const char *n = NameText(cdos[i]->Name);
    Logf("precast %s: cdo=%p (%s)", kPrecastLabels[i], cdos[i],
         n ? n : "<unnamed>");
  }

  Logf("%d/%d precast loadouts resolved "
       "(StaticLoadClass %.0f ms total, one CDO scan %.0f ms, %d/%d matched)",
       g_precastResolved, kPrecastCount, loadTotal, scanMs, found,
       kPrecastCount);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

typedef void(__fastcall *tAddLoadout)(void *mgr, void *loadout, uint8_t type);

static bool AddLoadoutGuarded(void *mgr, void *loadout) {
  __try {
    ((tAddLoadout)(g_base + RVA_ADD_LOADOUT))(mgr, loadout, 2);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// S15.1: this guard was a single pointer, so a listen server -- which has two
// player controllers, the human and the local player 256 -- could alternate
// between two managers and re-register on every alternation. A small set holds
// all of them.
static void *g_registeredFor[8] = {};
static int g_registeredCount = 0;

static bool AlreadyRegistered(void *mgr) {
  for (int i = 0; i < g_registeredCount; ++i)
    if (g_registeredFor[i] == mgr)
      return true;
  return false;
}

static void RegisterPrecastLoadouts(void *mgr) {
  if (!mgr || AlreadyRegistered(mgr))
    return;
  ResolvePrecastLoadouts();
  if (g_precastResolved == 0)
    return;
  if (g_registeredCount < (int)(sizeof(g_registeredFor) / sizeof(void *)))
    g_registeredFor[g_registeredCount++] = mgr;
  for (int i = 0; i < g_precastResolved; ++i) {
    bool ok = AddLoadoutGuarded(mgr, g_precastCDO[i]);
    Logf("register %s with manager %p -> %s", kPrecastLabels[i], mgr,
         ok ? "ok" : "EXCEPTION");
  }
}

// ---------------------------------------------------------------------------
// Any precast, on demand
//
// The four T1 mediums above are only the starter fleet. A player who fields a
// researched ship makes the host look up that ship's precast -- verified live
// 2026-09-24: "FindLoadoutByID miss for ... (Default__VH_SniperLight_T2_
// PrecastLoadout_BP_C) -> after registering: STILL MISSING", then "Active
// Loadout not found. Can't spawn" and no pawn. So a miss that the T1 set does not
// answer is resolved by NAME: "Default__<X>_C" -> <X> -> its class path from
// precast_paths.h (generated from the cooked asset list; the path cannot be
// derived, 48 of 102 precasts are outside a Precast/T<n>/ folder) ->
// StaticLoadClass -> its default object -> AddLoadout on the asking manager.
// Same mechanism as the T1 set, one class at a time, cached.
// ---------------------------------------------------------------------------

struct DynPrecast {
  // Hero entries resolve through a REUSED name buffer (g_heroNameBuf), so the
  // cache keeps a copy and compares by content, not pointer: two hero names
  // would otherwise alias one entry.
  char name[160];
  UObjectMin *cdo; // nullptr if resolution failed (not retried)
};
static DynPrecast g_dynPrecast[128] = {};
static int g_dynPrecastCount = 0;

struct DynRegistration {
  void *mgr;
  UObjectMin *cdo;
};
static DynRegistration g_dynRegistered[256] = {};
static int g_dynRegisteredCount = 0;

static const PrecastPath *FindPrecastPath(const char *assetName) {
  for (int i = 0; i < kAllPrecastCount; ++i)
    if (strcmp(kAllPrecastPaths[i].name, assetName) == 0)
      return &kAllPrecastPaths[i];
  return nullptr;
}

// "Default__VH_SniperLight_T2_PrecastLoadout_BP_C" -> table entry, or nullptr.
//
// Hero ships were added 2026-10-05: the table (generated from the cooked
// Precast asset list) holds none of the 48 hero assets because they live in a
// different folder family -- /Game/Generic/Loadouts/Hero/<name> for ALL of
// them (checked against data/loadouts/HeroLoadouts_cooked.jsonl, 48/48). So a
// "<X>_Heroloadout_BP" name is resolved directly, without a table entry.
// Verified live: match 18:48 asked for Default__VH_ScoutMedium_Phoenix_
// Heroloadout_BP_C four times; the on-demand path then had no table entry and
// registered only the four T1 starter precasts, leaving the hero pick unrecoverable.
static const wchar_t kHeroLoadoutDir[] = L"/Game/Generic/Loadouts/Hero/";
static PrecastPath g_heroPath;        // the last hero name resolved
static char g_heroNameBuf[160];       // static storage PrecastPath.name points at
static wchar_t g_heroPathBuf[512];    // ... and path
// "<X>_Heroloadout_BP" -> /Game/Generic/Loadouts/Hero/<X>.<X>_C
static const PrecastPath *HeroLoadoutPath(const char *asset) {
  size_t n = strlen(asset);
  if (n >= sizeof(g_heroNameBuf))
    return nullptr;
  memcpy(g_heroNameBuf, asset, n + 1);
  g_heroPath.name = g_heroNameBuf;
  if (swprintf_s(g_heroPathBuf, L"%s%hs.%hs_C", kHeroLoadoutDir, asset, asset) <= 0)
    return nullptr;
  g_heroPath.path = g_heroPathBuf;
  return &g_heroPath;
}

static const PrecastPath *PrecastForLoadoutId(const char *idText) {
  if (!idText || strncmp(idText, "Default__", 9) != 0)
    return nullptr;
  char asset[160];
  size_t n = strlen(idText + 9);
  if (n < 3 || n >= sizeof(asset) || strcmp(idText + 9 + n - 2, "_C") != 0)
    return nullptr;
  memcpy(asset, idText + 9, n - 2);
  asset[n - 2] = 0;
  const PrecastPath *pp = FindPrecastPath(asset);
  if (pp)
    return pp;
  return HeroLoadoutPath(asset);
}

static UObjectMin *ResolvePrecastByName(const PrecastPath *pp) {
  for (int i = 0; i < g_dynPrecastCount; ++i)
    if (strncmp(g_dynPrecast[i].name, pp->name,
                sizeof(g_dynPrecast[i].name)) == 0)
      return g_dynPrecast[i].cdo;

  UObjectMin *cdo = nullptr;
  UObjectMin *uclassClass = ResolveUClassClass();
  if (uclassClass) {
    tStaticLoadClass StaticLoadClass =
        (tStaticLoadClass)(g_base + RVA_STATIC_LOAD_CLASS);
    double t0 = NowMs();
    UObjectMin *cls = nullptr;
    __try {
      cls = (UObjectMin *)StaticLoadClass(uclassClass, nullptr, pp->path,
                                          nullptr, 0, nullptr, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      cls = nullptr;
    }
    if (IsReadable(cls, sizeof(UObjectMin))) {
      PinToRootSet(cls);
      UObjectMin *classes[1] = {cls};
      UObjectMin *cdos[1] = {};
      FindCDOsForClasses(classes, cdos, 1);
      cdo = cdos[0];
      if (cdo)
        PinToRootSet(cdo);
    }
    Logf("precast %s (on demand): class=%p cdo=%p (%.0f ms)", pp->name, cls,
         cdo, NowMs() - t0);
  }
  if (g_dynPrecastCount < (int)(sizeof(g_dynPrecast) / sizeof(g_dynPrecast[0]))) {
    // Copy the name: the hero path reuses one static buffer.
    DynPrecast &slot = g_dynPrecast[g_dynPrecastCount++];
    memset(&slot, 0, sizeof(slot));
    strncpy_s(slot.name, sizeof(slot.name), pp->name, _TRUNCATE);
    slot.cdo = cdo;
  }
  return cdo;
}

// Registers the named precast with this manager if the name is a known precast.
// Returns true if something new was registered.
static bool RegisterPrecastOnDemand(void *mgr, const char *idText) {
  const PrecastPath *pp = PrecastForLoadoutId(idText);
  if (!pp)
    return false;
  UObjectMin *cdo = ResolvePrecastByName(pp);
  if (!cdo)
    return false;
  for (int i = 0; i < g_dynRegisteredCount; ++i)
    if (g_dynRegistered[i].mgr == mgr && g_dynRegistered[i].cdo == cdo)
      return false;
  if (g_dynRegisteredCount <
      (int)(sizeof(g_dynRegistered) / sizeof(g_dynRegistered[0])))
    g_dynRegistered[g_dynRegisteredCount++] = {mgr, cdo};
  bool ok = AddLoadoutGuarded(mgr, cdo);
  Logf("register %s with manager %p -> %s", pp->name, mgr,
       ok ? "ok" : "EXCEPTION");
  return ok;
}

// ---------------------------------------------------------------------------
// The hook
// ---------------------------------------------------------------------------

typedef void *(__fastcall *tFindLoadoutByID)(void *mgr, void **id, uint8_t warn);
static tFindLoadoutByID g_origFindLoadoutByID = nullptr;


// ---------------------------------------------------------------------------
// The player's own fit (dn_host_player_loadouts.txt)
//
// Verified 2026-09-25 from the exe: a client tells the host only WHICH loadout
// it picked (ServerPlayerClickedShipLoadout(FName id)), and the host cannot
// look the fit up itself -- its mmog AutoLogin (0x2AABCB0) is a stub and its
// fleet manager (0x35FDF0) only reads the host's own account. The original
// servers were a separate build. So:
//
//   1. mmogbrain adds ?DNPID=<pid> to the YA_Connect travel address; the host
//      keeps the login URL at UNetConnection+0x198 (verified live), reached
//      via loadoutManager+0xA8 (owner controller) -> +0x5A8 (NetConnection).
//   2. The mod asks mmogbrain (GET /battle/loadout?pid=&id=, loopback) for the
//      record the client built its own loadout from.
//   3. It builds the loadout exactly as the client does in
//      HandleMmogbrainLoadoutAdded (0x348830, verified):
//        obj = StaticConstructObject(UYShipLoadout::StaticClass() 0x614E30,
//                                    outer = manager)             0xD759E0
//        FYShipImportLoadoutInfo from the record                  (0x34E550)
//        0x34D690(obj, *(owner+0x970), &info)
//        obj->m_cachedInitializationData (+0x38..) = info
//        obj+0x1B1 = obj+0x1B2 = 1
//        if !FindLoadoutByID(mgr, &obj->m_id) -> AddLoadout(mgr, obj, 2)
//
// The client picks by the precast's name, so registering a default precast
// first would shadow the player's fit: this runs BEFORE any precast fallback.
// ---------------------------------------------------------------------------

#define RVA_YSHIPLOADOUT_STATICCLASS 0x614E30
#define RVA_STATIC_CONSTRUCT_OBJECT 0xD759E0
#define RVA_LOADOUT_INIT_FROM_INFO 0x34D690
#define RVA_FSTRING_ASSIGN 0x21F5E0
#define RVA_TARRAY_INT_ASSIGN 0x1CF3240
// FName(const WIDECHAR*, EFindName). FIXED 2026-09-28: this was 0xC9CF20,
// which is the ANSI (char*) constructor -- the game calls 0xC9CF20 with
// "YMmogbrain" and 0xC9CFA0 with L"m_team". Given a wide string, 0xC9CF20
// built a garbage name: team sync logged "SetTeam not found", and the
// loadout's pid FName was garbage too (nothing read it).
#define RVA_FNAME_CTOR_WIDE 0xC9CFA0
// CORRECTED 2026-10-13: UYShipCustomizationComponent::ImportFromDisplayInfo, NOT
// UYShipLoadout (class sizes 0x320 vs 0x1B8; see the REMOVED note in
// RegisterPlayerLoadout). The UFunction thunk is 0x75CA10
// (.pdata ENTRY 0x75CA10-0x75CAF1), but a thunk wants a UFunction params
// frame -- call its real implementation directly instead. That is 0x422400
// (.pdata ENTRY 0x422400-0x42248B, 139 B): it copies the FString and hands it
// to the appearance parser whose entry chunk is 0x421FE0-0x42201A feeding the
// container at this+0x2C0 (the '#' / ';' tokenizer lives in CHUNK
// 0x42201A-0x4220FE of 0x421FE0 -- cold path, never a hook target). The
// fstr(obj + 0x68, ...) write above populates m_cachedInitializationData
// ONLY; a client-built loadout also carries the LIVE appearance, which the
// client imports from the display string before the pawn reads it. Without
// this call the live property ships empty and remote clients render the
// default look (the skin/cosmetic gap). Same class family: the registration
// block 0x6353A0-0x635584 pairs AddAdditionalMesh / BroadcastApperance /
// ExportToDisplayInfo / ImportFromDisplayInfo / ImportFromShipAppereance.
#define RVA_IMPORT_FROM_DISPLAY_INFO_IMPL 0x422400
// UYShipLoadout import from display (loadout, const FString&): copies the
// string, parses into this+0x108 via 0x421FE0. .pdata 0x35AB00-0x35AB71.
#define RVA_LOADOUT_IMPORT_FROM_DISPLAY 0x35AB00
#define OFF_COMPONENT_OWNER 0xA8
#define OFF_PC_NETCONNECTION 0x5A8
#define OFF_NETCONN_REQUEST_URL 0x198
#define OFF_OWNER_INIT_ARG 0x970

struct FStringMin {
  wchar_t *data;
  int32_t num; // characters including the terminator
  int32_t max;
};
struct TArrayIntMin {
  int32_t *data;
  int32_t num;
  int32_t max;
};
// FYShipImportLoadoutInfo, 0x70 bytes (dread-sdk DreadGame_Structs.h:4634).
struct ShipImportInfoMin {
  FNameMin loadoutID;  // 0x00
  FNameMin pid;        // 0x08
  int32_t precastID;   // 0x10
  int32_t pad14;       // 0x14
  FStringMin name;     // 0x18
  int32_t shipClass;   // 0x28
  int32_t pad2c;       // 0x2C
  FStringMin display;  // 0x30
  TArrayIntMin weapons;   // 0x40
  TArrayIntMin abilities; // 0x50
  TArrayIntMin perks;     // 0x60
};
static_assert(sizeof(ShipImportInfoMin) == 0x70, "FYShipImportLoadoutInfo size");

typedef void *(__fastcall *tStaticClass)();
typedef void *(__fastcall *tStaticConstructObject)(void *cls, void *outer,
                                                   uint64_t name, uint32_t flags,
                                                   uint32_t internalFlags,
                                                   void *tmpl, bool copyTransients,
                                                   void *instanceGraph);
typedef void(__fastcall *tLoadoutInitFromInfo)(void *obj, void *ownerArg,
                                               ShipImportInfoMin *info);
typedef void(__fastcall *tAssign)(void *dst, const void *src);
// tImportDisplayReal: the REAL ImportFromDisplayInfo body
// (RVA_IMPORT_FROM_DISPLAY_INFO_IMPL = 0x422400). ABI CLOSED (S74): the
// earlier 4-arg variant theory (r8/r9, `mov eax,[r12]`) was derived from the
// TWIN disasm region -- the 2-arg call is right. Proof: the client calls the
// same impl with exactly two args from 0x57e89c's custom-appearance branch
// (0x14057eae8: rdx=display FString, rcx=[rdi+0x470]=loadout), leaving r8/r9
// as normal register leftovers, so the 19:41 exception's r8/r9 wide-string
// fragments were never an ABI problem. The throwing-import AVs live at
// 0x42226D in the 0x422240 helper reached via the 0x188B4F6 path and are
// survivable aborts on the client too -- the fatal part was only the later
// deferred sweep walking a freed stored pointer (see guard comment below).
typedef void(__fastcall *tImportDisplayReal)(void *loadout,
                                             const FStringMin *display);
typedef FNameMin *(__fastcall *tFNameCtor)(FNameMin *out, const wchar_t *text,
                                           int findType);

// S70/S71 instrument (logging only). S70's +0x2C0 container read was FALSIFIED
// by the 19:40 run: a SUCCESSFUL import left {Data=NULL,Num=0} untouched at
// obj+0x2C0 on both registrations, while the two THROWING imports changed it
// (wide-string fragments leaking in from adjacent heap) -- so obj+0x2C0 was
// never the import's container and is not touched by code that succeeds. The
// exception AVs moved too: they are at 0x42226D `mov r14,[rdx+0x10]` in the
// small helper 0x422240 (a variant/property parse fn, no static callers --
// referenced only from a 0x30-stride registration table at 0x143D735D0), with
// rdx = NULL, a freed-looking block, or registers whose low bytes spell wide
// string fragments. The wrapper 0x422400 takes FOUR args (rcx,rdx,r8,r9); the
// mod passes two, so r8/r9 are leftover registers (their prologue does
// `mov eax,[r12]` unconditionally). Since guessing offsets keeps failing, this
// instrument diffs snapshot-vs-after for whatever code writes: take a 1 KiB
// snapshot of obj (and the init owner arg) at each stage and log every changed
// word afterwards, so the poison's writer is named by the log itself.
static uint64_t g_snapshotObj[128];   // 1 KiB
static uint64_t g_snapshotOwner[128]; // 1 KiB

static void SnapshotRegion(const void *base, uint64_t *snap) {
  if (IsReadable(base, 1024))
    memcpy(snap, base, 1024);
  else
    memset(snap, 0xEE, 1024);
}

static void LogRegionDiff(const void *base, const uint64_t *snap,
                          const char *what, const char *when) {
  if (!IsReadable(base, 1024))
    return;
  const uint64_t *now = (const uint64_t *)base;
  char line[512];
  int changed = 0;
  for (size_t i = 0; i < 128; ++i) {
    if (now[i] == snap[i])
      continue;
    if (changed == 0)
      Logf("appearance %s diff of %s:", when, what);
    else
      ++changed;
    int consumed = _snprintf_s(line, sizeof(line), _TRUNCATE,
                               "  +0x%03zX %016llX -> %016llX%s",
                               i * 8, (unsigned long long)snap[i],
                               (unsigned long long)now[i],
                               ((changed & 7) == 7 || i == 127) ? "\n" : " ");
    if (changed == 0 || (changed & 7) == 7)
      Logf("appearance %s %s:%s", when, what, line);
    if (consumed < 0)
      break;
  }
  if (changed && (changed & 7) != 7)
    Logf("appearance %s %s tail", when, what);
}

static bool PlayerLoadoutsEnabled();

// One ?KEY=value option from the controller's connection login URL (e.g.
// "DNPID=", "TEAM="), or false (the host's local player 256 has no connection).
static bool UrlOptionForController(uint8_t *owner, const wchar_t *key, char *out, size_t outLen) {
  out[0] = 0;
  __try {
    if (!IsReadable(owner, OFF_PC_NETCONNECTION + 8))
      return false;
    uint8_t *conn = *(uint8_t **)(owner + OFF_PC_NETCONNECTION);
    if (!IsReadable(conn, OFF_NETCONN_REQUEST_URL + sizeof(FStringMin)))
      return false;
    FStringMin *url = (FStringMin *)(conn + OFF_NETCONN_REQUEST_URL);
    if (!url->data || url->num <= 0 || url->num > 4096 ||
        !IsReadable(url->data, (size_t)url->num * 2))
      return false;
    const wchar_t *p = wcsstr(url->data, key);
    if (!p)
      return false;
    p += wcslen(key);
    size_t n = 0;
    while (p[n] && p[n] != L'?' && p[n] != L'&' && n + 1 < outLen) {
      out[n] = (char)p[n];
      ++n;
    }
    out[n] = 0;
    return n > 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// The same, as wide text (player names are not ASCII-only).
static bool UrlOptionWideForController(uint8_t *owner, const wchar_t *key, wchar_t *out, size_t outLen) {
  out[0] = 0;
  __try {
    if (!IsReadable(owner, OFF_PC_NETCONNECTION + 8))
      return false;
    uint8_t *conn = *(uint8_t **)(owner + OFF_PC_NETCONNECTION);
    if (!IsReadable(conn, OFF_NETCONN_REQUEST_URL + sizeof(FStringMin)))
      return false;
    FStringMin *url = (FStringMin *)(conn + OFF_NETCONN_REQUEST_URL);
    if (!url->data || url->num <= 0 || url->num > 4096 ||
        !IsReadable(url->data, (size_t)url->num * 2))
      return false;
    const wchar_t *p = wcsstr(url->data, key);
    if (!p)
      return false;
    p += wcslen(key);
    size_t n = 0;
    while (p[n] && p[n] != L'?' && p[n] != L'&' && n + 1 < outLen) {
      out[n] = p[n];
      ++n;
    }
    out[n] = 0;
    return n > 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// DNPID from the controller's connection login URL.
static bool PlayerPIDForController(uint8_t *owner, char *out, size_t outLen) {
  return UrlOptionForController(owner, L"DNPID=", out, outLen);
}

// Verified live 2026-10-05 (match 18:48, Derelict IVN): the client's pick
// reached the host ("ServerSpawnNearActor | Could not Set the active Loadout
// ..."), so the host knew WHICH precast was wanted, but every one of that
// match's FindLoadoutByID misses failed to build the player's fit because
// PlayerPIDForManager came up empty -- loadoutManager+0xA8 (owner controller)
// was not resolvable at orbit time -- and the function exits silently, so
// /battle/loadout was never asked for. The same flow succeeded in the 19:09
// match minutes later, so this is timing/instance dependent, not data.
//
// Fallback: PlayersTick (team sync, 1 s) already finds every connected human
// controller and its DNPID for naming, so it remembers the pair here, and a
// failed owner lookup takes a remembered pid whose controller still holds a
// readable connection. Single-human matches are exact; in a multi-human match
// the owner-less manager cannot tell humans apart, so the first remembered
// pid is used -- wrong only if two humans are in-flight at once, which
// PLAYERS_PER_MATCH currently does not produce in the operator's environment.
//
// Forward declaration: the controller-pid cache is defined below (it belongs
// to ReportMatchResult); PlayersTick (team sync) fills it.
// Forward declaration: Defined below the controller-pid cache it falls back
// to.
static bool PlayerPIDForManagerRobust(void *mgr, char *out, size_t outLen);

static bool PlayerPIDForManager(void *mgr, char *out, size_t outLen) {
  out[0] = 0;
  __try {
    return PlayerPIDForController(*(uint8_t **)((uint8_t *)mgr + OFF_COMPONENT_OWNER),
                                  out, outLen);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// The loadouts each player picked this match, for the match result's ship XP
// (ReportMatchResult). Recorded when mmogbrain served the fit; only touched on
// the game thread (FindLoadoutByID and the end-of-match RPC both run there).
struct FlownShips {
  char pid[80];
  char ids[512]; // comma-separated loadout ids
};
static FlownShips g_flown[64];

// The pid each controller had when its loadout was served. FIXED 2026-09-27:
// the first live match (20:53 host) sent the eom stats and then reported
// nothing -- ReportMatchResult's only silent exit was reading DNPID from the
// connection URL, which had worked for the same player's loadouts minutes
// earlier. Why it failed at match end is not known; this cache is the
// fallback, and every exit now logs.
struct ControllerPID {
  void *pc;
  char pid[80];
};
static ControllerPID g_controllerPIDs[64];

static void RememberControllerPID(void *pc, const char *pid) {
  ControllerPID *slot = nullptr;
  for (auto &c : g_controllerPIDs) {
    if (c.pc == pc) { slot = &c; break; }
    if (!slot && !c.pc) slot = &c;
  }
  if (!slot)
    return;
  slot->pc = pc;
  strncpy_s(slot->pid, sizeof(slot->pid), pid, _TRUNCATE);
}

static bool RememberedControllerPID(void *pc, char *out, size_t outLen) {
  for (auto &c : g_controllerPIDs)
    if (c.pc && c.pc == pc) {
      strncpy_s(out, outLen, c.pid, _TRUNCATE);
      return out[0] != 0;
    }
  return false;
}

// Verified live 2026-10-05 (match 18:48, Derelict IVN): the client's pick
// reached the host ("ServerSpawnNearActor | Could not Set the active Loadout
// ..."), so the host knew WHICH precast was wanted, but every one of that
// match's FindLoadoutByID misses failed to build the player's fit because the
// owner controller read from loadoutManager+0xA8 could not answer DNPID at
// orbit time -- the exit was silent, /battle/loadout was never asked, the pawn
// spawned with no fit (invalid active weapon). The same flow succeeded at
// 19:09, so this is instance/timing dependent, not data.
//
// The fallback takes a remembered (pc, pid) pair -- PlayersTick (team sync,
// 1 s) remembers every connected human's DNPID for naming -- whose controller
// still re-reads DNPID from its own URL, so neither stale pointer is trusted.
// Single-human matches are exact; a multi-human match with an owner-less
// manager cannot tell humans apart here, but PLAYERS_PER_MATCH currently
// keeps concurrent humans out of this path.
static bool PlayerPIDForManagerRobust(void *mgr, char *out, size_t outLen) {
  if (PlayerPIDForManager(mgr, out, outLen))
    return true;
  for (auto &c : g_controllerPIDs) {
    if (!c.pid[0] || !c.pc || !IsReadable(c.pc, OFF_PC_NETCONNECTION + 8))
      continue;
    if (!PlayerPIDForController(reinterpret_cast<uint8_t *>(c.pc), out, outLen))
      continue;
    // Re-read answered: trust the live URL text over the remembered copy.
    return out[0] != 0;
  }
  return false;
}

static void RecordFlownShip(const char *pid, const char *id) {
  FlownShips *slot = nullptr;
  for (auto &f : g_flown) {
    if (strcmp(f.pid, pid) == 0) { slot = &f; break; }
    if (!slot && !f.pid[0]) slot = &f;
  }
  if (!slot)
    return;
  if (!slot->pid[0])
    strncpy_s(slot->pid, sizeof(slot->pid), pid, _TRUNCATE);
  for (const char *p = strstr(slot->ids, id); p; p = strstr(p + 1, id)) {
    size_t n = strlen(id);
    if ((p == slot->ids || p[-1] == ',') && (p[n] == 0 || p[n] == ','))
      return; // already recorded
  }
  if (slot->ids[0])
    strncat_s(slot->ids, sizeof(slot->ids), ",", _TRUNCATE);
  strncat_s(slot->ids, sizeof(slot->ids), id, _TRUNCATE);
}

static void ClearFlownShips(const char *pid) {
  for (auto &f : g_flown)
    if (f.pid[0] && strcmp(f.pid, pid) == 0)
      f.ids[0] = 0;
}

static const char *FlownShipsFor(const char *pid) {
  for (auto &f : g_flown)
    if (f.pid[0] && strcmp(f.pid, pid) == 0)
      return f.ids;
  return "";
}

// Minimal HTTP/1.0 GET on loopback. Returns the body, or "" on any failure.
// HttpGetLoopbackRaw fetches pathAndQuery into buf and returns where the body
// starts and how long it is. Binary-safe: the body may contain NULs (the
// scoring table is a binary mmog document).
//
// timeoutMs bounds every send/recv on the socket, so it is also how long the
// *caller's thread* can be held when mmogbrain is slow, locked or restarting.
// It defaults to the 2 s the callers were written against; callers on the game
// thread that can afford to miss a beat pass something smaller. (2026-10-17,
// round 12/13: 2 s on the game thread was found to be the freeze, not the
// work -- the snapshot post moved to a worker thread and the /battle/commands
// poll below asks for 300 ms, because a missed poll is simply retried next
// tick while a blocked game thread drops frames for every player.)
static bool HttpGetLoopbackRaw(const char *pathAndQuery, char *buf, int bufLen,
                               const char **bodyOut, int *bodyLenOut,
                               const char *postBody = nullptr, int postLen = 0,
                               int timeoutMs = 2000) {
  static bool s_wsa = false;
  if (!s_wsa) {
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0)
      return false;
    s_wsa = true;
  }
  char host[64] = "127.0.0.1";
  int port = 8083;
  char env[80];
  DWORD n = GetEnvironmentVariableA("DN_MMOG_HTTP", env, sizeof(env));
  if (n > 0 && n < sizeof(env)) { // "host:port"
    char *colon = strrchr(env, ':');
    if (colon) {
      *colon = 0;
      strncpy_s(host, sizeof(host), env, _TRUNCATE);
      port = atoi(colon + 1);
    }
  }
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET)
    return false;
  DWORD tmo = (DWORD)(timeoutMs > 0 ? timeoutMs : 1); // 0 means "no timeout" to setsockopt, never what a caller here wants
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof(tmo));
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons((u_short)port);
  inet_pton(AF_INET, host, &a.sin_addr);
  if (connect(s, (sockaddr *)&a, sizeof(a)) != 0) {
    closesocket(s);
    return false;
  }
  char req[4096];
  int rl = postBody ? _snprintf_s(req, sizeof(req), _TRUNCATE,
                                  "POST %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n"
                                  "Content-Type: text/plain\r\nContent-Length: %d\r\n\r\n",
                                  pathAndQuery, host, postLen)
                    : _snprintf_s(req, sizeof(req), _TRUNCATE,
                                  "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                                  pathAndQuery, host);
  if (rl <= 0 || send(s, req, rl, 0) != rl || (postBody && postLen > 0 && send(s, postBody, postLen, 0) != postLen)) {
    closesocket(s);
    return false;
  }
  int total = 0, got;
  while (total < bufLen - 1 && (got = recv(s, buf + total, bufLen - 1 - total, 0)) > 0)
    total += got;
  closesocket(s);
  buf[total] = 0;
  if (total < 12 || strncmp(buf, "HTTP/1.", 7) != 0 || strncmp(buf + 9, "200", 3) != 0)
    return false;
  const char *b = strstr(buf, "\r\n\r\n");
  if (!b)
    return false;
  *bodyOut = b + 4;
  *bodyLenOut = total - (int)(b + 4 - buf);
  return true;
}

static bool HttpGetLoopback(const char *pathAndQuery, char *body, size_t bodyLen) {
  body[0] = 0;
  static char buf[8192];
  const char *b;
  int n;
  if (!HttpGetLoopbackRaw(pathAndQuery, buf, (int)sizeof(buf), &b, &n))
    return false;
  strncpy_s(body, bodyLen, b, _TRUNCATE);
  return true;
}

static const char *FieldValue(const char *body, const char *key, char *out, size_t outLen) {
  size_t kl = strlen(key);
  for (const char *line = body; line && *line;) {
    const char *end = strchr(line, '\n');
    size_t len = end ? (size_t)(end - line) : strlen(line);
    if (len > kl && strncmp(line, key, kl) == 0 && line[kl] == '=') {
      size_t vl = len - kl - 1;
      if (vl >= outLen)
        vl = outLen - 1;
      memcpy(out, line + kl + 1, vl);
      out[vl] = 0;
      return out;
    }
    line = end ? end + 1 : nullptr;
  }
  out[0] = 0;
  return nullptr;
}

static int ParseInts(const char *csv, int32_t *out, int maxN) {
  int n = 0;
  while (csv && *csv && n < maxN) {
    out[n++] = (int32_t)strtol(csv, nullptr, 10);
    const char *c = strchr(csv, ',');
    csv = c ? c + 1 : nullptr;
  }
  return n;
}

static bool SwitchOn(const char *envName, const char *markerFile); // first use is in here

static bool RegisterPlayerLoadout(void *mgr, void **id, const char *idText) {
  if (!PlayerLoadoutsEnabled() || !idText) {
    static bool s_offLogged = false;
    if (!s_offLogged) {
      s_offLogged = true; // once, or every miss spams -- the switch is static
      Logf("player loadouts switch is OFF and a loadout lookup missed; the "
           "player's fit cannot be served");
    }
    return false;
  }
  char pid[80];
  if (!PlayerPIDForManagerRobust(mgr, pid, sizeof(pid))) {
    // VERIFIED LIVE 2026-10-05 18:48 match: this exit used to be silent, so a
    // miss whose owner lookup failed produced no log at all and the pawn
    // spawned with "Could not Set the active Loadout. Given loadout ID does
    // not exist in loadout manager!" + an invalid active weapon. Never silent.
    Logf("player loadout: %s -- no readable pid from this manager (host local "
         "player, or owner +0xA8 not assigned yet)", idText);
    return false;
  }

  char path[512], body[4096];
  _snprintf_s(path, sizeof(path), _TRUNCATE, "/battle/loadout?pid=%s&id=%s", pid, idText);
  if (!HttpGetLoopback(path, body, sizeof(body))) {
    Logf("player loadout: %s for %s -- mmogbrain has no record (or unreachable); falling back to the default precast",
         idText, pid);
    return false;
  }
  RecordFlownShip(pid, idText);
  __try {
    RememberControllerPID(*(void **)((uint8_t *)mgr + OFF_COMPONENT_OWNER), pid);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }

  char v[512];
  static wchar_t nameW[128], displayW[512];
  ShipImportInfoMin info = {};
  info.loadoutID = *(FNameMin *)id;
  static wchar_t pidW[80];
  MultiByteToWideChar(CP_UTF8, 0, pid, -1, pidW, 80);
  ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&info.pid, pidW, 1 /* FNAME_Add */);
  info.precastID = (int32_t)strtol(FieldValue(body, "precast", v, sizeof(v)) ? v : "0", nullptr, 10);
  info.shipClass = (int32_t)strtol(FieldValue(body, "class", v, sizeof(v)) ? v : "0", nullptr, 10);
  FieldValue(body, "name", v, sizeof(v));
  int nl = MultiByteToWideChar(CP_UTF8, 0, v, -1, nameW, 128);
  info.name = {nameW, nl > 0 ? nl : 1, 128};
  FieldValue(body, "display", v, sizeof(v));
  int dl = MultiByteToWideChar(CP_UTF8, 0, v, -1, displayW, 512);
  info.display = {displayW, dl > 0 ? dl : 1, 512};
  // Same shapes 0x34E550 builds: 3 weapon entries (2 slots + a trailing 0),
  // 4 abilities, 4 perks, positional.
  static int32_t weapons[3], abilities[4], perks[4];
  memset(weapons, 0, sizeof(weapons));
  memset(abilities, 0, sizeof(abilities));
  memset(perks, 0, sizeof(perks));
  ParseInts(FieldValue(body, "weapons", v, sizeof(v)), weapons, 2);
  ParseInts(FieldValue(body, "abilities", v, sizeof(v)), abilities, 4);
  ParseInts(FieldValue(body, "perks", v, sizeof(v)), perks, 4);
  info.weapons = {weapons, 3, 3};
  info.abilities = {abilities, 4, 4};
  info.perks = {perks, 4, 4};

  uint8_t *obj = nullptr;
  __try {
    void *cls = ((tStaticClass)(g_base + RVA_YSHIPLOADOUT_STATICCLASS))();
    obj = (uint8_t *)((tStaticConstructObject)(g_base + RVA_STATIC_CONSTRUCT_OBJECT))(
        cls, mgr, 0, 0, 0, nullptr, false, nullptr);
    if (!obj)
      return false;
    SnapshotRegion(obj, g_snapshotObj); // diff base for init
    uint8_t *owner = *(uint8_t **)((uint8_t *)mgr + OFF_COMPONENT_OWNER);
    void *ownerArg = IsReadable(owner, OFF_OWNER_INIT_ARG + 8)
                         ? *(void **)(owner + OFF_OWNER_INIT_ARG) : nullptr;
    ((tLoadoutInitFromInfo)(g_base + RVA_LOADOUT_INIT_FROM_INFO))(obj, ownerArg, &info);
    LogRegionDiff(obj, g_snapshotObj, "obj", "after-init");
    SnapshotRegion(obj, g_snapshotObj); // diff base re-taken after init
    // m_cachedInitializationData, copied field by field as the client does
    // (engine allocations for the strings and arrays; ours stay ours).
    tAssign fstr = (tAssign)(g_base + RVA_FSTRING_ASSIGN);
    tAssign tarr = (tAssign)(g_base + RVA_TARRAY_INT_ASSIGN);
    *(FNameMin *)(obj + 0x38) = info.loadoutID;
    *(FNameMin *)(obj + 0x40) = info.pid;
    *(int32_t *)(obj + 0x48) = info.precastID;
    fstr(obj + 0x50, &info.name);
    *(int32_t *)(obj + 0x60) = info.shipClass;
    fstr(obj + 0x68, &info.display);
    tarr(obj + 0x78, &info.weapons);
    tarr(obj + 0x88, &info.abilities);
    tarr(obj + 0x98, &info.perks);
    *(uint16_t *)(obj + 0x1B1) = 0x0101;
    // Live appearance: the engine call applied below is what makes remote
    // clients render the customised look -- cosmetics proven applied by the
    // 18:41/19:41 runs with cosmetics present, S70's off-switch comparison.
    // NOTE (S71 retraction): the belief that it parses the display into a
    // container at this+0x2C0 holds for the CALL but the RECORD stores never
    // land: the gate at 0x422262 (`slotByte >= container Num -> skip`) sees
    // Num=0 on every fresh host-built loadout, so all stores drop and
    // +0x2C0 {Data,Num} stays {NULL,0}. What a successful import DOES do is
    // write four catalog-object pointers to container+0x10..+0x28
    // (obj+0x2D0..0x2E8, one per ';'-group via lookups 0x3E1630/0x3E1C30/
    // 0x3E1F30/0x3E1330 off the singleton at 0x1402CFF20).
    //
    // STORED-OBJECT LIFETIME (S74, replaces the S72 one-import guard): a 2nd
    // successful import in the same battle died 5/5 (17:41...20:06 runs),
    // 1-success survived 3/3 (17:31, 18:41, 20:16 build-B full match) -- the
    // 20:06 sweep crash dereferenced byte-identically the pointer import #1
    // had stored at its container+0x10 (rcx=0x7F6131429F00), i.e. an
    // unrooted stored object was collected/freed while the container still
    // referenced it. The import impl itself (0x422400-0x42248B) marks
    // NOTHING, but the client's DEFAULT-appearance constructor 0x4224E0
    // marks the object it stores at +0x2D8 RF_Standalone
    // (0x140422562 `or DWORD PTR [rax+8],0x40` -- UObject+0x8 ObjectFlags).
    // Client-built loadouts keep appearance objects alive through their GC
    // edges; our host-built loadout does not, so we now replicate the
    // engine's own mechanism: after each successful import mark the four
    // stored pointers Standalone. Cost if redundant: a bounded leak (4 objs
    // x imports per match). Build B fallback (latch) in git 5418f22.
    // REMOVED 2026-10-13 (S79): the import call that stood here was memory
    // corruption, and the cause of the battle-host spawn crash (render-thread
    // AV at 0x1CA7BD6 reading 0xFFFFFFFF, S75/S77). 0x422400 is a method of
    // UYShipCustomizationComponent, not UYShipLoadout: the registration block
    // above takes its class from 0x614D90 ("YShipCustomizationComponent",
    // GetPrivateStaticClassBody InSize 0x320 at 0x614E0E), while UYShipLoadout
    // is 0x1B8 bytes (InSize at 0x614EA7). The call parsed into "this+0x2C0"
    // and stored four pointers at +0x2D0..+0x2E8 -- 0x118..0x138 bytes past the
    // end of our loadout, into whatever heap block followed it. Evidence: in
    // every crashing sweep run the pre-import words there were the neighbour's
    // FFFFFFFFFFFFFFFF / 00000000FFFFFFFF (nothing of ours writes them; init
    // leaves +0x1B8.. untouched), in every clean run zeros (7/7 runs,
    // sweeps 10-12 23:17 and 10-13 06:10); import off: 5/5 clean (10-12 22:49).
    // So the S72 one-import guard, the S74 "second import frees a stored
    // object" lifetime story and the Standalone marking (S77 leg 3: still
    // crashed with it off) were all readings of this one overwrite.
    // The display string still reaches the engine through
    // m_cachedInitializationData (+0x68, set above). Whether remote clients
    // render a host-built loadout's look from it is NOT verified; the earlier
    // "cosmetics proven applied" (18:41/19:41) came with the overwrite active.
    // A correct live import needs the pawn's customization component as
    // `this`, at the point the client would import -- not this object.
    //
    // Loadout-side import (S79, OFF unless dn_host_loadout_import.txt /
    // DN_HOST_LOADOUT_IMPORT=1): the client's own UYShipLoadout import from a
    // display string is 0x35AB00 (loadout, const FString&) -- it copies the
    // string and calls the appearance parser 0x421FE0 on `this+0x108`
    // (`lea rcx,[rdi+0x108]` at 0x35AB50). +0x108 is what the ship actor
    // replicates: 0x594B40 -> builder 0x56B700 fills actor+0x920 from
    // 0x345FC0(loadout) = export 0x41C980(loadout+0x108, ...), and
    // 0x57E89C imports actor+0x920 into the pawn's customization component.
    // The +0x68 cached display set above is NOT on that path. Struct bound:
    // component+0x2C0 holds the same type in a 0x320 object, so it is
    // <= 0x60 bytes; +0x108+0x60 = 0x168 < 0x1B8 (inside our loadout).
    // NOT verified: that a remote client renders the look (needs 2 players).
    if (SwitchOn("DN_HOST_LOADOUT_IMPORT", "dn_host_loadout_import.txt") &&
        info.display.num > 1) {
      uint64_t before[12];
      memcpy(before, obj + 0x108, sizeof before);
      ((tImportDisplayReal)(g_base + RVA_LOADOUT_IMPORT_FROM_DISPLAY))(
          obj, &info.display);
      const uint64_t *after = (const uint64_t *)(obj + 0x108);
      for (int i = 0; i < 12; i++)
        if (before[i] != after[i])
          Logf("player loadout import: +0x%X %016llX -> %016llX",
               0x108 + i * 8, (unsigned long long)before[i],
               (unsigned long long)after[i]);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("player loadout: EXCEPTION building %s for %s", idText, pid);
    return false;
  }

  if (g_origFindLoadoutByID(mgr, (void **)(obj + 0xB0), 0))
    return true; // already there (the id matched an existing entry)
  bool ok = AddLoadoutGuarded(mgr, obj);
  Logf("player loadout: %s for %s -> precast %d, display \"%ls\", weapons %d/%d, abilities %d/%d/%d/%d -> %s",
       idText, pid, info.precastID, displayW, weapons[0], weapons[1],
       abilities[0], abilities[1], abilities[2], abilities[3],
       ok ? "registered" : "EXCEPTION");
  return ok;
}

static void *__fastcall HookFindLoadoutByID(void *mgr, void **id,
                                            uint8_t warn) {
  if (!g_origFindLoadoutByID)
    return nullptr;

  void *found = g_origFindLoadoutByID(mgr, id, warn);

  // The engine answered. Never second-guess it -- this is what keeps the hook
  // correct if a real backend ever populates the manager.
  if (found || !mgr || !id)
    return found;

  // Registering calls back into the manager, which re-enters this hook. The
  // per-manager guard in RegisterPrecastLoadouts already terminates that; this
  // makes the re-entrancy explicit rather than incidental.
  static thread_local bool s_inRetry = false;
  if (s_inRetry)
    return found;
  s_inRetry = true;

  static int s_logged = 0;
  bool verbose = (s_logged++ < 8);

  // Print the id as text as well as hex. The hex alone is not enough to tell a
  // successful lookup from a lookup of the wrong thing: two runs in which the
  // player picked visibly different hulls both logged 0x21F0F, and "FOUND"
  // looks identical either way. The name says which hull was actually asked
  // for, so a wrong hull shows up here rather than on the player's screen.
  uint64_t want = IsReadable(id, 8) ? *(uint64_t *)id : 0ull;
  const char *wantText = nullptr;
  if (IsReadable(id, 8))
    wantText = NameText(*(FNameMin *)id);

  // Order matters: the client picks by the precast's name, so the player's
  // own fit must be registered before any default precast could shadow it.
  void *retry = nullptr;
  if (RegisterPlayerLoadout(mgr, id, wantText))
    retry = g_origFindLoadoutByID(mgr, id, 0);
  // Otherwise that ship's default precast, on demand...
  if (!retry && RegisterPrecastOnDemand(mgr, wantText))
    retry = g_origFindLoadoutByID(mgr, id, 0);
  // ...and the original four-T1 set as the last resort.
  if (!retry) {
    RegisterPrecastLoadouts(mgr);
    retry = g_origFindLoadoutByID(mgr, id, 0);
  }

  s_inRetry = false;

  if (verbose)
    Logf("FindLoadoutByID miss for FName 0x%llX (%s) -> after registering: %s",
         (unsigned long long)want, wantText ? wantText : "<unresolved>",
         retry ? "FOUND" : "STILL MISSING");

  return retry;
}

// ---------------------------------------------------------------------------
// Gating and entry point
// ---------------------------------------------------------------------------

// A battle server, not a client. game-manager's spawner is the only thing that
// passes -MatchID=, so its presence identifies a headless host. A player's
// client never has it, which is what makes it safe for this DLL to sit in a
// directory both processes load from.
static bool IsBattleServer() {
  const wchar_t *cmd = GetCommandLineW();
  if (!cmd)
    return false;
  for (const wchar_t *p = cmd; *p; ++p) {
    if ((p[0] == L'-' || p[0] == L'/') && _wcsnicmp(p + 1, L"MatchID", 7) == 0)
      return true;
  }
  return false;
}

// Opt-in on the host. A marker file beside the executable survives however the
// operator starts the service; the environment variable is honoured too because
// dn-dedicated's spawner does inherit its environment (buildEnv, AGENT-CHAT
// S10.5).
// ---------------------------------------------------------------------------
// PostLogin: put a joining player straight into the arena
//
// OFF by default. This is a much larger behavioural change than the loadout
// registration above, and it removes a step players can see.
//
// Why it exists. Registering the precast loadouts got players a PAWN -- the host
// spawns them and SetYPawn assigns it -- but they still never reach the map,
// because the orbit teleport is gated:
//
//   FUN_3D92A0:  cmp byte ptr [rdx+0x948], 0 ; jne proceed
//                -> "Trying to teleport into level player %s that is not in orbit!"
//
// and 0x948 is AYPlayerReplicationInfo::m_highestFleetUnlocked, an EYFleetType
// from YMmogbrain_Structs.h. It is EYFT_None on a host that never logged in, and
// no payload we can send changes that, because the host holds no mmogbrain data
// (AGENT-CHAT S39, S40).
//
// dread-sdk's server mod does not satisfy that gate. It skips the orbit flow
// entirely: hook PostLogin, set the controller's active loadout, and call the
// engine's own ServerRestartPlayer(), which asks the GameMode for a PlayerStart
// and spawns there. That path never enters UYPlayerOrbitComponent, never reads
// m_highestFleetUnlocked, and never needs the GameState readiness mask. It is a
// proven route -- our operator has played matches with it (S42).
//
// What it costs, stated plainly: the player no longer picks a ship in orbit.
// Everyone spawns in one configured hull. That is a real regression in
// behaviour, and it is why this is opt-in and separate from the loadout fix.
// Prefer the orbit path if it can ever be made to work.
//
// How it hooks. Every reflected call goes through UObject::ProcessEvent, vtable
// index 0x35 on this build (dread-sdk resolves it the same way). Hooking there
// costs one pointer comparison per reflected call: the UFunction objects are
// resolved ONCE at install and the hook compares pointers, never strings.
// ---------------------------------------------------------------------------

#define VF_PROCESS_EVENT 0x35

// UYLoadoutManagerComponent::m_activeLoadout, from the SDK dump
// (DreadGame_Classes.h: "class UYShipLoadout* m_activeLoadout; // 0x0208").
#define OFF_ACTIVE_LOADOUT 0x208

typedef void *(__fastcall *tProcessEvent)(void *object, void *function,
                                          void *params);
static tProcessEvent g_origProcessEvent = nullptr;

static void *g_fnK2PostLogin = nullptr;
static void *g_fnGetLoadoutManager = nullptr;
static void *g_fnServerRestartPlayer = nullptr;
static void *g_fnServerReadyForJoining = nullptr;
static void *g_fnServerSpawnNearActor = nullptr;
static void *g_fnServerPlayerReadyUp = nullptr;
static bool g_postLoginArmed = false;
static bool g_fleetTierArmed = false;
static bool g_spawnArmed = false;

// Which of the four precast loadouts everyone spawns in. Index into
// kPrecastPaths / g_precastCDO; 0 is the Assault Medium T1.
static int g_postLoginLoadoutIndex = 0;

// FindUObjectByName walks GObjects for an object whose FName text CONTAINS the
// wanted text.
//
// Substring, not equality. The first version matched "K2_PostLogin" exactly and
// found nothing on a live host; dread-sdk matches
// GetFullName().find("PostLogin"), which also catches a plain "PostLogin" and
// any Blueprint variant. Equality was a guess about which of those this build
// creates, and it was wrong.
//
// Names are not unique across classes, so the caller gets the FIRST match and
// the outer is logged. A wrong pick shows up in the log rather than silently.
static void *FindUObjectByName(const char *want, const char **outerOut) {
  FUObjectArrayMin *arr = GObjects();
  if (!IsReadable(arr, sizeof(*arr)) || !IsReadable(arr->Objects, sizeof(FUObjectItemMin)))
    return nullptr;

  int count = arr->NumElements;
  if (count < 0 || count > 20000000)
    return nullptr;

  for (int i = 0; i < count; ++i) {
    FUObjectItemMin *item = &arr->Objects[i];
    if (!IsReadable(item, sizeof(*item)))
      continue;
    UObjectMin *obj = item->Object;
    if (!IsReadable(obj, sizeof(*obj)))
      continue;
    const char *text = NameText(obj->Name);
    if (!text || !strstr(text, want))
      continue;
    if (outerOut) {
      *outerOut = nullptr;
      if (IsReadable(obj->Outer, sizeof(UObjectMin)))
        *outerOut = NameText(obj->Outer->Name);
    }
    return obj;
  }
  return nullptr;
}

// AController::PlayerState, from the SDK dump (Engine_Classes.h:797).
#define OFF_PLAYERSTATE 0x3E0
// DISPROVED 2026-09-24 -- read before using anything in this block.
//
// The orbit gate does NOT read the PlayerState. TeleportPlayerIntoLevel
// (0x3D92A0) is called from TeleportPlayersFromOrbit (call at 0x3838D1) with an
// element of a weak-pointer array that passed an IsA check, and its null branch
// logs "Trying to teleport into level a null YPawn!" -- the object is the PAWN,
// and +0x948 on AYPawn is a native, unreflected field (the SDK has no member
// there), i.e. the pawn's own in-orbit state. The SDK's
// AYPlayerReplicationInfo::m_highestFleetUnlocked shares the offset by
// coincidence; the real PlayerState constructor (0x5A8820, reached from the
// class thunk at 0x5CE5D0) already sets it to Recruit.
//
// So the TeleportPlayerIntoLevel write this block fed forced the pawn's orbit
// flag -- the "fake the gate" the repo rule warns about, and the operator
// rejected it. The real cause is the net mode: see HookGetNetMode. The fleet
// tier feature is therefore disabled (g_fleetTierArmed is never set) and kept
// only as a record.
//
// Original text: AYPlayerReplicationInfo::m_highestFleetUnlocked
// (DreadGame_Classes.h:1920), an EYFleetType from YMmogbrain_Structs.h:
// None=0 Recruit=1 Veteran=2 Legendary=3.
#define OFF_HIGHEST_FLEET 0x948
#define EYFT_NONE 0
#define EYFT_RECRUIT 1

// EnsureFleetTier gives a player the Recruit tier when the host has none.
//
// This is the byte the orbit teleport is gated on:
//
//   FUN_3D92A0: cmp byte ptr [rdx+0x948], 0 ; jne proceed
//               -> "Trying to teleport into level player %s that is not in orbit!"
//
// It is EYFT_None on a host that never logged in, so nobody is ever teleported
// and the client sits in the orbit screen. Measured, and the reason the
// post-login spawn below is not sufficient on its own: the server spawned FOUR
// pawns for the player and the CLIENT still stayed in orbit, because the only
// thing that takes a client out of orbit is the teleport.
//
// Why Recruit, and why this is not fabrication. The tier is real backend data:
// the engine computes it from the YMmogbrain module (FUN_3A5831, which logs
// "EYFleetType::EYFT_Recruit: no FleetType override - FleetTier=%d") and cannot
// here, because the host holds no mmogbrain data. Recruit is the floor -- what
// a player who owns any fleet at all has unlocked, and every player who reaches
// a battle server owns one. It is the value a logged-in host would have had.
//
// The honest limit: Veteran and Legendary players are under-reported. If a real
// tier ever reaches the host this must defer to it, which is what the guard
// below does.
//
// EVERY path here logs. The first version returned silently on three of them,
// and when the tier did not get written there was no way to tell which -- a
// default indistinguishable from a real result, which is the exact trap
// CONTRIBUTING.md warns about. Capped so a busy match cannot flood the log.
// EnsureFleetTierOnPlayerState is the half that does the work, split out
// because the teleport hook already HAS the PlayerState -- it is the argument
// the gate is read from -- and must not go looking for one via a controller.
static void EnsureFleetTierOnPlayerState(void *ps, const char *where,
                                         bool verbose) {
  if (!ps) {
    if (verbose)
      Logf("fleet tier [%s]: NULL PlayerState", where);
    return;
  }
  if (!IsReadable(ps, OFF_HIGHEST_FLEET + 1)) {
    if (verbose)
      Logf("fleet tier [%s]: PlayerState %p not readable to +0x%X", where, ps,
           OFF_HIGHEST_FLEET);
    return;
  }

  uint8_t *tier = (uint8_t *)((uintptr_t)ps + OFF_HIGHEST_FLEET);
  uint8_t before = *tier;

  // Never overwrite a value the engine already has. Same rule the
  // FindLoadoutByID hook follows -- if the engine answered, do not second-guess
  // it -- and it is what keeps this correct if a real tier ever arrives.
  if (before != EYFT_NONE) {
    if (verbose)
      Logf("fleet tier [%s]: PlayerState %p already reads %u, leaving it", where,
           ps, (unsigned)before);
    return;
  }

  *tier = EYFT_RECRUIT;

  if (verbose)
    Logf("fleet tier [%s]: PlayerState %p EYFT_None -> EYFT_Recruit, reads "
         "back %u",
         where, ps, (unsigned)*tier);
}

static void EnsureFleetTier(void *pc, const char *where) {
  if (!g_fleetTierArmed)
    return;

  static int s_logged = 0;
  bool verbose = (s_logged++ < 24);

  if (!IsReadable(pc, OFF_PLAYERSTATE + sizeof(void *))) {
    if (verbose)
      Logf("fleet tier [%s]: controller %p not readable to +0x%X", where, pc,
           OFF_PLAYERSTATE);
    return;
  }

  void *ps = *(void **)((uintptr_t)pc + OFF_PLAYERSTATE);
  if (!ps) {
    if (verbose)
      Logf("fleet tier [%s]: controller %p has a NULL PlayerState (+0x%X) -- "
           "too early, the engine has not created it yet",
           where, pc, OFF_PLAYERSTATE);
    return;
  }
  EnsureFleetTierOnPlayerState(ps, where, verbose);
}

// SpawnJoiningPlayer runs after the engine's own PostLogin has finished.
static void SpawnJoiningPlayer(void *params) {
  if (!IsReadable(params, sizeof(void *)))
    return;

  // AGameMode_K2_PostLogin_Params is a single APlayerController* at +0x00.
  void *pc = *(void **)params;
  if (!IsReadable(pc, 0x200)) {
    Logf("post-login: NewPlayer is not readable, skipping");
    return;
  }

  // The tier first. It is what lets the NORMAL orbit flow finish, and it is
  // useful with or without the spawn below -- which is why the two are
  // separately switchable. Tier alone is the better outcome: the player still
  // picks a ship in orbit.
  //
  // PostLogin may be too early: the engine creates the PlayerState in
  // InitPlayerState, and for a networked join that can land after this. The
  // later trigger points in HookProcessEvent are what actually catch it; this
  // one is kept because when it does work it is the earliest.
  EnsureFleetTier(pc, "PostLogin");

  if (!g_spawnArmed)
    return;

  // GetLoadoutManager() is a UFunction returning UYLoadoutManagerComponent*.
  // Calling it through ProcessEvent avoids needing another hardcoded RVA.
  struct {
    void *ReturnValue;
  } gp = {};
  g_origProcessEvent(pc, g_fnGetLoadoutManager, &gp);

  void *mgr = gp.ReturnValue;
  if (!IsReadable(mgr, OFF_ACTIVE_LOADOUT + sizeof(void *))) {
    Logf("post-login: controller %p has no readable loadout manager, skipping",
         pc);
    return;
  }

  // Same registration the FindLoadoutByID path uses, so the manager holds real
  // loadouts before one is made active. Idempotent per manager.
  RegisterPrecastLoadouts(mgr);
  if (g_precastResolved == 0) {
    Logf("post-login: no precast loadouts resolved, skipping");
    return;
  }

  int idx = g_postLoginLoadoutIndex;
  if (idx < 0 || idx >= g_precastResolved)
    idx = 0;
  void *loadout = g_precastCDO[idx];
  if (!IsReadable(loadout, sizeof(void *))) {
    Logf("post-login: precast %d is not readable, skipping", idx);
    return;
  }

  *(void **)((uintptr_t)mgr + OFF_ACTIVE_LOADOUT) = loadout;

  // The engine's own respawn. It asks the GameMode for a PlayerStart and
  // spawns there -- no orbit, no readiness mask, no fleet tier.
  g_origProcessEvent(pc, g_fnServerRestartPlayer, nullptr);

  Logf("post-login: controller %p -> active loadout %s (%p), ServerRestartPlayer called",
       pc, kPrecastLabels[idx], loadout);
}

// Team sync (on unless dn_host_no_team_sync.txt / DN_HOST_NO_TEAM_SYNC=1),
// see PlayersTick.
static bool g_teamSyncArmed = false;

// Havoc event trace (on in Havoc unless DN_HOST_NO_PE_TRACE=1 /
// dn_host_no_pe_trace.txt). Logs every ProcessEvent on the PVE_HordeManager_BP
// actor and the Havoc game state -- function, first param, native caller RVA.
// Why: on every wave announcement the players' view snapped to the ship spawn
// point (live 2026-10-13/14). PVE_HordeManager_BP.OnPlayerSpawnedInArena
// (native-called BP event) runs TeleportPlayerReplicated(pawn,
// TargetPointPlayerN) -- reflect flags 0C0040C0 = reliable NetMulticast,
// K2_TeleportTo with the target point's LOCATION AND ROTATION on every
// client. The static caller of OnPlayerSpawnedInArena was not found (its
// FName global 0x3E107E8 has no rip ref besides its initializer), so this
// says who fires it and when. Objects are set by HavocDirectorTick at start.
static const char *ClassNameOf(void *obj);
static bool g_peTraceArmed = false;
static void *volatile g_peTraceObj[2] = {nullptr, nullptr};

// The function whose ProcessEvent is on this thread's stack right now. Every
// script-to-script call re-enters ProcessEvent from the same VM thunk (caller
// RVA is always 0x172C024 -- measured 22:37 headless Havoc), so the return
// address names nothing; the enclosing UFunction does.
static thread_local void *t_peCurFn = nullptr;
static thread_local void *t_peCurObj = nullptr;

static void TraceProcessEvent(void *object, void *function, void *params, void *caller) {
  static int s_lines = 0;
  if (s_lines >= 400) // a few waves' worth; never floods a long match
    return;
  const char *fn = IsReadable(function, sizeof(UObjectMin)) ? NameText(((UObjectMin *)function)->Name) : nullptr;
  if (!fn || !strncmp(fn, "ReceiveTick", 11) || !strcmp(fn, "Tick"))
    return;
  ++s_lines;
  void *p0 = params && IsReadable(params, 8) ? *(void **)params : nullptr;
  bool p0obj = p0 && IsReadable(p0, sizeof(UObjectMin)) && IsReadable(((UObjectMin *)p0)->Class, sizeof(UObjectMin));
  const char *pfn = t_peCurFn && IsReadable(t_peCurFn, sizeof(UObjectMin)) ? NameText(((UObjectMin *)t_peCurFn)->Name) : nullptr;
  Logf("pe trace: %s.%s p0 %p%s%s from %s.%s caller RVA 0x%llX", ClassNameOf(object), fn, p0, p0obj ? " " : "",
       p0obj ? ClassNameOf(p0) : "", t_peCurObj ? ClassNameOf(t_peCurObj) : "-", pfn ? pfn : "-",
       (unsigned long long)((uintptr_t)caller - (uintptr_t)GetModuleHandleW(nullptr)));
  // MulticastShowHUDMarkerOnActor(byte markerType @0, AActor *actor @8): the
  // horde manager fires it at every wave start (live log 2026-10-15 20:55:28,
  // type 0x0A). Native impl 0x3A0F40 (gs vt+0x818, exec thunk 0x761610) only
  // builds a marker {weak actor, actor name} for the local HUD (0x55A480) --
  // no camera call seen there. Name the actor so the wave-start snap can be
  // matched to it.
  if (!strcmp(fn, "MulticastShowHUDMarkerOnActor") && IsReadable(params, 16)) {
    void *a = *(void **)((char *)params + 8);
    bool ok = a && IsReadable(a, sizeof(UObjectMin)) && IsReadable(((UObjectMin *)a)->Class, sizeof(UObjectMin));
    Logf("pe trace:   marker type %u actor %p %s %s", *(unsigned char *)params, a, ok ? ClassNameOf(a) : "-",
         ok ? NameText(((UObjectMin *)a)->Name) : "-");
  }
}

static volatile bool g_terrainFine = false; // fine phase running, frame ticks wanted
static uint8_t *g_terrainGm = nullptr;
static DWORD g_gameThreadId = 0;
static void FrameTick();
static volatile bool g_frameWanted = false;
static thread_local void *t_peOuterSp = nullptr;
static volatile long g_peStaleResets = 0;
// FrameTick's frame while it runs (its re-entry latch), null otherwise. A
// fault the engine catches above FrameTick unwinds it without the clear and
// would leave it busy for good; a ProcessEvent whose frame is above FrameTick's
// proves FrameTick is no longer on the stack.
// Not thread_local like t_peOuterSp: one latch, set and cleared on the game
// thread only (the clear tests the thread id); other threads just read it.
static void *volatile g_frameBusySp = nullptr;
static volatile long g_frameBusyResets = 0;
static double g_frameWorstMs = 0; // worst game-thread frame gap since the last heartbeat
static long g_frameHitches = 0;    // gaps over 100 ms since the last heartbeat
static volatile long g_frameTicks = 0, g_framePhase = 0; // heartbeat (CommandsTick logs them)
// Stutter log (operator 2026-10-22: "we need to log server stutters as best as
// we can ... so we can figure out what creates them"). Our own game-thread work
// runs in named zones (each CommandsTick job, each chat command); the last few
// that took 2 ms or more are kept with their times, so FrameTick can say, for a
// frame gap over 100 ms, how much of it was ours and whose. Every zone of
// 30 ms or more is logged on its own ("stutter-zone:"). mmogbrain's admin
// Stutters tab (admin_waves.go) reads the "stutter:" lines back.
struct ZoneRec { char name[48]; double start, end; };
static ZoneRec g_zoneRing[16];
static int g_zoneNext;
static char g_zoneName[48];
static double g_zoneStart;
static char g_lastCmd[64];
static double g_lastCmdAt;
static void ZoneEnd() {
  if (!g_zoneStart)
    return;
  double now = NowMs(), d = now - g_zoneStart;
  if (d >= 2.0) {
    ZoneRec &z = g_zoneRing[g_zoneNext++ & 15];
    memcpy(z.name, g_zoneName, sizeof z.name);
    z.start = g_zoneStart, z.end = now;
  }
  if (d >= 30.0)
    Logf("stutter-zone: %s took %.0f ms", g_zoneName, d);
  g_zoneStart = 0;
}
static void ZoneBegin(const char *name, const char *arg = nullptr) {
  ZoneEnd();
  snprintf(g_zoneName, sizeof g_zoneName, arg && *arg ? "%s:%s" : "%s", name, arg);
  g_zoneStart = NowMs();
}
#define ZONE(name, call) (ZoneBegin(name), (call), ZoneEnd())

static void *__fastcall HookProcessEvent(void *object, void *function,
                                         void *params) {
  if (g_peTraceArmed && object && (object == g_peTraceObj[0] || object == g_peTraceObj[1]))
    TraceProcessEvent(object, function, params, __builtin_return_address(0));
  // A __try that catches a fault inside a nested ProcessEvent unwinds past
  // this frame without the restore below, leaving t_peCurFn set for good, and
  // then no call is ever outermost (FrameTick never ran: 0 ticks, 2026-10-17).
  // A call at or above the recorded outermost frame cannot be nested in it.
  void *sp = __builtin_frame_address(0);
  if (t_peCurFn && sp >= t_peOuterSp) {
    t_peCurFn = t_peCurObj = nullptr;
    ++g_peStaleResets;
  }
  if (g_frameBusySp && sp > g_frameBusySp && GetCurrentThreadId() == g_gameThreadId) {
    g_frameBusySp = nullptr;
    VqCacheOn(false);
    ++g_frameBusyResets;
  }
  void *prevFn = t_peCurFn, *prevObj = t_peCurObj;
  if (!prevFn)
    t_peOuterSp = sp;
  t_peCurFn = function;
  t_peCurObj = object;
  void *ret = g_origProcessEvent ? g_origProcessEvent(object, function, params)
                                 : nullptr;
  t_peCurFn = prevFn;
  t_peCurObj = prevObj;
  if (g_frameWanted && !prevFn)
    FrameTick();

  if (!g_postLoginArmed)
    return ret;

  // Later trigger points, all server RPCs on the PlayerController, all of which
  // route through ProcessEvent and all of which happen long after the
  // PlayerState exists. `object` is the controller.
  //
  // Several rather than one because PostLogin alone did not write the byte on a
  // live host and the silent version could not say why; these bracket the whole
  // orbit sequence, from picking a ship to readying up to the last event before
  // the teleport. Writing twice is free -- the second call sees a non-zero tier
  // and leaves it.
  if (function == g_fnServerReadyForJoining) {
    EnsureFleetTier(object, "ServerReadyForJoining");
    return ret;
  }
  if (function == g_fnServerSpawnNearActor) {
    EnsureFleetTier(object, "ServerSpawnNearActor");
    return ret;
  }
  if (function == g_fnServerPlayerReadyUp) {
    EnsureFleetTier(object, "ServerPlayerReadyUpForMatch");
    return ret;
  }

  // One pointer compare on the hot path. Everything else is behind it.
  if (function != g_fnK2PostLogin)
    return ret;

  // SpawnJoiningPlayer calls ProcessEvent twice, which re-enters this hook.
  // Neither call is K2_PostLogin, so the compare above already stops it; the
  // guard makes that explicit rather than incidental.
  static thread_local bool s_inSpawn = false;
  if (s_inSpawn)
    return ret;
  // AGameMode_K2_PostLogin_Params: APlayerController* NewPlayer at +0x00.
  s_inSpawn = true;
  SpawnJoiningPlayer(params);
  s_inSpawn = false;

  return ret;
}

// ---------------------------------------------------------------------------
// Dedicated net mode: let the game's own dedicated-server path run the orbit
//
// Why players never leave orbit (verified 2026-09-24 by disassembly and the
// full battle-server log run/battle-logs/battle-20260923-204528-port7777.log):
//
//   - The teleport into the arena needs the pawn's in-orbit state, which the
//     orbit sequence sets only once the GameState readiness mask is complete
//     (AYGameState_MP+0x1D60 == 0xF, AGENT-CHAT S39).
//   - On a DEDICATED server the game completes that mask itself.
//     0x3ACA40, called from ServerReadyForJoining (0x592350) and
//     ClientLoadingCompleted (0x390970), does:
//         if Role == Authority:
//             if GetNetMode() == NM_DedicatedServer: set bits 0x1|0x2|0x4|0x8
//         set bit 0x10
//         if Role == Authority: StartOrbitTransition
//   - Our host is the shipping GAME exe, which is never dedicated: engine
//     PreInit sets GIsClient = 1 on every non-commandlet launch (0x228F71),
//     whatever the command line says -- which is why "-server" and dropping it
//     changed nothing (S43). So GetNetMode() answers 2 (listen), the mask stops
//     at 0x2 (only OrbitLevelReady), and TeleportPlayersFromOrbit logs "not in
//     orbit" for every player. The log shows ServerReadyForJoining and
//     StartOrbitTransition running, and none of the other three bits.
//
// The fix: a server net driver reports DEDICATED instead of LISTEN. Nothing is
// forced; the engine then takes the path the shipping servers took. Clients
// are untouched (their net driver is not a server, the original answers 3),
// and GIsClient itself is left alone, so client-only systems in this process
// keep the state they initialised with.
//
// Calls the original first and only changes a LISTEN answer, per the hooking
// checklist. Logs the first rewrites so the effect is visible.
typedef int(__fastcall *tGetNetMode)(void *netDriver);
static tGetNetMode g_origGetNetMode = nullptr;

static int __fastcall HookGetNetMode(void *netDriver) {
  int mode = g_origGetNetMode(netDriver);
  if (mode != NM_LISTEN_SERVER)
    return mode;
  static volatile LONG s_logged = 0;
  if (InterlockedIncrement(&s_logged) <= 3)
    Logf("net mode: net driver %p answered LISTEN (2); reporting DEDICATED (1) "
         "so the game's dedicated-server orbit path runs",
         netDriver);
  return NM_DEDICATED_SERVER;
}

// ---------------------------------------------------------------------------
// AI in the proving ground (dn_host_bc_ai.txt)
//
// Verified 2026-09-24 (disassembly + live memory of a BC host):
//   - Every NPC spawn path ends in game-mode virtual 0x9D0 (0x3678F0), which
//     fills the teams (0x381FA0 -> per-NPC 0x3671C0 -> StartCombat) only if
//     GameMode+0x961 m_enableSpawnAI or +0x962 is set. +0x962 is written only by
//     the cheat-manager commands SpawnAI / SpawnAITeams (0x32AC10, 0x32AD90).
//   - For the proving ground the natural caller is this timer, at 0x36A806:
//     m_enableSpawnAI && !GameState.m_isMatchStarted && m_remainingTime <= 50.
//   - Native constructors: Multiplayer 0x361DF0 writes 0x961 = 0, TrainingMatch
//     0x362840 writes 1. GameInfo_BC_BP does not override it, so on our host it
//     read 0 with 45 NPC entries loaded and nothing ever spawned.
//   - Writing the byte live (scratchpad bcpoke.py) made the game fill both teams
//     at the 50 s mark (T1 7 + the player, T2 8) and log "StartCombat - starting
//     combat"; the operator fought the bots.
//
// So this sets the designer switch the mode's own AI data is waiting for; the
// game does the rest. Limited to Bootcamp: PvP modes have no bots by design.
typedef void(__fastcall *tGameModeTimer)(void *gameMode);
static tGameModeTimer g_origGameModeTimer = nullptr;

static bool g_bcAIArmed = false;
static void PlayersTick(uint8_t *gm);
static void ClassTimeTick(uint8_t *gm);

// Bots in every multiplayer mode (on; dn_host_bots_bc_only.txt /
// DN_HOST_BOTS_BC_ONLY=1 limits them to the proving ground again). Added
// 2026-09-28 at the operator's request. The spawner only fills a team from the
// mode's own bot list (gm+0x968, count gm+0x970) up to its team size
// (gm+0x97C); whether a PvP map loads such a list is NOT known yet -- the
// once-per-game-mode log line below answers it for each mode.
#define OFF_GM_NPC_COUNT 0x970
#define OFF_GM_TEAM_SIZE 0x97C
static bool g_botsBCOnly = false;
static int g_botTier = 0; // /host bottier N (1-5), 0 = the game's own tier rules
static FNameMin g_botShip[3] = {}; // /host botship <0 all|1|2> <ShipId|off>; zero = the game's own pick
static char g_botShipStr[3][64] = {}; // the ShipIds as typed, resolved per game mode (NpcIdResolve)
static void *g_botShipGm = nullptr;   // game mode g_botShip was last resolved for

// The game mode's own bot ShipIds (the m_npcPlayers +0x10 lists), captured
// before botship or /spawn rewrite them. Only these have spawn info in the NPC
// data asset: any other id logs "No valid NPC spawn ID found" at the fill and
// the chooser 0x371B50 falls back to a random ship of the tier (TDM
// 2026-10-20, /spawn AssaultHeavyT4 -> SniperMediumT2; AssaultHeavyT4 is a
// Havoc/Horde scene id).
static void *g_npcIdsGm = nullptr;
static uint64_t g_npcIds[128];
static int g_npcIdCount = 0;

static void NpcIdsSnapshot(uint8_t *gm) {
  if (g_npcIdsGm == gm || !IsReadable(gm + 0x968, 16))
    return;
  uint8_t *l = *(uint8_t **)(gm + 0x968);
  int n = *(int32_t *)(gm + 0x970);
  if (!l || n <= 0)
    return;
  g_npcIdsGm = gm, g_npcIdCount = 0;
  for (int i = 0; i < n && i < 128 && IsReadable(l + i * 0x38, 0x38); ++i) {
    uint64_t *ids = *(uint64_t **)(l + i * 0x38 + 0x10);
    int k = *(int32_t *)(l + i * 0x38 + 0x18);
    for (int j = 0; ids && j < k && j < 64 && IsReadable(ids + j, 8); ++j) {
      bool dup = !ids[j];
      for (int m = 0; m < g_npcIdCount && !dup; ++m)
        dup = g_npcIds[m] == ids[j];
      if (!dup && g_npcIdCount < 128)
        g_npcIds[g_npcIdCount++] = ids[j];
    }
  }
  Logf("bots: %d distinct ShipIds in game mode %p's bot lists", g_npcIdCount, gm);
}

// `ship` if the game mode's bots can fly it, else the same class at the
// nearest tier (higher first: AssaultHeavyT4 -> T5, T3, T6, T2, ...). With no
// snapshot yet, `ship` as typed. False when nothing of that class is there.
// A Havoc boss id ("<Class><Weight>...Boss01", /boss) is in no PvP list: it
// becomes its class's T5, as the player-start spawner flies it.
static bool NpcIdFind(const char *ship, FNameMin *id, char *used, size_t cap) {
  snprintf(used, cap, "%s", ship);
  wchar_t w[64];
  swprintf(w, 64, L"%hs", ship);
  ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(id, w, 1 /* FNAME_Add */);
  if (!g_npcIdCount)
    return true;
  size_t len = strlen(ship), p = len;
  while (p > 0 && isdigit((unsigned char)ship[p - 1]))
    --p;
  int tier = p < len && p > 0 && (ship[p - 1] == 'T' || ship[p - 1] == 't') ? atoi(ship + p) : -1;
  for (int d = 0; d <= 6; ++d)
    for (int sgn = 1; sgn >= -1; sgn -= 2) {
      if ((d == 0 && sgn < 0) || (d > 0 && tier < 0))
        continue;
      int t = tier + sgn * d;
      if (d > 0 && (t < 1 || t > 6))
        continue;
      char cand[64];
      if (d == 0)
        snprintf(cand, sizeof cand, "%s", ship);
      else
        snprintf(cand, sizeof cand, "%.*s%d", (int)p, ship, t);
      FNameMin c = {};
      swprintf(w, 64, L"%hs", cand);
      ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&c, w, 0 /* FNAME_Find */);
      for (int m = 0; *(uint64_t *)&c && m < g_npcIdCount; ++m)
        if (g_npcIds[m] == *(uint64_t *)&c) {
          *id = c;
          snprintf(used, cap, "%s", cand);
          return true;
        }
    }
  return false;
}
// A variant id (Havoc's DreadnoughtHeavyNukerT5, a custom boss's ship) is in
// no PvP list either: botname2 (TDM, /boss 1 "Gorgon") logged "no ship of
// that class" and nothing spawned while the chat reply said sent. It falls
// back to the plain class at its tier, and with `swap` (an elite or custom
// boss) to another hull like a Boss01 id.
static bool NpcIdResolve(const char *ship, FNameMin *id, char *used, size_t cap, bool swap = false) {
  static const char *cs[] = {"Assault", "Dreadnought", "Scout", "Sniper", "Support"}, *ws[] = {"Heavy", "Medium", "Light"};
  const char *c0 = nullptr, *w0 = nullptr;
  for (const char *c : cs)
    for (const char *w : ws)
      if (!c0 && !strncmp(ship, c, strlen(c)) && !strncmp(ship + strlen(c), w, strlen(w)))
        c0 = c, w0 = w;
  bool boss = strstr(ship, "Boss") != nullptr;
  if (!boss && NpcIdFind(ship, id, used, cap))
    return true;
  if (!c0)
    return false;
  size_t len = strlen(ship), p = len;
  while (p > 0 && isdigit((unsigned char)ship[p - 1]))
    --p;
  int tier = boss ? 5 : p < len && p > 0 && ship[p - 1] == 'T' ? atoi(ship + p) : 5;
  char alt[48];
  snprintf(alt, sizeof alt, "%s%sT%d", c0, w0, tier);
  // Callers log the swap ("using <id>"); logging here doubled it.
  if (_stricmp(alt, ship) && NpcIdFind(alt, id, used, cap))
    return true;
  if (!boss && !swap)
    return false;
  // A mode's lists can lack whole classes (gamebot-pod1: PodTDM's 42
  // ShipIds have no ScoutHeavy, the /boss spawned nothing). A boss
  // keeps its name and elite stats on another hull: same weight
  // first, then any. The caller's "using <id>" line shows the swap.
  for (int pass = 0; pass < 2; ++pass)
    for (const char *c2 : cs)
      for (const char *w2 : ws) {
        if ((pass == 0) != (w2 == w0) || (c2 == c0 && w2 == w0))
          continue;
        snprintf(alt, sizeof alt, "%s%sT%d", c2, w2, tier);
        if (NpcIdFind(alt, id, used, cap))
          return true;
      }
  return false;
}
// PvP /spawn through the game's own bots (SpawnGameBot), for game mode
// g_extraBotsGm: bots added per team, the bot list entries they took, and
// whether /spawn switched the spawner on itself (dn_host_bots_bc_only.txt:
// then those are the only bots).
static void *g_extraBotsGm = nullptr;
static int g_extraBots[3] = {};
static bool g_extraUsed[128] = {};
// /boss and /spawn xN as game bots: per bot list entry, the elite multiplier
// for every pawn that entry's bot flies (GameBotEliteTick).
struct BossDef;
struct GameBotElite { int mult; const BossDef *def; void *last; DWORD since; bool told; };
static GameBotElite g_gbElite[128] = {};
static int g_lastGameBotIdx = -1;
static bool g_extraOnly = false;
static int g_extraTier = 0; // the last /spawn'd game bot's ShipId tier (T<n>, capped at 5), 0 none
extern void *g_ffaGm; // /match ffa (FfaTick)
static int FfaKillLimit(uint8_t *gm);                                  // after FfaTick
static void FfaCountKill(uint8_t *gm, void *killer, void *victimCtrl); // after FfaTick
static bool FfaLeader(void *gm, uint8_t *gs);                          // after FfaTick

static void FlushPendingEomStats();

static void OnslaughtCreepTick(uint8_t *gm);
static void DumpReflection(UObjectMin *obj);
static void DescribeProps(const char *tag, UObjectMin *obj, const char *sub);
static void HavocDirectorTick(uint8_t *gm);
static void EscortDirectorTick(uint8_t *gm);
enum { TICK_MIN_MS = 50, TICK_SLOW_MS = 30 }; // GUESS: half the ~0.1 s timer period; a third of it
static void CommandsTick(uint8_t *gm);
static bool HostIsHavoc();
static bool HostIsHorde();
static void __fastcall HookEmptyExecThunk(void *ctx, uint8_t *stack, void *result);
typedef void(__fastcall *tExecThunk)(void *ctx, uint8_t *stack, void *result);
extern tExecThunk g_origEmptyExecThunk;
static void __fastcall HookIntParmExecThunk(void *ctx, uint8_t *stack, void *result);
extern tExecThunk g_origIntParmExecThunk;
static void __fastcall HookIntObjParmExecThunk(void *ctx, uint8_t *stack, void *result);
extern tExecThunk g_origIntObjParmExecThunk;
static void __fastcall HookHavocReadyUpThunk(void *ctx, uint8_t *stack, void *result);
static void __fastcall HookHavocUnReadyThunk(void *ctx, uint8_t *stack, void *result);
static void __fastcall HookHvcSelectedUpgrade(void *pc, int32_t slot);
extern void(__fastcall *g_origHvcSelectedUpgrade)(void *pc, int32_t slot);
extern tExecThunk g_origHavocReadyUpThunk;
extern tExecThunk g_origHavocUnReadyThunk;
extern tExecThunk g_origReadyUpForMatchThunk;
static void __fastcall HookReadyUpForMatchThunk(void *ctx, uint8_t *stack, void *result);
#define RVA_READY_UP_FOR_MATCH_THUNK 0x772380
#define RVA_BOOL_PARM_EXEC_THUNK 0x777000 // Havoc wave over (HookBoolParmExecThunk)
#define RVA_GM_SPAWN_GATE 0x368380        // Havoc respawn block (HookSpawnGate)
static void __fastcall HookBoolParmExecThunk(void *ctx, uint8_t *stack, void *result);
extern tExecThunk g_origBoolParmExecThunk;
typedef uint32_t(__fastcall *tSpawnGate)(void *gm, void *pc);
static uint32_t __fastcall HookSpawnGate(void *gm, void *pc);
extern tSpawnGate g_origSpawnGate;
static bool HavocRespawnBlockOn();
static bool HavocShipUpgradesOn();
static void ConquestTick(uint8_t *gm);
static void ReportFarAway(uint8_t *pawn, const char *what);
static void NpcAiStartTick(uint8_t *gm);

static bool ClassChainContains(UObjectMin *obj, const char *needle);
static bool ClassChainIs(UObjectMin *obj, const char *name);

static void __fastcall HookGameModeTimer(void *gameMode) {
  uint8_t *gm = (uint8_t *)gameMode;
  if (g_bcAIArmed && IsReadable(gm + OFF_GM_ENABLE_SPAWN_AI, 1) &&
      IsReadable(gm + OFF_GM_GAMESTATE, sizeof(void *))) {
    uint8_t *gs = *(uint8_t **)(gm + OFF_GM_GAMESTATE);
    if (gs && IsReadable(gs + OFF_GS_GAME_MODE_TYPE, 1)) {
      int type = gs[OFF_GS_GAME_MODE_TYPE];
      static void *s_reported = nullptr;
      if (s_reported != gameMode && IsReadable(gm + OFF_GM_TEAM_SIZE, 4)) {
        s_reported = gameMode;
        Logf("bots: game mode %p type %d: bot list %d entries, team size %d%s",
             gameMode, type, *(int32_t *)(gm + OFF_GM_NPC_COUNT),
             *(int32_t *)(gm + OFF_GM_TEAM_SIZE),
             (type == YGMT_BOOTCAMP || !g_botsBCOnly) ? "" : " (bots limited to the proving ground)");
        // m_showAISpawnDebug (+0x98D, reflection): the bot ship chooser 0x371B50
        // logs its class/tier picks only when set. dn_host_ai_spawn_debug.txt.
        if (SwitchOn("DN_HOST_AI_SPAWN_DEBUG", "dn_host_ai_spawn_debug.txt") && IsReadable(gm + 0x98D, 1))
          gm[0x98D] = 1, Logf("bots: m_showAISpawnDebug on");
      }
      // Bot tier (/host bottier N). m_aiSpawnTierRules (gm+0x9C0, YAISpawnTier,
      // 0x24 bytes, reflection): +0x00 byte m_aiTier_aiDificultyLevel, +0x18
      // float m_aiTier_DefaultTier, +0x20 float m_aiTier_Random. The tier picker
      // (0x36DF04) clamps its result to a range keyed by the fleet type
      // (0x396C50: GameState+0x1D48 on a server, see
      // docs/battle-server-data-path.md) when there is no mmog tier data
      // (never, here): 1 -> T1-2, 2 -> T2-3, 3 -> T4-5. Rewriting only the
      // rules left every bot T2 (TDM 2026-10-20). GUESS: the rule's level byte
      // is matched against that fleet type, so both get the same value. Read
      // at every bot spawn, so respawns change too.
      // The chooser keeps only the entry's ShipIds of the rolled tier; with
      // none left it logs "No valid NPC spawn ID found" and takes a random
      // ship from hardcoded data (TDM 2026-10-20: a /spawn'd AssaultHeavyT4
      // came out SniperMediumT2 at T2, then DreadnoughtMediumT1). So with only
      // /spawn'd bots on the map and no bottier, the tier follows the last
      // /spawn'd ship's. With fill bots too it is left alone (their tier would
      // change): there a /spawn needs a matching /host bottier.
      int tier = g_botTier > 0 ? g_botTier : g_extraBotsGm == gm && g_extraOnly ? g_extraTier : 0;
      if (tier > 0 && IsReadable(gs + 0x1D48, 1))
        gs[0x1D48] = tier <= 2 ? 1 : tier == 3 ? 2 : 3;
      if (tier > 0 && IsReadable(gm + 0x9C0, 16)) {
        uint8_t *r = *(uint8_t **)(gm + 0x9C0);
        int n = *(int32_t *)(gm + 0x9C8);
        for (int i = 0; r && i < n && i < 16 && IsReadable(r + i * 0x24, 0x24); ++i) {
          uint8_t *e = r + i * 0x24;
          e[0] = tier <= 2 ? 1 : tier == 3 ? 2 : 3;
          *(float *)(e + 0x18) = (float)tier;
          *(float *)(e + 0x20) = 0.f;
        }
      }
      // Bot ship (/host botship). m_npcPlayers entries (YTrainingMatchAI, 0x38
      // bytes, reflection): +0x08 byte m_team, +0x10 TArray<FName>
      // m_npcSpawnIDs (ShipIds, e.g. ScoutLightT2). The chooser 0x371B50 picks
      // an index into that list by class and tier, falling back to a random
      // element, and stores it at +0x30. Every element is overwritten in
      // place, so whatever it picks is that ship; nothing is reallocated, and
      // "off" cannot restore lists already rewritten this match. TDM
      // 2026-10-20, botship 2 DreadnoughtHeavyT5 + bottier 5: all 8 enemy bots
      // VH_DreadH_AI_Pawn_T5, the 7 friendly ones T5 of mixed classes.
      NpcIdsSnapshot(gm);
      if (g_botShipGm != gm && g_npcIdsGm == gm) {
        g_botShipGm = gm;
        for (int t = 0; t < 3; ++t) {
          char used[64];
          FNameMin r = {};
          if (!g_botShipStr[t][0])
            continue;
          if (!NpcIdResolve(g_botShipStr[t], &r, used, sizeof used))
            Logf("bots: bot ship team %d: no %s-class ship in this mode's bot lists, keeping %s", t, g_botShipStr[t], g_botShipStr[t]);
          else if (_stricmp(used, g_botShipStr[t]))
            g_botShip[t] = r, Logf("bots: bot ship team %d: %s is not in this mode's bot lists, using %s", t, g_botShipStr[t], used);
        }
      }
      if (IsReadable(gm + 0x968, 16) && (*(uint64_t *)&g_botShip[0] || *(uint64_t *)&g_botShip[1] || *(uint64_t *)&g_botShip[2])) {
        uint8_t *l = *(uint8_t **)(gm + 0x968);
        int n = *(int32_t *)(gm + 0x970);
        for (int i = 0; l && i < n && i < 128 && IsReadable(l + i * 0x38, 0x38); ++i) {
          uint8_t *e = l + i * 0x38;
          if (g_extraBotsGm == gm && i < 128 && g_extraUsed[i])
            continue; // a /spawn'd bot keeps its ship
          uint64_t want = *(uint64_t *)&g_botShip[e[8] == 1 || e[8] == 2 ? e[8] : 0];
          if (!want) want = *(uint64_t *)&g_botShip[0];
          uint64_t *ids = *(uint64_t **)(e + 0x10);
          int k = *(int32_t *)(e + 0x18);
          for (int j = 0; want && ids && j < k && j < 64 && IsReadable(ids + j, 8); ++j)
            ids[j] = want;
        }
      }
      // TE: fill once, then no respawns. The fill has happened once the AI
      // targets are set (gm+0x980/0x984, written by 0x3678F0 when the teams are
      // filled -- the same signal bot balance uses); 15 s later the spawner is
      // switched off for the rest of the match.
      static void *s_teFilled = nullptr;
      static DWORD s_teFilledAt = 0;
      static void *s_teStopped = nullptr;
      if (type == YGMT_TEAM_ELIMINATION && s_teStopped != gameMode &&
          IsReadable(gm + OFF_GM_AI_TARGET_T1, 8)) {
        bool filled = *(int32_t *)(gm + OFF_GM_AI_TARGET_T1) > 0 ||
                      *(int32_t *)(gm + OFF_GM_AI_TARGET_T1 + 4) > 0;
        if (filled && s_teFilled != gameMode) {
          s_teFilled = gameMode;
          s_teFilledAt = GetTickCount();
        }
        if (s_teFilled == gameMode && GetTickCount() - s_teFilledAt > 15000 &&
            gm[OFF_GM_ENABLE_SPAWN_AI]) {
          gm[OFF_GM_ENABLE_SPAWN_AI] = 0;
          s_teStopped = gameMode;
          Logf("bots: Team Elimination %p -- teams filled, m_enableSpawnAI 1 -> 0: "
               "destroyed bots no longer respawn", gameMode);
        }
      }
      // Not on a PvE season episode (YGameMode_PVE: Horde, Escort): its waves
      // come from the map's own PVE managers, and the multiplayer bot fill
      // has nothing to do there (2026-10-09, probe host on PVE_S1E1).
      static int s_pve = -1;
      static void *s_pveFor = nullptr;
      if (s_pveFor != gameMode) {
        s_pveFor = gameMode;
        s_pve = ClassChainContains((UObjectMin *)gameMode, "YGameMode_PVE") ? 1 : 0;
        if (s_pve) {
          Logf("pve: game mode %p (type %d) is a PvE episode -- no multiplayer bot fill", gameMode, type);
          DumpReflection((UObjectMin *)gameMode);
          if (IsReadable(gm + OFF_GM_GAMESTATE, 8))
            DumpReflection(*(UObjectMin **)(gm + OFF_GM_GAMESTATE));
        }
      }
      if (g_ffaGm == gameMode && gm[OFF_GM_ENABLE_SPAWN_AI]) {
        gm[OFF_GM_ENABLE_SPAWN_AI] = 0;
        Logf("bots: FFA match %p -- m_enableSpawnAI -> 0, no bots", gameMode);
      }
      if (g_ffaGm != gameMode && gm[OFF_GM_ENABLE_SPAWN_AI] == 0 && s_teStopped != gameMode && !s_pve &&
          (type == YGMT_BOOTCAMP || !g_botsBCOnly)) {
        gm[OFF_GM_ENABLE_SPAWN_AI] = 1;
        Logf("bots: game mode %p (type %d) m_enableSpawnAI 0 -> 1. The game fills "
             "both teams when the pre-match countdown reaches 50 s%s.",
             gameMode, type,
             type == YGMT_TEAM_ELIMINATION ? " (Team Elimination: switched off again once filled)" : "");
      }
    }
  }
  g_origGameModeTimer(gameMode);
  // Our ticks run at most every TICK_MIN_MS of wall time. This timer loops
  // (~0.1 s: viewer snapshots arrive ~9.4/s) and the engine re-fires a
  // looping timer that is overdue to catch up -- GUESS: FTimerManager catch-up,
  // inferred from the log, not read from the binary. Havoc 2026-10-17, after
  // the operator spawned 17 elite bosses: engine ticks 25 s, then 56 s, then
  // 178 s apart ("TickDispatch: Took too long to receive packets. Time:
  // 178.29") while our snapshots kept coming at the timer's rate, every ship
  // frozen. That is the shape of a timer whose work outgrew its period, so the
  // catch-up calls must not redo our work. Slow sections are logged by name.
  static double s_last = 0;
  double t0 = NowMs();
  if (t0 - s_last < TICK_MIN_MS)
    return;
  s_last = t0;
  struct Sec {
    const char *name;
    void (*fn)(uint8_t *);
  };
  static const Sec secs[] = {{"onslaught", OnslaughtCreepTick}, {"havoc", HavocDirectorTick},
                             {"escort", EscortDirectorTick},    {"commands", CommandsTick},
                             {"conquest", ConquestTick},        {"npc-ai", NpcAiStartTick},
                             {"players", PlayersTick},          {"class-time", ClassTimeTick}};
  g_vqWhere = 5;
  for (const Sec &x : secs) {
    double a = NowMs();
    x.fn(gm);
    double ms = NowMs() - a;
    static DWORD s_warned = 0;
    if (ms > TICK_SLOW_MS && GetTickCount() - s_warned > 2000) {
      s_warned = GetTickCount();
      Logf("tick: %s took %.0f ms (timer tick budget ~100 ms)", x.name, ms);
    }
  }
  g_vqWhere = 0;
  FlushPendingEomStats();
}

// ---------------------------------------------------------------------------
// Onslaught match points (on by default; dn_host_no_onslaught_points.txt /
// DN_HOST_NO_ONSLAUGHT_POINTS=1 turns it off)
//
// Operator report 2026-10-07: "the game should finish if 1 of the teams reaches
// 300 points but now it finishes at around 248 points".
//
// Verified in the exe and the cooked assets (2026-10-07):
//   - Onslaught is YGameMode_Invasion / YGameState_Invasion (host mode code
//     "IVN", m_gameModeType 16). GameState_Onslaught_BP sets
//     m_matchPointsForWinning 300 (+0x1E28) and the points per kill:
//     m_playerkillPoints 7 (+0x1E2C), m_smallCreepkillPoints 1 (+0x1E30),
//     m_midCreepkillPoints 2 (+0x1E34), m_bigCreepkillPoints 30 (+0x1E38); the
//     replicated creep-kill counters sit at +0x1E3C.. (small/mid/big T1, then
//     T2). GameInfo_Onslaught_BP sets m_killsForWinning 30.
//   - NOTHING in this exe reads +0x1E28 or writes the creep counters (full
//     scan of .text for those displacements). The points rule was server-build
//     code, like Team Elimination's rounds.
//   - What does end an Onslaught match here is the deathmatch rule: the kill
//     handler 0x375F30 adds 1 to the killer's team (GameState +0x1DF0/+0x1DF4,
//     only for victims whose vtable +0x7E0 is false -- not creeps) and calls
//     vtable +0x8D0 = 0x3685C0, which ends the match when a team's kills reach
//     the game mode's m_killsForWinning (+0x9DC = 30). 30 kills x 7 points plus
//     creeps is the ~248 the operator saw.
//   - The winner is the team with more KILLS (0x384C90 writes GameState
//     +0x4E8: 1 team 1, 2 team 2, 3 draw).
//
// So for Onslaught the mod: lifts the kill limit (game mode +0x9DC only; the
// GameState's copy the clients see stays), counts creep kills into the
// GameState's own counters, ends the match through EndMatch (vtable +0x668,
// as 0x3685C0 does) when a team reaches m_matchPointsForWinning, and makes the
// winner the team with more points.
//
// Which creep is which: the AI ships are VH_ONS_AITarget{L,M,H}_Pawn_BP (H has
// _V/_L tier variants). Their scoring events name them: AITargetL "Fighter",
// AITargetM "Assault Ship", AITargetH "Command Ship" (ribbon names), and the
// datamine's NPC Onslaught weapons list the same three. GUESS: L = small,
// M = mid, H = big, which also matches the points (fighter 1, command ship 30).
#define RVA_TDM_KILLED 0x375F30          // AYGameMode_TDM::Killed (vtable)
#define RVA_TDM_DETERMINE_LEADER 0x384C90 // writes GameState +0x4E8 from kills
#define YGMT_INVASION 16
#define OFF_GM_KILLS_FOR_WINNING 0x9DC
#define OFF_GS_KILLS_T1 0x1DF0
#define OFF_GS_KILLS_T2 0x1DF4
#define OFF_GS_WINNING_TEAM 0x4E8
#define OFF_GS_ONS_POINTS_TO_WIN 0x1E28
#define OFF_GS_ONS_PLAYER_POINTS 0x1E2C   // then small, mid, big (+4 each)
#define OFF_GS_ONS_CREEP_KILLS 0x1E3C     // small, mid, big T1, then T2 (+4 each)
#define OFF_CONTROLLER_PAWN 0x3C8         // as 0x375F30 reads the killer's ship
#define OFF_SHIP_TEAM 0x8C0               // 0x57C100: a ship's EYTeam
#define VT_GM_END_MATCH 0x668
#define ONS_KILL_LIMIT_OFF 1000000
#define OFF_GS_WINNING_TEAM_FINAL 0x56B // GameState final match result (0 while playing)
#define OFF_PAWN_IS_AI_TARGET 0x950 // YPawn: what vtable +0x7E0 returns
#ifndef OFF_USTRUCT_SUPER
#define OFF_USTRUCT_SUPER 0x30
#endif

typedef void(__fastcall *tTdmKilled)(void *gm, void *killer, void *victimCtrl,
                                     void *victimPawn, void *damageType);
static tTdmKilled g_origTdmKilled = nullptr;
typedef void(__fastcall *tDetermineLeader)(void *gm);
static tDetermineLeader g_origDetermineLeader = nullptr;

struct OnslaughtMatch {
  void *gm;
  int creeps[2][3];    // [team-1][small, mid, big]
  bool ended;
};
static OnslaughtMatch g_ons = {};

static uint8_t *InvasionGameState(uint8_t *gm) {
  if (!gm || !IsReadable(gm + OFF_GM_GAMESTATE, sizeof(void *)))
    return nullptr;
  uint8_t *gs = *(uint8_t **)(gm + OFF_GM_GAMESTATE);
  if (!gs || !IsReadable(gs + OFF_GS_GAME_MODE_TYPE, 1) ||
      gs[OFF_GS_GAME_MODE_TYPE] != YGMT_INVASION ||
      !IsReadable(gs + OFF_GS_KILLS_T1, OFF_GS_ONS_CREEP_KILLS + 24 - OFF_GS_KILLS_T1))
    return nullptr;
  return gs;
}

// The GameState's points per kill and target, or the cooked defaults when a
// value is missing.
static void OnslaughtRules(uint8_t *gs, int *perPlayer, int perCreep[3], int *target) {
  const int def[4] = {7, 1, 2, 30};
  int32_t *v = (int32_t *)(gs + OFF_GS_ONS_PLAYER_POINTS);
  *perPlayer = v[0] > 0 ? v[0] : def[0];
  for (int i = 0; i < 3; ++i)
    perCreep[i] = v[1 + i] > 0 ? v[1 + i] : def[1 + i];
  int32_t t = *(int32_t *)(gs + OFF_GS_ONS_POINTS_TO_WIN);
  *target = t > 0 ? t : 300;
}

static int OnslaughtPoints(uint8_t *gs, int team /*1 or 2*/) {
  int perPlayer, perCreep[3], target;
  OnslaughtRules(gs, &perPlayer, perCreep, &target);
  int kills = *(int32_t *)(gs + (team == 1 ? OFF_GS_KILLS_T1 : OFF_GS_KILLS_T2));
  int pts = kills * perPlayer;
  for (int i = 0; i < 3; ++i)
    pts += g_ons.creeps[team - 1][i] * perCreep[i];
  return pts;
}

// 0 small, 1 mid, 2 big; -1 not a creep. Walks the class chain so a tier
// variant (VH_ONS_AITargetH_Pawn_V_BP) is found by its own name.
static int CreepSize(void *pawn) {
  UObjectMin *cls = IsReadable(pawn, sizeof(UObjectMin)) ? ((UObjectMin *)pawn)->Class : nullptr;
  for (int depth = 0; cls && depth < 6 && IsReadable(cls, OFF_USTRUCT_SUPER + 8); ++depth) {
    const char *n = NameText(cls->Name);
    if (n) {
      if (strstr(n, "AITargetH")) return 2;
      if (strstr(n, "AITargetM")) return 1;
      if (strstr(n, "AITargetL")) return 0;
    }
    cls = *(UObjectMin **)((uint8_t *)cls + OFF_USTRUCT_SUPER);
  }
  return -1;
}

static const char *ClassNameOf(void *obj) {
  if (!IsReadable(obj, sizeof(UObjectMin)) || !IsReadable(((UObjectMin *)obj)->Class, sizeof(UObjectMin)))
    return "?";
  const char *n = NameText(((UObjectMin *)obj)->Class->Name);
  return n ? n : "?";
}

static int ShipTeam(void *pawn) {
  if (!pawn || !IsReadable((uint8_t *)pawn + OFF_SHIP_TEAM, 1))
    return 0;
  int t = ((uint8_t *)pawn)[OFF_SHIP_TEAM];
  return (t == 1 || t == 2) ? t : 0;
}

// GameState +0x4E8 to the team with more points; false when a team has no
// players (the original's own +0x4F8/+0x4FC branches decide those).
static bool SetLeaderByPoints(uint8_t *gs, int p1, int p2) {
  if (!IsReadable(gs + 0x4F8, 8) || *(int32_t *)(gs + 0x4F8) == 0 || *(int32_t *)(gs + 0x4FC) == 0)
    return false;
  gs[OFF_GS_WINNING_TEAM] = p1 > p2 ? 1 : p2 > p1 ? 2 : 3;
  return true;
}

// ---------------------------------------------------------------------------
// Kill detail for daily contracts (on; dn_host_no_kill_detail.txt turns it off)
//
// mmogbrain counts the daily contracts (mmogbrain/daily_contracts.go), and
// many count only some kills: "Destroy Enemy Corvettes" (the destroyed ship's
// class), "Destroy Enemies with a Corvette" (the class the player flies). The
// match result carried only a kill count. Each kill is now recorded here --
// the destroyed ship's EYShipClass (pawn +0x940, read by 0x57BAE0) and the
// killer's ship's, by the killer's PlayerReplicationInfo -- and reported with
// the result as kl=victim.killer,... (ReportMatchResult).
#define OFF_PAWN_SHIP_CLASS 0x940
#define OFF_PC_PLAYER_STATE 0x3E0 // also defined with the match result below
#define KILL_LOG_MAX 64
struct KillLog {
  void *pri;
  int n;
  uint8_t victim[KILL_LOG_MAX], killer[KILL_LOG_MAX];
};
static KillLog g_killLogs[64] = {};
static SRWLOCK g_killLogLock = SRWLOCK_INIT;

static bool SwitchOn(const char *envName, const char *markerFile);

static bool KillDetailEnabled() {
  static int s_on = -1;
  if (s_on < 0)
    s_on = SwitchOn("DN_HOST_NO_KILL_DETAIL", "dn_host_no_kill_detail.txt") ? 0 : 1;
  return s_on == 1;
}

static void RecordKill(void *killerCtrl, uint8_t victimClass) {
  uint8_t *kc = (uint8_t *)killerCtrl;
  if (!kc || !IsReadable(kc + OFF_PC_PLAYER_STATE, 8) || !IsReadable(kc + OFF_CONTROLLER_PAWN, 8))
    return;
  void *pri = *(void **)(kc + OFF_PC_PLAYER_STATE);
  uint8_t *kp = *(uint8_t **)(kc + OFF_CONTROLLER_PAWN);
  uint8_t killerClass = (kp && IsReadable(kp + OFF_PAWN_SHIP_CLASS, 1)) ? kp[OFF_PAWN_SHIP_CLASS] : 0;
  if (!pri)
    return;
  AcquireSRWLockExclusive(&g_killLogLock);
  KillLog *slot = nullptr;
  for (auto &k : g_killLogs) {
    if (k.pri == pri) { slot = &k; break; }
    if (!slot && !k.pri) slot = &k;
  }
  if (slot) {
    slot->pri = pri;
    if (slot->n < KILL_LOG_MAX) {
      slot->victim[slot->n] = victimClass;
      slot->killer[slot->n] = killerClass;
      slot->n++;
    }
  }
  ReleaseSRWLockExclusive(&g_killLogLock);
}

// ---------------------------------------------------------------------------
// Seconds per ship class, for the Statistics screen (on; added 2026-10-15)
//
// YA_GetPlayerStatistics has DestroyerTimeSec .. CorvetteTimeSec, and nothing
// in this exe counts them (the TimeUseShipClass stats counter has no sender:
// the client's only YA_IncrementPlayerStatsCounter calls are Customize and
// FinishTutorial, 0x30FFA0/0x426740/0x54B690). So the players tick samples,
// once a second, the class of every human's current ship (controller +0x3C8
// -> pawn +0x940, as RecordKill reads it) and adds the elapsed time to it.
// No pawn (dead, in orbit) adds nothing. Reported with the result as
// ct=class.seconds,... (mmogbrain/match_statistics.go).
#define CLASS_TIME_MAX 21 // EYShipClass values 0..20
struct ClassTime {
  void *pri;
  DWORD ms[CLASS_TIME_MAX];
};
static ClassTime g_classTimes[64] = {};
static SRWLOCK g_classTimeLock = SRWLOCK_INIT;

static void AddClassTime(void *pri, uint8_t cls, DWORD ms) {
  if (!pri || cls == 0 || cls >= CLASS_TIME_MAX || ms == 0 || ms > 10000)
    return;
  AcquireSRWLockExclusive(&g_classTimeLock);
  ClassTime *slot = nullptr;
  for (auto &c : g_classTimes) {
    if (c.pri == pri) { slot = &c; break; }
    if (!slot && !c.pri) slot = &c;
  }
  if (slot) {
    slot->pri = pri;
    slot->ms[cls] += ms;
  }
  ReleaseSRWLockExclusive(&g_classTimeLock);
}

// The player's seconds per class as "class.seconds,..." into out.
static void TakeClassTime(void *pri, char *out, size_t outLen) {
  out[0] = 0;
  size_t o = 0;
  AcquireSRWLockExclusive(&g_classTimeLock);
  for (auto &c : g_classTimes) {
    if (c.pri != pri)
      continue;
    for (int i = 1; i < CLASS_TIME_MAX && o + 16 < outLen; ++i)
      if (c.ms[i] >= 1000)
        o += _snprintf_s(out + o, outLen - o, _TRUNCATE, "%s%d.%lu", o ? "," : "", i, (unsigned long)(c.ms[i] / 1000));
    c = ClassTime{};
    break;
  }
  ReleaseSRWLockExclusive(&g_classTimeLock);
}

// The player's kills as "victim.killer,..." into out; false when the kill
// detail is off (mmogbrain then counts only the class-free contracts).
static bool TakeKillLog(void *pri, char *out, size_t outLen) {
  out[0] = 0;
  if (!KillDetailEnabled())
    return false;
  size_t o = 0;
  AcquireSRWLockExclusive(&g_killLogLock);
  for (auto &k : g_killLogs) {
    if (k.pri != pri)
      continue;
    for (int i = 0; i < k.n && o + 12 < outLen; ++i)
      o += _snprintf_s(out + o, outLen - o, _TRUNCATE, "%s%u.%u", o ? "," : "", k.victim[i], k.killer[i]);
    k = KillLog{};
    break;
  }
  ReleaseSRWLockExclusive(&g_killLogLock);
  return true;
}

// ---------------------------------------------------------------------------
// MVP and Man Of The Day ribbons (on; dn_host_no_eom_ribbons.txt /
// DN_HOST_NO_EOM_RIBBONS=1 turns them off). Added 2026-10-15.
//
// Both are end-of-match scoring events (EYScoringEventID ManOfTheDay 41, MVP
// 42), and mmogbrain keeps every end-of-match event out of the host's table:
// with them in it, every match that reached EndMatch overflowed the host's
// stack (2026-09-28). So nobody ever earned either ribbon. The original texts:
//   MVP          "Player with most RP gathered total during the match"
//   ManOfTheDay  "Score the last kill that makes your team win."
// The mod adds them to the player's event counts (ev=) itself: MVP to the
// player(s) with the highest m_score at the match result (GUESS: RP = the
// match score; ties all get it; bots have no player state and do not
// compete), Man Of The Day to the killer whose kill ends the match for his
// team (TDM family: the kill that reaches the kill limit; Onslaught: the kill
// that reaches the points target).
#define SCORING_EVENT_MAN_OF_THE_DAY 41
#define SCORING_EVENT_MVP 42
static void AddScoringEvent(void *pri, uint32_t event);
static bool EomRibbonsEnabled() {
  static int s_on = -1;
  if (s_on < 0)
    s_on = SwitchOn("DN_HOST_NO_EOM_RIBBONS", "dn_host_no_eom_ribbons.txt") ? 0 : 1;
  return s_on == 1;
}

// The match-winning kill: killer's player state and team, if the killer is a
// player.
static void AwardManOfTheDay(void *killer, int winningTeam, const char *why) {
  uint8_t *kc = (uint8_t *)killer;
  if (!EomRibbonsEnabled() || !kc || !IsReadable(kc + OFF_PC_PLAYER_STATE, 8))
    return;
  uint8_t *pri = *(uint8_t **)(kc + OFF_PC_PLAYER_STATE);
  if (!IsReadable(pri, 0x941) || !ClassChainContains((UObjectMin *)pri, "YPlayerReplicationInfo"))
    return;
  int team = pri[0x940]; // OFF_PRI_TEAM
  if (team != winningTeam) {
    Logf("ribbons: match-ending kill (%s) by team %d, winner %d -- no Man Of The Day", why, team, winningTeam);
    return;
  }
  AddScoringEvent(pri, SCORING_EVENT_MAN_OF_THE_DAY);
  Logf("ribbons: Man Of The Day -> player state %p (team %d, %s)", pri, team, why);
}

// Kill feed for the battle viewer (2026-10-17): the last VIEWER_KILLS kills,
// recorded here as text because the victim's pawn is gone by the next
// snapshot, and sent with every snapshot as "k ..." lines (ViewerScoreLines);
// mmogbrain keeps the log by seq. Game thread only (this hook and the
// snapshot), so no lock.
enum { VIEWER_KILLS = 12 };
struct ViewerKill {
  uint32_t seq;
  int kt, vt;
  char kcls[64], vcls[64], kpid[40], vpid[40], kname[64], vname[64];
};
static ViewerKill g_vKills[VIEWER_KILLS];
static uint32_t g_vKillSeq = 0;
static void PlayerStateName(void *ps, char *out, int cap);
static bool ObjectLive(void *obj);
static bool RememberedControllerPID(void *pc, char *out, size_t outLen);

static void ViewerKillSide(void *ctrl, void *pawn, int *team, char *cls, char *pid, char *name) {
  *team = 0;
  strcpy_s(cls, 64, "-"), strcpy_s(pid, 40, "-"), strcpy_s(name, 64, "-");
  uint8_t *c = (uint8_t *)ctrl;
  if (!pawn && c && IsReadable(c + OFF_CONTROLLER_PAWN, 8))
    pawn = *(void **)(c + OFF_CONTROLLER_PAWN);
  if (pawn && ObjectLive(pawn)) {
    *team = ShipTeam(pawn);
    strcpy_s(cls, 64, ClassNameOf(pawn));
  }
  if (c && ObjectLive(c)) {
    char tmp[80] = "";
    if (RememberedControllerPID(c, tmp, sizeof tmp) && tmp[0])
      strcpy_s(pid, 40, tmp);
    if (IsReadable(c + OFF_PC_PLAYER_STATE, 8))
      PlayerStateName(*(void **)(c + OFF_PC_PLAYER_STATE), name, 64);
  }
}

static void ViewerRecordKill(void *killer, void *victimCtrl, void *victimPawn) {
  uint32_t seq = g_vKillSeq + 1;
  ViewerKill &k = g_vKills[seq % VIEWER_KILLS];
  memset(&k, 0, sizeof k);
  k.seq = seq;
  __try {
    ViewerKillSide(killer, nullptr, &k.kt, k.kcls, k.kpid, k.kname);
    ViewerKillSide(victimCtrl, victimPawn, &k.vt, k.vcls, k.vpid, k.vname);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  if (killer == victimCtrl || !killer)
    strcpy_s(k.kcls, sizeof k.kcls, "-"), strcpy_s(k.kname, sizeof k.kname, "-"), strcpy_s(k.kpid, sizeof k.kpid, "-"), k.kt = 0;
  g_vKillSeq = seq;
}

static void __fastcall HookTdmKilled(void *gameMode, void *killer, void *victimCtrl,
                                     void *victimPawn, void *damageType) {
  uint8_t *gm = (uint8_t *)gameMode;
  uint8_t *gs = nullptr;
  __try {
    gs = InvasionGameState(gm);
    if (gs) {
      if (g_ons.gm != gameMode) {
        memset(&g_ons, 0, sizeof(g_ons));
        g_ons.gm = gameMode;
        Logf("onslaught: match %p -- points rule armed (kill limit %d lifted, "
             "points to win %d)", gameMode, *(int32_t *)(gm + OFF_GM_KILLS_FOR_WINNING),
             *(int32_t *)(gs + OFF_GS_ONS_POINTS_TO_WIN));
      }
      // The deathmatch check runs inside the original call; keep it from
      // ending Onslaught on kills.
      *(int32_t *)(gm + OFF_GM_KILLS_FOR_WINNING) = ONS_KILL_LIMIT_OFF;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    gs = nullptr;
  }
  // FFA: the stock rule counts every kill by a team-3+ ship into team 2
  // (0x39E470), so it would end the match after m_killsForWinning kills IN
  // TOTAL. Lifted for the call; FfaCountKill keeps a tally per player.
  int ffaLimit = g_ffaGm == gameMode ? FfaKillLimit(gm) : 0;

  int32_t kills1 = 0, kills2 = 0;
  if (gs) {
    kills1 = *(int32_t *)(gs + OFF_GS_KILLS_T1);
    kills2 = *(int32_t *)(gs + OFF_GS_KILLS_T2);
  }

  // The assault and command ships are AI targets only while their kill is
  // scored (see SpawnOnslaughtAIShip): set the byte for the original's
  // AI-scoring route (victim vtable +0x7E0), clear it again after so the
  // game's ship-list removal on death runs for them.
  int victimSize = gs ? CreepSize(victimPawn) : -1;
  bool flagged = false;
  if (victimSize >= 1 && IsReadable((uint8_t *)victimPawn + OFF_PAWN_IS_AI_TARGET, 1) &&
      !((uint8_t *)victimPawn)[OFF_PAWN_IS_AI_TARGET]) {
    ((uint8_t *)victimPawn)[OFF_PAWN_IS_AI_TARGET] = 1;
    flagged = true;
  }
  // The destroyed ship's class, read before the original runs (it may
  // destroy the pawn). Kill detail for daily contracts, see RecordKill.
  int victimClass = -1;
  if (KillDetailEnabled() && killer && killer != victimCtrl && victimPawn &&
      IsReadable((uint8_t *)victimPawn + OFF_PAWN_SHIP_CLASS, 1))
    victimClass = ((uint8_t *)victimPawn)[OFF_PAWN_SHIP_CLASS];
  // Man Of The Day, TDM family: the original call ends the match when a
  // team's kills reach the game mode's m_killsForWinning (+0x9DC, via
  // 0x3685C0). Lifted to ONS_KILL_LIMIT_OFF in Onslaught, so it never fires
  // there (Onslaught is handled where its points end the match).
  int32_t limit = 0, before1 = 0, before2 = 0;
  uint8_t *tdmGS = nullptr;
  __try {
    if (EomRibbonsEnabled() && killer && killer != victimCtrl && IsReadable(gm + OFF_GM_KILLS_FOR_WINNING, 4) &&
        IsReadable(gm + OFF_GM_GAMESTATE, 8)) {
      tdmGS = *(uint8_t **)(gm + OFF_GM_GAMESTATE);
      limit = *(int32_t *)(gm + OFF_GM_KILLS_FOR_WINNING);
      if (tdmGS && IsReadable(tdmGS + OFF_GS_KILLS_T2, 4)) {
        before1 = *(int32_t *)(tdmGS + OFF_GS_KILLS_T1);
        before2 = *(int32_t *)(tdmGS + OFF_GS_KILLS_T2);
      } else {
        tdmGS = nullptr;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    tdmGS = nullptr;
  }
  ViewerRecordKill(killer, victimCtrl, victimPawn);
  g_origTdmKilled(gameMode, killer, victimCtrl, victimPawn, damageType);
  if (tdmGS && limit > 0 && limit < ONS_KILL_LIMIT_OFF) {
    __try {
      int32_t after1 = *(int32_t *)(tdmGS + OFF_GS_KILLS_T1);
      int32_t after2 = *(int32_t *)(tdmGS + OFF_GS_KILLS_T2);
      if (before1 < limit && after1 >= limit)
        AwardManOfTheDay(killer, 1, "kill limit");
      else if (before2 < limit && after2 >= limit)
        AwardManOfTheDay(killer, 2, "kill limit");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  if (ffaLimit > 0) {
    __try {
      FfaCountKill(gm, killer, victimCtrl);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      Logf("ffa: EXCEPTION while counting a kill");
    }
  }
  if (victimClass >= 0) {
    __try {
      RecordKill(killer, (uint8_t)victimClass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  if (flagged)
    ((uint8_t *)victimPawn)[OFF_PAWN_IS_AI_TARGET] = 0;

  if (!gs)
    return;
  __try {
    int size = CreepSize(victimPawn);
    // The assault ship and command ship are not creeps (no vtable +0x7E0),
    // so the deathmatch handler just counted them as a player kill. They
    // score as creeps only.
    if (size >= 0) {
      if (*(int32_t *)(gs + OFF_GS_KILLS_T1) > kills1)
        *(int32_t *)(gs + OFF_GS_KILLS_T1) = kills1;
      if (*(int32_t *)(gs + OFF_GS_KILLS_T2) > kills2)
        *(int32_t *)(gs + OFF_GS_KILLS_T2) = kills2;
    }
    int scorer = 0;
    if (size >= 0) {
      void *killerShip = (killer && IsReadable((uint8_t *)killer + OFF_CONTROLLER_PAWN, 8))
                             ? *(void **)((uint8_t *)killer + OFF_CONTROLLER_PAWN) : nullptr;
      scorer = ShipTeam(killerShip);
      int victimTeam = ShipTeam(victimPawn);
      if (scorer == 0 || scorer == victimTeam)
        scorer = victimTeam == 1 ? 2 : victimTeam == 2 ? 1 : 0;
      if (scorer) {
        g_ons.creeps[scorer - 1][size]++;
        *(int32_t *)(gs + OFF_GS_ONS_CREEP_KILLS + 4 * (3 * (scorer - 1) + size)) =
            g_ons.creeps[scorer - 1][size];
      }
    }
    int p1 = OnslaughtPoints(gs, 1), p2 = OnslaughtPoints(gs, 2);
    int perPlayer, perCreep[3], target;
    OnslaughtRules(gs, &perPlayer, perCreep, &target);
    Logf("onslaught: %s %s -> team %d | points T1 %d (kills %d) T2 %d (kills %d) / %d",
         size >= 0 ? (size == 0 ? "small creep" : size == 1 ? "mid creep" : "big creep") : "kill",
         ClassNameOf(victimPawn), size >= 0 ? scorer : 0, p1,
         *(int32_t *)(gs + OFF_GS_KILLS_T1), p2, *(int32_t *)(gs + OFF_GS_KILLS_T2), target);
    if (!g_ons.ended && (p1 >= target || p2 >= target)) {
      g_ons.ended = true;
      SetLeaderByPoints(gs, p1, p2);
      Logf("onslaught: team %d reached %d points (T1 %d, T2 %d) -- ending the match",
           p1 >= p2 ? 1 : 2, target, p1, p2);
      AwardManOfTheDay(killer, p1 >= p2 ? 1 : 2, "onslaught points");
      void **vt = *(void ***)gm;
      ((void(__fastcall *)(void *))vt[VT_GM_END_MATCH / 8])(gm);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("onslaught: EXCEPTION while counting a kill");
  }
}

// The leader/winner by points, not kills, in Onslaught. The original's
// "a team has no players" branches (+0x4F8/+0x4FC) are kept.
static void __fastcall HookDetermineLeader(void *gameMode) {
  g_origDetermineLeader(gameMode);
  __try {
    if (g_ffaGm == gameMode && IsReadable((uint8_t *)gameMode + OFF_GM_GAMESTATE, 8) &&
        FfaLeader(gameMode, *(uint8_t **)((uint8_t *)gameMode + OFF_GM_GAMESTATE)))
      return;
    uint8_t *gs = InvasionGameState((uint8_t *)gameMode);
    if (!gs || g_ons.gm != gameMode)
      return;
    uint8_t byKills = gs[OFF_GS_WINNING_TEAM];
    int p1 = OnslaughtPoints(gs, 1), p2 = OnslaughtPoints(gs, 2);
    if (SetLeaderByPoints(gs, p1, p2) && g_ons.ended && gs[OFF_GS_WINNING_TEAM] != byKills)
      Logf("onslaught: leader by points %d (T1 %d, T2 %d), was %d by kills",
           gs[OFF_GS_WINNING_TEAM], p1, p2, byKills);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// ---------------------------------------------------------------------------
// Onslaught AI ships (on; dn_host_no_onslaught_creeps.txt turns it off)
//
// Onslaught's fighters, assault ships and command ship never spawned: every
// kill in a live Onslaught match was a bot (2026-10-07). Verified in the exe
// and the cooked content the same day:
//   - The ships are VH_ONS_AITarget{L,M,H}_Pawn_BP (YCreepPawn). The map's
//     _Onslaught sublevel (loaded on our hosts) holds YCreepNavigationSpawn
//     actors ONS_AITarget{L,M,H}_InitialSpawn_T1/T2 and _Spawn_T1/T2:
//     m_spawnClass +0x428, m_spawnTeam +0x430 (EYTeam), m_beginMatchSpawnDelay
//     +0x434. Nothing in this exe reads them (only reflection refers to the
//     class) and YGameMode_Invasion adds no code to the deathmatch mode: the
//     spawner was server-build code.
//   - The spawn itself is in the exe: AYAICombatSceneManager::SpawnCreep
//     0x264B30 (this, UClass* creepClass, AActor* spawnAt -- its root
//     component's location and rotation --, uint8* team); server only, needs
//     a YCreepManager, respects the creep manager's per-team cap. The scene
//     manager comes from 0x2529E0 (no arguments).
//   - The rules are the game mode's own (GameInfo_Onslaught_BP): small creep
//     class +0xA58, max +0xA60 (12), respawn +0xA64 (2 s); mid +0xA68 / +0xA70
//     (6) / +0xA74 (8 s); big +0xA78, Veteran +0xA80, Legendary +0xA88, max
//     +0xA90 (1), respawn +0xA94 (180 s). The 2018 launch notes give the same
//     counts and the command ship's 180 s.
// So once a second, on an Onslaught host, this spawns the initial ships at the
// InitialSpawn points, then keeps each team at its maximum: a missing ship is
// respawned at a Spawn point once it has been missing for the respawn time.
// GUESS: the first wave goes out 20 s after the match starts (the per-point
// m_beginMatchSpawnDelay is read but its use is not traced); alive ships are
// counted from GObjects (class + team, not pending kill).
#define RVA_SPAWN_CREEP 0x264B30
#define RVA_COMBAT_SCENE_MANAGER 0x2529E0
#define RVA_GS_FLEET_TYPE 0x396C50        // as GetAISpawnTier reads it
#define OFF_CLASSPTR_CREEP_NAV_SPAWN 0x3E12298 // YCreepNavigationSpawn UClass*, once registered
#define OFF_CLASSPTR_CREEP_NAV 0x3E11A58       // YCreepNavigation UClass* (registration 0x6007C0)
#define OFF_SPAWN_CLASS 0x428
#define OFF_SPAWN_TEAM 0x430
#define OFF_GM_ONS_SMALL_CLASS 0xA58
#define OFF_GM_ONS_MID_CLASS 0xA68
#define OFF_GM_ONS_BIG_CLASS 0xA78
#define ONS_FIRST_WAVE_DELAY_MS 20000
#define RF_CDO_OR_ARCHETYPE 0x30
#define OBJ_PENDING_KILL (1 << 29)

typedef void *(__fastcall *tSpawnCreep)(void *sceneMgr, void *cls, void *spawnAt, uint8_t *team);
typedef void *(__fastcall *tGetSceneManager)();
typedef uint8_t(__fastcall *tGsFleetType)(void *gs);

struct OnsSpawnPoint {
  void *actor;
  int size;  // 0 small, 1 mid, 2 big
  int team;  // 1 or 2
  bool initial;
};
struct OnsCreepState {
  void *gm;
  DWORD firstSeen;
  bool firstWave;
  int nPoints;
  OnsSpawnPoint points[256];
  DWORD missingSince[2][3];
  DWORD lastScan;
  int spawned, failed;
};
static OnsCreepState g_onsCreeps = {};
static void *g_onsBigShip[2] = {}; // last seen command ship per team, for the position log
static void *g_onsMidShip[2] = {}; // last seen assault ship per team, likewise
static void *g_onsBigNav = nullptr; // the map's ONS_AITargetH_Navigation node, if any

static bool SwitchOn(const char *envName, const char *markerFile);

static bool OnslaughtCreepsEnabled() {
  return !SwitchOn("DN_HOST_NO_ONSLAUGHT_CREEPS", "dn_host_no_onslaught_creeps.txt");
}

static int SizeFromName(const char *n) {
  if (!n) return -1;
  if (strstr(n, "AITargetH")) return 2;
  if (strstr(n, "AITargetM")) return 1;
  if (strstr(n, "AITargetL")) return 0;
  return -1;
}

// The map's Onslaught spawn points, by their own names.
static void FindOnslaughtSpawnPoints() {
  OnsCreepState &st = g_onsCreeps;
  st.nPoints = 0;
  UObjectMin *cls = *(UObjectMin **)(g_base + OFF_CLASSPTR_CREEP_NAV_SPAWN);
  if (!cls)
    return;
  FUObjectArrayMin *arr = GObjects();
  if (!arr || !arr->Objects)
    return;
  UObjectMin *navCls = *(UObjectMin **)(g_base + OFF_CLASSPTR_CREEP_NAV);
  g_onsBigNav = nullptr;
  for (int i = 0; i < arr->NumElements && st.nPoints < 256; ++i) {
    UObjectMin *o = arr->Objects[i].Object;
    if (o && navCls && o->Class == navCls && !(o->Flags & RF_CDO_OR_ARCHETYPE) && !g_onsBigNav &&
        !(arr->Objects[i].Flags & OBJ_PENDING_KILL)) {
      const char *nn = NameText(o->Name);
      if (nn && strstr(nn, "AITargetH_Navigation"))
        g_onsBigNav = o;
      continue;
    }
    if (!o || o->Class != cls || (o->Flags & RF_CDO_OR_ARCHETYPE) ||
        (arr->Objects[i].Flags & OBJ_PENDING_KILL))
      continue;
    const char *n = NameText(o->Name);
    int size = SizeFromName(n);
    int team = ((uint8_t *)o)[OFF_SPAWN_TEAM];
    if (size < 0 || (team != 1 && team != 2))
      continue;
    OnsSpawnPoint &p = st.points[st.nPoints++];
    p.actor = o;
    p.size = size;
    p.team = team;
    p.initial = n && strstr(n, "Initial");
  }
}

// The class the game mode spawns for a size; the big one by fleet tier.
static void *OnslaughtCreepClass(uint8_t *gm, uint8_t *gs, int size, void *pointClass) {
  void *cls = nullptr;
  if (size == 0)
    cls = *(void **)(gm + OFF_GM_ONS_SMALL_CLASS);
  else if (size == 1)
    cls = *(void **)(gm + OFF_GM_ONS_MID_CLASS);
  else {
    uint8_t fleet = ((tGsFleetType)(g_base + RVA_GS_FLEET_TYPE))(gs);
    int off = fleet == 3 ? 0x10 : fleet == 2 ? 0x08 : 0;
    cls = *(void **)(gm + OFF_GM_ONS_BIG_CLASS + off);
    if (!cls)
      cls = *(void **)(gm + OFF_GM_ONS_BIG_CLASS);
  }
  return cls ? cls : pointClass;
}

// The NPC AI switch (verified 2026-10-08 in the exe; not yet verified live).
// The AYNPCAIController constructor (0x276EA0) sets byte +0xAFA to 1, and while
// it is set the controller's tick (0x2BA2A0) and its AI think timer callback
// (0x2B23B0) both return at once: no TickBehavior, no behaviour, no movement,
// no firing. Only 0x288AE0 clears it. That function also starts the think
// timer (+0xAD0, interval +0x830). The game calls it from its own NPC
// spawner (0x2693A0, at 0x26A36C) and from the game mode's NPC-spawned
// handler (vtable +0x978 = 0x378DE0), and only while the match runs
// (GameState +0x569). Our assault and command ships never got it. They were
// registered with behaviour ROUTE and sat still (host 2026-10-08 08:44). So
// once a second, while the match runs, any M/H ship whose controller is
// still halted gets it.
#define OFF_NPC_AI_HALTED 0xAFA
#define RVA_NPC_START_AI 0x288AE0
#define OFF_GS_MATCH_RUNNING 0x569
#define OFF_GS_MATCH_OVER 0x56A
typedef void(__fastcall *tNpcStartAI)(void *ctrl);
static bool ClassIsAncestor(UObjectMin *c, void *ancestor) {
  for (int depth = 0; c && ancestor && depth < 32; ++depth, c = *(UObjectMin **)((uint8_t *)c + 0x30))
    if (c == ancestor)
      return true;
  return false;
}

// Alive Onslaught ships by team and size: instances of the creep classes the
// game mode and the spawn points name, not pending kill. Compared by class
// pointer: a name lookup per object costs syscalls (IsReadable), and a
// GObjects scan doing that per object once froze a host for 90 s (README).
static void CountOnslaughtCreeps(uint8_t *gm, uint8_t *gs, int alive[2][3]) {
  memset(alive, 0, sizeof(int) * 6);
  void *cls[16];
  int csize[16], nc = 0;
  auto add = [&](void *c, int size) {
    if (!c || nc >= 16) return;
    for (int i = 0; i < nc; ++i)
      if (cls[i] == c) return;
    cls[nc] = c;
    csize[nc++] = size;
  };
  add(*(void **)(gm + OFF_GM_ONS_SMALL_CLASS), 0);
  add(*(void **)(gm + OFF_GM_ONS_MID_CLASS), 1);
  for (int v = 0; v < 3; ++v)
    add(*(void **)(gm + OFF_GM_ONS_BIG_CLASS + 8 * v), 2);
  for (int i = 0; i < g_onsCreeps.nPoints; ++i)
    add(*(void **)((uint8_t *)g_onsCreeps.points[i].actor + OFF_SPAWN_CLASS), g_onsCreeps.points[i].size);
  FUObjectArrayMin *arr = GObjects();
  if (!arr || !arr->Objects || nc == 0)
    return;
  for (int i = 0; i < arr->NumElements; ++i) {
    UObjectMin *o = arr->Objects[i].Object;
    if (!o || (o->Flags & RF_CDO_OR_ARCHETYPE) || (arr->Objects[i].Flags & OBJ_PENDING_KILL))
      continue;
    for (int c = 0; c < nc; ++c) {
      if (o->Class == cls[c]) {
        int team = ((uint8_t *)o)[OFF_SHIP_TEAM];
        if (team == 1 || team == 2) {
          alive[team - 1][csize[c]]++;
          ReportFarAway((uint8_t *)o, "onslaught ship");
          if (csize[c] >= 1 && gs && gs[OFF_GS_MATCH_RUNNING] && !gs[OFF_GS_MATCH_OVER]) {
            uint8_t *ctrl = *(uint8_t **)((uint8_t *)o + 0x3F8); // APawn::Controller
            if (ctrl && IsReadable(ctrl + OFF_NPC_AI_HALTED, 1) && ctrl[OFF_NPC_AI_HALTED] &&
                ClassIsAncestor(((UObjectMin *)ctrl)->Class, *(void **)(g_base + 0x3E18DE0))) {
              ((tNpcStartAI)(g_base + RVA_NPC_START_AI))(ctrl);
              Logf("onslaught creeps: %s %p -- controller %p AI started (+0xAFA %d)",
                   csize[c] == 2 ? "command ship" : "assault ship", o, ctrl, ctrl[OFF_NPC_AI_HALTED]);
            }
          }
          if (csize[c] == 2)
            g_onsBigShip[team - 1] = o;
          else if (csize[c] == 1)
            g_onsMidShip[team - 1] = o;
        }
        break;
      }
    }
  }
}

// The fault address of a caught exception, for the log.
static int CaptureFault(EXCEPTION_POINTERS *ep, uintptr_t *at) {
  *at = ep && ep->ExceptionRecord ? (uintptr_t)ep->ExceptionRecord->ExceptionAddress : 0;
  return EXCEPTION_EXECUTE_HANDLER;
}

// AActor::SpawnCollisionHandlingMethod (+0x179, registration 0x141D57F40) on
// the class default object, which SpawnCreep's default spawn parameters fall
// back to. The assault ships were refused at their spawn points ("Couldn't
// spawn Pawn of type VH_ONS_AITargetM_Pawn_BP_C at ONS_AITargetM_Spawn_T2_n",
// host 2026-10-07 23:41) -- the engine's answer when a spawn collides and the
// method says not to. 2 = AdjustIfPossibleButAlwaysSpawn. GUESS: the original
// server spawned them with its own parameters; this is the CDO-side
// equivalent. Once per class.
#define OFF_ACTOR_SPAWN_COLLISION 0x179
#define SPAWN_ADJUST_BUT_ALWAYS 2
static void AllowCreepSpawnWhenColliding(void *cls) {
  static void *s_done[16];
  static int s_n = 0;
  for (int i = 0; i < s_n; ++i)
    if (s_done[i] == cls)
      return;
  if (s_n >= 16)
    return;
  s_done[s_n++] = cls;
  FUObjectArrayMin *arr = GObjects();
  if (!arr || !arr->Objects)
    return;
  for (int i = 0; i < arr->NumElements; ++i) {
    UObjectMin *o = arr->Objects[i].Object;
    if (o && o->Class == cls && (o->Flags & 0x10)) { // RF_ClassDefaultObject
      uint8_t was = ((uint8_t *)o)[OFF_ACTOR_SPAWN_COLLISION];
      ((uint8_t *)o)[OFF_ACTOR_SPAWN_COLLISION] = SPAWN_ADJUST_BUT_ALWAYS;
      const char *n = NameText(((UObjectMin *)cls)->Name);
      Logf("onslaught creeps: %s spawn collision handling %d -> %d", n ? n : "?", was, SPAWN_ADJUST_BUT_ALWAYS);
      return;
    }
  }
}

// The assault ship and command ship are NOT creeps. VH_ONS_AITargetL (the
// fighter) is a YCreepPawn with m_creepAIController; AITargetM/H are ordinary
// AI ships with APawn::AIControllerClass (+0x3D8, ..._Behavior_YAICtrl_BP), and
// SpawnCreep faulted on every one of them at 0x264E4C, walking the class chain
// of creep-only field +0xCB8 (host 2026-10-07 23:50): the actor was left half
// made -- an icon and a hitbox, no ship (operator). For them this does what
// SpawnCreep does without the creep part: UWorld::SpawnActor 0x1A0C8D0 at the
// spawn point's transform (root component +0x198: rotation quat +0x150,
// location +0x160, quat -> rotator 0xC40250), SetTeam 0x5961C0, then the
// pawn's own APawn::SpawnDefaultController (vtable +0x6B0; exec thunk
// 0x1EB9560 jumps there), and a net update (+0x580), as SpawnCreep ends.
#define RVA_WORLD_SPAWN_ACTOR 0x1A0C8D0
#define RVA_SPAWN_PARAMS_CTOR 0x1CD1780
#define RVA_PAWN_SET_TEAM 0x5961C0
#define RVA_QUAT_TO_ROTATOR 0xC40250
#define OFF_ACTOR_ROOT_COMPONENT 0x198
#define OFF_COMP_WORLD_ROT 0x150
#define OFF_COMP_WORLD_LOC 0x160
#define VT_ACTOR_GET_WORLD 0x108
#define VT_PAWN_SPAWN_DEFAULT_CONTROLLER 0x6B0
#define OFF_PAWN_CONTROLLER 0x3F8            // APawn::Controller
#define OFF_PAWN_AI_CONTROLLER_CLASS 0x3D8   // APawn::AIControllerClass
#define OFF_NPC_BEHAVIOUR_STATE 0xA38        // EYCombatSceneBehavior the scene spawn resets
#define YCSB_NONE 0x14
#define VT_CONTROLLER_POSSESS 0x640          // AController::Possess (as 0x26A096 / SpawnCreep)
#define VT_CONTROLLER_POSSESS2 0x668         // called right after it in both
typedef void(__fastcall *tPossess)(void *ctrl, void *pawn);
#define OFF_CLASSPTR_NPC_AI_CONTROLLER 0x3E18DE0 // UClass* owning m_spawnActor
#define OFF_NPC_SPAWN_ACTOR 0x9EC            // TWeakObjectPtr<AActor> m_spawnActor
#define OFF_NAV_NEXT_ARRAY 0x3E8             // YCreepNavigation next points (weak)
#define OFF_NAV_NEXT_COUNT 0x3F0
#define RVA_WEAK_PTR_SET 0xD64B60            // TWeakObjectPtr = UObject*
#define RVA_WEAK_PTR_GET 0xD6AD50            // TWeakObjectPtr -> UObject*
typedef void(__fastcall *tWeakSet)(void *weak, void *obj);
typedef void *(__fastcall *tWeakGet)(void *weak);
typedef void *(__fastcall *tSpawnActor)(void *world, void *cls, const float *loc, const float *rot, void *params);
typedef void *(__fastcall *tSpawnParamsCtor)(void *params);
typedef void(__fastcall *tPawnSetTeam)(void *pawn, uint8_t *team);
typedef float *(__fastcall *tQuatToRot)(const float *quat, float *rot);
typedef void *(__fastcall *tGetWorld)(void *actor);
typedef void(__fastcall *tActorVoidFn)(void *actor);

// Why the assault and command ships sat still (verified 2026-10-08 from the
// cooked Blueprints and the exe; not yet verified live):
//   - Their controllers (VH_ONS_AITarget{M,H}_Behavior_YAICtrl_BP, decoded from
//     the Kismet bytecode) move the ship ONLY through the combat scene manager:
//     TickBehavior -> RouteStart = cast<YCreepNavigation>(m_spawnActor) ->
//     combatManager.ReplacePawnNPCBehaviours(pawn, [{m_behaviorTime -1,
//     m_behaviorType ROUTE (16), m_newControlValues RouteController (None),
//     m_location RouteStart, m_targetActor GetNPCId(pawn)}], false). The
//     combat manager is GetPlayerController(0).GetCombatManager() (thunk
//     0x74FFA0 -> the scene manager singleton 0x2529E0).
//   - And only when m_runBehaviourTree is set; otherwise it draws "Behavior
//     Tree Disabled". m_runBehaviourTree is the controller byte +0x780 (its
//     reflection setter 0x786BE0), which the scene manager sets when it spawns
//     an NPC (0x2693A0: = !entry+0xA0) and nothing else did for ours.
//   - ReplacePawnNPCBehaviours (exec 0x76F2D0 -> 0x2600E0) does nothing unless
//     GetNPCId (0x254360) finds the pawn in the scene manager's NPC table
//     (+0x3C0 data, +0x3C8 count, 0xA8-byte entries, weak pawn at +0x8C):
//     ours were never in it, so it returned -1.
// So this registers the ship the way the scene manager's own NPC spawn does
// (0x2693A0 ~0x26A096): AddNPCSpawn (0x23A280, the native behind the
// AddNPCSpawn UFunction) with the pawn class (+0x20), spawn point (+0x30) and
// team (+0x38); then the instanced flag (+0x88 = 1, so the scene manager's own
// tick never spawns the entry again) and the weak pawn (+0x8C), the
// controller's scene index (+0xAAC) and +0xAB0 (= entry+0x10), behaviour state
// YCSB_NONE (+0xA38), m_runBehaviourTree; then the ROUTE behaviour the
// Blueprint itself asks for, through ReplaceNPCBehaviours (0x25FE80: stores
// the behaviours, sets +0x780, NextBehaviour 0x28E340, register 0x254990).
// NOT done (left for evidence): the scene manager also spawns an NPC
// PlayerState, calls the level script's OnNPCSpawned (0x572220) and the game
// mode's vtable +0x978.
#define RVA_SCENE_ADD_NPC_SPAWN 0x23A280     // (sceneMgr, FYNPCSpawn*) -> id
#define RVA_SCENE_REPLACE_NPC_BEHAVIOURS 0x25FE80 // (sceneMgr, id, TArray* by value, bool)
#define RVA_FMEMORY_MALLOC 0xC06B70          // (size, align); GMalloc vtable +0x10
#define OFF_SCENE_NPCS 0x3C0                 // TArray<FYNPCSpawn> data; count +0x3C8
#define OFF_SCENE_NPCS_COUNT 0x3C8
#define OFF_SCENE_RANDOM_CLASS 0x418         // AddNPCSpawn replaces +0x20 with a random class when set
#define NPC_SPAWN_SIZE 0xA8
#define RVA_SCENE_START_COMBAT 0x2681A0      // (sceneMgr, bool notifyLevelScript); no-op unless +0x444
#define OFF_SCENE_COMBAT_NOT_STARTED 0x444   // byte; tick spawns nothing while set
#define OFF_NPCSPAWN_TIME 0x00               // float world time the tick spawns the entry at
#define OFF_NPCSPAWN_TYPE 0x10               // int, AddNPCSpawn's default 5; copied to ctrl +0xAB0
#define OFF_NPCSPAWN_DATASET_ID 0x18          // FName: AddNPCSpawnByDataset's pawn id, resolved at spawn
#define OFF_NPCSPAWN_PAWN_CLASS 0x20
#define OFF_NPCSPAWN_SPAWN_POINT 0x30
#define OFF_NPCSPAWN_TEAM 0x38
#define OFF_NPCSPAWN_INSTANCED 0x88
#define OFF_NPCSPAWN_PAWN 0x8C               // TWeakObjectPtr<APawn>
#define OFF_NPC_RUN_BEHAVIOUR_TREE 0x780
#define OFF_NPC_SCENE_INDEX 0xAAC
#define OFF_NPC_SCENE_TYPE 0xAB0
#define YCSB_ROUTE 16
#define BEHAVIOUR_DATA_SIZE 0x28             // FYCombatSceneBehaviorData (0x702345)
typedef int(__fastcall *tSceneAddNpcSpawn)(void *scene, void *spawn);
typedef void(__fastcall *tSceneStartCombat)(void *scene, uint8_t notifyLevelScript);
typedef void(__fastcall *tSceneReplaceBehaviours)(void *scene, int id, void *behaviours, uint8_t keepTree);
typedef void *(__fastcall *tFMemoryMalloc)(size_t size, uint32_t align);
struct TArrayRaw {
  void *data;
  int32_t num, max;
};

static bool ClassIsA(UObjectMin *c, void *ancestor) {
  for (; c && ancestor; c = *(UObjectMin **)((uint8_t *)c + OFF_USTRUCT_SUPER))
    if (c == ancestor)
      return true;
  return false;
}

// Returns the scene NPC id, or -1.
static int RegisterOnslaughtNPC(void *pawn, void *ctrl, void *cls, void *routeStart, int team) {
  uint8_t *scene = (uint8_t *)((tGetSceneManager)(g_base + RVA_COMBAT_SCENE_MANAGER))();
  if (!scene)
    return -1;
  // AddNPCSpawn's own defaults (exec thunk 0x73FAE0).
  alignas(16) uint8_t spawn[NPC_SPAWN_SIZE] = {};
  *(float *)(spawn + 0x5C) = 10.0f;
  *(float *)(spawn + 0x9C) = -1.0f;
  *(int32_t *)(spawn + 0x8C) = -1; // weak pointers: index -1 = null
  *(int32_t *)(spawn + 0x94) = -1;
  *(int32_t *)(spawn + OFF_NPCSPAWN_TYPE) = 5;
  *(void **)(spawn + OFF_NPCSPAWN_PAWN_CLASS) = cls;
  *(void **)(spawn + OFF_NPCSPAWN_SPAWN_POINT) = routeStart;
  spawn[OFF_NPCSPAWN_TEAM] = (uint8_t)team;
  uint8_t randomClass = scene[OFF_SCENE_RANDOM_CLASS];
  scene[OFF_SCENE_RANDOM_CLASS] = 0;
  int id = ((tSceneAddNpcSpawn)(g_base + RVA_SCENE_ADD_NPC_SPAWN))(scene, spawn);
  scene[OFF_SCENE_RANDOM_CLASS] = randomClass;
  if (id < 0 || id >= *(int32_t *)(scene + OFF_SCENE_NPCS_COUNT))
    return -1;
  uint8_t *entry = *(uint8_t **)(scene + OFF_SCENE_NPCS) + (size_t)id * NPC_SPAWN_SIZE;
  entry[OFF_NPCSPAWN_INSTANCED] = 1;
  ((tWeakSet)(g_base + RVA_WEAK_PTR_SET))(entry + OFF_NPCSPAWN_PAWN, pawn);
  uint8_t *c = (uint8_t *)ctrl;
  *(int32_t *)(c + OFF_NPC_SCENE_INDEX) = id;
  *(int32_t *)(c + OFF_NPC_SCENE_TYPE) = *(int32_t *)(entry + OFF_NPCSPAWN_TYPE);
  c[OFF_NPC_BEHAVIOUR_STATE] = YCSB_NONE;
  c[OFF_NPC_RUN_BEHAVIOUR_TREE] = 1;
  // The Blueprint's ROUTE, now, so the ship moves even if its BeginPlay found
  // no player controller to take the combat manager from. The callee takes
  // the array by value and frees it (FMemory::Free 0xBFC9C0).
  uint8_t *b = (uint8_t *)((tFMemoryMalloc)(g_base + RVA_FMEMORY_MALLOC))(BEHAVIOUR_DATA_SIZE, 0);
  if (b) {
    memset(b, 0, BEHAVIOUR_DATA_SIZE);
    *(float *)(b + 0x00) = -1.0f;              // m_behaviorTime
    b[0x08] = YCSB_ROUTE;                      // m_behaviorType
    *(void **)(b + 0x10) = nullptr;            // m_newControlValues (RouteController, unset in the BP)
    *(void **)(b + 0x18) = routeStart;         // m_location
    *(int32_t *)(b + 0x20) = id;               // m_targetActor (the BP passes GetNPCId(pawn))
    TArrayRaw arr = {b, 1, 1};
    ((tSceneReplaceBehaviours)(g_base + RVA_SCENE_REPLACE_NPC_BEHAVIOURS))(scene, id, &arr, 0);
  }
  return id;
}

// The NPC PlayerState the combat scene manager gives every NPC ship it spawns
// (0x2693A0, 0x26A234-0x26A328), which carries the name the HUD shows over the
// ship. Without it the nameplate fell back to the class name, cut to 20
// characters: "VH_ONS_AITargetH_Paw" (operator screenshot 2026-10-16 22:32).
// The game's sequence, mirrored: FActorSpawnParameters with Owner (+0x10) =
// the controller, +0x28 = 1, flag bit 0x40 at +0x2C; UWorld::SpawnActor of the
// class from 0x603600; then the name -- entry +0x60 FName -> FString ->
// PlayerState vtable +0x5E0 (else vtable +0x5E8 with controller +0x6A8 when
// the FName is None; we always pass our own name); ps +0x4B0 = 0x57B8A0(pawn),
// ps +0x4B8 assigned (0x237A70) from 0x57A9F0(pawn), ps +0x4C0 = *0x57BAE0(pawn,
// &tmp); finally pawn +0x658 = ps. What the three ps fields mean is NOT known
// (copied as the game does). Not verified live yet.
#define RVA_NPC_PLAYERSTATE_CLASS 0x603600 // GetPrivateStaticClass("/Script/DreadGame")
#define VT_PS_SET_PLAYER_NAME 0x5E0
#define OFF_PAWN_NPC_PLAYERSTATE 0x658
typedef void *(__fastcall *tStaticClassPkg)(const wchar_t *pkg);
typedef void(__fastcall *tSetPlayerName)(void *ps, FStringMin *name);
typedef int(__fastcall *tPawnInt)(void *pawn);
typedef void *(__fastcall *tPawnPtr)(void *pawn);
typedef void(__fastcall *tAssignPtr)(void *dst, void *src);
typedef uint8_t *(__fastcall *tPawnOutByte)(void *pawn, void *tmp);
static void *GiveNpcPlayerState(void *world, void *pawn, void *ctrl, const wchar_t *name) {
  __try {
    void *psCls = ((tStaticClassPkg)(g_base + RVA_NPC_PLAYERSTATE_CLASS))(L"/Script/DreadGame");
    if (!psCls)
      return nullptr;
    alignas(16) uint8_t params[0x80] = {};
    ((tSpawnParamsCtor)(g_base + RVA_SPAWN_PARAMS_CTOR))(params);
    *(void **)(params + 0x10) = ctrl;
    params[0x28] = 1;
    *(uint32_t *)(params + 0x2C) |= 0x40;
    uint8_t *ps = (uint8_t *)((tSpawnActor)(g_base + RVA_WORLD_SPAWN_ACTOR))(world, psCls, nullptr, nullptr, params);
    if (!ps)
      return nullptr;
    FStringMin fs = {(wchar_t *)name, (int32_t)wcslen(name) + 1, (int32_t)wcslen(name) + 1};
    ((tSetPlayerName)(*(void ***)ps)[VT_PS_SET_PLAYER_NAME / 8])(ps, &fs);
    *(int32_t *)(ps + 0x4B0) = ((tPawnInt)(g_base + 0x57B8A0))(pawn);
    ((tAssignPtr)(g_base + 0x237A70))(ps + 0x4B8, ((tPawnPtr)(g_base + 0x57A9F0))(pawn));
    alignas(16) uint8_t tmp[16] = {};
    ps[0x4C0] = *((tPawnOutByte)(g_base + 0x57BAE0))(pawn, tmp);
    *(void **)((uint8_t *)pawn + OFF_PAWN_NPC_PLAYERSTATE) = ps;
    const char *cn = NameText(((UObjectMin *)psCls)->Name);
    Logf("onslaught creeps: %p named \"%ls\" (PlayerState %p %s)", pawn, name, ps, cn ? cn : "?");
    return ps;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("onslaught creeps: NPC PlayerState for %p faulted", pawn);
    return nullptr;
  }
}

static int g_onsLastNpcId = -1; // scene NPC id of the newest assault/command ship (/spawn elites)
static const wchar_t *g_onsNextName = nullptr; // nameplate override for the next AI ship (/spawn <ShipId>)
static void *g_onsNextCtrlCls = nullptr;       // controller class when the pawn names none (/spawn <ShipId>)
static const char *PropNameAt(UObjectMin *obj, uint32_t off);
static int FixNpcFireAbilities(void *ctrl, void *pawn, bool log);
static void NoteNpcFireAbilities(void *ctrl, void *pawn);
typedef void (*tParmFillNamed)(uint8_t *parms, const char *propClass, const char *propName, uint32_t off, void *ctx);
static bool CallNamed(void *obj, const wchar_t *name, tParmFillNamed fill, void *ctx, uint8_t *parms);
static bool CallNamed(void *obj, const wchar_t *name, tParmFillNamed fill, void *ctx);
static inline int HvcMax(int a, int b) { return a > b ? a : b; }
static bool HavocIsEliminated(void *pc);
#define OFF_PAWN_ABILITY_MANAGER 0x888 // AYPawn::m_abilityManager
static const char *g_onsNextShip = nullptr;     // Havoc ShipId whose dataset abilities to install (/spawn)
static bool g_spawnNoGameBot = false; // heralds, hunters: an AI pawn at a player start, never a TDM game bot
static bool g_spawnAtStart = false;             // /spawn outside Onslaught: `at` is a player start, no route
static void InstallNpcAbilities(void *pawn, const char *ship);
static void *SpawnOnslaughtAIShip(uint8_t *gm, void *cls, void *at, int team, bool bigShip) {
  uint8_t *comp = *(uint8_t **)((uint8_t *)at + OFF_ACTOR_ROOT_COMPONENT);
  if (!comp)
    return nullptr;
  float quat[4], loc[3], rot[3] = {0, 0, 0};
  memcpy(quat, comp + OFF_COMP_WORLD_ROT, sizeof(quat));
  memcpy(loc, comp + OFF_COMP_WORLD_LOC, sizeof(loc));
  if (!bigShip) { // collision is waived at the spawn point, so they stacked (2026-10-17 18:28)
    loc[0] += (float)(rand() % 4001 - 2000); // GUESS: 2000 uu clears an assault ship
    loc[1] += (float)(rand() % 4001 - 2000);
  }
  ((tQuatToRot)(g_base + RVA_QUAT_TO_ROTATOR))(quat, rot);
  void *world = ((tGetWorld)(*(void ***)gm)[VT_ACTOR_GET_WORLD / 8])(gm);
  if (!world)
    return nullptr;
  alignas(16) uint8_t params[0x80] = {};
  ((tSpawnParamsCtor)(g_base + RVA_SPAWN_PARAMS_CTOR))(params);
  // /boss DreadnoughtMediumBoss01 came back null with no other trace (Onslaught
  // 16:28 2026-10-18, two bosses already at the points): a refused colliding
  // spawn. +0x28 SpawnCollisionHandlingOverride, as SpawnHerald.
  params[0x28] = SPAWN_ADJUST_BUT_ALWAYS;
  void *pawn = ((tSpawnActor)(g_base + RVA_WORLD_SPAWN_ACTOR))(world, cls, loc, rot, params);
  if (!pawn)
    return Logf("onslaught creeps: SpawnActor %s refused at %.0f %.0f %.0f", ClassNameOf(cls), loc[0], loc[1], loc[2]), nullptr;
  uint8_t t = (uint8_t)team;
  ((tPawnSetTeam)(g_base + RVA_PAWN_SET_TEAM))(pawn, &t);
  // Mark it an AI target. The kill handler (0x375850) sends a kill to the
  // game mode's AI-ship scoring -- scoring manager vtable +0x208 -> the
  // Invasion handler 0x415AF0 ("Invasion ScoringEvent - Kill AI!!!"), which
  // picks AITargetH/M/L by the victim's class against the game mode's own
  // creep classes (+0xA78/+0xA80/+0xA88, +0xA68, +0xA58) -- only when the
  // victim's YPawn vtable +0x7E0 is true, and that is just the byte at
  // +0x950 (0x57FD70). Fighters have it; the assault ship and command ship
  // Blueprints do not, so destroying them scored nothing (operator,
  // 2026-10-08) although the handler names their classes: the server that
  // spawned them set it.
  // NOT set here any more (2026-10-08): it is set only for the kill itself,
  // in HookTdmKilled. As an AI target the ship was never registered in the
  // game's list of ships (0x57FF20 adds only pawns whose +0x7E0 is false; the
  // death/destroy removals 0x571500/0x575970 skip them too), and that list is
  // where the Onslaught bots' GetCommandShip (0x28BDA0: ship class 0x1D,
  // YSC_AI_CREEP_LARGE) looks. Every bot got None ("Accessed None" floods from
  // VH_ONS_ScoutLightStandard_Behavior TickBehaviorPerception, host logs
  // 2026-10-08) and had no objective. Off, the ship is also in the scene
  // manager's ship list that turrets (CAPITALSHIP) and the AI perceive.
  void **vt = *(void ***)pawn;
  // The controller, set up the way the combat scene manager sets up the NPC
  // ships it spawns (0x2693A0, around 0x26A096): spawn the pawn's
  // AIControllerClass (+0x3D8), set its m_spawnActor (TWeakObjectPtr +0x9EC)
  // and m_runBehaviourTree, possess (vtable +0x640, then +0x668), then
  // register it with the scene manager (RegisterOnslaughtNPC above). Two
  // earlier versions skipped the registration ("these ships belong to no
  // scene") and every assault ship and command ship sat still (host
  // 2026-10-08 00:51, operator): the Blueprint's only move order goes through
  // the scene manager's NPC table.
  // m_spawnActor is the spawn point itself, as the scene manager sets it
  // (entry +0x30), when it is a YCreepNavigation -- the Blueprint casts it to
  // one to start its ROUTE from; else the spawn point's first next-navigation
  // point (+0x3E8 weak array, count +0x3F0).
  void *ctrl = *(void **)((uint8_t *)pawn + OFF_PAWN_CONTROLLER);
  bool hadController = ctrl != nullptr;
  if (!ctrl) {
    void *ctrlCls = *(void **)((uint8_t *)pawn + OFF_PAWN_AI_CONTROLLER_CLASS);
    if (!ctrlCls)
      ctrlCls = g_onsNextCtrlCls;
    if (ctrlCls) {
      alignas(16) uint8_t cparams[0x80] = {};
      ((tSpawnParamsCtor)(g_base + RVA_SPAWN_PARAMS_CTOR))(cparams);
      ctrl = ((tSpawnActor)(g_base + RVA_WORLD_SPAWN_ACTOR))(world, ctrlCls, loc, rot, cparams);
    }
  }
  if (!ctrl) {
    Logf("onslaught creeps: %p got no controller (no AIControllerClass, or its spawn failed)", pawn);
  } else {
    bool isNpc = ClassIsA(((UObjectMin *)ctrl)->Class, *(void **)(g_base + OFF_CLASSPTR_NPC_AI_CONTROLLER));
    void *navCls = *(void **)(g_base + OFF_CLASSPTR_CREEP_NAV);
    void *nav = nullptr;
    int next = g_spawnAtStart ? 0 : *(int32_t *)((uint8_t *)at + OFF_NAV_NEXT_COUNT);
    if (ClassIsA(((UObjectMin *)at)->Class, navCls))
      nav = at;
    else if (g_spawnAtStart)
      // GUESS: registered with the start as its route start the ship still
      // perceives and fights; the Blueprint's ROUTE cast to YCreepNavigation fails.
      nav = at;
    else if (next > 0)
      nav = ((tWeakGet)(g_base + RVA_WEAK_PTR_GET))(*(void **)((uint8_t *)at + OFF_NAV_NEXT_ARRAY));
    else if (bigShip && g_onsBigNav)
      // GUESS: a command ship spawn point that is no navigation point and has
      // no next point; the map's command-ship route node is where it heads.
      nav = g_onsBigNav;
    if (isNpc) {
      ((tWeakSet)(g_base + RVA_WEAK_PTR_SET))((uint8_t *)ctrl + OFF_NPC_SPAWN_ACTOR, nav ? nav : at);
      ((uint8_t *)ctrl)[OFF_NPC_BEHAVIOUR_STATE] = YCSB_NONE;
      ((uint8_t *)ctrl)[OFF_NPC_RUN_BEHAVIOUR_TREE] = 1;
    }
    // Dataset abilities (kept; the modules fix is FixNpcFireAbilities below).
    if (g_onsNextShip)
      InstallNpcAbilities(pawn, g_onsNextShip);
    void **cvt = *(void ***)ctrl;
    ((tPossess)cvt[VT_CONTROLLER_POSSESS / 8])(ctrl, pawn);
    ((tPossess)cvt[VT_CONTROLLER_POSSESS2 / 8])(ctrl, pawn);
    if (g_onsNextShip) {
      // (Not AbilitiesInitialized / InitializeTriggerAbilityDelegates: those
      // natives belong to YLEDLightingComponent and the YVimInstance HUD
      // widgets; boss11 called one by name and FindFunctionChecked killed the
      // battle.)
      FixNpcFireAbilities(ctrl, pawn, true);
      NoteNpcFireAbilities(ctrl, pawn);
    }
    int npcId = -1;
    if (isNpc && nav)
      npcId = g_onsLastNpcId = RegisterOnslaughtNPC(pawn, ctrl, cls, nav, team);
    if (!SwitchOn("DN_HOST_NO_NPC_NAMES", "dn_host_no_npc_names.txt"))
      GiveNpcPlayerState(world, pawn, ctrl, g_onsNextName ? g_onsNextName : bigShip ? L"Command Ship" : L"Assault Ship");
    const char *an = NameText(((UObjectMin *)at)->Name);
    const char *nn = nav ? NameText(((UObjectMin *)nav)->Name) : nullptr;
    const char *cn = NameText(((UObjectMin *)ctrl)->Class->Name);
    Logf("onslaught creeps: %p controller %s (%s, %s), spawn point %s (%d next); route start %s; "
         "scene NPC id %d, behaviour %d, run tree %d",
         pawn, cn ? cn : "?", hadController ? "auto-possessed" : "spawned here",
         isNpc ? "NPC" : "NOT an NPC controller", an ? an : "?", next, nav ? (nn ? nn : "?") : "NONE", npcId,
         ((uint8_t *)ctrl)[OFF_NPC_BEHAVIOUR_STATE], ((uint8_t *)ctrl)[OFF_NPC_RUN_BEHAVIOUR_TREE]);
  }
  ((tActorVoidFn)vt[0x580 / 8])(pawn); // AActor::ForceNetUpdate (VT_ACTOR_FORCE_NET_UPDATE)
  return pawn;
}

// /bots N (CommandsTick). Havoc: BotsTick copies each wave enemy N-1 times.
// Onslaught: the enemy team's fighter and assault-ship caps are multiplied by
// N (command ships stay one), so the spawner tick keeps N times as many alive.
static int g_botsMult = 1;
static int CollectHumanPCs(uint8_t *gm, uint8_t **out, int max);

static bool SpawnOnslaughtCreep(uint8_t *gm, uint8_t *gs, int team, int size, bool initialOnly) {
  OnsCreepState &st = g_onsCreeps;
  int cand[256], nc = 0;
  for (int i = 0; i < st.nPoints; ++i) {
    OnsSpawnPoint &p = st.points[i];
    if (p.team == team && p.size == size && p.initial == initialOnly)
      cand[nc++] = i;
  }
  if (nc == 0)
    for (int i = 0; i < st.nPoints; ++i)
      if (st.points[i].team == team && st.points[i].size == size)
        cand[nc++] = i;
  if (nc == 0)
    return false;
  OnsSpawnPoint &p = st.points[cand[rand() % nc]];
  void *scene = ((tGetSceneManager)(g_base + RVA_COMBAT_SCENE_MANAGER))();
  if (!scene) {
    if (st.failed++ < 3)
      Logf("onslaught creeps: no AYAICombatSceneManager in the world; cannot spawn");
    return false;
  }
  void *cls = OnslaughtCreepClass(gm, gs, size, *(void **)((uint8_t *)p.actor + OFF_SPAWN_CLASS));
  if (!cls)
    return false;
  uint8_t t = (uint8_t)team;
  const char *cn = NameText(((UObjectMin *)cls)->Name);
  const char *pn = NameText(((UObjectMin *)p.actor)->Name);
  AllowCreepSpawnWhenColliding(cls);
  void *pawn = nullptr;
  uintptr_t faultAt = 0;
  __try {
    pawn = size == 0 ? ((tSpawnCreep)(g_base + RVA_SPAWN_CREEP))(scene, cls, p.actor, &t)
                     : SpawnOnslaughtAIShip(gm, cls, p.actor, team, size == 2);
  } __except (CaptureFault(GetExceptionInformation(), &faultAt)) {
    if (st.failed++ < 40)
      Logf("onslaught creeps: EXCEPTION at exe+0x%llX spawning %s for team %d at %s",
           (unsigned long long)(faultAt - g_base), cn ? cn : "?", team, pn ? pn : "?");
    return false;
  }
  if (pawn) {
    st.spawned++;
    Logf("onslaught creeps: spawned %s for team %d at %s -> %p", cn ? cn : "?", team, pn ? pn : "?", pawn);
  } else if (st.failed++ < 20) {
    Logf("onslaught creeps: SpawnCreep returned null (%s, team %d, at %s)", cn ? cn : "?", team, pn ? pn : "?");
  }
  return pawn != nullptr;
}

// Diagnostics for "turrets, bots and the AI ships never target each other"
// (operator report 2026-10-08). Verified in the exe, the candidates all come
// from the combat scene manager. Its tick (0x2693A0) rebuilds three lists from
// the world's pawns every +0x3FC seconds:
//   - +0x448/+0x450: ships that are not AI targets (vtable +0x7E0) and alive
//     (+0x4E8);
//   - +0x458/+0x460: AI targets whose controller is no YFighterDroneAIController
//     -- the fighters, assault ships and command ships;
//   - +0x468/+0x470: fighter drones.
// Turrets (0x272490) cycle them by their m_targetShipTypes (+0x540: the
// support auto gun has CAPITALSHIP, CREEP, JET -- all three). The AI's candidate
// pass (0x2C6180) drops pawns whose state byte +0x941 (0x57BD10) is 2 or 3.
// This logs the list sizes, how many of list 2 are the mod's ships, and each
// sampled ship's state, so one match shows which step drops them.
#define OFF_PAWN_STATE 0x941
static void LogTargetLists() {
  uint8_t *sm = (uint8_t *)((tGetSceneManager)(g_base + RVA_COMBAT_SCENE_MANAGER))();
  if (!sm || !IsReadable(sm + 0x448, 0x30))
    return;
  int n[3] = {*(int32_t *)(sm + 0x450), *(int32_t *)(sm + 0x460), *(int32_t *)(sm + 0x470)};
  int ours = 0, oursShips = 0, states[4] = {0, 0, 0, 0};
  uint8_t *ships = *(uint8_t **)(sm + 0x448);
  if (ships && n[0] > 0 && n[0] < 4096 && IsReadable(ships, (size_t)n[0] * 8))
    for (int i = 0; i < n[0]; ++i) {
      uint8_t *p = (uint8_t *)((tWeakGet)(g_base + RVA_WEAK_PTR_GET))(ships + (size_t)i * 8);
      if (p && CreepSize(p) >= 1)
        ++oursShips;
    }
  uint8_t *data = *(uint8_t **)(sm + 0x458);
  if (data && n[1] > 0 && n[1] < 4096 && IsReadable(data, (size_t)n[1] * 8))
    for (int i = 0; i < n[1]; ++i) {
      uint8_t *p = (uint8_t *)((tWeakGet)(g_base + RVA_WEAK_PTR_GET))(data + (size_t)i * 8);
      if (!p || !IsReadable(p + OFF_PAWN_STATE, 1))
        continue;
      if (CreepSize(p) >= 0)
        ++ours;
      int st = p[OFF_PAWN_STATE];
      if (st >= 0 && st < 4)
        ++states[st];
    }
  Logf("onslaught targets: scene manager lists ships %d (%d assault/command ships) / AI targets %d (%d Onslaught "
       "ships; states 0:%d 1:%d 2:%d 3:%d) / drones %d", n[0], oursShips, n[1], ours, states[0], states[1], states[2],
       states[3], n[2]);
}

static void OnslaughtCreepTick(uint8_t *gm) {
  static int s_enabled = -1;
  if (s_enabled < 0)
    s_enabled = OnslaughtCreepsEnabled() ? 1 : 0;
  if (!s_enabled)
    return;
  __try {
    uint8_t *gs = InvasionGameState(gm);
    if (!gs)
      return;
    OnsCreepState &st = g_onsCreeps;
    DWORD now = GetTickCount();
    if (st.gm != gm) {
      memset(&st, 0, sizeof(st));
      st.gm = gm;
      st.firstSeen = now;
    }
    if (g_ons.gm == gm && g_ons.ended)
      return;
    if (IsReadable(gs + OFF_GS_WINNING_TEAM_FINAL, 1) && gs[OFF_GS_WINNING_TEAM_FINAL] != 0)
      return; // the match has a result
    if (now - st.firstSeen < ONS_FIRST_WAVE_DELAY_MS)
      return;
    if (st.nPoints == 0 || now - st.lastScan > 30000) {
      st.lastScan = now;
      int before = st.nPoints;
      FindOnslaughtSpawnPoints();
      if (st.nPoints != before)
        Logf("onslaught creeps: %d spawn points on this map", st.nPoints);
      if (st.nPoints == 0)
        return;
    }
    int maxN[3] = {*(int32_t *)(gm + 0xA60), *(int32_t *)(gm + 0xA70), *(int32_t *)(gm + 0xA90)};
    int every[3] = {*(int32_t *)(gm + 0xA64), *(int32_t *)(gm + 0xA74), *(int32_t *)(gm + 0xA94)};
    const int defMax[3] = {12, 6, 1}, defEvery[3] = {2, 8, 180};
    for (int k = 0; k < 3; ++k) {
      if (maxN[k] <= 0 || maxN[k] > 64) maxN[k] = defMax[k];
      if (every[k] <= 0 || every[k] > 600) every[k] = defEvery[k];
    }
    // Team humans are on, read from their ships (GUESS: team 1 when none
    // is alive yet, as in CommandNuke).
    int humanTeam = 1;
    {
      uint8_t *pcs[64];
      int np = CollectHumanPCs(gm, pcs, 64);
      for (int i = 0; i < np; ++i) {
        uint8_t *hp = IsReadable(pcs[i] + OFF_CONTROLLER_PAWN, 8) ? *(uint8_t **)(pcs[i] + OFF_CONTROLLER_PAWN) : nullptr;
        if (hp && IsReadable(hp, OFF_SHIP_TEAM + 1) && (hp[OFF_SHIP_TEAM] == 1 || hp[OFF_SHIP_TEAM] == 2)) {
          humanTeam = hp[OFF_SHIP_TEAM];
          break;
        }
      }
    }
    auto capFor = [&](int t, int k) { return t != humanTeam && k < 2 ? maxN[k] * g_botsMult : maxN[k]; };
    int alive[2][3];
    CountOnslaughtCreeps(gm, gs, alive);
    static DWORD s_lastCountLog = 0;
    if (now - s_lastCountLog > 15000) {
      s_lastCountLog = now;
      Logf("onslaught creeps: alive T1 %d/%d/%d T2 %d/%d/%d (small/mid/big); spawned %d, failed %d",
           alive[0][0], alive[0][1], alive[0][2], alive[1][0], alive[1][1], alive[1][2], st.spawned, st.failed);
      for (int i = 0; i < 4; ++i) {
        int t = i & 1, big = i < 2;
        uint8_t *ship = (uint8_t *)(big ? g_onsBigShip[t] : g_onsMidShip[t]);
        uint8_t *comp = (ship && alive[t][big ? 2 : 1] > 0) ? *(uint8_t **)(ship + 0x198) : nullptr;
        uint8_t *ctrl = ship ? *(uint8_t **)(ship + OFF_PAWN_CONTROLLER) : nullptr;
        int behaviour = ctrl && IsReadable(ctrl + OFF_NPC_BEHAVIOUR_STATE, 1) ? ctrl[OFF_NPC_BEHAVIOUR_STATE] : -1;
        if (comp && IsReadable(comp + 0x160, 12))
          Logf("onslaught creeps: %s T%d %p at (%.0f, %.0f, %.0f), behaviour %d, state %d, ai target %d, team %d, "
               "controller %p", big ? "command ship" : "assault ship", t + 1, ship,
               *(float *)(comp + 0x160), *(float *)(comp + 0x164), *(float *)(comp + 0x168), behaviour,
               ship[OFF_PAWN_STATE], ship[OFF_PAWN_IS_AI_TARGET], ship[OFF_SHIP_TEAM], ctrl);
      }
      LogTargetLists();
    }
    if (!st.firstWave) {
      st.firstWave = true;
      Logf("onslaught creeps: first wave (max small %d / mid %d / big %d per team)", maxN[0], maxN[1], maxN[2]);
      for (int t = 1; t <= 2; ++t)
        for (int k = 0; k < 3; ++k)
          for (int n = alive[t - 1][k]; n < capFor(t, k); ++n)
            if (!SpawnOnslaughtCreep(gm, gs, t, k, true))
              break;
      return;
    }
    for (int t = 1; t <= 2; ++t)
      for (int k = 0; k < 3; ++k) {
        DWORD &since = st.missingSince[t - 1][k];
        if (alive[t - 1][k] >= capFor(t, k)) {
          since = 0;
          continue;
        }
        if (!since) {
          since = now;
          continue;
        }
        if (now - since >= (DWORD)every[k] * 1000) {
          SpawnOnslaughtCreep(gm, gs, t, k, false);
          since = now; // the next missing one waits its own respawn time
        }
      }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("onslaught creeps: EXCEPTION 0x%08X in the spawner tick", GetExceptionCode());
  }
}

// ---------------------------------------------------------------------------
// Conquest score (on; dn_host_no_conquest_score.txt / DN_HOST_NO_CONQUEST_SCORE=1
// turns it off)
//
// No Conquest match on our hosts ever ended: all 10 TER host logs up to
// 2026-10-08 ran until the reaper killed them (one 25 min), none reached
// ClientStartEndOfMatchTransition.
//
// The rules (Grey Box, "Game Update Preview: Conquest Mode", 2018-04-13):
// "Every 10 seconds the percentage of overall territory each team controls is
// added to their Score ... The first team to 2,500 points wins!" The cooked
// GameMode_TER_BP agrees: m_scoreForWinning 2500, and the Territory mode's own
// m_scoringTerritorySecuredUpdateTime 10.
//
// Verified 2026-10-08 (raw .text scan, leaf functions included):
//   - GameMode_TER_BP is YGameMode_Territory (size 0xA30) <- the capture-points
//     mode (0xA10): m_scoreForWinning +0x9F0 (int), m_territoryScoreMultiplier
//     +0x9F4 (float), m_territoryScoreThreshold +0x9F8 (int),
//     m_scoreUpdateInterval +0xA0C (int, 9), m_scoringTerritorySecuredUpdateTime
//     +0xA10 (float, 10). Territory overrides nothing but its destructor.
//   - GameState_TER_BP is YGameState_TER <- YGameState_CapturePointsGame:
//     the team scores m_currentTicketCountT1/T2 +0x1E08/+0x1E0C (int, the HUD's
//     scores and voice lines read them), m_scoreForWinning +0x1E10 (int),
//     m_currentTerritoryT1/T2 +0x1E80/+0x1E84 (int, the HUD's % bar;
//     getters 0x3968C0/0x3968D0, OnRep 0x38F4F0).
//   - NOTHING writes those five GameState fields but the constructors, and
//     nothing reads the game mode's score target or intervals: the scoring was
//     server-build code, like Onslaught's.
//   - What the exe DOES do server-side is the territory itself: the GameState
//     tick (0x3AAB90, GameState_TER vtable slot 122, runs while gs+0x569 &&
//     !gs+0x56A) rebuilds a per-tile influence map from its influence sources
//     (0x3ACB60: gs+0x1FE8, 0x40-byte entries -> AYTerritoryVolume query
//     0x5AEC50 -> merge 0x38DFE0) into gs+0x1EF8, a TMap<uint32 tile,
//     {float T1, float T2}> (0x18-byte set elements; sparse-array Num +0x08,
//     NumFree +0x34, allocation bits inline +0x10 / heap +0x20). Its per-ship
//     pass (0x394A30) gives a tile to team 2 when T1 <= T2 (and T1+T2 > 0),
//     else team 1, and divides by the volume's grid size (m_gameVolume
//     gs+0x2030, floats +0x40C x +0x410).
// So once a second this counts the map's tiles per team the way 0x394A30
// does, writes each team's percentage of the whole grid to the GameState, and
// every m_scoringTerritorySecuredUpdateTime seconds adds it to the team's
// score; at m_scoreForWinning it sets the winner (+0x4E8) and calls EndMatch
// (vtable +0x668), as the Onslaught rule does.
// GUESS: a team's territory is its tiles in the capture-point map plus its
// ships' claimed tiles; its percentage is its share of both teams' tiles
// (see the tick) (pawn +0x858, see CountShipTiles); the original's own
// team sum is not in this exe.
// GUESS: the multiplier is applied when the designers set one (> 0), else 1;
// the threshold is "a team must hold more than this % to score"; percentages
// round to nearest. m_scoreUpdateInterval (9) is not used: the preview's 10 s
// is the Territory mode's own field.
#define YGMT_TER 6                        // host log "type 6" on ?game=TER
#define OFF_GM_TER_SCORE_FOR_WINNING 0x9F0
#define OFF_GM_TER_SCORE_MULTIPLIER 0x9F4
#define OFF_GM_TER_SCORE_THRESHOLD 0x9F8
#define OFF_GM_TER_SCORE_INTERVAL 0xA10
#define OFF_GS_TER_SCORE_T1 0x1E08
#define OFF_GS_TER_SCORE_T2 0x1E0C
#define OFF_GS_TER_SCORE_FOR_WINNING 0x1E10
#define OFF_GS_TER_TERRITORY_T1 0x1E80
#define OFF_GS_TER_TERRITORY_T2 0x1E84
#define OFF_GS_TER_TILE_MAP 0x1EF8
#define OFF_GS_TER_VOLUME 0x2030
#define OFF_TER_VOLUME_GRID 0x40C
#define TER_TILE_ELEMENT 0x18

static bool ConquestScoreEnabled() {
  return !SwitchOn("DN_HOST_NO_CONQUEST_SCORE", "dn_host_no_conquest_score.txt");
}

// Tiles per team in the GameState's influence map; false if unreadable.
static bool CountTerritoryTiles(uint8_t *gs, int *t1, int *t2, int *mapTiles) {
  *t1 = *t2 = *mapTiles = 0;
  uint8_t *map = gs + OFF_GS_TER_TILE_MAP;
  uint8_t *data = *(uint8_t **)map;
  int num = *(int32_t *)(map + 0x08);
  int numFree = *(int32_t *)(map + 0x34);
  if (num <= 0)
    return true;
  if (!data || num > 4 * 1024 * 1024 || !IsReadable(data, (size_t)num * TER_TILE_ELEMENT))
    return false;
  uint32_t *bits = *(uint32_t **)(map + 0x20) ? *(uint32_t **)(map + 0x20) : (uint32_t *)(map + 0x10);
  if (numFree > 0 && !IsReadable(bits, ((size_t)num + 31) / 32 * 4))
    return false;
  for (int i = 0; i < num; ++i) {
    if (numFree > 0 && !(bits[i >> 5] & (1u << (i & 31))))
      continue;
    float a = *(float *)(data + (size_t)i * TER_TILE_ELEMENT + 4);
    float b = *(float *)(data + (size_t)i * TER_TILE_ELEMENT + 8);
    ++*mapTiles;
    if (a + b > 0.0f) {
      if (a <= b) ++*t2;
      else ++*t1;
    }
  }
  return true;
}

struct ConquestMatch {
  void *gm;
  DWORD lastScore, lastLog, lastPointScan;
  bool ended, pointsReady;
};
static ConquestMatch g_ter = {};

// The capture points (verified 2026-10-08 in the exe and MP_Gorge_Territory;
// not yet verified live). Operator test 10:24: "no capture indicator, and I
// couldn't capture them", and the score rule above logged "0 in the map" all
// match. Each _Territory sublevel places CapturePoint_TER_BP actors -- A and B
// (capturable, m_timeToOwn 20) and the two team bases (m_bIsLocked,
// m_initialOwningTeam) -- and NOTHING in this exe sets them up:
//   - YCapturePoint_TER (UClass* at 0x3E11FA0, size 0x500; ctor 0x52CF40)
//     starts with m_isActive (+0x4B0) false, m_ownedByTeam (+0x450) none and
//     m_stationIndex (+0x3E4) unset.
//   - Its tick (0x55F030, vtable slot 122) runs the capture logic (0x5583F0)
//     only when m_isActive is set (and +0x400, which the TER ctor sets, and the
//     match runs). Activate (exec 0x73CC40 -> 0x533420) sets it.
//   - The GameState's capture-point setup 0x39F750 -- the one that fills the
//     influence sources (gs+0x1FE8, one 0x40-byte entry per m_stationIndex:
//     owner team, location, radius +0x4FC, linkable points) and builds the
//     tile map (0x3ACB60) -- skips points whose index is below 0 ("Tried
//     initializing a capture point with an invalid index") and has no caller
//     in this exe at all. Captures call the GameState back (owner change ->
//     vtable +0x838 = 0x3A2610 -> rebuild) once it has run.
//   - SetCapturePoint (exec 0x7761C0 -> 0x558CD0, arg EYTeam) sets the owner.
// So once a second until it has happened, this gives every capture point an
// index (by name -- GUESS: the original order is not known; only uniqueness
// matters to the code above), the team bases their m_initialOwningTeam, calls
// Activate on each, then runs 0x39F750.
#define OFF_CLASSPTR_CAPTURE_POINT_TER 0x3E11FA0
#define OFF_CP_STATION_INDEX 0x3E4
#define OFF_CP_LOCKED 0x401
#define OFF_CP_INITIAL_OWNER 0x402
#define OFF_CP_OWNER 0x450
#define OFF_CP_ACTIVE 0x4B0
#define RVA_CP_SET_OWNER 0x558CD0
#define RVA_CP_ACTIVATE 0x533420
#define RVA_GS_TER_SETUP_CAPTURE_POINTS 0x39F750
// The GameState's own ship pass (0x394A30, from its tick 0x3AAB90) leaves each
// ship's claimed tile count at pawn +0x858 -- tiles where neither the
// capture-point map already gives them to the ship's team nor a stronger
// influence wins, and not tiles an ally already claimed -- over the ships it
// gathered at gs+0x1E88 (TArray<TWeakObjectPtr<APawn>>, count +0x1E90).
#define OFF_GS_TER_SHIPS 0x1E88
#define OFF_PAWN_TER_TILES 0x858
typedef void(__fastcall *tCpSetOwner)(void *cp, uint8_t team);
typedef void(__fastcall *tCpActivate)(void *cp);
typedef void(__fastcall *tGsSetupCapturePoints)(void *gs);

static bool SetUpCapturePoints(uint8_t *gs) {
  void *cpCls = *(void **)(g_base + OFF_CLASSPTR_CAPTURE_POINT_TER);
  FUObjectArrayMin *arr = GObjects();
  if (!cpCls || !arr || !arr->Objects)
    return false;
  uint8_t *cps[16];
  const char *names[16];
  int n = 0;
  for (int i = 0; i < arr->NumElements && n < 16; ++i) {
    UObjectMin *o = arr->Objects[i].Object;
    if (!o || (o->Flags & RF_CDO_OR_ARCHETYPE) || (arr->Objects[i].Flags & OBJ_PENDING_KILL))
      continue;
    UObjectMin *c = o->Class;
    bool is = false;
    for (int d = 0; c && d < 3; ++d, c = *(UObjectMin **)((uint8_t *)c + OFF_USTRUCT_SUPER))
      if (c == cpCls) { is = true; break; }
    if (!is)
      continue;
    names[n] = NameText(o->Name);
    cps[n++] = (uint8_t *)o;
  }
  if (n == 0)
    return false;
  for (int a = 1; a < n; ++a) // by name: A, B, ..., Team1Base, Team2Base
    for (int b = a; b > 0 && names[b - 1] && names[b] && strcmp(names[b - 1], names[b]) > 0; --b) {
      uint8_t *t = cps[b]; cps[b] = cps[b - 1]; cps[b - 1] = t;
      const char *tn = names[b]; names[b] = names[b - 1]; names[b - 1] = tn;
    }
  for (int i = 0; i < n; ++i) {
    uint8_t *cp = cps[i];
    if (*(int32_t *)(cp + OFF_CP_STATION_INDEX) < 0 || *(int32_t *)(cp + OFF_CP_STATION_INDEX) >= n)
      *(int32_t *)(cp + OFF_CP_STATION_INDEX) = i;
    uint8_t initial = cp[OFF_CP_INITIAL_OWNER];
    if ((initial == 1 || initial == 2) && cp[OFF_CP_OWNER] != initial)
      ((tCpSetOwner)(g_base + RVA_CP_SET_OWNER))(cp, initial);
    if (!cp[OFF_CP_ACTIVE])
      ((tCpActivate)(g_base + RVA_CP_ACTIVATE))(cp);
    Logf("conquest: capture point %s -- index %d, owner %d (initial %d), locked %d, active %d", names[i] ? names[i] : "?",
         *(int32_t *)(cp + OFF_CP_STATION_INDEX), cp[OFF_CP_OWNER], initial, cp[OFF_CP_LOCKED], cp[OFF_CP_ACTIVE]);
  }
  ((tGsSetupCapturePoints)(g_base + RVA_GS_TER_SETUP_CAPTURE_POINTS))(gs);
  Logf("conquest: %d capture points set up; influence sources %d, tile map %d tiles", n,
       *(int32_t *)(gs + 0x1FF0), *(int32_t *)(gs + OFF_GS_TER_TILE_MAP + 8));
  return true;
}

// Tiles each team's ships claimed (the GameState's own per-ship counts).
static void CountShipTiles(uint8_t *gs, int *t1, int *t2, int *ships) {
  *t1 = *t2 = *ships = 0;
  uint8_t *data = *(uint8_t **)(gs + OFF_GS_TER_SHIPS);
  int num = *(int32_t *)(gs + OFF_GS_TER_SHIPS + 8);
  if (!data || num <= 0 || num > 256 || !IsReadable(data, (size_t)num * 8))
    return;
  for (int i = 0; i < num; ++i) {
    uint8_t *pawn = (uint8_t *)((tWeakGet)(g_base + RVA_WEAK_PTR_GET))(data + (size_t)i * 8);
    if (!pawn || !IsReadable(pawn + OFF_PAWN_TER_TILES, 4))
      continue;
    int tiles = *(int32_t *)(pawn + OFF_PAWN_TER_TILES);
    int team = ShipTeam(pawn);
    if (tiles <= 0 || !team)
      continue;
    ++*ships;
    (team == 1 ? *t1 : *t2) += tiles;
  }
}

static void ConquestTick(uint8_t *gm) {
  static int s_enabled = -1;
  if (s_enabled < 0)
    s_enabled = ConquestScoreEnabled() ? 1 : 0;
  if (!s_enabled)
    return;
  __try {
    if (!IsReadable(gm + OFF_GM_GAMESTATE, 8) || !IsReadable(gm + OFF_GM_TER_SCORE_INTERVAL, 4))
      return;
    uint8_t *gs = *(uint8_t **)(gm + OFF_GM_GAMESTATE);
    if (!gs || !IsReadable(gs + OFF_GS_GAME_MODE_TYPE, 1) || gs[OFF_GS_GAME_MODE_TYPE] != YGMT_TER ||
        !IsReadable(gs + OFF_GS_TER_SCORE_T1, OFF_GS_TER_VOLUME + 8 - OFF_GS_TER_SCORE_T1))
      return;
    DWORD now = GetTickCount();
    int target = *(int32_t *)(gm + OFF_GM_TER_SCORE_FOR_WINNING);
    if (target <= 0 || target > 1000000)
      target = 2500; // GameMode_TER_BP
    float every = *(float *)(gm + OFF_GM_TER_SCORE_INTERVAL);
    if (!(every >= 1.0f && every <= 600.0f))
      every = 10.0f; // GameMode_TER_BP
    float mult = *(float *)(gm + OFF_GM_TER_SCORE_MULTIPLIER);
    float useMult = (mult > 0.0f && mult <= 100.0f) ? mult : 1.0f;
    int threshold = *(int32_t *)(gm + OFF_GM_TER_SCORE_THRESHOLD);
    uint8_t *vol = *(uint8_t **)(gs + OFF_GS_TER_VOLUME);
    float gw = 0, gh = 0;
    if (vol && IsReadable(vol + OFF_TER_VOLUME_GRID, 8)) {
      gw = *(float *)(vol + OFF_TER_VOLUME_GRID);
      gh = *(float *)(vol + OFF_TER_VOLUME_GRID + 4);
    }
    if (g_ter.gm != gm) {
      memset(&g_ter, 0, sizeof(g_ter));
      g_ter.gm = gm;
      g_ter.lastScore = now;
      *(int32_t *)(gs + OFF_GS_TER_SCORE_FOR_WINNING) = target;
      Logf("conquest: match %p -- score rule armed: %d to win, every %.1f s, multiplier %.3f (using %.3f), "
           "threshold %d; territory volume %p grid %.0f x %.0f; m_maxMatchTime %.2f min (+0x884; GameMode_TER_BP "
           "sets none, the other modes 20)",
           gm, target, every, mult, useMult, threshold, vol, gw, gh, *(float *)(gm + 0x884));
    }
    if (g_ter.ended)
      return;
    if (!gs[OFF_GS_MATCH_RUNNING] || gs[OFF_GS_MATCH_OVER]) {
      g_ter.lastScore = now; // the score clock runs only while the match does
      return;
    }
    if (!g_ter.pointsReady && now - g_ter.lastPointScan >= 1000) {
      g_ter.lastPointScan = now;
      g_ter.pointsReady = SetUpCapturePoints(gs);
    }
    int t1 = 0, t2 = 0, mapTiles = 0, s1 = 0, s2 = 0, ships = 0;
    bool ok = CountTerritoryTiles(gs, &t1, &t2, &mapTiles);
    CountShipTiles(gs, &s1, &s2, &ships);
    t1 += s1;
    t2 += s2;
    float total = gw * gh;
    int pct1 = 0, pct2 = 0;
    // A team's percentage is its share of the CLAIMED territory, t / (t1 + t2).
    // GUESS, chosen by the operator 2026-10-08 after the first live Conquest
    // matches. As a share of the whole 30 x 30 grid, a team held 5-23%, and a
    // 12-minute match stood at 736 vs 1050 when the players left. That is not
    // the designers' 2500-to-win at +score every 10 s. As a share of the
    // claimed tiles the two add up to 100, and an even match reaches 2500 in
    // about 8-9 minutes.
    (void)total;
    if (ok && t1 + t2 > 0) {
      pct1 = (int)(100.0f * t1 / (t1 + t2) + 0.5f);
      pct2 = 100 - pct1;
    }
    int32_t *terr = (int32_t *)(gs + OFF_GS_TER_TERRITORY_T1);
    bool changed = terr[0] != pct1 || terr[1] != pct2;
    terr[0] = pct1;
    terr[1] = pct2;
    int32_t *score = (int32_t *)(gs + OFF_GS_TER_SCORE_T1);
    if (now - g_ter.lastScore >= (DWORD)(every * 1000.0f)) {
      g_ter.lastScore = now;
      if (pct1 > threshold)
        score[0] += (int)(pct1 * useMult + 0.5f);
      if (pct2 > threshold)
        score[1] += (int)(pct2 * useMult + 0.5f);
      changed = true;
      Logf("conquest: territory T1 %d%% T2 %d%% (%d/%d tiles of %.0f: %d in the point map, ships %d/%d from %d) "
           "-> score T1 %d T2 %d / %d",
           pct1, pct2, t1, t2, total, mapTiles, s1, s2, ships, score[0], score[1], target);
    } else if (now - g_ter.lastLog > 30000) {
      g_ter.lastLog = now;
      Logf("conquest: territory T1 %d%% T2 %d%% (%d/%d tiles of %.0f, %d in the map%s)", pct1, pct2, t1, t2,
           total, mapTiles, ok ? "" : ", map UNREADABLE");
    }
    if (changed)
      ((tActorVoidFn)(*(void ***)gs)[0x580 / 8])(gs); // AActor::ForceNetUpdate
    if (score[0] >= target || score[1] >= target) {
      g_ter.ended = true;
      gs[OFF_GS_WINNING_TEAM] = score[0] > score[1] ? 1 : score[1] > score[0] ? 2 : 3;
      Logf("conquest: team %d reached %d (T1 %d, T2 %d) -- ending the match", gs[OFF_GS_WINNING_TEAM], target,
           score[0], score[1]);
      void **vt = *(void ***)gm;
      ((void(__fastcall *)(void *))vt[VT_GM_END_MATCH / 8])(gm);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("conquest: EXCEPTION 0x%08X in the score tick", GetExceptionCode());
  }
}

// ---------------------------------------------------------------------------
// Bots spawned before the match starts (on; dn_host_no_npc_ai_start.txt turns
// it off)
//
// Operator report 2026-10-08: Conquest and Onslaught bots "just sit still and
// ignore the objective". Verified in the exe the same day:
//   - An NPC controller is born halted (+0xAFA = 1, ctor 0x276EA0) until
//     0x288AE0 starts its AI (see the Onslaught AI ships above).
//   - The scene manager's NPC spawn (0x2693A0) calls 0x288AE0 itself only when
//     the game mode is NOT a YGameMode_Multiplayer (class 0x601440). For the
//     multiplayer modes it leaves the start to the game mode's NPC-spawned
//     handler (vtable +0x978 = 0x378DE0, called only at 0x26A438, in that
//     spawn). That handler starts it only if the match already runs
//     (GameState +0x569), and the pawn's vtable +0x7D8 agrees.
//   - The bots are spawned when the pre-match countdown reaches 50 s, before
//     the match runs. So no one starts them, and they stay halted until they
//     die and respawn mid-match.
// So once a second, while the match runs, every scene-manager NPC (table
// +0x3C0, 0xA8-byte entries, weak pawn +0x8C) whose controller is still halted
// gets exactly what 0x378DE0 would have given it.
#define OFF_SCENE_NPC_PAWN 0x8C
#define VT_PAWN_NPC_AI_OK 0x7D8
typedef bool(__fastcall *tPawnBoolFn)(void *pawn);

// Diagnostic (2026-10-09): two Amirani Onslaught matches logged 47,490 and
// 119,500 "Native NetSerialize ... ReplicatedMovement ... failed" (about 150 a
// second for the last minutes; Gorge 0). FRepMovement fails to pack a location
// or velocity beyond about +-2^19, so an actor left the playable space and its
// movement stopped replicating. The log does not say which actor. This names
// any bot or Onslaught ship that is that far out, or has a NaN position, once
// per actor.
static void ReportFarAway(uint8_t *pawn, const char *what) {
  uint8_t *comp = IsReadable(pawn + 0x198, 8) ? *(uint8_t **)(pawn + 0x198) : nullptr;
  if (!comp || !IsReadable(comp + 0x160, 12))
    return;
  float x = *(float *)(comp + 0x160), y = *(float *)(comp + 0x164), z = *(float *)(comp + 0x168);
  bool bad = x != x || y != y || z != z || fabsf(x) > 500000.f || fabsf(y) > 500000.f || fabsf(z) > 500000.f;
  if (!bad)
    return;
  static void *s_seen[64];
  static int s_n = 0;
  for (int i = 0; i < s_n; ++i)
    if (s_seen[i] == pawn)
      return;
  // Once the list is full, stop: an unlisted actor would otherwise be logged
  // again on every tick.
  if (s_n >= 64)
    return;
  s_seen[s_n++] = pawn;
  Logf("far away: %s %s %p at (%.0f, %.0f, %.0f) -- its movement no longer replicates (RepMovement packing)", what,
       ClassNameOf(pawn), pawn, x, y, z);
}

static void NpcAiStartTick(uint8_t *gm) {
  static int s_enabled = -1;
  if (s_enabled < 0)
    s_enabled = SwitchOn("DN_HOST_NO_NPC_AI_START", "dn_host_no_npc_ai_start.txt") ? 0 : 1;
  if (!s_enabled)
    return;
  __try {
    if (!IsReadable(gm + OFF_GM_GAMESTATE, 8))
      return;
    uint8_t *gs = *(uint8_t **)(gm + OFF_GM_GAMESTATE);
    if (!gs || !IsReadable(gs + OFF_GS_MATCH_RUNNING, 2) || !gs[OFF_GS_MATCH_RUNNING] || gs[OFF_GS_MATCH_OVER])
      return;
    uint8_t *sm = (uint8_t *)((tGetSceneManager)(g_base + RVA_COMBAT_SCENE_MANAGER))();
    if (!sm || !IsReadable(sm + OFF_SCENE_NPCS, 16))
      return;
    uint8_t *data = *(uint8_t **)(sm + OFF_SCENE_NPCS);
    int n = *(int32_t *)(sm + OFF_SCENE_NPCS_COUNT);
    if (!data || n <= 0 || n > 4096 || !IsReadable(data, (size_t)n * NPC_SPAWN_SIZE))
      return;
    void *npcCls = *(void **)(g_base + OFF_CLASSPTR_NPC_AI_CONTROLLER);
    static int s_started = 0;
    for (int i = 0; i < n; ++i) {
      uint8_t *pawn = (uint8_t *)((tWeakGet)(g_base + RVA_WEAK_PTR_GET))(data + (size_t)i * NPC_SPAWN_SIZE + OFF_SCENE_NPC_PAWN);
      if (!pawn || !IsReadable(pawn + OFF_PAWN_CONTROLLER, 8))
        continue;
      ReportFarAway(pawn, "scene NPC");
      uint8_t *ctrl = *(uint8_t **)(pawn + OFF_PAWN_CONTROLLER);
      if (!ctrl || !IsReadable(ctrl + OFF_NPC_AI_HALTED, 1) || !ctrl[OFF_NPC_AI_HALTED] ||
          !ClassIsAncestor(((UObjectMin *)ctrl)->Class, npcCls))
        continue;
      if (!((tPawnBoolFn)(*(void ***)pawn)[VT_PAWN_NPC_AI_OK / 8])(pawn))
        continue;
      ((tNpcStartAI)(g_base + RVA_NPC_START_AI))(ctrl);
      if (++s_started <= 40)
        Logf("bots: NPC %d %s (%s) was spawned before the match started -- AI started", i, ClassNameOf(pawn),
             ClassNameOf(ctrl));
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("bots: EXCEPTION 0x%08X in the NPC AI start tick", GetExceptionCode());
  }
}

// ---------------------------------------------------------------------------
// Veteran / Legendary command ship guns (on; dn_host_no_ons_gun_rows.txt turns
// it off)
//
// Operator report 2026-10-08: the command ship shoots in Recruit Onslaught
// but not in Veteran or Legendary. Verified in the cooked files the same day:
//   - The tiers' command ships (GameInfo_Onslaught_BP m_bigCreepClass / Vet /
//     Leg = VH_ONS_AITargetH_Pawn_BP / _Pawn_V_BP / _Pawn_L_BP) carry
//     WP_ONS_AITargetH_weapon01 / 02 / 03.
//   - The three weapon Blueprints are identical except their projectile row,
//     proj01 / 02 / 03. Every field of the three projectile rows matches
//     except the damage: 400/200/100, 700/300/150, 1000/500/200.
//   - DN_Weapons_OTS_DT has a row for weapon01 only. The host gets no tuning
//     (it never logs in), so its weapon groups fall back to the cooked table,
//     and every Veteran / Legendary match logged "Weapon Data for
//     'WP_ONS_AITargetH_weapon02_BP' Couldn't be found": that gun has no
//     weapon row and never fires.
// So when UYWeaponGroup::LoadWeaponDataTableRow (0x518EE0; the row name is the
// FString at +0x2B8, the row is cached at +0x2B0) finds nothing for weapon02 or
// weapon03, it is asked once more under weapon01's name. The weapon row is the
// gun's mechanics. The tier's damage stays in its own projectile row.
// GUESS: weapon02/03's own rows were copies of weapon01's. The three Blueprints
// agree field for field, but the rows themselves are lost.
#define RVA_WEAPON_GROUP_LOAD_ROW 0x518EE0
#define OFF_WEAPON_GROUP_ROW_NAME 0x2B8
#define OFF_WEAPON_GROUP_ROW 0x2B0
typedef void *(__fastcall *tWeaponGroupLoadRow)(void *group);
static tWeaponGroupLoadRow g_origWeaponGroupLoadRow = nullptr;

static void *__fastcall HookWeaponGroupLoadRow(void *group) {
  void *row = g_origWeaponGroupLoadRow(group);
  if (row)
    return row;
  uint8_t *g = (uint8_t *)group;
  // The group's own row name, kept so it is put back on EVERY path: left
  // pointing at the static buffer below, the engine would one day free it.
  wchar_t *name = nullptr;
  int32_t num = 0, max = 0;
  bool swapped = false;
  __try {
    name = *(wchar_t **)(g + OFF_WEAPON_GROUP_ROW_NAME);
    if (!name || (lstrcmpW(name, L"WP_ONS_AITargetH_weapon02_BP") != 0 &&
                  lstrcmpW(name, L"WP_ONS_AITargetH_weapon03_BP") != 0))
      return row;
    static wchar_t s_weapon01[] = L"WP_ONS_AITargetH_weapon01_BP";
    num = *(int32_t *)(g + OFF_WEAPON_GROUP_ROW_NAME + 8);
    max = *(int32_t *)(g + OFF_WEAPON_GROUP_ROW_NAME + 12);
    swapped = true;
    *(wchar_t **)(g + OFF_WEAPON_GROUP_ROW_NAME) = s_weapon01;
    *(int32_t *)(g + OFF_WEAPON_GROUP_ROW_NAME + 8) = (int32_t)(sizeof(s_weapon01) / sizeof(wchar_t));
    *(int32_t *)(g + OFF_WEAPON_GROUP_ROW_NAME + 12) = (int32_t)(sizeof(s_weapon01) / sizeof(wchar_t));
    *(void **)(g + OFF_WEAPON_GROUP_ROW) = nullptr;
    row = g_origWeaponGroupLoadRow(group);
    static volatile LONG s_logged = 0;
    if (InterlockedIncrement(&s_logged) <= 4)
      Logf("command ship gun: no weapon row for %S; using WP_ONS_AITargetH_weapon01_BP's -> %p", name, row);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("command ship gun: EXCEPTION 0x%08X", GetExceptionCode());
  }
  if (swapped) {
    *(wchar_t **)(g + OFF_WEAPON_GROUP_ROW_NAME) = name;
    *(int32_t *)(g + OFF_WEAPON_GROUP_ROW_NAME + 8) = num;
    *(int32_t *)(g + OFF_WEAPON_GROUP_ROW_NAME + 12) = max;
  }
  return row;
}

// ---------------------------------------------------------------------------
// Player ship physics (dn_host_ship_physics.txt)
//
// Verified 2026-09-24 (disassembly + live memory while the operator flew):
//   - Ship inputs reach the host (ServerUpdateThrottle/Steering/VerticalState
//     -> m_replicatedState +0x210, copied to +0x228.. for non-local ships at
//     0x5C92E5): the host read throttle 1.0 / steering -1.0 as pressed.
//   - The force builder 0x5C8C00 skips ALL forces for a ship that is not
//     locally controlled (+0x488 = owner->IsLocallyControlled(), set at
//     0x5C4910 -- false for every human player on any server) when +0x489 is
//     set, unless the ship is near and in front of the LOCAL player's camera
//     (+0x498 distance squared, +0x49C view dot, cvar via 0x5B9B40). A client
//     optimisation: don't simulate remote ships nobody is looking at.
//   - +0x489 is written only here, and only when the world has a first local
//     player controller. A real dedicated server has none, so every ship is
//     simulated. Our host is the game exe and has one (player 256, parked at the
//     orbit camera), so player ships ~73,000 units away were never simulated:
//     the host copy only drifted, and every server correction (e.g. on firing)
//     snapped the client back -- "position resets and I can't move".
//   - Clearing +0x489 live (scratchpad fixnow.py) made the host copy follow the
//     inputs at once: accelerate, turn, climb; the operator confirmed no resets.
//
// So after the original runs, the flag is cleared again: the host behaves like
// the dedicated server it stands in for. Locally controlled ships (AI) return
// early in the original and are untouched.
typedef void(__fastcall *tVehicleViewCull)(void *movementComp);
static tVehicleViewCull g_origVehicleViewCull = nullptr;

static void __fastcall HookVehicleViewCull(void *movementComp) {
  g_origVehicleViewCull(movementComp);
  uint8_t *mc = (uint8_t *)movementComp;
  if (mc[OFF_VMC_LOCALLY_CONTROLLED] == 0 && mc[OFF_VMC_VIEW_CULLED] != 0) {
    mc[OFF_VMC_VIEW_CULLED] = 0;
    static volatile LONG s_logged = 0;
    if (InterlockedIncrement(&s_logged) <= 8)
      Logf("ship physics: movement component %p is a remote player's ship; "
           "cleared the local-camera physics cull so the host simulates it",
           movementComp);
  }
}

// ---------------------------------------------------------------------------
// Game-mode modifiers: Turbo TDM (on by default, TurboTDM hosts only)
//
// Turbo TDM's whole effect is three feats on every ship -- DN_GameModifiers_DT
// row YGMT_TURBO_TDM: m_feats "ModifierSpeed;ModifierDamage;ModifierFireRate",
// m_affectedTeam YHMAT_ALL (the only row with feats; YGMT_TDM's is empty). The
// feats are defined in DN_Feats_Custom_Modifiers_OTS_DT (ModifierSpeed +600%
// forward/reverse, +300% vertical/steer; ModifierDamage +50%; ModifierFireRate
// -50% weapon cooldowns).
//
// 0x34BB30 (.pdata 0x34BB30-0x34BCFC): the feats component's setup -- reads
// its owner (+0xA8), binds "&UYFeatsComponent::OnPawnPossessed". After it, the
// row's feats are added with the game's own AddFeat body 0x337850(comp, FName)
// (.pdata 0x337850-0x337953; exec thunk 0x73EDF0 is registered as "AddFeat"
// and reads one FName). On the authority it adds locally; the AddFeatServer
// RPC (0x5DFC40) is only taken when the caller lacks authority.
// VERIFIED live 2026-10-16: feats added on every ship (player + bots), and the
// client moved faster and smoothly (operator: "faster and smooth no stutter").
#define RVA_FEATS_COMPONENT_BEGIN 0x34BB30
#define RVA_FEATS_ADD_FEAT 0x337850
typedef void(__fastcall *tFeatsBegin)(void *featsComp);
typedef bool(__fastcall *tAddFeat)(void *featsComp, uint64_t featName);
static tFeatsBegin g_origFeatsBegin = nullptr;

// Havoc boost feats must stack (a second purchase, an elite's N stacks), but
// on the host they never did: AddFeat 0x337850 re-adds an existing feat only if
// feat+0x51 != 0 (else returns false). The feat is built by 0x343B40, which
// copies def+0x38 (m_stackOnAdding) -> feat+0x51 and def+0x39 (m_isPerkFeat)
// -> +0x50. AddFeatInternal 0x337960 zeroes its local def (word at +0x38,
// 0x337AC9) and fills it from 0x3C9A40, which reads only the three string
// fields (m_enabling/m_triggers/m_effects) from the tuning JSON -- the bools
// stay false. The cooked DN_Feats_Havoc_Boosts_Shared_OTS_DT has
// m_stackOnAdding = true on all 38 HVC_Boost_* rows (BoolProperty value read
// from the .uasset, 2026-10-16), as does data/'s OTS copy; restore that.
// Not verified: whether 0x3C9FF0 (the cooked-table fallback) keeps the bools.
#define RVA_FEATS_FIND_FEAT 0x345FF0
static bool AddFeatStackable(void *featsComp, const FNameMin &name, const wchar_t *feat) {
  bool ok = ((tAddFeat)(g_base + RVA_FEATS_ADD_FEAT))(featsComp, *(const uint64_t *)&name);
  if (wcsncmp(feat, L"HVC_Boost_", 10))
    return ok;
  uint8_t *f = (uint8_t *)((void *(__fastcall *)(void *, uint64_t))(g_base + RVA_FEATS_FIND_FEAT))(
      featsComp, *(const uint64_t *)&name);
  if (f && IsReadable(f + 0x51, 1) && !f[0x51]) {
    f[0x51] = 1;
    // The first add of a fresh feat already counts; a re-add that hit the
    // false flag returned false and added nothing -- add it now.
    if (!ok)
      ok = ((tAddFeat)(g_base + RVA_FEATS_ADD_FEAT))(featsComp, *(const uint64_t *)&name);
  }
  return ok;
}
static void RecordFeatsComp(void *pawn, void *comp);

static const wchar_t *const kTurboTdmFeats[] = {L"ModifierSpeed", L"ModifierDamage",
                                                L"ModifierFireRate"};

static volatile bool g_turboOn = false; // /turbo (CommandsTick)
static bool g_turboTdmMods = false;    // Turbo TDM host with modifiers on

static bool HostIsTurboTDM() {
  const wchar_t *cmd = GetCommandLineW();
  return cmd && wcsstr(cmd, L"-GameMode=TurboTDM ") != nullptr;
}

static void __fastcall HookFeatsBegin(void *featsComp) {
  g_origFeatsBegin(featsComp);
  uint8_t *fc = (uint8_t *)featsComp;
  void *owner = IsReadable(fc + OFF_COMPONENT_OWNER, 8) ? *(void **)(fc + OFF_COMPONENT_OWNER) : nullptr;
  if (!owner || !ClassChainIs((UObjectMin *)owner, "Pawn"))
    return;
  RecordFeatsComp(owner, featsComp); // Havoc boosts (HavocApplyBoosts)
  if (!g_turboTdmMods && !g_turboOn)
    return;
  static volatile LONG s_logged = 0;
  bool logIt = InterlockedIncrement(&s_logged) <= 16;
  for (const wchar_t *feat : kTurboTdmFeats) {
    FNameMin name{};
    ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&name, feat, 1 /* FNAME_Add */);
    bool ok = false;
    __try {
      ok = ((tAddFeat)(g_base + RVA_FEATS_ADD_FEAT))(featsComp, *(uint64_t *)&name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      Logf("turbo modifiers: AddFeat(%ls) faulted on %p", feat, featsComp);
      return;
    }
    if (logIt || !ok)
      Logf("turbo modifiers: AddFeat(%ls) on %s -> %s", feat,
           ClassNameOf(owner), ok ? "added" : "FAILED");
  }
}

// ---------------------------------------------------------------------------
// Stack overflow at match end (always on)
//
// Verified 2026-09-24 by scraping the overflowed game-thread stack of a host
// that died as a proving-ground match was won (Wine: EXCEPTION_STACK_OVERFLOW
// c00000fd): one 12-frame loop repeated ~3,884 times --
//   exec ClientSetPlayerRestrictions (0x746B40) -> _Implementation (0x5743B0)
//   -> SetPlayerRestrictions (0x5954F0) -> ClientSetPlayerRestrictions RPC
//   (0x5E5B80) -> ProcessEvent ... -> exec 0x746B40 -> ...
// 0x5954F0 applies the restrictions locally only if the world's net mode is
// Standalone/Client (0x1CDB7C0 -> UNetDriver::GetNetMode) or world+0x840 == 3;
// on any server it sends the client RPC instead. For a remote player that RPC
// goes over the wire and the loop never forms. Our host also has a LOCAL
// player (256, the game exe cannot run without one), and a client RPC on a
// local controller executes in-process -- straight back into 0x5954F0, which
// sends it again. The shipping servers had no local player, so this never
// fired there. The same applies whether the host reports LISTEN or DEDICATED.
//
// The guard lets the first call through and drops a nested one on the same
// thread: nesting here can only be that local loop. Remote players are
// unaffected (their RPC is sent, not executed here).
typedef void(__fastcall *tClientSetPlayerRestrictions)(
    void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t);
static tClientSetPlayerRestrictions g_origClientSetPlayerRestrictions = nullptr;

static void __fastcall HookClientSetPlayerRestrictions(
    void *pc, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
    uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11,
    uint64_t a12, uint64_t a13, uint64_t a14, uint64_t a15, uint64_t a16,
    uint64_t a17, uint64_t a18) {
  static thread_local int s_depth = 0;
  if (s_depth > 0) {
    static volatile LONG s_logged = 0;
    if (InterlockedIncrement(&s_logged) <= 3)
      Logf("restrictions: dropped a re-entrant ClientSetPlayerRestrictions on "
           "controller %p (the local player's RPC loop that overflowed the "
           "stack at match end)",
           pc);
    return;
  }
  ++s_depth;
  g_origClientSetPlayerRestrictions(pc, a2, a3, a4, a5, a6, a7, a8, a9, a10,
                                    a11, a12, a13, a14, a15, a16, a17, a18);
  --s_depth;
}

// The guard above is not enough on its own: on 2026-09-28 three hosts
// (14:07, 14:19, 20:25) still overflowed at match end, each right after the
// guard dropped ONE nested call -- clean match ends show it firing twice. So
// the loop also runs through a path the _Implementation guard does not see.
// This cuts it at its source instead: the ClientSetPlayerRestrictions RPC stub
// (0x5E5B80, 18 args like the body -- this, 3 register bools, 14 stack slots)
// does nothing for a controller WITHOUT a network connection (PC+0x5A8 null),
// i.e. the host's own local player, whose "client RPC" would execute
// in-process and re-enter SetPlayerRestrictions. Remote players' RPCs go out
// unchanged. The local player is the host's spectator; its restrictions are
// never shown to anyone.
// ---------------------------------------------------------------------------
// ClientApplyRespawnFilter re-entrancy guard (not switchable)
//
// THE end-of-match stack overflow (~40% of matches, ~3 ms after EndMatch).
// Found 2026-09-29 by the stack probe below, which caught the cycle live:
//
//   0x28D6FB -> 0x5E5107 -> 0x41723D -> 0x744B7C -> 0xD1878B -> 0xD5B430 ->
//   0xD5B2A4 -> 0x28D6FB -> ...        (862 repetitions in 512 KB of stack)
//
// 0x5E50C0 is the RPC stub of ClientApplyRespawnFilter (its FName global
// 0x3E10168 is initialised at 0xCD48D). CORRECTION 2026-10-15: that global is
// initialised from L"ClientAddAchievement" (0xCD486); ClientApplyRespawnFilter
// is the next one (0xCD4A6). The stub is ClientAddAchievement on the scoring
// component; the loop and the guard are unchanged (see the live ribbons). It calls ProcessEvent (0xD5B180), which
// runs the exec thunk (0x744AF0) and the body 0x4171E0: that adds the filter's
// value to this+0x1F0 and then, when the object's net check (0x141839880)
// returns 1 -- which it does on this host, which reports itself DEDICATED --
// forwards the same filter to the client by calling the stub again. For a
// REMOTE player that is a network RPC and ends there; for the host's own
// LOCAL player it executes in-process and calls the stub again, forever. The
// same shape as the ClientSetPlayerRestrictions loop above.
//
// The guard drops only a NESTED call on the same thread: the first call
// always goes through, so remote players get their RPC unchanged, and the
// local player's in-process copy runs once.
#define RVA_CLIENT_APPLY_RESPAWN_FILTER_RPC 0x5E50C0
typedef void(__fastcall *tClientApplyRespawnFilter)(void *self, void *filter);
static tClientApplyRespawnFilter g_origClientApplyRespawnFilter = nullptr;
static __declspec(thread) int t_respawnFilterDepth = 0;

static void __fastcall HookClientApplyRespawnFilter(void *self, void *filter) {
  if (t_respawnFilterDepth > 0) {
    static volatile LONG s_logged = 0;
    if (InterlockedIncrement(&s_logged) <= 3)
      Logf("respawn filter: dropped a nested ClientApplyRespawnFilter on %p (the "
           "local player's in-process RPC loop that overflowed the stack at match end)",
           self);
    return;
  }
  ++t_respawnFilterDepth;
  g_origClientApplyRespawnFilter(self, filter);
  --t_respawnFilterDepth;
}

// ---------------------------------------------------------------------------
// End-of-match stack probe (DIAGNOSTIC; dn_host_no_stack_probe.txt /
// DN_HOST_NO_STACK_PROBE=1 turns it off)
//
// The host overflows its stack in about 40% of matches, ~3 ms after EndMatch
// starts ("Match over event" -> music reset -> EXCEPTION_STACK_OVERFLOW; 5 of
// 12 matches on 2026-09-29). Neither Wine nor the game logs a backtrace, and
// the restrictions guard above did not stop it, so the recursing function is
// unknown. This finds it: the local-controller restrictions RPC fires on the
// game thread right before every overflow, so it arms a watcher for 5 s. The
// watcher repeatedly suspends the game thread and reads its RSP; once the stack
// is PROBE_DEPTH deeper than at arm time, it scans the stack above RSP for
// values inside the exe's code section, counts them, and writes the most
// frequent ones -- the return addresses of the recursion -- as RVAs.
//
// While the game thread is suspended nothing here touches the C runtime or the
// heap (the thread may hold their locks): wsprintfA into static buffers and a
// raw WriteFile to dn_host_stackprobe.log beside the exe.
#define PROBE_DEPTH (768 * 1024)
#define PROBE_SCAN (512 * 1024)
#define PROBE_SLOTS 4096
static HANDLE g_probeTarget = nullptr;
static volatile LONG g_probeArmed = 0;
static volatile DWORD g_probeUntil = 0;
static uintptr_t g_probeBaseline = 0;
static uintptr_t g_textLo = 0, g_textHi = 0;
static uint64_t g_probeKeys[PROBE_SLOTS];
static uint32_t g_probeCounts[PROBE_SLOTS];
static char g_probeLine[512];
static HANDLE g_probeFile = INVALID_HANDLE_VALUE;
static bool SwitchOn(const char *envName, const char *markerFile);

static void ProbeWrite(const char *text) {
  if (g_probeFile == INVALID_HANDLE_VALUE)
    return;
  DWORD n = 0;
  WriteFile(g_probeFile, text, (DWORD)lstrlenA(text), &n, nullptr);
}

static void ProbeScanAndReport(uintptr_t rsp, uintptr_t rip) {
  memset(g_probeKeys, 0, sizeof(g_probeKeys));
  memset(g_probeCounts, 0, sizeof(g_probeCounts));
  uint32_t candidates = 0;
  // Every page is checked before it is read, the FIRST one included: the probe
  // can fire just as the thread reaches the bottom of its stack, where RSP sits
  // on the guard page -- reading it faulted this thread and took the host down
  // (21:45:42 on 2026-09-29, "Unhandled page fault on read access to
  // 00007FFFFE100000", thread = the probe). Guard / no-access pages end the scan.
  uintptr_t readableTo = 0;
  for (uintptr_t p = rsp; p < rsp + PROBE_SCAN; p += 8) {
    if (p >= readableTo) {
      MEMORY_BASIC_INFORMATION mbi;
      if (!VirtualQuery((void *)p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
          (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
        break;
      readableTo = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    uint64_t v;
    __try {
      v = *(uint64_t *)p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      break;
    }
    if (v < g_textLo || v >= g_textHi)
      continue;
    candidates++;
    uint32_t h = (uint32_t)((v * 0x9E3779B97F4A7C15ull) >> 52) & (PROBE_SLOTS - 1);
    for (int i = 0; i < PROBE_SLOTS; ++i, h = (h + 1) & (PROBE_SLOTS - 1)) {
      if (g_probeKeys[h] == v || g_probeKeys[h] == 0) {
        g_probeKeys[h] = v;
        g_probeCounts[h]++;
        break;
      }
    }
  }
  wsprintfA(g_probeLine,
            "stack probe: game thread is %u KB deeper than at match end; rip RVA 0x%X; "
            "%u code addresses in the top %u KB of stack. Most frequent (RVA x count):\r\n",
            (unsigned)((g_probeBaseline - rsp) / 1024),
            (unsigned)(rip >= g_base ? rip - g_base : 0), candidates, PROBE_SCAN / 1024);
  ProbeWrite(g_probeLine);
  for (int rank = 0; rank < 24; ++rank) {
    int best = -1;
    for (int i = 0; i < PROBE_SLOTS; ++i)
      if (g_probeCounts[i] && (best < 0 || g_probeCounts[i] > g_probeCounts[best]))
        best = i;
    if (best < 0)
      break;
    wsprintfA(g_probeLine, "  0x%X x %u\r\n", (unsigned)(g_probeKeys[best] - g_base),
              g_probeCounts[best]);
    ProbeWrite(g_probeLine);
    g_probeCounts[best] = 0;
  }
}

static DWORD WINAPI StackProbeThread(LPVOID) {
  for (;;) {
    if (!g_probeArmed) {
      Sleep(20);
      continue;
    }
    if ((LONG)(GetTickCount() - g_probeUntil) >= 0) {
      g_probeArmed = 0;
      continue;
    }
    if (SuspendThread(g_probeTarget) == (DWORD)-1) {
      g_probeArmed = 0;
      continue;
    }
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(g_probeTarget, &ctx) && g_probeBaseline > ctx.Rsp &&
        g_probeBaseline - ctx.Rsp > PROBE_DEPTH) {
      ProbeScanAndReport((uintptr_t)ctx.Rsp, (uintptr_t)ctx.Rip);
      g_probeArmed = 0;
    }
    ResumeThread(g_probeTarget);
    SwitchToThread();
  }
}

// The exe's code range and the raw output file, shared by the stack probe and
// the crash handler below. Safe to call more than once.
static void InitProbeOutput() {
  if (g_textLo)
    return;
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
  IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
  g_textLo = g_base + nt->OptionalHeader.BaseOfCode;
  g_textHi = g_textLo + nt->OptionalHeader.SizeOfCode;
  char path[MAX_PATH];
  DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
  while (len > 0 && path[len - 1] != '\\' && path[len - 1] != '/')
    --len;
  lstrcpyA(path + len, "dn_host_stackprobe.log");
  g_probeFile = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// ---------------------------------------------------------------------------
// Host crash handler (DIAGNOSTIC; same switch as the stack probe)
//
// The second host crash: "Unhandled Exception: EXCEPTION_ACCESS_VIOLATION
// reading address 0xfffffff8" MID-match (3-5 min in), then the engine's crash
// handler exits with status 3 -- 7 matches on 2026-09-28/29, 5 of them
// identical. The engine's report lists frames without addresses and Wine logs
// none, so the faulting code is unknown. A vectored handler sees the exception
// first: for an access violation or stack overflow whose RIP is inside the
// exe's code, it writes the fault RVA, the address read, and the exe code
// addresses found on the stack above RSP in order (a rough call chain), to
// dn_host_stackprobe.log. Faults in wer.dll itself (the IsReadable probes)
// are not in the exe and are skipped. It only observes: always
// EXCEPTION_CONTINUE_SEARCH. At most 4 reports per process.
static volatile LONG g_crashReports = 0;
static char g_crashLine[256];

static LONG CALLBACK HostCrashHandler(EXCEPTION_POINTERS *ep) {
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW)
    return EXCEPTION_CONTINUE_SEARCH;
  uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
  if (rip < g_textLo || rip >= g_textHi)
    return EXCEPTION_CONTINUE_SEARCH;
  if (InterlockedIncrement(&g_crashReports) > 4)
    return EXCEPTION_CONTINUE_SEARCH;
  uintptr_t addr = ep->ExceptionRecord->NumberParameters >= 2
                       ? (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1]
                       : 0;
  uintptr_t rsp = (uintptr_t)ep->ContextRecord->Rsp;
  wsprintfA(g_crashLine, "crash: %s at RVA 0x%X, address 0x%I64X, thread %u. Stack chain (RVA):\r\n",
            code == EXCEPTION_STACK_OVERFLOW ? "STACK OVERFLOW" : "ACCESS VIOLATION",
            (unsigned)(rip - g_base), (unsigned long long)addr, (unsigned)GetCurrentThreadId());
  ProbeWrite(g_crashLine);
  // Registers often hold the object that was null; worth a line.
  CONTEXT *c = ep->ContextRecord;
  wsprintfA(g_crashLine, "  rax=%I64X rbx=%I64X rcx=%I64X rdx=%I64X rsi=%I64X rdi=%I64X\r\n",
            c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi);
  ProbeWrite(g_crashLine);
  // Separate line: both in one would be up to 298 chars in this 256-byte
  // buffer (wsprintfA does not bound it). r8-r15 from PR #80.
  wsprintfA(g_crashLine, "  r8=%I64X r9=%I64X r10=%I64X r11=%I64X r12=%I64X r13=%I64X r14=%I64X r15=%I64X\r\n",
            c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15);
  ProbeWrite(g_crashLine);
  int shown = 0;
  uintptr_t limit = rsp + (code == EXCEPTION_STACK_OVERFLOW ? 4096 : 65536);
  uintptr_t readableTo = 0;
  for (uintptr_t p = rsp; p < limit && shown < 40; p += 8) {
    if (p >= readableTo) {
      MEMORY_BASIC_INFORMATION mbi;
      if (!VirtualQuery((void *)p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
          (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        break;
      readableTo = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    uint64_t v = *(uint64_t *)p;
    if (v >= g_textLo && v < g_textHi) {
      wsprintfA(g_crashLine, "  0x%X\r\n", (unsigned)(v - g_base));
      ProbeWrite(g_crashLine);
      shown++;
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

static void InstallHostCrashHandler() {
  if (SwitchOn("DN_HOST_NO_STACK_PROBE", "dn_host_no_stack_probe.txt"))
    return;
  InitProbeOutput();
  if (AddVectoredExceptionHandler(1, HostCrashHandler))
    Logf("crash handler: installed; access violations / stack overflows in the exe "
         "are written to dn_host_stackprobe.log");
}

// Called on the game thread at match end (the restrictions RPC hook).
static void ArmStackProbe() {
  static int s_state = -1; // -1 unknown, 0 off, 1 on
  if (s_state < 0) {
    s_state = SwitchOn("DN_HOST_NO_STACK_PROBE", "dn_host_no_stack_probe.txt") ? 0 : 1;
    if (s_state) {
      InitProbeOutput();
      if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                           &g_probeTarget, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0) ||
          !CreateThread(nullptr, 0, StackProbeThread, nullptr, 0, nullptr)) {
        s_state = 0;
        Logf("stack probe: could not start (thread handle or watcher thread)");
      } else {
        Logf("stack probe: armed on the game thread; a deep stack in the next 5 s is "
             "written to dn_host_stackprobe.log");
      }
    }
  }
  if (!s_state)
    return;
  volatile char here = 0;
  g_probeBaseline = (uintptr_t)&here;
  g_probeUntil = GetTickCount() + 5000;
  g_probeArmed = 1;
}

#define RVA_CLIENT_SET_PLAYER_RESTRICTIONS_RPC 0x5E5B80
static tClientSetPlayerRestrictions g_origClientSetPlayerRestrictionsRPC = nullptr;

static void __fastcall HookClientSetPlayerRestrictionsRPC(
    void *pc, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
    uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11,
    uint64_t a12, uint64_t a13, uint64_t a14, uint64_t a15, uint64_t a16,
    uint64_t a17, uint64_t a18) {
  bool local = false;
  __try {
    local = pc && *(void **)((uint8_t *)pc + OFF_PC_NETCONNECTION) == nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    local = false;
  }
  if (local) {
    ArmStackProbe(); // the overflow follows within milliseconds when it happens
    static volatile LONG s_logged = 0;
    if (InterlockedIncrement(&s_logged) <= 3)
      Logf("restrictions: skipped the ClientSetPlayerRestrictions RPC to the host's "
           "local controller %p (it would execute in-process and loop)", pc);
    return;
  }
  g_origClientSetPlayerRestrictionsRPC(pc, a2, a3, a4, a5, a6, a7, a8, a9, a10,
                                       a11, a12, a13, a14, a15, a16, a17, a18);
}

// ---------------------------------------------------------------------------
// End-of-match screen (on unless dn_host_no_eom_stats.txt / DN_HOST_NO_EOM_STATS=1)
//
// Verified 2026-09-25 from a client log + the exe: the client's end-of-match
// stage graph runs Init -> FadeToBlack -> UnloadSublevels -> LoadLevel ->
// LoadContent -> SetupUIWidgets and stops there, on a black screen. That stage
// (0x34B7E0) reads PC+0x3E0 (the AYPlayerReplicationInfo) and waits for
// PRI+0x7E0: if clear it binds to the PRI+0x5E0 delegate and waits; once set it
// moves on to stage 7. The only writer of +0x7E0 is 0x5B0240, the
// ClientSetTopPlayerMatchStats(TArray<FYPlayerMatchStat>) body (exec thunk
// 0x747180, vtable slot 0x6A8), which copies the array to PRI+0x7D0, sets the
// flag and broadcasts.
//
// Nothing in this exe SENDS that RPC: the FName global it would be called by
// (0x3E102F8) is referenced only by its own startup initializer (full .text
// scan). The server code that ranked players at match end lived in the
// separate server build, like the loadout lookup. So the mod sends it, at the
// moment the host starts a player's end-of-match flow: right after the
// ClientStartEndOfMatchTransition RPC stub (0x5E5D70, UYPlayerOrbitComponent,
// the only sender; FindFunctionChecked 0xD57C90 + ProcessEvent vtable 0x1A8,
// read from that stub), on the same controller's PRI.
//
// The array is sent EMPTY: the handler only copy-assigns it (0x410B60), and
// ranking stats is data the host does not have. The MVP page shows no
// entries rather than invented ones.
#define RVA_CLIENT_START_EOM_TRANSITION 0x5E5D70
#define RVA_FIND_FUNCTION_CHECKED 0xD57C90
#define RVA_FNAME_CLIENT_SET_TOP_PLAYER_MATCH_STATS 0x3E102F8
#define OFF_PC_ORBIT_COMPONENT 0xBE8 // AYPlayerController::m_orbitComponent
#define OFF_PC_PLAYER_STATE 0x3E0
#define VT_PROCESS_EVENT 0x1A8

typedef void(__fastcall *tClientStartEomTransition)(void *orbitComp);
static tClientStartEomTransition g_origClientStartEomTransition = nullptr;
typedef void *(__fastcall *tFindFunctionChecked)(void *obj, uint64_t name);
typedef void(__fastcall *tProcessEventVirt)(void *obj, void *fn, void *parms);
// UObject::FindFunctionChecked is a fatal error when the class lacks the
// function (boss11: "Failed to find function AbilitiesInitialized" took the
// battle down), so walk the class chain first and return null instead.
static void *FindFunctionSafe(void *obj, uint64_t name) {
  FNameMin n;
  memcpy(&n, &name, sizeof n);
  for (UObjectMin *cls = IsReadable(obj, sizeof(UObjectMin)) ? ((UObjectMin *)obj)->Class : nullptr;
       cls && IsReadable(cls, 0x40); cls = *(UObjectMin **)((uint8_t *)cls + OFF_USTRUCT_SUPER))
    for (UObjectMin *f = *(UObjectMin **)((uint8_t *)cls + 0x38); f && IsReadable(f, sizeof(UObjectMin));
         f = *(UObjectMin **)((uint8_t *)f + 0x28))
      if (f->Name.ComparisonIndex == n.ComparisonIndex && f->Name.Number == n.Number)
        return ((tFindFunctionChecked)(g_base + RVA_FIND_FUNCTION_CHECKED))(obj, name);
  return nullptr;
}

// ---------------------------------------------------------------------------
// Match result -> mmogbrain (on unless dn_host_no_match_result.txt /
// DN_HOST_NO_MATCH_RESULT=1)
//
// Added 2026-09-26. The original backend paid XP and credits from a result the
// server build reported; nothing in this exe does, so no match ever paid
// anything. At the same moment the eom stats go out -- once per connected
// player, as that player's end-of-match flow starts -- this reads the player's
// numbers from the host and sends them to mmogbrain's loopback
// GET /battle/result, which is idempotent per (match, player).
//
// Field offsets and types from the AYPlayerReplicationInfo property
// registration (0x140647C00: m_kills/m_deaths/m_assists are IntProperty,
// damage is the FloatProperty constructor shared with m_rawDamageReceived*,
// m_team a ByteProperty), AYGameState m_finalMatchResult +0x56B (EYMatchResult:
// 1 team 1, 2 team 2, 3 draw). The game state is reached PRI -> Outer (level)
// -> Outer (world) -> UWorld::GameState +0x58, and checked by class name.
// NOT verified live: whether m_finalMatchResult is already set when the
// transition starts. A 0 is reported as-is (mmogbrain pays it as a loss and
// logs outcome=unknown), which is the line to look for.
#define OFF_PRI_KILLS 0x848
#define OFF_PRI_DEATHS 0x850
#define OFF_PRI_ASSISTS 0x858
#define OFF_PRI_DAMAGE_WEAPONS 0x908
#define OFF_PRI_DAMAGE_ABILITIES 0x90C
#define OFF_PRI_TEAM 0x940
// Statistics screen figures (2026-10-15), from the SDK dump like the rest:
// m_score IntProperty, m_totalHealingDone FloatProperty. NOT verified live.
#define OFF_PRI_SCORE 0x7FC
#define OFF_PRI_HEALING 0x924
// AYPlayerController::m_team (SDK: Net, 0x0C30). FIXED 2026-09-27: the first
// reported match read PRI m_team = 0 (YT_NONE; EYTeam is NONE 0, TEAM1 1,
// TEAM2 2 per its registration 0x140697ED0) on a proving-ground host, so a won
// match was paid as a loss. The controller's copy is the fallback. Offset from
// the SDK dump, which matched every PRI/GameState offset verified live
// (0x654, 0x848, 0x56B); this one is NOT yet verified live.
#define OFF_PC_TEAM 0xC30
#define OFF_WORLD_GAMESTATE 0x58
#define OFF_GS_FINAL_MATCH_RESULT 0x56B
#define OFF_USTRUCT_SUPER 0x30

// Exact class name somewhere in the chain. ClassChainContains(o, "Actor")
// also matches ActorComponent: the projectile filter admitted
// YProjectileReplicator (a component) that way, and K2_GetActorLocation on it
// was fatal ("Failed to find function K2_GetActorLocation in
// YProjectileReplicator", ScriptCore.cpp:1268, Highlands + Space02 sweep
// 2026-10-17 13:10). Deeper limit too: blueprint shells sit far below Actor.
// Also used for "Pawn" / "Controller" wherever a hit leads to actor-only calls
// (UE4 has PawnMovementComponent, PawnSensingComponent, ...; DeepSeek round 15).
static bool ClassChainIs(UObjectMin *obj, const char *name) {
  UObjectMin *cls = IsReadable(obj, sizeof(UObjectMin)) ? obj->Class : nullptr;
  for (int depth = 0; cls && depth < 24; ++depth) {
    if (!IsReadable(cls, OFF_USTRUCT_SUPER + 8))
      return false;
    const char *n = NameText(cls->Name);
    if (n && !strcmp(n, name))
      return true;
    cls = *(UObjectMin **)((uint8_t *)cls + OFF_USTRUCT_SUPER);
  }
  return false;
}

// Walks as deep as ClassChainIs (24): at 8, a Blueprint chain deeper than
// that failed its "Pawn"/"PlayerController" test silently (AGENT-CHAT, the
// projectile audit). Misses cost up to 24 name reads instead of 8.
static bool ClassChainContains(UObjectMin *obj, const char *needle) {
  UObjectMin *cls = IsReadable(obj, sizeof(UObjectMin)) ? obj->Class : nullptr;
  for (int depth = 0; cls && depth < 24; ++depth) {
    if (!IsReadable(cls, OFF_USTRUCT_SUPER + 8))
      return false;
    const char *n = NameText(cls->Name);
    if (n && strstr(n, needle))
      return true;
    cls = *(UObjectMin **)((uint8_t *)cls + OFF_USTRUCT_SUPER);
  }
  return false;
}

static uint8_t *GameStateForPRI(uint8_t *pri) {
  UObjectMin *level = ((UObjectMin *)pri)->Outer;
  UObjectMin *world = IsReadable(level, sizeof(UObjectMin)) ? level->Outer : nullptr;
  if (!IsReadable(world, OFF_WORLD_GAMESTATE + 8))
    return nullptr;
  uint8_t *gs = *(uint8_t **)((uint8_t *)world + OFF_WORLD_GAMESTATE);
  if (!IsReadable(gs, OFF_GS_FINAL_MATCH_RESULT + 1) ||
      !ClassChainContains((UObjectMin *)gs, "GameState"))
    return nullptr;
  return gs;
}

// -MatchID=<id> from the host's command line (dn-dedicated passes it).
static bool MatchIDFromCommandLine(char *out, size_t outLen) {
  out[0] = 0;
  const wchar_t *cmd = GetCommandLineW();
  const wchar_t *p = cmd ? wcsstr(cmd, L"MatchID=") : nullptr;
  if (!p)
    return false;
  p += 8;
  size_t n = 0;
  while (p[n] && p[n] != L' ' && p[n] != L'"' && p[n] != L'?' && p[n] != L'&' &&
         n + 1 < outLen) {
    wchar_t c = p[n];
    out[n] = (c < 128 && (isalnum((int)c) || c == L'-' || c == L'_' || c == L'.'))
                 ? (char)c : '_';
    ++n;
  }
  out[n] = 0;
  return n > 0;
}

static int s_round = 0;

static void WriteEomRewards(uint8_t *pri, const char *body, const char *match);
static bool RewardsScreenEnabled();

// ---------------------------------------------------------------------------
// Original match XP (on; dn_host_no_event_xp.txt turns it off)
//
// mmogbrain now serves the ORIGINAL scoring table (the operator's datamine of
// the 1.11.1 / 1.12.0 tables, data/scoring/ScoringTable.json), XP and credits
// per event included. The host awards them itself, verified 2026-10-07:
// AYPlayerReplicationInfo::AssignScoringReward 0x5AD140 (one caller,
// 0x5B3170: rcx = the PRI, rdx = the event's reward record) runs on the
// server (role +0x148 == 3) and adds
//   score   rec[3] + rec[4]   to the PRI's score
//   XP      rec[5] + rec[6]   to the XP manager (AddMatchShipXP 0x3EB680 /
//                             AddMatchShipXPToAllShips 0x3EBA00)
//   credits rec[7] + rec[8]   (0x5ABDC0)
// with rec[0] the EYScoringEventID. Summing the XP from the XP manager would
// count "to all ships" events once per ship, so the record is summed here,
// per player, and reported with the match result (event_xp / event_credits);
// mmogbrain pays its XP from it instead of the placeholder formula.
#define RVA_ASSIGN_SCORING_REWARD 0x5AD140
typedef void(__fastcall *tAssignScoringReward)(void *pri, int32_t *reward);
static tAssignScoringReward g_origAssignScoringReward = nullptr;

// EYScoringEventID ordinals (Invalid = 75); see mmogbrain scoringEventIDs.
#define SCORING_EVENT_COUNT 76
struct EventTotals {
  void *pri;
  int64_t xp, credits;
  int events;
  // How often each scoring event fired for this player, rewarded or not, for
  // mmogbrain's ribbons (one per EventsForRibbon occurrences). The host
  // counts events too (0x431660) but never awards a ribbon itself: its reward
  // path builds only event achievements (0x5A9D30 type 1).
  uint16_t counts[SCORING_EVENT_COUNT];
};
static EventTotals g_eventTotals[128] = {};
static SRWLOCK g_eventTotalsLock = SRWLOCK_INIT;

// ---------------------------------------------------------------------------
// Ribbons announced in the match (on; dn_host_no_live_ribbons.txt /
// DN_HOST_NO_LIVE_RIBBONS=1 turns it off). Added 2026-10-15.
//
// A ribbon never showed during a match (operator: "ribbons dont get rewarded
// in matches"), although mmogbrain awards them from ev= after it. The host's
// reward path (0x426DC0) builds only a type-1 (event) achievement (0x5A9D30
// is called with 1), adds it to the scoring component's event list
// (0x4135A0) and, when the component's net check 0x141839880 says 1, sends it
// to the player with ClientAddAchievement -- the RPC stub 0x5E50C0 (its FName
// global 0x3E10168 is initialised from L"ClientAddAchievement" at 0xCD486;
// the respawn-filter guard further down hooks the same stub). Type 2 is the
// ribbon: 0x5A9D30 fills it from the RIBBON half of the record, and 0x413740
// is the component's ribbon list. Nothing in this exe ever builds one; the
// original server build did.
//
// So, the original rule: when a player's count of an event reaches a
// multiple of the row's EventsForRibbon (the same floor(count/n) mmogbrain
// pays), the mod builds the type-2 achievement and sends it exactly as the
// host sends type 1. The ribbon values per mode come from mmogbrain
// (GET /battle/ribbons?mode=, loaded when the match table is built).
// Achievement layout (0x1C, from 0x5A9D30 and the RPC's 0x1C-byte copy):
//   +0x00 u8 type (1 event, 2 ribbon)   +0x04 i32 EYScoringEventID
//   +0x08 u8 1, +0x09..0x0B 0           +0x0C score  +0x10 XP  +0x14 credits
//   +0x18 i32 the ship's precast loadout id (record [2] = PRI +0x654)
// The scoring component is AYPlayerReplicationInfo::m_scoringEventComponent
// (+0x928, SDK). NOT verified live.
#define RVA_CLIENT_ADD_ACHIEVEMENT_RPC 0x5E50C0
#define RVA_COMPONENT_ADD_RIBBON 0x413740
#define RVA_COMPONENT_NET_CHECK 0x1839880
#define OFF_PRI_SCORING_COMPONENT 0x928
#define OFF_PRI_PRECAST_LOADOUT 0x654
#define SCORING_EVENT_MATCH_END 40
struct RibbonRule {
  int n, score, xp, credits;
};
static RibbonRule g_ribbonRules[SCORING_EVENT_COUNT] = {};
static bool SwitchOn(const char *envName, const char *markerFile);

static bool LiveRibbonsEnabled() {
  static int s_on = -1;
  if (s_on < 0)
    s_on = SwitchOn("DN_HOST_NO_LIVE_RIBBONS", "dn_host_no_live_ribbons.txt") ? 0 : 1;
  return s_on == 1;
}

// The ribbon rows for the match's scoring mode, from mmogbrain.
static void LoadRibbonRules(const wchar_t *mode) {
  if (!LiveRibbonsEnabled())
    return;
  char m[24] = {0}, path[96], body[4096];
  for (int i = 0; i < 20 && mode && mode[i]; ++i)
    m[i] = (mode[i] < 128 && (isalnum((int)mode[i]) || mode[i] == L'_')) ? (char)mode[i] : '_';
  _snprintf_s(path, sizeof(path), _TRUNCATE, "/battle/ribbons?mode=%s", m);
  if (!HttpGetLoopback(path, body, sizeof(body))) {
    Logf("ribbons: GET %s failed; no ribbons announced in this match", path);
    return;
  }
  RibbonRule rules[SCORING_EVENT_COUNT] = {};
  int loaded = 0;
  for (char *line = body; line && *line;) {
    char *next = strchr(line, '\n');
    if (next)
      *next++ = 0;
    int ord, n, sc, xp, cr;
    if (sscanf_s(line, "%d %d %d %d %d", &ord, &n, &sc, &xp, &cr) == 5 && ord >= 0 &&
        ord < SCORING_EVENT_COUNT && n > 0) {
      rules[ord] = RibbonRule{n, sc, xp, cr};
      ++loaded;
    }
    line = next;
  }
  memcpy(g_ribbonRules, rules, sizeof(rules));
  Logf("ribbons: %d ribbon rules loaded for mode \"%s\"", loaded, m);
}

// Sends one ribbon (type-2 achievement) for event to the player owning pri.
static void SendRibbonAchievement(void *pri, uint32_t event) {
  if (!LiveRibbonsEnabled() || event >= SCORING_EVENT_COUNT || g_ribbonRules[event].n <= 0)
    return;
  __try {
    uint8_t *p = (uint8_t *)pri;
    if (!IsReadable(p + OFF_PRI_SCORING_COMPONENT, 8) || !IsReadable(p + OFF_PRI_PRECAST_LOADOUT, 4))
      return;
    void *comp = *(void **)(p + OFF_PRI_SCORING_COMPONENT);
    if (!comp || !IsReadable(comp, 0x200))
      return;
    alignas(8) uint8_t ach[0x20] = {};
    ach[0] = 2;
    *(int32_t *)(ach + 0x04) = (int32_t)event;
    ach[0x08] = 1;
    *(int32_t *)(ach + 0x0C) = g_ribbonRules[event].score;
    *(int32_t *)(ach + 0x10) = g_ribbonRules[event].xp;
    *(int32_t *)(ach + 0x14) = g_ribbonRules[event].credits;
    *(int32_t *)(ach + 0x18) = *(int32_t *)(p + OFF_PRI_PRECAST_LOADOUT);
    ((void(__fastcall *)(void *, void *))(g_base + RVA_COMPONENT_ADD_RIBBON))(comp, ach);
    int net = ((int(__fastcall *)(void *))(g_base + RVA_COMPONENT_NET_CHECK))(comp);
    if (net == 1)
      ((void(__fastcall *)(void *, void *))(g_base + RVA_CLIENT_ADD_ACHIEVEMENT_RPC))(comp, ach);
    Logf("ribbons: event %u -> ribbon for player state %p (net %d%s)", event, pri, net,
         net == 1 ? ", sent" : ", not sent: not a remote owner");
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("ribbons: EXCEPTION announcing event %u for %p", event, pri);
  }
}

// After a count went up: a ribbon when it reaches a multiple of the row's n.
static void MaybeAnnounceRibbon(void *pri, uint32_t event, unsigned count) {
  if (event < SCORING_EVENT_COUNT && count > 0 && g_ribbonRules[event].n > 0 &&
      count % (unsigned)g_ribbonRules[event].n == 0)
    SendRibbonAchievement(pri, event);
}

// The in-match ribbon popup (on; dn_host_no_ribbon_popup.txt /
// DN_HOST_NO_RIBBON_POPUP=1 turns it off). Added 2026-10-16.
//
// ClientAddAchievement (above) only files the ribbon into the client's
// scoring component list (the client's +0x350 is 0x4171E0 itself: type 2 ->
// 0x413740, no UI). The popup is a different RPC:
// AYPlayerReplicationInfo::ClientEventResult(FYEventResponse) -- sender stub
// 0x5E5280 (FName 0x3E101C8), client body 0x5AFEA0 (PRI vtable +0x698). The
// client takes the row's RIBBON name when m_ribbonAchieved (+0x04) is set,
// broadcasts it (0x5C59C0, isRibbon = that byte, points = eventScore +
// ribbonScore) to AYHUD::OnPlayerRepScoringEvent -> 0x494950 ->
// UYHUDWidget_ScoringEvents::BP_EventAddScoringEvent(text, level,
// isRibbonEvent, ribbonTexture), and plays "#Scoring_<name>".
//
// The host sends it already: PRI::ScoreEvent 0x5B30D0 fills the
// FYEventResponse (0x426DC0), calls AssignScoringReward (this hook) on it,
// then sends ClientEventResult if m_requiresHUDUpdate (+0x28) and the row's
// HUD flag (+0xB0) are set. It never sets the ribbon half. So, BEFORE the
// original runs, when this event completes a ribbon:
//   +0x04 m_ribbonAchieved = 1, +0x10 m_ribbonScore = the row's RibbonScore
//   (AssignScoringReward adds eventScore + ribbonScore to the score, as the
//   original server's did), +0x28 m_requiresHUDUpdate = 1 (0x426DC0 sets it
//   only for rows with an event score; GUESS: a ribbon also shows).
// Ribbon XP (+0x18) and credits (+0x20) stay 0: mmogbrain pays them from ev=.
// FYEventResponse (SDK): +0 m_id, +4 m_ribbonAchieved, +8 m_shipId,
// +0xC m_eventScore, +0x10 m_ribbonScore, +0x14 m_eventReward,
// +0x18 m_ribbonReward, +0x1C m_eventCredits, +0x20 m_ribbonCredits,
// +0x24 m_visibility, +0x28 m_requiresHUDUpdate.
static bool RibbonPopupEnabled() {
  static int s_on = -1;
  if (s_on < 0)
    s_on = SwitchOn("DN_HOST_NO_RIBBON_POPUP", "dn_host_no_ribbon_popup.txt") ? 0 : 1;
  return s_on == 1;
}

static void __fastcall HookAssignScoringReward(void *pri, int32_t *reward) {
  bool tracked = false, ribbon = false;
  uint32_t event = SCORING_EVENT_COUNT;
  __try {
    if (pri && IsReadable(reward, 0x2C) && IsReadable((uint8_t *)pri + 0x148, 1) &&
        ((uint8_t *)pri)[0x148] == 3) {
      tracked = true;
      event = (uint32_t)reward[0] & 0xff; // m_id: the row's Id byte
      unsigned count = 0;
      AcquireSRWLockExclusive(&g_eventTotalsLock);
      EventTotals *slot = nullptr;
      for (auto &t : g_eventTotals) {
        if (t.pri == pri) { slot = &t; break; }
        if (!slot && !t.pri) slot = &t;
      }
      if (slot) {
        slot->pri = pri;
        if (event < SCORING_EVENT_COUNT && slot->counts[event] < 0xFFFF)
          count = ++slot->counts[event];
      }
      ReleaseSRWLockExclusive(&g_eventTotalsLock);
      ribbon = LiveRibbonsEnabled() && event < SCORING_EVENT_COUNT && count > 0 &&
               g_ribbonRules[event].n > 0 && count % (unsigned)g_ribbonRules[event].n == 0;
      if (ribbon && RibbonPopupEnabled()) {
        ((uint8_t *)reward)[0x04] = 1;
        reward[4] = g_ribbonRules[event].score;
        ((uint8_t *)reward)[0x28] = 1;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    tracked = ribbon = false;
  }
  g_origAssignScoringReward(pri, reward);
  if (!tracked)
    return;
  __try {
    int32_t xp = reward[5] + reward[6], cr = reward[7] + reward[8];
    if (xp > 0 || cr > 0) {
      AcquireSRWLockExclusive(&g_eventTotalsLock);
      for (auto &t : g_eventTotals)
        if (t.pri == pri) {
          t.xp += xp > 0 ? xp : 0;
          t.credits += cr > 0 ? cr : 0;
          t.events++;
          break;
        }
      ReleaseSRWLockExclusive(&g_eventTotalsLock);
    }
    if (ribbon) {
      SendRibbonAchievement(pri, event);
      if (RibbonPopupEnabled())
        Logf("ribbons: event %u -> ribbon popup (ClientEventResult m_ribbonAchieved, ribbon score %d)",
             event, g_ribbonRules[event].score);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// One more occurrence of a scoring event for this player, for an event the
// host itself never fires here (see MVP / Man Of The Day below).
static void AddScoringEvent(void *pri, uint32_t event) {
  if (!pri || event >= SCORING_EVENT_COUNT)
    return;
  AcquireSRWLockExclusive(&g_eventTotalsLock);
  EventTotals *slot = nullptr;
  for (auto &t : g_eventTotals) {
    if (t.pri == pri) { slot = &t; break; }
    if (!slot && !t.pri) slot = &t;
  }
  unsigned count = 0;
  if (slot) {
    slot->pri = pri;
    if (slot->counts[event] < 0xFFFF)
      count = ++slot->counts[event];
  }
  ReleaseSRWLockExclusive(&g_eventTotalsLock);
  MaybeAnnounceRibbon(pri, event, count);
}

// The player's summed event XP and credits; false when the host awarded none
// (hook off, or an older mmogbrain without XP in its table).
static bool TakeEventTotals(void *pri, int64_t *xp, int64_t *credits, int *events,
                            char *ev = nullptr, size_t evSize = 0) {
  bool found = false;
  if (ev && evSize)
    ev[0] = 0;
  AcquireSRWLockExclusive(&g_eventTotalsLock);
  for (auto &t : g_eventTotals) {
    if (t.pri == pri) {
      *xp = t.xp; *credits = t.credits; *events = t.events;
      // "ordinal.count,..." for mmogbrain's ribbons (the "ev" parameter).
      size_t o = 0;
      for (int i = 0; ev && i < SCORING_EVENT_COUNT && o + 16 < evSize; ++i)
        if (t.counts[i])
          o += _snprintf_s(ev + o, evSize - o, _TRUNCATE, "%s%d.%u", o ? "," : "", i, (unsigned)t.counts[i]);
      t = EventTotals{};
      found = true;
      break;
    }
  }
  ReleaseSRWLockExclusive(&g_eventTotalsLock);
  return found;
}

static void ReportMatchResult(void *orbitComp) {
  char pid[80], match[96], path[3584], body[2048];
  uint8_t *priOut = nullptr;
  int kills = 0, deaths = 0, assists = 0, team = 0, result = 0, score = 0;
  const char *teamSource = "none";
  float damage = 0, healing = 0;
  __try {
    uint8_t *pc = *(uint8_t **)((uint8_t *)orbitComp + OFF_COMPONENT_OWNER);
    if (!PlayerPIDForController(pc, pid, sizeof(pid))) {
      if (!RememberedControllerPID(pc, pid, sizeof(pid))) {
        Logf("match result: controller %p has no DNPID (connection URL unreadable "
             "and no loadout was served to it); not reported", pc);
        return;
      }
      Logf("match result: controller %p -- DNPID not readable from the connection "
           "at match end; using %s from its loadout", pc, pid);
    }
    uint8_t *pri = *(uint8_t **)(pc + OFF_PC_PLAYER_STATE);
    if (!IsReadable(pri, OFF_PRI_TEAM + 1)) {
      Logf("match result: %s has no player state; not reported", pid);
      return;
    }
    priOut = pri;
    kills = *(int32_t *)(pri + OFF_PRI_KILLS);
    deaths = *(int32_t *)(pri + OFF_PRI_DEATHS);
    assists = *(int32_t *)(pri + OFF_PRI_ASSISTS);
    damage = *(float *)(pri + OFF_PRI_DAMAGE_WEAPONS) +
             *(float *)(pri + OFF_PRI_DAMAGE_ABILITIES);
    score = *(int32_t *)(pri + OFF_PRI_SCORE);
    healing = *(float *)(pri + OFF_PRI_HEALING);
    team = pri[OFF_PRI_TEAM];
    teamSource = "PRI";
    if ((team < 1 || team > 2) && IsReadable(pc + OFF_PC_TEAM, 1)) {
      team = pc[OFF_PC_TEAM];
      teamSource = "controller";
    }
    uint8_t *gs = GameStateForPRI(pri);
    // One -MatchID per host process, but the process could play a second
    // round after a map travel; a new game state object is a new round, so
    // mmogbrain's (match, player) key does not swallow it.
    static uint8_t *s_lastGS = nullptr;
    if (gs && gs != s_lastGS) {
      s_lastGS = gs;
      ++s_round;
    }
    if (gs)
      result = gs[OFF_GS_FINAL_MATCH_RESULT];
    else
      Logf("match result: game state not found from PRI %p; reporting final=0", pri);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("match result: EXCEPTION reading the result for %p", orbitComp);
    return;
  }
  if (!MatchIDFromCommandLine(match, sizeof(match))) {
    Logf("match result: no -MatchID= on the command line; not reported");
    return;
  }
  if (s_round > 1) {
    size_t ml = strlen(match);
    _snprintf_s(match + ml, sizeof(match) - ml, _TRUNCATE, "-r%d", s_round);
  }
  if (!(damage >= 0 && damage < 1e9f))
    damage = 0;
  if (!(healing >= 0 && healing < 1e9f))
    healing = 0;
  if (score < 0 || score > 10000000)
    score = 0;
  // Loadout ids are FName text; escape anything a query string cannot carry.
  char ships[1024];
  size_t o = 0;
  for (const char *c = FlownShipsFor(pid); *c && o + 4 < sizeof(ships); ++c) {
    if (isalnum((unsigned char)*c) || strchr("-_.,", *c))
      ships[o++] = *c;
    else
      o += _snprintf_s(ships + o, sizeof(ships) - o, _TRUNCATE, "%%%02X", (unsigned char)*c);
  }
  ships[o] = 0;
  _snprintf_s(path, sizeof(path), _TRUNCATE,
              "/battle/result?match=%s&pid=%s&team=%d&final=%d&kills=%d&deaths=%d"
              "&assists=%d&damage=%d&ships=%s",
              match, pid, team, result, kills, deaths, assists, (int)damage, ships);
  // The original table's XP and credits the host awarded this player (see
  // HookAssignScoringReward). Absent when none were awarded: mmogbrain then
  // pays its formula, as before.
  // Match Finish: every player who finishes earns it (mmogbrain counts it per
  // result); announced here, at the player's end of match.
  if (priOut)
    SendRibbonAchievement(priOut, SCORING_EVENT_MATCH_END);
  // MVP: the highest match score among the players (see AddScoringEvent).
  if (priOut && EomRibbonsEnabled()) {
    __try {
      uint8_t *gs = GameStateForPRI(priOut);
      if (gs && IsReadable(gs + 0x470, 16)) { // AGameState::PlayerArray (SDK)
        uint8_t **arr = *(uint8_t ***)(gs + 0x470);
        int count = *(int32_t *)(gs + 0x470 + 8);
        int best = 0;
        if (count > 0 && count <= 256 && IsReadable(arr, (size_t)count * 8))
          for (int i = 0; i < count; ++i)
            if (IsReadable(arr[i], OFF_PRI_SCORE + 4) &&
                ClassChainContains((UObjectMin *)arr[i], "YPlayerReplicationInfo") &&
                *(int32_t *)(arr[i] + OFF_PRI_SCORE) > best)
              best = *(int32_t *)(arr[i] + OFF_PRI_SCORE);
        if (best > 0 && score == best) {
          AddScoringEvent(priOut, SCORING_EVENT_MVP);
          Logf("ribbons: MVP -> %s (score %d, the match's highest)", pid, score);
        }
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  int64_t evXp = 0, evCr = 0;
  int evN = 0;
  char evCounts[SCORING_EVENT_COUNT * 10 + 8];
  if (priOut && TakeEventTotals(priOut, &evXp, &evCr, &evN, evCounts, sizeof(evCounts))) {
    size_t pl = strlen(path);
    // event_xp only when something paid XP or credits: its presence switches
    // mmogbrain from its formula to the table's figures.
    if (evN > 0)
      _snprintf_s(path + pl, sizeof(path) - pl, _TRUNCATE, "&event_xp=%lld&event_credits=%lld&events=%d",
                  (long long)evXp, (long long)evCr, evN);
    pl = strlen(path);
    if (evCounts[0])
      _snprintf_s(path + pl, sizeof(path) - pl, _TRUNCATE, "&ev=%s", evCounts);
  }
  // The kills by class, for mmogbrain's daily contracts (see RecordKill).
  char kl[KILL_LOG_MAX * 8 + 8];
  if (priOut && TakeKillLog(priOut, kl, sizeof(kl))) {
    size_t pl = strlen(path);
    _snprintf_s(path + pl, sizeof(path) - pl, _TRUNCATE, "&kl=%s", kl);
  }
  // Statistics screen: score, healing, seconds per ship class (ct= always
  // sent, empty when nothing was flown, so mmogbrain knows not to estimate).
  {
    char ct[CLASS_TIME_MAX * 16 + 8];
    ct[0] = 0;
    if (priOut)
      TakeClassTime(priOut, ct, sizeof(ct));
    size_t pl = strlen(path);
    _snprintf_s(path + pl, sizeof(path) - pl, _TRUNCATE, "&score=%d&heal=%d&ct=%s", score, (int)healing, ct);
  }
  bool ok = HttpGetLoopback(path, body, sizeof(body));
  if (ok && priOut && RewardsScreenEnabled())
    WriteEomRewards(priOut, body, match);
  for (char *c = body; *c; ++c)
    if (*c == '\n') *c = ' ';
  Logf("match result: %s team %d (%s) final %d kills %d deaths %d assists %d damage %d "
       "score %d healing %d event xp %lld credits %lld (%d events) ships [%s] -> %s%s",
       pid, team, teamSource, result, kills, deaths, assists, (int)damage, score, (int)healing,
       (long long)evXp, (long long)evCr, evN, FlownShipsFor(pid),
       ok ? "mmogbrain: " : "FAILED (mmogbrain unreachable or refused)", ok ? body : "");
  if (ok)
    ClearFlownShips(pid);
}

// ---------------------------------------------------------------------------
// End-of-match rewards screen (on; dn_host_no_eom_rewards.txt turns it off)
//
// The screen was empty although mmogbrain pays: the client reads its rewards
// from two structures that only the missing server build ever filled and
// finalized. Its own log says "IsInEndOfMatchDataFinalized - Credits info /
// Match XP is NOT finalized therefore the server closed before EOM was
// finalized" (AYPlayerControllerBase 0x5BBD20), and the host's says the same.
// Verified from the exe's reflection data (2026-09-28):
//
//   PRI +0x800  m_creditsInfo  FYCreditsInfo (0x28):  +0x00 m_credits
//               TArray<int32> per EYXPPoolType, +0x10 m_finalized, +0x18
//               m_battleID FString. Checked at PRI+0x810.
//   PRI +0x930  -> the XP manager; +0xF8 m_matchXPInfo FYMatchXPInfo (0x38):
//               +0x00 m_freeXP TArray<int32> per pool, +0x10 m_shipsXP
//               TArray<YMatchShipXP>, +0x20 m_finalized (checked by 0x401DE0
//               at +0x118), +0x28 m_battleID.
//   YMatchShipXP (0x28): +0x00 ship id (int, what 0x3FB0D0 matches),
//               +0x08 m_shipXp TArray<int32>, +0x18 m_freeXp TArray<int32>.
//
// mmogbrain answers /battle/result with the payout split into those pools
// (credit_pools / xp_pools / ship_xp_pools, 13 each) and the fleet's ship ids,
// and this writes them in with the engine's own array/string assignment, then
// sets both finalized flags -- so the screen shows what was actually paid.
// Replication to the client is the engine's (both are replicated properties).
// Not verified live yet.
// ---------------------------------------------------------------------------

#define OFF_PRI_CREDITS_INFO 0x800
#define OFF_PRI_XP_MANAGER 0x930
#define OFF_XPM_MATCH_XP 0xF8
#define REWARD_POOLS 13
#define MAX_FLEET_SHIPS 16
#define RVA_OPERATOR_NEW 0xE02DE0 // operator new(size) -> FMemory::Malloc

typedef void *(__fastcall *tOperatorNew)(size_t size);

static bool SwitchOn(const char *envName, const char *markerFile);

static bool RewardsScreenEnabled() {
  return !SwitchOn("DN_HOST_NO_EOM_REWARDS", "dn_host_no_eom_rewards.txt");
}

#define RVA_ACTOR_FLUSH_NET_DORMANCY 0x171C000
#define VT_ACTOR_FORCE_NET_UPDATE 0x580
typedef void(__fastcall *tActorVoid)(void *actor);

// PushActorNow asks the engine to replicate an actor on its next net tick:
// FlushNetDormancy, then ForceNetUpdate. 1 if both calls were made.
static int PushActorNow(void *actor) {
  if (!actor)
    return 0;
  __try {
    ((tActorVoid)(g_base + RVA_ACTOR_FLUSH_NET_DORMANCY))(actor);
    void **vt = *(void ***)actor;
    ((tActorVoid)vt[VT_ACTOR_FORCE_NET_UPDATE / 8])(actor);
    return 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("eom rewards: EXCEPTION 0x%08X pushing actor %p", GetExceptionCode(), actor);
    return 0;
  }
}

static void WriteEomRewards(uint8_t *pri, const char *body, const char *match) {
  char v[512];
  int32_t credits[REWARD_POOLS] = {}, xp[REWARD_POOLS] = {}, shipXp[REWARD_POOLS] = {};
  int32_t zero[REWARD_POOLS] = {}, unplayed[REWARD_POOLS] = {};
  int32_t fleet[MAX_FLEET_SHIPS] = {}, flown[MAX_FLEET_SHIPS] = {};
  if (ParseInts(FieldValue(body, "credit_pools", v, sizeof(v)), credits, REWARD_POOLS) != REWARD_POOLS ||
      ParseInts(FieldValue(body, "xp_pools", v, sizeof(v)), xp, REWARD_POOLS) != REWARD_POOLS ||
      ParseInts(FieldValue(body, "ship_xp_pools", v, sizeof(v)), shipXp, REWARD_POOLS) != REWARD_POOLS) {
    Logf("eom rewards: mmogbrain sent no pools (older mmogbrain?); screen stays empty");
    return;
  }
  // What every fleet ship NOT flown earned (mmogbrain unplayedShipPools, 10% of
  // the XP without performance by default). Optional: an older mmogbrain does
  // not send it, and those ships then show 0, as before.
  bool haveUnplayed =
      ParseInts(FieldValue(body, "unplayed_ship_xp_pools", v, sizeof(v)), unplayed, REWARD_POOLS) == REWARD_POOLS;
  int nFleet = ParseInts(FieldValue(body, "fleet_ships", v, sizeof(v)), fleet, MAX_FLEET_SHIPS);
  int nFlown = ParseInts(FieldValue(body, "flown_ships", v, sizeof(v)), flown, MAX_FLEET_SHIPS);

  wchar_t wmatch[96];
  int ml = MultiByteToWideChar(CP_UTF8, 0, match, -1, wmatch, 96);
  FStringMin battleID = {wmatch, ml > 0 ? ml : 1, ml > 0 ? ml : 1};
  TArrayIntMin creditsArr = {credits, REWARD_POOLS, REWARD_POOLS};
  TArrayIntMin xpArr = {xp, REWARD_POOLS, REWARD_POOLS};
  TArrayIntMin shipArr = {shipXp, REWARD_POOLS, REWARD_POOLS};
  TArrayIntMin zeroArr = {zero, REWARD_POOLS, REWARD_POOLS};
  TArrayIntMin unplayedArr = {haveUnplayed ? unplayed : zero, REWARD_POOLS, REWARD_POOLS};
  tAssign fstr = (tAssign)(g_base + RVA_FSTRING_ASSIGN);
  tAssign tarr = (tAssign)(g_base + RVA_TARRAY_INT_ASSIGN);

  uint8_t *xpmOut = nullptr;
  __try {
    uint8_t *ci = pri + OFF_PRI_CREDITS_INFO;
    tarr(ci + 0x00, &creditsArr);
    ci[0x10] = 1;
    fstr(ci + 0x18, &battleID);

    uint8_t *xpm = *(uint8_t **)(pri + OFF_PRI_XP_MANAGER);
    xpmOut = xpm;
    if (!xpm || !IsReadable(xpm + OFF_XPM_MATCH_XP, 0x38)) {
      Logf("eom rewards: credits written; PRI %p has no XP manager, match XP not written", pri);
      return;
    }
    uint8_t *mx = xpm + OFF_XPM_MATCH_XP;
    tarr(mx + 0x00, &xpArr);
    uint8_t *ships = nullptr;
    if (nFleet > 0) {
      ships = (uint8_t *)((tOperatorNew)(g_base + RVA_OPERATOR_NEW))((size_t)nFleet * 0x28);
      if (!ships) {
        Logf("eom rewards: allocation failed; match XP not written");
        return;
      }
      memset(ships, 0, (size_t)nFleet * 0x28);
      for (int i = 0; i < nFleet; ++i) {
        uint8_t *e = ships + i * 0x28;
        *(int32_t *)e = fleet[i];
        bool wasFlown = false;
        for (int j = 0; j < nFlown; ++j)
          wasFlown |= flown[j] == fleet[i];
        tarr(e + 0x08, wasFlown ? &shipArr : &unplayedArr); // m_shipXp
        tarr(e + 0x18, &zeroArr);                        // m_freeXp
      }
    }
    // The previous (empty) array is not freed: it has nothing to free.
    TArrayIntMin *shipsXP = (TArrayIntMin *)(mx + 0x10);
    shipsXP->data = (int32_t *)ships;
    shipsXP->num = nFleet;
    shipsXP->max = nFleet;
    mx[0x20] = 1;
    fstr(mx + 0x28, &battleID);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("eom rewards: EXCEPTION 0x%08X writing PRI %p", GetExceptionCode(), pri);
    return;
  }
  // Push both actors to the owning client NOW rather than whenever the net
  // driver next considers them. The rewards are written straight into memory;
  // replication picks the change up only on the actor's next net update, and
  // with only the delay to rely on the client's end-of-match init stage still
  // found no pools in 4 of 5 matches (ShipXpError at 22:29, 23:59, 02:03,
  // 08:18 on 2026-09-29/30, each with the rewards written 4.5-5 s before the
  // transition, for exactly the reported ship ids). FlushNetDormancy first,
  // in case an actor is dormant (a dormant actor does not replicate at all).
  // Both are the engine's own AActor functions, from their native
  // registrations (0x1D3810A / 0x1D3812C): ForceNetUpdate's exec thunk
  // 0x1E70010 ends in "jmp [vtable+0x580]" (a virtual); FlushNetDormancy's
  // thunk 0x1E6FF00 jumps to its body 0x171C000 (this).
  //
  // ONLY the PRI. The XP manager (PRI+0x930) is a UYXPManager : UActorComponent,
  // NOT an actor (SDK: DreadGame.YXPManager), so FlushNetDormancy and vtable
  // +0x580 do not exist on it -- slot +0x580 of a component's vtable is some
  // other virtual. The first build of this pushed it too, and the client's
  // end-of-match screen lost its Rewards tab (MVP/Stats/Scoreboard only,
  // operator 2026-09-30). A component replicates through its owner's actor
  // channel, so forcing the PRI covers m_matchXPInfo as well.
  // dn_host_no_eom_push.txt / DN_HOST_NO_EOM_PUSH=1 turns the push off, to
  // bisect it against the end-of-match screen without a rebuild.
  (void)xpmOut;
  int pushed = SwitchOn("DN_HOST_NO_EOM_PUSH", "dn_host_no_eom_push.txt") ? 0 : PushActorNow(pri);
  Logf("eom rewards: PRI %p written -- scoring pool credits %d xp %d, %d fleet ships "
       "(%d flown, unplayed share %d), both finalized; PRI pushed %d (FlushNetDormancy + ForceNetUpdate)",
       pri, credits[0], xp[0], nFleet, nFlown, haveUnplayed ? unplayed[0] : 0, pushed);
}

// ---------------------------------------------------------------------------
// Teams, names and bot balance, from the game-mode timer
// (on unless dn_host_no_team_sync.txt / DN_HOST_NO_TEAM_SYNC=1)
//
// Three gaps the host has because the original server build fed it match data:
//
//   Teams. The host never reads ?TEAM= (the only "TEAM=" in the exe is the
//   CLIENT's "TRAVEL %s?TEAM=%s"), and each human's PlayerState team
//   (AYPlayerReplicationInfo::m_team +0x940) stayed YT_NONE 0 -- so players
//   were missing from the scoreboard, results scored them as no team, and two
//   TDM players shared a side.
//
//   Names. Every join arrives with an empty ?Name= (the client's Browse
//   replaces it with its empty nickname), so AGameMode::InitNewPlayer named
//   players 257, 258. The name comes from mmogbrain by DNPID
//   (GET /battle/player), else the join URL's PlayerName=.
//
//   Bots. In the proving ground the bot targets are the mode's fixed team size
//   (gm+0x97C, copied to gm+0x980 / +0x984 by 0x3678F0); nothing subtracts the
//   humans, so three players still got 8 bots on their side. The game's own
//   SetTeamSizeAI (body 0x381550: sets the target, then 0x381FA0 trims surplus
//   bots) is called with teamSize - humans.
//
// CHANGED 2026-09-28: the first version did the team and name at PostLogin
// through the ProcessEvent hook, calling AYPlayerController::SetTeam. With it,
// only the LAST player to join got his own loadout (the others' ship picks
// never reached FindLoadoutByID -- host logs 01:07 and 01:09); with it off
// (DN_HOST_NO_TEAM_SYNC=1, 01:24) both players' fits registered. So this runs
// from the once-a-second game-mode timer instead, writes only the team bytes
// that are wrong (the controller already had the right team in the proving
// ground), and calls no SetTeam. NOT verified live yet.
#define OFF_GS_PLAYER_ARRAY 0x470 // AGameState::PlayerArray (SDK)
#define OFF_ACTOR_OWNER 0xC8      // AActor::Owner (SDK)
#define OFF_GM_TEAM_SIZE 0x97C
#ifndef OFF_GM_AI_TARGET_T1
#define OFF_GM_AI_TARGET_T1 0x980
#endif
#define RVA_SET_TEAM_SIZE_AI 0x381550

typedef void(__fastcall *tSetTeamSizeAI)(void *gameMode, int team, int size);
static bool SwitchOn(const char *envName, const char *markerFile);

struct PlayerSeen {
  void *pc;
  bool named;
  int lastTeamFix;
};
static PlayerSeen g_seen[64];

static PlayerSeen *SeenFor(void *pc) {
  PlayerSeen *freeSlot = nullptr;
  for (auto &p : g_seen) {
    if (p.pc == pc)
      return &p;
    if (!freeSlot && !p.pc)
      freeSlot = &p;
  }
  if (freeSlot) {
    freeSlot->pc = pc;
    freeSlot->named = false;
    freeSlot->lastTeamFix = -1;
  }
  return freeSlot;
}

// Captain appearance. AYPlayerReplicationInfo::m_captainDisplayInformation
// is a replicated FString with RepNotify (probe 2026-10-14 22:02, headless PG:
// StrProperty +0x758, flags 0018000100002220 = CPF_Net|CPF_RepNotify; listed in
// GetLifetimeReplicatedProps 0x5B6FFF). On a client OnRep_CaptainData builds
// the captain from it (UYCaptain::ImportDisplayInfo 0x307650 -> parser
// 0x306ED0, format "GENDER_x;#iiS=..#iiH=.."). The login-less host never fills
// it (value was empty), so every client drew every other player -- and its
// own lineup entry -- with the default captain. The string is the player's
// saved one from mmogbrain (/battle/player captain=, the same DisplayInfo the
// client sends in YA_SavePlayerDisplayInformation), i.e. data the original
// server had, not a fabricated look. Offset resolved by reflection each time.
// NOT verified live: that clients re-render the lineup/end-of-match captain
// from the OnRep alone.
static int PropOffset(UObjectMin *obj, const char *name);
static void SetCaptainDisplayInfo(uint8_t *pc, const char *body) {
  if (SwitchOn("DN_HOST_NO_CAPTAIN", "dn_host_no_captain.txt")) // upstream d5b5cff switch
    return;
  char v[512];
  const char *got = FieldValue(body, "captain", v, sizeof(v));
  if (!got || !v[0] || strlen(v) >= sizeof(v) - 1)
    return;
  uint8_t *pri = IsReadable(pc + OFF_PC_PLAYER_STATE, 8) ? *(uint8_t **)(pc + OFF_PC_PLAYER_STATE) : nullptr;
  int off = pri ? PropOffset((UObjectMin *)pri, "m_captainDisplayInformation") : -1;
  if (off <= 0 || !IsReadable(pri + off, sizeof(FStringMin))) {
    Logf("captain: no m_captainDisplayInformation on player state %p of %p", pri, pc);
    return;
  }
  static wchar_t w[512];
  int wl = MultiByteToWideChar(CP_UTF8, 0, v, -1, w, 512);
  if (wl <= 1)
    return;
  FStringMin tmp = {w, wl, wl};
  ((tAssign)(g_base + RVA_FSTRING_ASSIGN))(pri + off, &tmp);
  Logf("captain: player state %p +0x%x = \"%s\"", pri, off, v);
}

static void NamePlayer(uint8_t *gm, uint8_t *pc, PlayerSeen *seen) {
  seen->named = true; // one attempt per player, logged either way
  static wchar_t name[64];
  name[0] = 0;
  const char *source = "mmogbrain";
  char pid[80], path[160], body[1024], v[128];
  if (PlayerPIDForController(pc, pid, sizeof(pid))) {
    _snprintf_s(path, sizeof(path), _TRUNCATE, "/battle/player?pid=%s", pid);
    if (HttpGetLoopback(path, body, sizeof(body))) {
      if (FieldValue(body, "name", v, sizeof(v)) && v[0])
        MultiByteToWideChar(CP_UTF8, 0, v, -1, name, 32);
      SetCaptainDisplayInfo(pc, body);
    }
  }
  if (!name[0]) {
    source = "join URL";
    if (!UrlOptionWideForController(pc, L"PlayerName=", name, 32)) {
      Logf("names: no name for controller %p (pid %s): mmogbrain had none and "
           "the join URL has no PlayerName", pc, pid[0] ? pid : "<none>");
      return;
    }
  }
  static uint64_t s_changeName = 0;
  if (!s_changeName) {
    FNameMin n = {};
    ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&n, L"ChangeName", 1 /* FNAME_Add */);
    s_changeName = *(uint64_t *)&n;
  }
  void *fn = FindFunctionSafe(gm, s_changeName);
  if (!fn) {
    Logf("names: ChangeName not found on game mode %p", gm);
    return;
  }
  // AGameMode_ChangeName_Params (SDK): Controller +0x00, FString +0x08,
  // bool bNameChange +0x18.
  struct {
    void *controller;
    FStringMin newName;
    uint8_t bNameChange;
    uint8_t pad[7];
  } parms = {};
  parms.controller = pc;
  int len = (int)wcslen(name);
  parms.newName = {name, len + 1, len + 1};
  void **vt = *(void ***)gm;
  ((tProcessEventVirt)vt[VT_PROCESS_EVENT / 8])(gm, fn, &parms);
  char narrow[64];
  WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr);
  Logf("names: controller %p is now \"%s\" (from %s)", pc, narrow, source);
}

// /team <pid> <1|2>: operator team moves, consulted before the TEAM= travel
// option. Applied by FixTeam on the next player tick.
struct TeamOverride { char pid[40]; int team; };
static TeamOverride g_teamOverride[16];
static UObjectMin *FindFnOnChain(void *obj, const char *name);

static int TeamOverrideFor(uint8_t *pc) {
  char pid[80];
  if (!UrlOptionForController(pc, L"DNPID=", pid, sizeof(pid)))
    return 0;
  for (TeamOverride &o : g_teamOverride)
    if (o.team && !_stricmp(o.pid, pid))
      return o.team;
  return 0;
}

// /match ffa (the brain sends "ffa on" to that match): every human's ship on
// a team of its own, 3 and up, and no bots. Only the ship (pawn) team is set
// -- targeting, damage and colours follow it, as the herald and /boss showed
// (2026-10-18) -- while the player state and controller keep the matchmaker's
// 1/2, so spawn points and the scoreboard stay the stock TDM ones.
// GUESS: kills between two players of the same 1/2 side may score as team
// kills; TDM's team score only knows 1 and 2.
void *g_ffaGm = nullptr;
static struct { void *pc; uint8_t team; int kills; } g_ffaSlot[64];
static int g_ffaN = 0;
// FFA score (2026-10-20): the kill handler 0x375F30 passes the killer SHIP's
// team (0x57C100, pawn +0x8C0) to 0x39E470, which adds 1 to gs+0x1DF0 for
// team 1 and to gs+0x1DF4 for any other -- teams 3+ all score as team 2 --
// and 0x3685C0 ends the match when either reaches gm+0x9DC. The winner byte
// gs+0x4E8 holds only 1, 2 or 3 (draw). So FFA keeps its own tally per
// player (g_ffaSlot[].kills), ends the match when one reaches the game
// mode's own limit (saved in g_ffaLimit, then lifted like Onslaught's), and
// names as winner the leader's PLAYER STATE team (1/2, the matchmaker's).
// GUESS: the victory screen follows +0x4E8, so the leader's whole 1/2 side
// sees "victory"; a per-player screen is unknown (not run with 2+ players).
static int g_ffaLimit = 0, g_ffaLead = -1, g_ffaLimitArg = 0; // "ffa on N": N kills (else the mode's)
static int g_ffaLimitSaved = 0; // the game mode's own m_killsForWinning, restored on "ffa off"
static void *g_ffaLimitGm = nullptr;
static bool g_ffaEnded = false;
static int FfaKillLimit(uint8_t *gm) {
  if (!IsReadable(gm + OFF_GM_KILLS_FOR_WINNING, 4))
    return 0;
  int32_t *lim = (int32_t *)(gm + OFF_GM_KILLS_FOR_WINNING);
  if (g_ffaLimitGm != gm) {
    g_ffaLimitGm = gm;
    g_ffaLimit = g_ffaLimitSaved = *lim > 0 && *lim < ONS_KILL_LIMIT_OFF ? *lim : 0;
    if (g_ffaLimitArg > 0)
      Logf("ffa: limit %d from the command (game mode's %d)", g_ffaLimitArg, g_ffaLimit), g_ffaLimit = g_ffaLimitArg;
    g_ffaLead = -1, g_ffaEnded = false;
    for (int i = 0; i < g_ffaN; ++i)
      g_ffaSlot[i].kills = 0;
    Logf("ffa: match %p -- kill limit %d per player (team limit lifted)", gm, g_ffaLimit);
  }
  *lim = ONS_KILL_LIMIT_OFF;
  return g_ffaLimit;
}
static void FfaCountKill(uint8_t *gm, void *killer, void *victimCtrl) {
  if (!killer || killer == victimCtrl || g_ffaEnded)
    return; // GUESS: a suicide scores nothing (stock TDM gives the other side 1)
  int i = 0;
  while (i < g_ffaN && g_ffaSlot[i].pc != killer)
    ++i;
  if (i == g_ffaN)
    return; // not a player FfaTick has seen (FFA runs without bots)
  int k = ++g_ffaSlot[i].kills;
  if (g_ffaLead < 0 || k > g_ffaSlot[g_ffaLead].kills)
    g_ffaLead = i;
  Logf("ffa: controller %p (team %d) %d kill(s), limit %d", killer, g_ffaSlot[i].team, k, g_ffaLimit);
  if (g_ffaLimit <= 0 || k < g_ffaLimit || !IsReadable(gm + OFF_GM_GAMESTATE, 8))
    return;
  uint8_t *gs = *(uint8_t **)(gm + OFF_GM_GAMESTATE);
  if (!gs || !FfaLeader(gm, gs))
    return;
  g_ffaEnded = true;
  Logf("ffa: controller %p reached %d kills -- ending the match, winner byte %d", killer, k, gs[OFF_GS_WINNING_TEAM]);
  AwardManOfTheDay(killer, gs[OFF_GS_WINNING_TEAM], "ffa kill limit");
  void **vt = *(void ***)gm;
  ((void(__fastcall *)(void *))vt[VT_GM_END_MATCH / 8])(gm);
}
// gs+0x4E8 from the FFA leader's player-state team; false = leave the stock value.
static bool FfaLeader(void *gm, uint8_t *gs) {
  if (g_ffaLimitGm != gm || g_ffaLead < 0 || !gs || !IsReadable(gs + OFF_GS_WINNING_TEAM, 1))
    return false;
  uint8_t *pc = (uint8_t *)g_ffaSlot[g_ffaLead].pc;
  uint8_t *pri = IsReadable(pc + OFF_PC_PLAYER_STATE, 8) ? *(uint8_t **)(pc + OFF_PC_PLAYER_STATE) : nullptr;
  int t = pri && IsReadable(pri + 0x940 /* OFF_PRI_TEAM */, 1) ? pri[0x940] : 0;
  int tie = 0;
  for (int i = 0; i < g_ffaN; ++i)
    tie += g_ffaSlot[i].kills == g_ffaSlot[g_ffaLead].kills;
  gs[OFF_GS_WINNING_TEAM] = tie > 1 || (t != 1 && t != 2) ? 3 : (uint8_t)t;
  return true;
}
static void FfaTick(uint8_t *pc) {
  int i = 0;
  while (i < g_ffaN && g_ffaSlot[i].pc != pc)
    ++i;
  if (i == g_ffaN) {
    if (g_ffaN >= 64)
      return;
    g_ffaSlot[g_ffaN].pc = pc;
    g_ffaSlot[g_ffaN].team = (uint8_t)(3 + g_ffaN);
    ++g_ffaN;
  }
  uint8_t want = g_ffaSlot[i].team;
  uint8_t *pawn = IsReadable(pc + OFF_CONTROLLER_PAWN, 8) ? *(uint8_t **)(pc + OFF_CONTROLLER_PAWN) : nullptr;
  if (!pawn || !IsReadable(pawn + OFF_SHIP_TEAM, 1) || pawn[OFF_SHIP_TEAM] == want)
    return;
  int was = pawn[OFF_SHIP_TEAM];
  __try {
    ((tPawnSetTeam)(g_base + RVA_PAWN_SET_TEAM))(pawn, &want);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return (void)Logf("ffa: controller %p ship team set faulted", pc);
  }
  Logf("ffa: controller %p ship %s team %d -> %d", pc, ClassNameOf((UObjectMin *)pawn), was, want);
}

static void FixTeam(uint8_t *pc, uint8_t *pri, PlayerSeen *seen) {
  char v[8];
  int over = TeamOverrideFor(pc);
  if (!over && !UrlOptionForController(pc, L"TEAM=", v, sizeof(v)))
    return;
  int want = over ? over : atoi(v);
  if (want < 1 || want > 2)
    return;
  int priTeam = pri[OFF_PRI_TEAM];
  int pcTeam = IsReadable(pc + OFF_PC_TEAM, 1) ? pc[OFF_PC_TEAM] : want;
  if (priTeam == want && pcTeam == want)
    return;
  pri[OFF_PRI_TEAM] = (uint8_t)want;
  if (IsReadable(pc + OFF_PC_TEAM, 1))
    pc[OFF_PC_TEAM] = (uint8_t)want;
  if (seen->lastTeamFix != want) // do not repeat the line every second
    Logf("team sync: controller %p TEAM=%d -- player state %d -> %d, controller %d -> %d",
         pc, want, priTeam, want, pcTeam, want);
  seen->lastTeamFix = want;
  // A ship already in flight carries its own team (targeting, colours): set
  // it with the pawn SetTeam 0x5961C0 the NPC spawner uses. The first try
  // called a "Suicide" UFunction to respawn instead -- not on the controller's
  // class chain (headless 2026-10-16), so the ship kept its old side.
  if (over && priTeam != want) {
    void *pawn = IsReadable(pc + OFF_CONTROLLER_PAWN, 8) ? *(void **)(pc + OFF_CONTROLLER_PAWN) : nullptr;
    if (pawn && IsReadable(pawn, sizeof(UObjectMin))) {
      uint8_t t = (uint8_t)want;
      ((tPawnSetTeam)(g_base + RVA_PAWN_SET_TEAM))(pawn, &t);
    }
    Logf("team sync: operator move %d -> %d, ship %s", priTeam, want, pawn ? ClassNameOf(pawn) : "none");
  }
}

// Runs once a second on the game thread (HookGameModeTimer).
static void PlayersTick(uint8_t *gm) {
  if (!g_teamSyncArmed)
    return;
  __try {
    if (!IsReadable(gm + OFF_GM_GAMESTATE, 8))
      return;
    uint8_t *gs = *(uint8_t **)(gm + OFF_GM_GAMESTATE);
    if (!IsReadable(gs + OFF_GS_PLAYER_ARRAY, 16))
      return;
    uint8_t **arr = *(uint8_t ***)(gs + OFF_GS_PLAYER_ARRAY);
    int count = *(int32_t *)(gs + OFF_GS_PLAYER_ARRAY + 8);
    if (count <= 0 || count > 256 || !IsReadable(arr, (size_t)count * 8))
      return;
    int humans = 0;
    int humansOnTeam[3] = {0, 0, 0};
    for (int i = 0; i < count; ++i) {
      uint8_t *pri = arr[i];
      if (!IsReadable(pri, OFF_PRI_TEAM + 1))
        continue;
      uint8_t *pc = *(uint8_t **)(pri + OFF_ACTOR_OWNER);
      // Humans only: a player controller with a network connection -- not the
      // host's own local player, not a bot's AI controller.
      if (!IsReadable(pc, OFF_PC_NETCONNECTION + 8) ||
          !ClassChainContains((UObjectMin *)pc, "PlayerController") ||
          !*(void **)(pc + OFF_PC_NETCONNECTION))
        continue;
      ++humans;
      PlayerSeen *seen = SeenFor(pc);
      if (!seen)
        continue;
      // Remember (pc, pid) every tick: RegisterPlayerLoadout's fallback needs
      // it when the loadout manager's owner read fails at orbit time (see
      // PlayerPIDForManagerRobust). Re-writing the same pair is idempotent.
      {
        char pidTmp[80];
        if (UrlOptionForController(pc, L"DNPID=", pidTmp, sizeof(pidTmp)))
          RememberControllerPID(pc, pidTmp);
      }
      FixTeam(pc, pri, seen);
      if (g_ffaGm == gm)
        FfaTick(pc);
      int t = pri[OFF_PRI_TEAM];
      if (t == 1 || t == 2)
        ++humansOnTeam[t];
      if (!seen->named) {
        NamePlayer(gm, pc, seen);
        if (SwitchOn("DN_HOST_CAPTAIN_PROBE", "dn_host_captain_probe.txt"))
          DescribeProps("captain probe:", (UObjectMin *)pri, "aptain");
      }
    }

    // Humans take bot slots on their own team, in any mode with bots
    // (m_enableSpawnAI set): proving ground (all humans on team 1) and PvP.
    static int s_botBalance = -1;
    if (s_botBalance < 0)
      s_botBalance = SwitchOn("DN_HOST_NO_BOT_BALANCE", "dn_host_no_bot_balance.txt") ? 0 : 1;
    if (s_botBalance && IsReadable(gm + OFF_GM_ENABLE_SPAWN_AI, 1) && gm[OFF_GM_ENABLE_SPAWN_AI]) {
      int size = *(int32_t *)(gm + OFF_GM_TEAM_SIZE);
      bool mine = g_extraBotsGm == gm, only = mine && g_extraOnly;
      for (int team = 1; team <= 2; ++team) {
        int target = *(int32_t *)(gm + OFF_GM_AI_TARGET_T1 + (team - 1) * 4); // +0x980, +0x984
        int extra = mine ? g_extraBots[team] : 0;
        int want = only ? extra : std::max(0, size - humansOnTeam[team]) + extra;
        // target > 0: the bots have been set up (0x3678F0 ran); before that a
        // call would fill early. That was once wanted for only /spawn'd bots,
        // but an info added before the fill never gets a ship (see
        // SpawnGameBot), so they wait for the fill too.
        if (only ? target > 0 && target != want : size > 0 && target > 0 && (humansOnTeam[team] > 0 || extra > 0) && target != want) {
          ((tSetTeamSizeAI)(g_base + RVA_SET_TEAM_SIZE_AI))(gm, team, want);
          Logf("bot balance: %d human(s) on team %d, %d /spawn'd; team size %d -> team %d bots %d -> %d",
               humansOnTeam[team], team, extra, size, team, target, want);
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("players tick: EXCEPTION on game mode %p", gm);
  }
}

// Seconds per ship class (see AddClassTime). Runs from the same once-a-second
// game-mode timer, timed by the tick count rather than assumed to be 1 s.
static void ClassTimeTick(uint8_t *gm) {
  static DWORD s_last = 0;
  static void *s_lastGM = nullptr;
  DWORD now = GetTickCount();
  DWORD elapsed = (s_lastGM == gm && s_last) ? now - s_last : 0;
  s_last = now;
  s_lastGM = gm;
  if (!elapsed)
    return;
  __try {
    if (!IsReadable(gm + OFF_GM_GAMESTATE, 8))
      return;
    uint8_t *gs = *(uint8_t **)(gm + OFF_GM_GAMESTATE);
    if (!IsReadable(gs + OFF_GS_PLAYER_ARRAY, 16))
      return;
    uint8_t **arr = *(uint8_t ***)(gs + OFF_GS_PLAYER_ARRAY);
    int count = *(int32_t *)(gs + OFF_GS_PLAYER_ARRAY + 8);
    if (count <= 0 || count > 256 || !IsReadable(arr, (size_t)count * 8))
      return;
    for (int i = 0; i < count; ++i) {
      uint8_t *pri = arr[i];
      if (!IsReadable(pri, OFF_PRI_TEAM + 1))
        continue;
      uint8_t *pc = *(uint8_t **)(pri + OFF_ACTOR_OWNER);
      if (!IsReadable(pc, OFF_PC_NETCONNECTION + 8) ||
          !ClassChainContains((UObjectMin *)pc, "PlayerController") ||
          !*(void **)(pc + OFF_PC_NETCONNECTION) || !IsReadable(pc + OFF_CONTROLLER_PAWN, 8))
        continue;
      uint8_t *pawn = *(uint8_t **)(pc + OFF_CONTROLLER_PAWN);
      if (pawn && IsReadable(pawn + OFF_PAWN_SHIP_CLASS, 1))
        AddClassTime(pri, pawn[OFF_PAWN_SHIP_CLASS], elapsed);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

static bool SwitchOn(const char *envName, const char *markerFile);

// The end-of-match TRANSITION itself is deferred, and the rewards go first.
//
// The client gathers its rewards in UYEndOfMatchInitStage (0x32D4B0 ->
// 0x34A1F0 -> UYEndOfMatchDataManager::GatherEomReward 0x340C40), the FIRST
// stage of the end-of-match flow that ClientStartEndOfMatchTransition starts.
// For every fleet ship it looks the ship up in m_shipsXP (0x3FB0D0) and needs
// exactly 13 pools (the EYXPPoolType::MAX array built by 0x332690), else it
// files "ShipXpError: Ship XP pools were not gathered correctly" per ship.
// The rewards reach the client by property replication, so they must be
// written BEFORE the transition RPC leaves, with time to replicate.
//
// History, both verified from client_reports against this log:
//   2026-09-28: stats RPC, then rewards -> a ShipXpError batch per player,
//     0.5 s after "eom rewards: written".
//   2026-09-29 morning: rewards, then the stats RPC 3 s later -> the SAME
//     batches, now 3 s BEFORE "sent ... (deferred)": the gather had already
//     run at the transition, which went out untouched before the write.
// Now: rewards at once, then after EOM_TRANSITION_DELAY_MS the original
// transition, and the stats RPC right after it (the order that fixed the
// black screen). Sent from the game mode's once-a-second timer, which keeps
// running after the match (verified 2026-09-29: "sent ... (deferred)" lines).
// dn_host_eom_no_defer.txt / DN_HOST_EOM_NO_DEFER=1 restores the old flow
// (transition, stats, then rewards).
#define EOM_TRANSITION_DELAY_MS 3000
struct PendingEomTransition {
  void *orbitComp;
  uint8_t *pc;
  uint8_t *pri;
  DWORD due;
};
static PendingEomTransition g_pendingEom[32];

// Personal stats page (the end-of-match "Player Stats" rows).
//
// The client fills UYWidget_EndOfMatchPlayerStatsPage from the array this RPC
// delivers (PRI+0x7D0, set by ClientSetTopPlayerMatchStats; 0x4654D0 makes one
// row per entry, NO filter by player) and each row (0x469280) reads +0x38 the
// comparison, +0x3C the value (float, shown as a number), +0x18 the label
// (FText) and +0x40 the difference to average (%.1f). The code that computed
// these -- UYPlayerMatchStatisticsManager, RetrieveTopStatsForPlayers -- is not
// in this exe (none of its log strings is referenced): it was server-build
// code. So the mod builds the rows from the player's own PRI.
//
// FYPlayerMatchStat, 0x48 bytes (reflection 0x721FF0):
//   +0x00 m_pid (8 bytes, unused by the page)
//   +0x08 m_category FYPlayerMatchStatisticsCategory (reflection 0x7227A0):
//         +0x08 vptr (FTableRowBase), +0x10 m_ID (byte), +0x18 m_name (FText,
//         0x18), +0x30 m_priority (int)
//   +0x38 m_comparison (byte)  +0x3C m_stat (float)  +0x40 m_statDiffToAvg
//   (float)  +0x44 m_medal (byte)
// Enums by registration order (0x6911EE): comparison NONE 0 .. PureStat 5;
// category NONE 0, Assists 1, Kills 2, DoubleKills 3, DamageCausedDestroyer 4
// .. DamageCausedByAbilities 9, PowerUsage 10, Healing 11, ...
//
// Rows: the three categories the PRI holds -- Kills (+0x848), Assists
// (+0x858), "Damage with Modules" (DamageCausedByAbilities, +0x90C). Weapon
// damage is one total on the PRI while the game splits it per target class, so
// it is left out rather than split by guesswork. Labels are the English names
// in DN_PlayerMatchStatistics_DT. Comparison PureStat: the value alone (no
// average to compare against). The label FText is made by the engine's own
// Conv_StringToText body (thunk 0x1E63460 -> 0x19D21F0, FText* (FText* out,
// const FString* in)), so it is a real, ref-counted FText. The rows are
// engine-allocated and never freed (a few hundred bytes per match); the local
// copy in the PRI is the engine's own.
#define RVA_TEXT_FROM_STRING 0x19D21F0
typedef void *(__fastcall *tTextFromString)(void *outText, FStringMin *in);
#define STAT_COMPARISON_PURE 5

static int BuildPlayerMatchStats(uint8_t *pri, TArrayIntMin *out) {
  struct Row {
    uint8_t id;
    const wchar_t *label;
    float value;
  } rows[3];
  int n = 0;
  __try {
    rows[n++] = {2, L"Kills", (float)*(int32_t *)(pri + OFF_PRI_KILLS)};
    rows[n++] = {1, L"Assists", (float)*(int32_t *)(pri + OFF_PRI_ASSISTS)};
    rows[n++] = {9, L"Damage with Modules", *(float *)(pri + OFF_PRI_DAMAGE_ABILITIES)};
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
  tOperatorNew alloc = (tOperatorNew)(g_base + RVA_OPERATOR_NEW);
  uint8_t *arr = (uint8_t *)alloc((size_t)n * 0x48);
  if (!arr)
    return 0;
  memset(arr, 0, (size_t)n * 0x48);
  tTextFromString mkText = (tTextFromString)(g_base + RVA_TEXT_FROM_STRING);
  for (int i = 0; i < n; ++i) {
    uint8_t *e = arr + i * 0x48;
    e[0x10] = rows[i].id;
    int len = lstrlenW(rows[i].label) + 1;
    wchar_t *buf = (wchar_t *)alloc((size_t)len * sizeof(wchar_t));
    if (!buf)
      return 0;
    memcpy(buf, rows[i].label, (size_t)len * sizeof(wchar_t));
    FStringMin str = {buf, len, len};
    mkText(e + 0x18, &str);
    e[0x38] = STAT_COMPARISON_PURE;
    *(float *)(e + 0x3C) = rows[i].value;
  }
  out->data = (int32_t *)arr;
  out->num = n;
  out->max = n;
  return n;
}

static void SendTopPlayerMatchStats(uint8_t *pc, uint8_t *pri, const char *how) {
  __try {
    if (!IsReadable(pc, OFF_PC_NETCONNECTION + 8) || *(uint8_t **)(pc + OFF_PC_PLAYER_STATE) != pri ||
        !IsReadable(pri, 0x800)) {
      Logf("eom stats: controller %p / PRI %p is gone; not sent (%s)", pc, pri, how);
      return;
    }
    uint64_t name = *(uint64_t *)(g_base + RVA_FNAME_CLIENT_SET_TOP_PLAYER_MATCH_STATS);
    void *fn = FindFunctionSafe(pri, name);
    if (!fn) {
      Logf("eom stats: ClientSetTopPlayerMatchStats not found on %p; not sent", pri);
      return;
    }
    int rows = 0;
    TArrayIntMin parms = {nullptr, 0, 0}; // TArray<FYPlayerMatchStat>
    if (!SwitchOn("DN_HOST_EMPTY_EOM_STATS", "dn_host_empty_eom_stats.txt"))
      rows = BuildPlayerMatchStats(pri, &parms);
    void **vt = *(void ***)pri;
    ((tProcessEventVirt)vt[VT_PROCESS_EVENT / 8])(pri, fn, &parms);
    Logf("eom stats: sent ClientSetTopPlayerMatchStats (%d personal stat rows, %s) to "
         "controller %p PRI %p -- the client's SetupUIWidgets stage waits for it",
         rows, how, pc, pri);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("eom stats: EXCEPTION sending ClientSetTopPlayerMatchStats for %p", pc);
  }
}

// PvE end-of-match host crash (escort1, pveend3, escort3, escort4 2026-10-17):
// ~0.5 s after ClientStartEndOfMatchTransition the ?listen host builds its own
// PvE EOM team page. UYWidget_EndOfMatch_PVE_TeamPage::
// SetupFromTeamPlayerEntryData (0x469B70, this + const TArray<entry data>&,
// void) calls 0x5E0010 -> BP_CreateTeamPlayerEntry, which hits "Accessed None"
// on the host and returns null, then passes the null widget straight to
// 0x140ACB0 (call at 0x469BDE; the null test only comes at 0x469BED) --
// EXCEPTION_ACCESS_VIOLATION reading 0x0, stack probe RVA 0x140ACB0 <- 0x469BE3,
// r12 = its "didn't return a valid player entry widget" literal. Nobody looks
// at the host's UI, so the hook returns without building the page (the
// function's own early-out when entries already exist does the same).
// Off with DN_HOST_NO_EOM_TEAMPAGE_SKIP.
#define RVA_EOM_PVE_TEAMPAGE_SETUP 0x469B70
typedef void(__fastcall *tEomTeamPageSetup)(void *page, void *entries);
static tEomTeamPageSetup g_origEomTeamPageSetup = nullptr;
static void __fastcall HookEomTeamPageSetup(void *page, void *entries) {
  static int s_logged = 0;
  if (s_logged++ < 2)
    Logf("eom ui: skipped the host's PvE team page setup (%p)", page);
}

// The deferred half: the game's own transition, then the stats RPC.
static void SendEomTransition(void *orbitComp, uint8_t *pc, uint8_t *pri, const char *how) {
  bool ok = false;
  __try {
    ok = IsReadable(pc, OFF_PC_ORBIT_COMPONENT + 8) &&
         *(void **)(pc + OFF_PC_ORBIT_COMPONENT) == orbitComp;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  if (!ok) {
    Logf("eom transition: controller %p left before its end-of-match transition (%s)", pc, how);
    return;
  }
  g_origClientStartEomTransition(orbitComp);
  Logf("eom transition: ClientStartEndOfMatchTransition sent to controller %p (%s)", pc, how);
  SendTopPlayerMatchStats(pc, pri, how);
}

// Game thread, once a second (HookGameModeTimer).
static void FlushPendingEomStats() {
  DWORD now = GetTickCount();
  for (auto &p : g_pendingEom) {
    if (p.pc && (int32_t)(now - p.due) >= 0) {
      PendingEomTransition q = p;
      p.pc = nullptr;
      SendEomTransition(q.orbitComp, q.pc, q.pri, "deferred");
    }
  }
}

static void __fastcall HookClientStartEomTransition(void *orbitComp) {
  uint8_t *pc = nullptr, *pri = nullptr;
  __try {
    pc = *(uint8_t **)((uint8_t *)orbitComp + OFF_COMPONENT_OWNER);
    if (!IsReadable(pc, OFF_PC_ORBIT_COMPONENT + 8) ||
        *(void **)(pc + OFF_PC_ORBIT_COMPONENT) != orbitComp) {
      Logf("eom stats: orbit component %p has no matching controller; not sent",
           orbitComp);
      pc = nullptr;
    } else if (!*(void **)(pc + OFF_PC_NETCONNECTION)) {
      pc = nullptr; // the host's own local player; its flow is not what anyone sees
    } else {
      pri = *(uint8_t **)(pc + OFF_PC_PLAYER_STATE);
      if (!IsReadable(pri, 0x800)) {
        Logf("eom stats: controller %p has no player state; not sent", pc);
        pc = nullptr;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("eom stats: EXCEPTION reading the controller of %p", orbitComp);
    pc = nullptr;
  }
  bool noDefer = SwitchOn("DN_HOST_EOM_NO_DEFER", "dn_host_eom_no_defer.txt");
  if (!pc || noDefer) {
    // Local player, unknown controller, or the old flow: transition now.
    g_origClientStartEomTransition(orbitComp);
    if (pc)
      SendTopPlayerMatchStats(pc, pri, "immediate");
  }
  // Rewards (ReportMatchResult writes them) -- before the deferred transition.
  if (!SwitchOn("DN_HOST_NO_MATCH_RESULT", "dn_host_no_match_result.txt"))
    ReportMatchResult(orbitComp);
  else
    Logf("match result: OFF (dn_host_no_match_result.txt / DN_HOST_NO_MATCH_RESULT=1)");
  if (!pc || noDefer)
    return;
  for (auto &p : g_pendingEom) {
    if (!p.pc) {
      p = {orbitComp, pc, pri, GetTickCount() + EOM_TRANSITION_DELAY_MS};
      Logf("eom transition: controller %p held for %d ms so its rewards replicate "
           "before the client's end-of-match init stage gathers them",
           pc, EOM_TRANSITION_DELAY_MS);
      return;
    }
  }
  SendEomTransition(orbitComp, pc, pri, "queue full, immediate");
}

// Both of the switches below are opt-in separately from the loadout fix,
// because both change what players see.
// ---------------------------------------------------------------------------
// Scoring table (issue #67): every score was 0.
//
// The host scores every event (kill, assist, ...) from a table it copies out of
// the YMmogbrain subsystem: UYScoringEventManager::InitializeData (0x423610) ->
// 0x423590 -> 0x423450 copies subsystem +0x43F8/+0x4408/+0x4460, but only when
// the "present" flag at +0x4470 is set. The one writer of that block is the
// reply parser 0x2A75740, which a logged-in game runs when mmogbrain answers
// its scoring request (dispatcher 0x2A2574D, slot +0x3680). The host never logs
// in, so the flag stayed 0 and every event was worth 0 -- kills, deaths and
// damage were counted, the SCORE was not. (Verified from the exe 2026-09-28.)
//
// So, before InitializeData runs, this fetches the scoring document from
// mmogbrain (GET /battle/scoring, loopback) and feeds it through the client's
// OWN decoder and parser -- supplying the data a logged-in game would have had,
// not writing a score:
//
//   1. A one-slot response holder, laid out like the client's (slot array at
//      +0x27F0, 0x88 bytes each): key +0x00, state +0x14, chunk list +0x18
//      (each chunk = next pointer + 0x7FF8 data bytes), length +0x7C. The
//      reader's Read (0x2A7BE30 -> 0x2A7BFB0) only walks that chunk list.
//   2. A reader initialised by 0x2A57080 and pointed at it the way 0x2A65850
//      does after matching a request id: +0x38 holder, +0x40 slot, +0x08 = 0.
//   3. 0x2A3E450(reader, root) decodes the document into the node tree, as
//      the dispatcher does for every reply (0x2A23539).
//   4. 0x2A75740(subsystem + 0x43F8, root) parses ScoringTable and
//      ScoringParamsTable and sets the flag (+0x78 of that block = +0x4470).
//
// The subsystem is found exactly as 0x423450 finds it: FName("YMmogbrain")
// (0xC9CF20) -> FindModule (0xCA9060) -> vtable +0x48.
// ---------------------------------------------------------------------------

#define RVA_SCORING_INIT_DATA 0x423610 // UYScoringEventManager::InitializeData
#define RVA_SCORING_COPY 0x423450      // copies the subsystem block; arg 2 = mode name
#define RVA_FNAME_FROM_ANSI 0xC9CF20
#define RVA_FIND_MODULE 0xCA9060
#define RVA_MMOG_READER_INIT 0x2A57080
#define RVA_MMOG_DECODE 0x2A3E450
#define RVA_SCORING_PARSE 0x2A75740
#define OFF_MMOG_SCORING 0x43F8
#define OFF_MMOG_SCORING_PRESENT 0x4470
#define OFF_MMOG_SLOTS 0x27F0
#define MMOG_SLOT_SIZE 0x88
#define MMOG_CHUNK_DATA 0x7FF8

typedef void *(__fastcall *tFNameFromAnsi)(void *out, const char *name, int findType);
typedef void *(__fastcall *tFindModule)(uint64_t name);
typedef void *(__fastcall *tModuleGetClient)(void *module);
typedef void *(__fastcall *tMmogReaderInit)(void *reader);
typedef void(__fastcall *tMmogDecode)(void *reader, void *root);
typedef void(__fastcall *tScoringParse)(void *dest, void *root);
typedef void(__fastcall *tScoringInitData)(void *mgr);
typedef void(__fastcall *tScoringCopy)(void *table, void *modeName);

static tScoringInitData g_origScoringInitData = nullptr;
static tScoringCopy g_origScoringCopy = nullptr;

static uint8_t *MmogClient() {
  uint64_t name[2] = {};
  void *fn = ((tFNameFromAnsi)(g_base + RVA_FNAME_FROM_ANSI))(name, "YMmogbrain", 1);
  if (!fn)
    return nullptr;
  void *module = ((tFindModule)(g_base + RVA_FIND_MODULE))(*(uint64_t *)fn);
  if (!module || !IsReadable(module, 8))
    return nullptr;
  void **vt = *(void ***)module;
  if (!IsReadable(vt, 0x50))
    return nullptr;
  return (uint8_t *)((tModuleGetClient)vt[0x48 / 8])(module);
}

// Runs the document through the client's decoder and scoring parser. Returns
// the number of rows the parser stored, or -1.
static int ParseScoringDocument(uint8_t *client, const uint8_t *doc, int len) {
  // Response holder with one slot. Deliberately never freed: the parser copies
  // what it needs, and this runs once per host process.
  uint8_t *holder = (uint8_t *)calloc(1, OFF_MMOG_SLOTS + MMOG_SLOT_SIZE);
  if (!holder)
    return -1;
  uint8_t *slot = holder + OFF_MMOG_SLOTS;
  *(int32_t *)(slot + 0x14) = 6; // complete
  void **link = (void **)(slot + 0x18);
  for (int off = 0; off < len; off += MMOG_CHUNK_DATA) {
    int n = len - off < MMOG_CHUNK_DATA ? len - off : MMOG_CHUNK_DATA;
    uint8_t *chunk = (uint8_t *)calloc(1, 8 + MMOG_CHUNK_DATA);
    if (!chunk)
      return -1;
    memcpy(chunk + 8, doc + off, n);
    *link = chunk;
    link = (void **)chunk;
  }
  *(int32_t *)(slot + 0x7C) = len;

  uint8_t *reader = (uint8_t *)calloc(1, 0x100);
  uint8_t *root = (uint8_t *)calloc(1, 0x100);
  if (!reader || !root)
    return -1;
  ((tMmogReaderInit)(g_base + RVA_MMOG_READER_INIT))(reader);
  *(void **)(reader + 0x38) = holder;
  *(int32_t *)(reader + 0x40) = 0;
  *(int32_t *)(reader + 0x08) = 0;

  __try {
    ((tMmogDecode)(g_base + RVA_MMOG_DECODE))(reader, root);
    Logf("scoring: decoded %d bytes -> root has %d fields", len, *(int32_t *)(root + 0x20));
    ((tScoringParse)(g_base + RVA_SCORING_PARSE))(client + OFF_MMOG_SCORING, root);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("scoring: EXCEPTION 0x%08X while decoding/parsing the table", GetExceptionCode());
    return -1;
  }
  return *(int32_t *)(client + OFF_MMOG_SCORING + 8);
}

static void FeedScoringTable() {
  uint8_t *client = nullptr;
  __try {
    client = MmogClient();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    client = nullptr;
  }
  if (!client || !IsReadable(client + OFF_MMOG_SCORING, 0x80)) {
    Logf("scoring: YMmogbrain subsystem not found; scores stay 0");
    return;
  }
  if (client[OFF_MMOG_SCORING_PRESENT]) {
    Logf("scoring: table already present (%d rows)", *(int32_t *)(client + OFF_MMOG_SCORING + 8));
    return;
  }
  static char buf[256 * 1024];
  const char *body;
  int n;
  if (!HttpGetLoopbackRaw("/battle/scoring", buf, (int)sizeof(buf), &body, &n) || n <= 6) {
    Logf("scoring: GET /battle/scoring failed; scores stay 0");
    return;
  }
  int rows = ParseScoringDocument(client, (const uint8_t *)body, n);
  Logf("scoring: table %s -- %d rows, present flag %d",
       rows > 0 && client[OFF_MMOG_SCORING_PRESENT] ? "LOADED" : "NOT loaded", rows,
       client[OFF_MMOG_SCORING_PRESENT]);
}

static void __fastcall HookScoringInitData(void *mgr) {
  FeedScoringTable();
  g_origScoringInitData(mgr);
}

// Logs the mode name the table is filtered by: a row counts only when its
// GameModes lists it (0x424190). mmogbrain lists every alias; this line shows
// which one the host actually uses.
static void __fastcall HookScoringCopy(void *table, void *modeName) {
  const wchar_t *mode = L"";
  __try {
    if (modeName && *(int32_t *)((uint8_t *)modeName + 8) > 0)
      mode = *(const wchar_t **)modeName;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    mode = L"?";
  }
  // Team Elimination has NO mode name, and with the table loaded both TE
  // matches crashed the host a few minutes in (11:40 and 16:02 on 2026-09-28:
  // EXCEPTION_ACCESS_VIOLATION reading 0xfffffff8, the same in both); the two
  // TE matches before the table existed ended cleanly. An empty mode name
  // makes the filter (0x424190) take EVERY row, so TE ran the table with no
  // mode context. GUESS: some reward/ribbon path looks the mode up and reads
  // through a missing entry (null - 8). The crashing function is not
  // identified (no minidump: this DLL stubs WER). So an empty mode builds no
  // table -- TE scores 0, as before, and does not crash.
  //
  // CHANGED 2026-09-29: hosts now report TE's mode as "TE" (the ?game=TE
  // match URL), so the empty-name guard stopped firing and the table was built
  // for TE again -- and ALL 7 TE matches since 2026-09-28 crashed the same way
  // (7 of 7 AVs reading 0xfffffff8 were ?game=TE; no TDM match had one), 3-5
  // min in: about when TE's first ROUND ends. GUESS: a round end scores an
  // event the table leaves out on purpose (Winner/MatchEnd, excluded because
  // they overflowed the host at EndMatch) and reads through the missing row.
  // Until the crash handler below names the function, TE gets no table.
  bool isTE = mode[0] == L'T' && mode[1] == L'E' && mode[2] == 0;
  if (!mode[0] || isTE) {
    Logf("scoring: mode \"%ls\" is Team Elimination -- table not built, scores stay 0 "
         "in this mode (every TE match crashed the host with the table)", mode);
    return;
  }
  Logf("scoring: building the match table for mode \"%ls\"", mode);
  g_origScoringCopy(table, modeName);
  LoadRibbonRules(mode);
}

static DWORD WINAPI PostLoginInstallThread(LPVOID);

// SwitchOn reports whether an env var is "1" or a marker file sits beside the
// executable. Two ways because the spawner inherits its environment but a file
// survives however the operator starts the service.
static bool SwitchOn(const char *envName, const char *markerFile) {
  char buf[8];
  DWORD n = GetEnvironmentVariableA(envName, buf, sizeof(buf));
  if (n == 1 && buf[0] == '1')
    return true;

  char path[MAX_PATH];
  if (!GetModuleFileNameA(NULL, path, MAX_PATH))
    return false;
  char *slash = strrchr(path, '\\');
  if (!slash)
    return false;
  strcpy_s(slash + 1, sizeof(path) - (slash + 1 - path), markerFile);
  return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// The ServerRestartPlayer bypass: spawns joining players straight into the
// arena, at the cost of removing ship selection.
static bool PostLoginSpawnEnabled() {
  return SwitchOn("DN_HOST_POSTLOGIN_SPAWN", "dn_host_postlogin.txt");
}

// The fleet tier: DISABLED, see the DISPROVED note above OFF_HIGHEST_FLEET.
// Still read, only so a leftover marker file is reported instead of ignored.
static bool FleetTierEnabled() {
  return SwitchOn("DN_HOST_FLEET_TIER", "dn_host_fleet_tier.txt");
}

// Dedicated net mode (HookGetNetMode). Opt-in like every host mod, so a match
// can still be diagnosed with it off.
static bool DedicatedNetModeEnabled() {
  return SwitchOn("DN_HOST_DEDICATED", "dn_host_dedicated.txt");
}

// The player's own fit via mmogbrain (RegisterPlayerLoadout).
static bool PlayerLoadoutsEnabled() {
  return SwitchOn("DN_HOST_PLAYER_LOADOUTS", "dn_host_player_loadouts.txt");
}

// AI in the proving ground (HookGameModeTimer).
static bool BootcampAIEnabled() {
  return SwitchOn("DN_HOST_BC_AI", "dn_host_bc_ai.txt");
}

// Player ship physics without the local-camera cull (HookVehicleViewCull).
static bool ShipPhysicsEnabled() {
  return SwitchOn("DN_HOST_SHIP_PHYSICS", "dn_host_ship_physics.txt");
}

// Installs one single-argument hook and reports it either way.
// ---------------------------------------------------------------------------
// Havoc respawn ship (UYLoadoutManager::ActivateLoadout, 0x336C90)
//
// 2026-10-14 playtest: picking another ship on the Havoc respawn screen showed
// its icon but respawned the old one; the host logged "ActivateLoadout |
// Loadout [Default__VH_HVC_UTI_AssaultM_Loadout_T2_BP_C] is not valid" once
// per attempt. ActivateLoadout(mgr, loadout, bool r8b) (.pdata ENTRY
// 0x336C90-0x336D9A) bails unless 0x33C680(mgr, loadout) is true; on success
// it stores the loadout at mgr+0x208 (0x336DBB).
//
// 0x33C680, read 2026-10-14:
//   1. scan every manager group (+0x108 array, count +0x110, 0x30-byte
//      entries: TArray of loadout ptrs, type byte +0x28) for the loadout;
//   2. with no filter (mgr+0x200 null): found -> the type-4 path, NOT found
//      -> 0x33CA66 -> TRUE;
//   3. with a filter: found in a type-2 group and filter+0x28 == 0 -> FALSE;
//      otherwise switch on loadout+0xD8 (ship class, 1..15, table 0x33CA90):
//      each case needs two filter bytes in +0x29..+0x3C (family + class
//      allow flags) -> true only if both set; out of range -> TRUE.
// The active filter in Havoc is VH_LoadoutFilter_Havoc_DA, whose cooked data
// sets none of the m_allow<Family> flags (only m_allowCustomLoadouts and
// m_specialLoadouts: exactly the five VH_HVC_* loadouts) -- so every Havoc
// ship fails step 3. The respawn screen offers those five, so the filter's own
// data says they are allowed; whatever on the original server accepted
// special loadouts is not in this exe.
//
// RETRACTED 2026-10-14 20:50: the first version registered the loadout with
// AddLoadout(mgr, cdo, 2). It could not work (step 3: a type-2 hit with
// filter+0x28 == 0 is a hard FALSE, and an unregistered one still hits the
// class switch), and it never logged in the 20:46 match either.
//
// RETRACTED 2026-10-14 21:05 (operator match 20:56, this hook's own log):
// the filter-clearing version below never fired. At respawn the filter is
// ALREADY null ("ClearLoadoutFilter | In Havoc we do not want to reactivate
// the previous loadout ID here!" at match start), and the object is a native
// YShipLoadout NAMED Default__VH_HVC_*_C -- ClassNameOf gave "YShipLoadout",
// so the VH_HVC_ class test never matched (that is also why 94d7498 logged
// nothing). The real failure is step 2's other arm, read in full:
//   no filter + found in a type-4 group (0x33C840 collects type 4,
//   0x33C8C0 hit -> 0x33C825 -> FALSE).
// The five Havoc ships are in a type-4 group (LoadLoadoutFilterLoadouts,
// "Loading loadout filter VH_LoadoutFilter_Havoc_DA"). With the filter still
// set at the first pick (20:56:50) the same object passed: gate 1, class 14.
// So once the filter is cleared no Havoc ship can be activated -- the
// original server must have restored the filter or bypassed this gate on
// respawn; neither path is in this exe.
//
// Now: when the gate fails for an object named Default__VH_HVC_* (one of the
// filter's own m_specialLoadouts), the gate 0x33C680 (.pdata ENTRY
// 0x33C680-0x33C6AF) is hooked to answer TRUE for exactly that loadout while
// the original ActivateLoadout runs. Logs every call (first 40).
// ---------------------------------------------------------------------------
#define RVA_ACTIVATE_LOADOUT 0x336C90
#define RVA_LOADOUT_IS_REGISTERED 0x33C680
#define OFF_LOADOUT_MGR_FILTER 0x200
#define OFF_LOADOUT_MGR_ACTIVE 0x208
typedef void(__fastcall *tActivateLoadout)(void *mgr, void *loadout, uint8_t flag);
static tActivateLoadout g_origActivateLoadout = nullptr;

typedef bool(__fastcall *tLoadoutGate)(void *mgr, void *loadout);
static tLoadoutGate g_origLoadoutGate = nullptr;
static void *volatile g_forceValidLoadout = nullptr;

// Pawn class a ship loadout spawns: loadout vt+0x208, the call
// SpawnDefaultPawnAtTransform makes at 0x3825A6. Getter, no side effects seen.
static const char *LoadoutPawnClassName(void *loadout) {
  if (!loadout || !IsReadable(loadout, sizeof(UObjectMin)))
    return "?";
  void **vt = *(void ***)loadout;
  if (!vt || !IsReadable(vt + 0x208 / 8, 8))
    return "?";
  UObjectMin *cls = ((UObjectMin * (__fastcall *)(void *)) vt[0x208 / 8])(loadout);
  if (!cls || !IsReadable(cls, sizeof(UObjectMin)))
    return "null";
  const char *n = NameText(cls->Name);
  return n ? n : "?";
}

static bool __fastcall HookLoadoutGate(void *mgr, void *loadout) {
  if (loadout && loadout == g_forceValidLoadout)
    return true;
  return g_origLoadoutGate(mgr, loadout);
}

static void __fastcall HookActivateLoadout(void *mgr, void *loadout, uint8_t flag) {
  static int s_logged = 0;
  uint8_t *m = (uint8_t *)mgr;
  if (!m || !loadout || !IsReadable(m + OFF_LOADOUT_MGR_ACTIVE, 8) || !IsReadable(loadout, sizeof(UObjectMin))) {
    g_origActivateLoadout(mgr, loadout, flag);
    return;
  }
  bool valid = g_origLoadoutGate(mgr, loadout);
  void *filter = *(void **)(m + OFF_LOADOUT_MGR_FILTER);
  const char *name = NameText(((UObjectMin *)loadout)->Name);
  const char *fname = filter && IsReadable(filter, sizeof(UObjectMin)) ? NameText(((UObjectMin *)filter)->Name) : nullptr;
  // Was: !valid && name starts "Default__VH_HVC_". RETRACTED 2026-10-14 21:15
  // (headless 21:08 + operator 21:02 logs): NameText(loadout->Name) prints
  // "YShipLoadout" for every pick, so the prefix test could never match and
  // 36c9833 was a no-op. Keyed on the failing arm itself instead: gate FALSE
  // with no filter (Havoc cleared it at match start). This hook is installed
  // only under HostIsHavoc, where the respawn screen offers just the five
  // VH_HVC_* ships the filter's m_specialLoadouts lists.
  // GUESS: no non-Havoc-ship ActivateLoadout reaches here with filter null.
  bool force = !valid && !filter;
  if (force)
    g_forceValidLoadout = loadout;
  g_origActivateLoadout(mgr, loadout, flag);
  if (force)
    g_forceValidLoadout = nullptr;
  if (s_logged < 40) {
    ++s_logged;
    void *active = *(void **)(m + OFF_LOADOUT_MGR_ACTIVE);
    const char *an = active && IsReadable(active, sizeof(UObjectMin)) ? NameText(((UObjectMin *)active)->Name) : nullptr;
    Logf("loadout: ActivateLoadout(%p, %s %p -> pawn %s, %u) gate %d, filter %s, class byte %u%s -> active %p", mgr,
         name ? name : "?", loadout, LoadoutPawnClassName(loadout), flag, valid,
         fname ? fname : (filter ? "?" : "none"), ((uint8_t *)loadout)[0xD8],
         force ? ", gate forced for Havoc ship" : "", active);
    (void)an;
  }
}

// AYGameMode::SpawnDefaultPawnAtTransform 0x382530(gm, ctrl, loadout r8,
// transform r9, 5th stack arg -- caller 0x3827D0 stores rsi at [rsp+0x20]
// before the call; a 4-arg detour crashed the first spawn, headless 10-16
// 20:28): pawn class = loadout vt+0x208 (0x3825A6); logs "Active
// Loadout not found" when r8 is null. Diagnostic only, for the Havoc respawn
// ship switch (operator 10-15: forced ActivateLoadout, pawn still old ship).
// Logs which loadout object reaches the spawn and which class it yields, so it
// can be compared with the ActivateLoadout line's pointer.
typedef void *(__fastcall *tSpawnAtTransform)(void *gm, void *ctrl, void *loadout, void *xf, void *a5);
static tSpawnAtTransform g_origSpawnAtTransform = nullptr;

static void *__fastcall HookSpawnAtTransform(void *gm, void *ctrl, void *loadout, void *xf, void *a5) {
  void *pawn = g_origSpawnAtTransform(gm, ctrl, loadout, xf, a5);
  static int s_logged = 0;
  if (s_logged < 40) {
    ++s_logged;
    const char *pc = pawn && IsReadable(pawn, sizeof(UObjectMin)) && ((UObjectMin *)pawn)->Class
                         ? NameText(((UObjectMin *)pawn)->Class->Name) : nullptr;
    Logf("loadout: SpawnDefaultPawnAtTransform(ctrl %p, loadout %p -> pawn class %s) spawned %p %s", ctrl, loadout,
         LoadoutPawnClassName(loadout), pawn, pc ? pc : "?");
  }
  return pawn;
}

// Virtual 0x9D0 (0x3678F0, 0x3678F0-0x367A34): copies the team size gm+0x97C
// into both AI targets (+0x980/+0x984) when it is > 0, then fills the teams
// (0x381FA0, if +0x962). The fill and SetTeamSizeAI (0x381550) first trim a
// team over its target -- 0x36AC10 destroys the team's FIRST live entries in
// list order -- then spawn dead entries in list order up to the target.
// With only /spawn'd bots that put a full 8 a side on the map for a moment and
// bot balance then trimmed ours: gamebot-boss10 (TDM 2026-10-20) lost entry
// 21's SupportMediumT5 and kept a fill bot's ScoutMediumT5. A cap in the 1 s
// timer never fired (gamebot-boss11): the copy and the fill are one call. So
// for that call +0x97C reads 0 (the copy is skipped; the fill takes the same
// branch, +0x97C >= 0) and the targets are ours, and the first dead entry of
// the team -- the one SpawnGameBot took -- is the one spawned.
typedef void(__fastcall *tGmSetupAI)(void *gm, uint64_t arg);
static tGmSetupAI g_origGmSetupAI = nullptr;
static void __fastcall HookGmSetupAI(void *gameMode, uint64_t arg); // after SpawnGameBot
static void __fastcall HookSceneReplace(void *scene, int id, void *behaviours, uint8_t keepTree); // /squad
static tSceneReplaceBehaviours g_origSceneReplace = nullptr;
typedef void(__fastcall *tRegisterProjectileFn)(void *mgr, void *proj);
static tRegisterProjectileFn g_origRegisterProjectile;

static void InstallSwitchedHook(const char *what, uintptr_t rva, void *detour,
                                void **orig) {
  void *target = (void *)(g_base + rva);
  if (MH_CreateHook(target, detour, (LPVOID *)orig) != MH_OK ||
      MH_EnableHook(target) != MH_OK)
    Logf("%s: FAILED to hook RVA 0x%X (%p)", what, (unsigned)rva, target);
  else
    Logf("installed: %s hooked at RVA 0x%X (%p)", what, (unsigned)rva, target);
}

// The install runs on its own thread and WAITS, because GObjects is not
// populated when this DLL is attached.
//
// Measured: the first version resolved at DLL_PROCESS_ATTACH and logged
// "K2_PostLogin not found in GObjects" on a live host, while the loadout half of
// this file worked -- because that half resolves lazily, on the first
// FindLoadoutByID miss, by which time the engine is up. dread-sdk sleeps 20
// seconds before its server callbacks for the same reason.
//
// Polling rather than a fixed sleep, so a fast host is not held back and a slow
// one is not cut off. The window is generous: the hook only has to be in place
// before the first player joins, which is many seconds after map load.
#define POSTLOGIN_WAIT_MS 90000
#define POSTLOGIN_POLL_MS 500

static void *WaitForUObjectByName(const char *want, const char **outerOut,
                                  int *waitedMsOut) {
  int waited = 0;
  for (;;) {
    void *found = FindUObjectByName(want, outerOut);
    if (found) {
      if (waitedMsOut)
        *waitedMsOut = waited;
      return found;
    }
    if (waited >= POSTLOGIN_WAIT_MS)
      break;
    Sleep(POSTLOGIN_POLL_MS);
    waited += POSTLOGIN_POLL_MS;
  }
  if (waitedMsOut)
    *waitedMsOut = waited;
  return nullptr;
}

static void InstallPostLoginHook() {
  g_spawnArmed = PostLoginSpawnEnabled();
  // g_fleetTierArmed is already set in Startup, which must know it before this
  // thread exists so the teleport hook can install without racing it.

  // The ProcessEvent hook carries both features, so it installs if either is
  // wanted.
  g_peTraceArmed = HostIsHavoc() && !SwitchOn("DN_HOST_NO_PE_TRACE", "dn_host_no_pe_trace.txt");
  // FrameTick (terrain fine pass, viewer projectile scan) runs from this hook
  // too. Without this, only Havoc hosts (the trace) ever had it: every other
  // map logged "frame: 0 ticks" for the whole match (2026-10-17).
  bool frameHook = !SwitchOn("DN_HOST_NO_FRAME_TICK", "dn_host_no_frame_tick.txt");
  if (!g_spawnArmed && !g_fleetTierArmed && !g_peTraceArmed && !frameHook) {
    Logf("post-login hook is OFF. Enable the fleet tier "
         "(dn_host_fleet_tier.txt or DN_HOST_FLEET_TIER=1) to let the normal "
         "orbit flow finish, and/or the spawn bypass (dn_host_postlogin.txt or "
         "DN_HOST_POSTLOGIN_SPAWN=1) to skip orbit entirely.");
    return;
  }
  Logf("post-login hook: fleet tier %s, spawn bypass %s, frame tick %s",
       g_fleetTierArmed ? "ON" : "off", g_spawnArmed ? "ON" : "off", frameHook ? "ON" : "off");

  const char *outer = nullptr;
  int waited = 0;
  g_fnK2PostLogin = FindUObjectByName("PostLogin", &outer);
  if (!g_fnK2PostLogin)
    g_fnK2PostLogin = WaitForUObjectByName("PostLogin", &outer, &waited);
  if (!g_fnK2PostLogin) {
    FUObjectArrayMin *arr = GObjects();
    Logf("post-login: no UFunction containing \"PostLogin\" after %d ms "
         "(GObjects reports %d objects). Not hooking.",
         waited,
         IsReadable(arr, sizeof(*arr)) ? arr->NumElements : -1);
    return;
  }
  if (waited)
    Logf("post-login: waited %d ms for GObjects to carry PostLogin", waited);
  Logf("post-login: K2_PostLogin found at %p (outer %s)", g_fnK2PostLogin,
       outer ? outer : "<unknown>");

  // Only the spawn bypass needs these two. The fleet tier writes a byte on the
  // PlayerState and calls nothing, so it must not be blocked by their absence.
  if (g_spawnArmed) {
    g_fnGetLoadoutManager = WaitForUObjectByName("GetLoadoutManager", &outer, nullptr);
    g_fnServerRestartPlayer = WaitForUObjectByName("ServerRestartPlayer", &outer, nullptr);
    if (!g_fnGetLoadoutManager || !g_fnServerRestartPlayer) {
      Logf("post-login: GetLoadoutManager=%p ServerRestartPlayer=%p -- spawn "
           "bypass disabled, continuing with fleet tier only.",
           g_fnGetLoadoutManager, g_fnServerRestartPlayer);
      g_spawnArmed = false;
      if (!g_fleetTierArmed && !g_peTraceArmed && !frameHook)
        return;
    }
  }

  // The later tier trigger points. All optional: a missing one costs a trigger,
  // not the feature.
  g_fnServerReadyForJoining = FindUObjectByName("ServerReadyForJoining", &outer);
  g_fnServerSpawnNearActor = FindUObjectByName("ServerSpawnNearActor", &outer);
  g_fnServerPlayerReadyUp =
      FindUObjectByName("ServerPlayerReadyUpForMatch", &outer);
  Logf("post-login: tier triggers -- ServerReadyForJoining=%p "
       "ServerSpawnNearActor=%p ServerPlayerReadyUpForMatch=%p",
       g_fnServerReadyForJoining, g_fnServerSpawnNearActor,
       g_fnServerPlayerReadyUp);

  // ProcessEvent is virtual on UObject, so any UObject's vtable has it. The
  // UFunction just resolved is one.
  if (!IsReadable(g_fnK2PostLogin, sizeof(void *))) {
    Logf("post-login: K2_PostLogin object is not readable. Not hooking.");
    return;
  }
  void **vtable = *(void ***)g_fnK2PostLogin;
  if (!IsReadable(vtable, (VF_PROCESS_EVENT + 1) * sizeof(void *))) {
    Logf("post-login: vtable is not readable to index 0x%X. Not hooking.",
         VF_PROCESS_EVENT);
    return;
  }
  void *processEvent = vtable[VF_PROCESS_EVENT];

  if (MH_CreateHook((LPVOID)processEvent, (LPVOID)&HookProcessEvent,
                    (LPVOID *)&g_origProcessEvent) != MH_OK ||
      MH_EnableHook(processEvent) != MH_OK) {
    Logf("post-login: failed to hook ProcessEvent at %p. Not hooking.",
         processEvent);
    return;
  }

  // The trace alone must not arm the fleet-tier / spawn paths.
  g_postLoginArmed = g_spawnArmed || g_fleetTierArmed;
  if (g_peTraceArmed)
    Logf("pe trace: ProcessEvent hooked at %p for the Havoc event trace", processEvent);
  if (g_spawnArmed)
    Logf("post-login: ProcessEvent hooked at %p; joining players will spawn "
         "directly as %s, bypassing the orbit flow.",
         processEvent, kPrecastLabels[g_postLoginLoadoutIndex]);
  else
    Logf("post-login: ProcessEvent hooked at %p; players keep ship selection "
         "and the normal orbit flow.",
         processEvent);
}

static DWORD WINAPI PostLoginInstallThread(LPVOID) {
  InstallPostLoginHook();
  return 0;
}

static bool IsEnabled() {
  char buf[8];
  DWORD n = GetEnvironmentVariableA("DN_SERVER_LOADOUT", buf, sizeof(buf));
  if (n == 1 && buf[0] == '1')
    return true;

  char path[MAX_PATH];
  if (!GetModuleFileNameA(NULL, path, MAX_PATH))
    return false;
  char *slash = strrchr(path, '\\');
  if (!slash)
    return false;
  strcpy_s(slash + 1, sizeof(path) - (slash + 1 - path),
           "dn_server_loadout.txt");
  return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static DWORD WINAPI Startup(LPVOID) {
  if (!IsBattleServer())
    return 0; // a client. Do nothing at all, and say nothing.

  LogOpen();

  if (!IsEnabled()) {
    Logf("battle server detected but the fix is off "
         "(create dn_server_loadout.txt beside the executable, or set "
         "DN_SERVER_LOADOUT=1, to enable). Standing down.");
    return 0;
  }

  g_base = (uintptr_t)GetModuleHandleW(L"DreadGame-Win64-Shipping.exe");
  if (!g_base) {
    Logf("could not find DreadGame-Win64-Shipping.exe in this process. "
         "Standing down.");
    return 0;
  }
  Logf("battle server detected and enabled. module base 0x%llX",
       (unsigned long long)g_base);
  InstallHostCrashHandler();

  if (MH_Initialize() != MH_OK) {
    Logf("MH_Initialize failed. Standing down.");
    return 0;
  }

  void *target = (void *)(g_base + RVA_FIND_LOADOUT_BY_ID);
  if (MH_CreateHook((LPVOID)target, (LPVOID)&HookFindLoadoutByID,
                    (LPVOID *)&g_origFindLoadoutByID) != MH_OK ||
      MH_EnableHook(target) != MH_OK) {
    Logf("failed to hook FindLoadoutByID at RVA 0x%X. Standing down.",
         RVA_FIND_LOADOUT_BY_ID);
    return 0;
  }

  Logf("installed: FindLoadoutByID hooked at RVA 0x%X (%p). Waiting for a "
       "loadout lookup.",
       RVA_FIND_LOADOUT_BY_ID, target);

  // The fleet tier write is disabled for good (see OFF_HIGHEST_FLEET). Say so
  // if an old marker file is still asking for it.
  g_fleetTierArmed = false;
  if (FleetTierEnabled())
    Logf("fleet tier: dn_host_fleet_tier.txt / DN_HOST_FLEET_TIER is set but "
         "IGNORED -- it forced the pawn's orbit state. Use dn_host_dedicated.txt.");

  // Dedicated net mode. Installed here, before the map loads, so every
  // GetNetMode the orbit code asks is answered the same way.
  if (DedicatedNetModeEnabled()) {
    void *nm = (void *)(g_base + RVA_NETDRIVER_GET_NET_MODE);
    if (MH_CreateHook((LPVOID)nm, (LPVOID)&HookGetNetMode, (LPVOID *)&g_origGetNetMode) != MH_OK ||
        MH_EnableHook(nm) != MH_OK) {
      Logf("net mode: FAILED to hook UNetDriver::GetNetMode at RVA 0x%X (%p). "
           "The host stays a listen server and players will stay in orbit.",
           RVA_NETDRIVER_GET_NET_MODE, nm);
    } else {
      Logf("installed: UNetDriver::GetNetMode hooked at RVA 0x%X (%p). A server "
           "net driver now reports DEDICATED.",
           RVA_NETDRIVER_GET_NET_MODE, nm);
    }
  } else {
    Logf("net mode: dedicated OFF (create dn_host_dedicated.txt beside the "
         "executable, or set DN_HOST_DEDICATED=1). The host stays a listen "
         "server; players will not leave orbit.");
  }

  // Not switchable: without it a host dies whenever restrictions change.
  InstallSwitchedHook("restrictions (ClientSetPlayerRestrictions_Implementation)",
                      RVA_CLIENT_SET_PLAYER_RESTRICTIONS_IMPL,
                      (void *)&HookClientSetPlayerRestrictions,
                      (void **)&g_origClientSetPlayerRestrictions);
  InstallSwitchedHook("restrictions (ClientSetPlayerRestrictions RPC, local controller)",
                      RVA_CLIENT_SET_PLAYER_RESTRICTIONS_RPC,
                      (void *)&HookClientSetPlayerRestrictionsRPC,
                      (void **)&g_origClientSetPlayerRestrictionsRPC);

  InstallSwitchedHook("respawn filter (ClientApplyRespawnFilter RPC, re-entrancy guard)",
                      RVA_CLIENT_APPLY_RESPAWN_FILTER_RPC,
                      (void *)&HookClientApplyRespawnFilter,
                      (void **)&g_origClientApplyRespawnFilter);

  // On by default: without it every score is 0. Opt out to diagnose.
  if (!SwitchOn("DN_HOST_NO_SCORING", "dn_host_no_scoring.txt")) {
    InstallSwitchedHook("scoring table (UYScoringEventManager::InitializeData)",
                        RVA_SCORING_INIT_DATA, (void *)&HookScoringInitData,
                        (void **)&g_origScoringInitData);
    InstallSwitchedHook("scoring mode log (scoring table copy)", RVA_SCORING_COPY,
                        (void *)&HookScoringCopy, (void **)&g_origScoringCopy);
  } else
    Logf("scoring: OFF (dn_host_no_scoring.txt / DN_HOST_NO_SCORING=1). Every "
         "score stays 0.");

  if (!SwitchOn("DN_HOST_NO_EOM_TEAMPAGE_SKIP", "dn_host_no_eom_teampage_skip.txt"))
    InstallSwitchedHook("eom ui (host PvE team page setup skipped)", RVA_EOM_PVE_TEAMPAGE_SETUP,
                        (void *)&HookEomTeamPageSetup, (void **)&g_origEomTeamPageSetup);
  if (!SwitchOn("DN_HOST_NO_EOM_STATS", "dn_host_no_eom_stats.txt"))
    InstallSwitchedHook("eom stats (ClientStartEndOfMatchTransition)",
                        RVA_CLIENT_START_EOM_TRANSITION,
                        (void *)&HookClientStartEomTransition,
                        (void **)&g_origClientStartEomTransition);
  else
    Logf("eom stats: OFF (dn_host_no_eom_stats.txt / DN_HOST_NO_EOM_STATS=1). "
         "Clients will stop on a black screen at SetupUIWidgets.");

  // The game-mode timer carries the proving-ground bots AND the per-second
  // team/name/bot-balance pass (PlayersTick), so it installs if either is on.
  g_bcAIArmed = BootcampAIEnabled();
  g_botsBCOnly = SwitchOn("DN_HOST_BOTS_BC_ONLY", "dn_host_bots_bc_only.txt");
  g_teamSyncArmed = !SwitchOn("DN_HOST_NO_TEAM_SYNC", "dn_host_no_team_sync.txt");
  if (!g_bcAIArmed)
    Logf("bc ai: OFF (create dn_host_bc_ai.txt beside the executable, or set "
         "DN_HOST_BC_AI=1). The proving ground has no bots.");
  Logf("team sync / names / bot balance: %s", g_teamSyncArmed ? "ON (game-mode timer)"
       : "OFF (dn_host_no_team_sync.txt / DN_HOST_NO_TEAM_SYNC=1)");
  if (g_bcAIArmed || g_teamSyncArmed)
    InstallSwitchedHook("game-mode timer (bc ai, teams, names, bot balance)",
                        RVA_GAMEMODE_MP_TIMER, (void *)&HookGameModeTimer,
                        (void **)&g_origGameModeTimer);
  InstallSwitchedHook("squad behaviours (ReplaceNPCBehaviours)", RVA_SCENE_REPLACE_NPC_BEHAVIOURS, (void *)&HookSceneReplace,
                      (void **)&g_origSceneReplace);
  InstallSwitchedHook("bot setup 0x3678F0 (/spawn'd bots' targets)", 0x3678F0, (void *)&HookGmSetupAI,
                      (void **)&g_origGmSetupAI);

  if (!SwitchOn("DN_HOST_NO_EVENT_XP", "dn_host_no_event_xp.txt"))
    InstallSwitchedHook("original match xp (AssignScoringReward)", RVA_ASSIGN_SCORING_REWARD,
                        (void *)&HookAssignScoringReward, (void **)&g_origAssignScoringReward);
  else
    Logf("event xp: OFF (dn_host_no_event_xp.txt / DN_HOST_NO_EVENT_XP=1). mmogbrain "
         "pays its formula XP.");

  Logf("onslaught creeps: %s", OnslaughtCreepsEnabled() ? "ON (game-mode timer; Onslaught hosts only)"
       : "OFF (dn_host_no_onslaught_creeps.txt / DN_HOST_NO_ONSLAUGHT_CREEPS=1)");
  if (!SwitchOn("DN_HOST_NO_ONSLAUGHT_POINTS", "dn_host_no_onslaught_points.txt")) {
    InstallSwitchedHook("onslaught points (TDM Killed)", RVA_TDM_KILLED,
                        (void *)&HookTdmKilled, (void **)&g_origTdmKilled);
    InstallSwitchedHook("onslaught winner by points (TDM leader)", RVA_TDM_DETERMINE_LEADER,
                        (void *)&HookDetermineLeader, (void **)&g_origDetermineLeader);
  } else
    Logf("onslaught points: OFF (dn_host_no_onslaught_points.txt / "
         "DN_HOST_NO_ONSLAUGHT_POINTS=1). Onslaught ends at 30 kills.");

  if (!SwitchOn("DN_HOST_NO_ONS_GUN_ROWS", "dn_host_no_ons_gun_rows.txt"))
    InstallSwitchedHook("command ship gun rows (LoadWeaponDataTableRow)", RVA_WEAPON_GROUP_LOAD_ROW,
                        (void *)&HookWeaponGroupLoadRow, (void **)&g_origWeaponGroupLoadRow);

  const char *featsWhy = "/turbo, /npc";
  if ((HostIsHavoc() || HostIsHorde()) && !SwitchOn("DN_HOST_NO_HAVOC_DIRECTOR", "dn_host_no_havoc_director.txt"))
    InstallSwitchedHook("havoc director (compiled-out native exec thunk)", 0x74C640 /* RVA_EMPTY_EXEC_THUNK */,
                        (void *)&HookEmptyExecThunk, (void **)&g_origEmptyExecThunk);
  if ((HostIsHavoc() || HostIsHorde()) && !SwitchOn("DN_HOST_NO_HAVOC_DIRECTOR", "dn_host_no_havoc_director.txt"))
    InstallSwitchedHook("havoc enemy list (compiled-out int-parm exec thunk)", 0x742E90 /* RVA_INT_PARM_EXEC_THUNK */,
                        (void *)&HookIntParmExecThunk, (void **)&g_origIntParmExecThunk);
  if ((HostIsHavoc() || HostIsHorde()) && !SwitchOn("DN_HOST_NO_HAVOC_DIRECTOR", "dn_host_no_havoc_director.txt"))
    InstallSwitchedHook("havoc kill xp (compiled-out int+object exec thunk)", 0x740AF0 /* RVA_INT_OBJ_PARM_EXEC_THUNK */,
                        (void *)&HookIntObjParmExecThunk, (void **)&g_origIntObjParmExecThunk);
  if (HostIsHavoc() && !SwitchOn("DN_HOST_NO_HAVOC_DIRECTOR", "dn_host_no_havoc_director.txt"))
    InstallSwitchedHook("havoc ready up (ServerReadyUpPlayerForNextWave exec thunk)", 0x772650 /* RVA_HVC_READY_UP_THUNK */,
                        (void *)&HookHavocReadyUpThunk, (void **)&g_origHavocReadyUpThunk);
  if (HostIsHavoc() && !SwitchOn("DN_HOST_NO_HAVOC_DIRECTOR", "dn_host_no_havoc_director.txt"))
    InstallSwitchedHook("havoc unready (ServerUnReadyPlayerForNextWave exec thunk)", 0x774B40 /* RVA_HVC_UNREADY_THUNK */,
                        (void *)&HookHavocUnReadyThunk, (void **)&g_origHavocUnReadyThunk);

  InstallSwitchedHook("match ready up (ServerPlayerReadyUpForMatch exec thunk, log only)", RVA_READY_UP_FOR_MATCH_THUNK,
                      (void *)&HookReadyUpForMatchThunk, (void **)&g_origReadyUpForMatchThunk);
  if (HostIsHavoc() && !SwitchOn("DN_HOST_NO_HAVOC_DIRECTOR", "dn_host_no_havoc_director.txt")) {
    InstallSwitchedHook("havoc respawn ship gate (UYLoadoutManager loadout validity)", RVA_LOADOUT_IS_REGISTERED,
                        (void *)&HookLoadoutGate, (void **)&g_origLoadoutGate);
    InstallSwitchedHook("havoc respawn ship (UYLoadoutManager::ActivateLoadout)", RVA_ACTIVATE_LOADOUT,
                        (void *)&HookActivateLoadout, (void **)&g_origActivateLoadout);
    InstallSwitchedHook("havoc respawn ship (SpawnDefaultPawnAtTransform, log only)", 0x382530,
                        (void *)&HookSpawnAtTransform, (void **)&g_origSpawnAtTransform);
    InstallSwitchedHook("havoc upgrade pick (ServerReceiveSelectedUpgradeIndex_Implementation)",
                        0x406040 /* RVA_HVC_SERVER_SELECTED_UPGRADE_IMPL */, (void *)&HookHvcSelectedUpgrade,
                        (void **)&g_origHvcSelectedUpgrade);
    InstallSwitchedHook("havoc wave over (compiled-out bool-parm exec thunk)", RVA_BOOL_PARM_EXEC_THUNK,
                        (void *)&HookBoolParmExecThunk, (void **)&g_origBoolParmExecThunk);
    if (HavocRespawnBlockOn())
      InstallSwitchedHook("havoc respawn block (game mode spawn gate, SpawnPlayer)", RVA_GM_SPAWN_GATE,
                          (void *)&HookSpawnGate, (void **)&g_origSpawnGate);
    else
      Logf("havoc respawn block: OFF (dn_host_no_havoc_respawn_block.txt / DN_HOST_NO_HAVOC_RESPAWN_BLOCK=1)");
    if (!HavocShipUpgradesOn())
      Logf("havoc ship upgrades: OFF (dn_host_no_havoc_ship_upgrades.txt / DN_HOST_NO_HAVOC_SHIP_UPGRADES=1)");
    featsWhy = "havoc boosts, /turbo, /npc";
  }
  if (!HostIsTurboTDM())
    ;
  else if (SwitchOn("DN_HOST_NO_GAME_MODIFIERS", "dn_host_no_game_modifiers.txt"))
    Logf("turbo modifiers: OFF (dn_host_no_game_modifiers.txt / "
         "DN_HOST_NO_GAME_MODIFIERS=1). Turbo TDM plays as plain TDM.");
  else {
    g_turboTdmMods = true;
    featsWhy = "turbo modifiers, /turbo, /npc";
  }
  // One hook for all of them. Outside Havoc/Turbo TDM it only records each
  // ship's feats component, which /turbo and /npc need in every mode (onsfun2
  // 10-16: /npc brutal in Onslaught found no ships -- the hook was
  // Havoc/Turbo-only then).
  char featsLabel[96];
  snprintf(featsLabel, sizeof featsLabel, "%s (UYFeatsComponent begin-play)", featsWhy);
  InstallSwitchedHook(featsLabel, RVA_FEATS_COMPONENT_BEGIN,
                      (void *)&HookFeatsBegin, (void **)&g_origFeatsBegin);

  if (ShipPhysicsEnabled())
    InstallSwitchedHook("ship physics (UYVehicleMovementComp view cull)",
                        RVA_VEHICLE_VIEW_CULL_SETUP, (void *)&HookVehicleViewCull,
                        (void **)&g_origVehicleViewCull);
  else
    Logf("ship physics: OFF (create dn_host_ship_physics.txt beside the "
         "executable, or set DN_HOST_SHIP_PHYSICS=1). Player ships far from the "
         "host's orbit camera are not simulated; players snap back.");

  // On its own thread: InstallPostLoginHook waits for GObjects, and Startup
  // must return so the FindLoadoutByID hook above is live immediately.
  CreateThread(NULL, 0, PostLoginInstallThread, NULL, 0, NULL);
  return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hModule);
    CreateThread(NULL, 0, Startup, NULL, 0, NULL);
  }
  return TRUE;
}

// ---------------------------------------------------------------------------
// wer.dll stand-in exports
//
// The game imports these four from wer.dll (Windows Error Reporting) and
// resolves them from its own directory first, which is how this DLL gets loaded
// without an injector. They are no-ops: the engine only calls them while
// writing a crash report, and a host that is writing a crash report has already
// lost the match. See README.md -- this is the deployment mechanism, not part
// of the fix.
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) void WerReportAddFile() {}
extern "C" __declspec(dllexport) void WerReportSubmit() {}
extern "C" __declspec(dllexport) void WerReportSetParameter() {}
extern "C" __declspec(dllexport) void WerReportCreate() {}

// ---------------------------------------------------------------------------
// Reflection dump (DN_HOST_REFLECT_DUMP=1 / dn_host_reflect_dump.txt; off by
// default). Logs every property declared on the object's class chain up to the
// engine classes (stops at "GameMode"/"GameState"/"Actor"): name, property
// class and the raw dwords at +0x30..+0x4C of the UProperty, from which the
// 4.13 layout (ArrayDim, ElementSize, PropertyFlags, RepIndex, Offset_Internal)
// is read off against offsets already known from disassembly (Horde game state
// wave index +0x1F4C, wave state +0x1F50, duration +0x1F58 -- 0x39E010).
// UField::Next +0x28, UStruct::Children +0x38 (after SuperStruct +0x30).
static void DumpReflection(UObjectMin *obj) {
  if (!SwitchOn("DN_HOST_REFLECT_DUMP", "dn_host_reflect_dump.txt") || !IsReadable(obj, sizeof(UObjectMin)))
    return;
  Logf("reflect: object %p %s", obj, ClassNameOf(obj));
  UObjectMin *cls = obj->Class;
  for (int depth = 0; cls && depth < 12 && IsReadable(cls, 0x40); ++depth) {
    const char *cn = NameText(cls->Name);
    if (!cn || !strcmp(cn, "GameMode") || !strcmp(cn, "GameModeBase") || !strcmp(cn, "GameState") ||
        !strcmp(cn, "GameStateBase") || !strcmp(cn, "Actor"))
      break;
    Logf("reflect: class %s", cn);
    UObjectMin *f = *(UObjectMin **)((uint8_t *)cls + 0x38);
    for (int i = 0; f && i < 400 && IsReadable(f, 0x50); ++i) {
      const char *fn = NameText(f->Name);
      const char *fc = ClassNameOf(f);
      if (!strcmp(fc, "Function") && IsReadable((uint8_t *)f + 0x88, 0x40)) {
        // UFunction: FunctionFlags +0x88; native Func somewhere in +0x98..+0xC0
        // (printed as RVAs when the qword lands in the image).
        uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
        char rv[160] = "";
        int o = 0;
        for (int k = 0x98; k < 0xC8; k += 8) {
          uintptr_t q = *(uintptr_t *)((uint8_t *)f + k);
          if (q > base && q < base + 0x6000000)
            o += snprintf(rv + o, sizeof rv - o, " +%x=%llx", k, (unsigned long long)(q - base));
        }
        Logf("reflect:   fn %-40s flags %08x parms %u%s", fn ? fn : "?", *(uint32_t *)((uint8_t *)f + 0x88),
             (unsigned)*(uint16_t *)((uint8_t *)f + 0x90), rv);
        // Parameters: the function's own Children (same UProperty layout).
        UObjectMin *pp = *(UObjectMin **)((uint8_t *)f + 0x38);
        for (int j = 0; pp && j < 16 && IsReadable(pp, 0x58); ++j) {
          const char *pn = NameText(pp->Name);
          Logf("reflect:       parm %-32s %-20s +0x%x size %u flags %08x", pn ? pn : "?", ClassNameOf(pp),
               *(uint32_t *)((uint8_t *)pp + 0x50), *(uint32_t *)((uint8_t *)pp + 0x34),
               *(uint32_t *)((uint8_t *)pp + 0x38));
          pp = *(UObjectMin **)((uint8_t *)pp + 0x28);
        }
      } else {
        const uint32_t *d = (const uint32_t *)((uint8_t *)f + 0x30);
        Logf("reflect:   %-40s %-24s %08x %08x %08x %08x | %08x %08x %08x %08x %08x %08x", fn ? fn : "?", fc,
             d[0], d[1], d[2], d[3], d[8], d[9], d[10], d[11], d[12], d[13]);
      }
      f = *(UObjectMin **)((uint8_t *)f + 0x28);
    }
    cls = *(UObjectMin **)((uint8_t *)cls + OFF_USTRUCT_SUPER);
  }
}

// One-shot layout probe (diagnostic): every reflected property on obj's class
// chain whose name contains `sub` (case-sensitive): class, Offset_Internal
// (+0x50), ElementSize (+0x34), the first 0x30 value bytes, and -- for a
// struct property -- the UScriptStruct found among the UProperty's trailing
// qwords (+0x58..+0x88) with its members. Written for the captain appearance
// (YPlayerReplicationInfo::m_captainDisplayInformation, replicated per
// GetLifetimeReplicatedProps 0x5B6FFF) whose type was not known statically.
static void DescribeProps(const char *tag, UObjectMin *obj, const char *sub) {
  if (!IsReadable(obj, sizeof(UObjectMin)))
    return;
  for (UObjectMin *cls = obj->Class; cls && IsReadable(cls, 0x40); cls = *(UObjectMin **)((uint8_t *)cls + OFF_USTRUCT_SUPER))
    for (UObjectMin *f = *(UObjectMin **)((uint8_t *)cls + 0x38); f && IsReadable(f, 0x90); f = *(UObjectMin **)((uint8_t *)f + 0x28)) {
      const char *fn = NameText(f->Name), *fc = ClassNameOf(f);
      if (!fn || !strstr(fn, sub) || !strcmp(fc, "Function"))
        continue;
      uint32_t off = *(uint32_t *)((uint8_t *)f + 0x50), sz = *(uint32_t *)((uint8_t *)f + 0x34);
      char hex[200] = "";
      uint8_t *v = (uint8_t *)obj + off;
      for (int i = 0, o = 0; i < 0x30 && IsReadable(v + i, 1); ++i)
        o += snprintf(hex + o, sizeof hex - o, "%02x%s", v[i], (i & 7) == 7 ? " " : "");
      Logf("%s %s.%s %s +0x%x size %u flags %016llx | %s", tag, NameText(cls->Name), fn, fc, off, sz,
           (unsigned long long)*(uint64_t *)((uint8_t *)f + 0x38), hex);
      for (int k = 0x58; k <= 0x88; k += 8) {
        UObjectMin *st = *(UObjectMin **)((uint8_t *)f + k);
        if (!IsReadable(st, 0x40) || !IsReadable(st->Class, sizeof(UObjectMin)) || strcmp(ClassNameOf(st), "ScriptStruct"))
          continue;
        Logf("%s   struct %s (UProperty+0x%x)", tag, NameText(st->Name), k);
        for (UObjectMin *m = *(UObjectMin **)((uint8_t *)st + 0x38); m && IsReadable(m, 0x58); m = *(UObjectMin **)((uint8_t *)m + 0x28))
          Logf("%s     %-32s %-20s +0x%x size %u", tag, NameText(m->Name), ClassNameOf(m), *(uint32_t *)((uint8_t *)m + 0x50),
               *(uint32_t *)((uint8_t *)m + 0x34));
        break;
      }
    }
}

// Offset_Internal (+0x50) of the reflected property `name` on obj's class
// chain, or -1.
static int PropOffset(UObjectMin *obj, const char *name) {
  if (!IsReadable(obj, sizeof(UObjectMin)))
    return -1;
  for (UObjectMin *cls = obj->Class; cls && IsReadable(cls, 0x40); cls = *(UObjectMin **)((uint8_t *)cls + OFF_USTRUCT_SUPER))
    for (UObjectMin *f = *(UObjectMin **)((uint8_t *)cls + 0x38); f && IsReadable(f, 0x58); f = *(UObjectMin **)((uint8_t *)f + 0x28)) {
      const char *fn = NameText(f->Name);
      if (fn && !strcmp(fn, name) && strcmp(ClassNameOf(f), "Function"))
        return (int)*(uint32_t *)((uint8_t *)f + 0x50);
    }
  return -1;
}

// Name of the reflected property of obj's class chain whose storage covers
// byte `off` (Offset_Internal +0x50, ElementSize*ArrayDim +0x34), or "?" when
// the byte is native-only. Diagnostics only.
static const char *PropNameAt(UObjectMin *obj, uint32_t off) {
  if (!IsReadable(obj, sizeof(UObjectMin)))
    return "?";
  for (UObjectMin *cls = obj->Class; cls && IsReadable(cls, 0x40); cls = *(UObjectMin **)((uint8_t *)cls + OFF_USTRUCT_SUPER))
    for (UObjectMin *f = *(UObjectMin **)((uint8_t *)cls + 0x38); f && IsReadable(f, 0x58); f = *(UObjectMin **)((uint8_t *)f + 0x28)) {
      if (!strcmp(ClassNameOf(f), "Function"))
        continue;
      uint32_t o = *(uint32_t *)((uint8_t *)f + 0x50), sz = *(uint32_t *)((uint8_t *)f + 0x34);
      if (off >= o && off < o + (sz ? sz : 1)) {
        const char *n = NameText(f->Name);
        return n ? n : "?";
      }
    }
  return "?";
}

// Logs instance values of the named reflected properties of obj (class chain,
// Offset_Internal +0x50), formatted by property class. A name ending in '*'
// matches as a substring. Diagnostics only.
static void LogProps(const char *tag, UObjectMin *obj, const char *const *names, int n) {
  if (!IsReadable(obj, sizeof(UObjectMin)))
    return;
  char out[900];
  int o = snprintf(out, sizeof out, "%s %s:", tag, ClassNameOf(obj));
  for (UObjectMin *cls = obj->Class; cls && IsReadable(cls, 0x40); cls = *(UObjectMin **)((uint8_t *)cls + OFF_USTRUCT_SUPER)) {
    UObjectMin *f = *(UObjectMin **)((uint8_t *)cls + 0x38);
    for (int i = 0; f && i < 400 && IsReadable(f, 0x58); ++i, f = *(UObjectMin **)((uint8_t *)f + 0x28)) {
      const char *fn = NameText(f->Name), *fc = ClassNameOf(f);
      if (!fn || !strcmp(fc, "Function"))
        continue;
      bool hit = false;
      for (int k = 0; k < n && !hit; ++k) {
        if (!strchr(names[k], '*')) {
          hit = !strcmp(fn, names[k]);
          continue;
        }
        char want[64], have[128];
        int w = 0, h = 0;
        for (const char *c = names[k]; *c && w < 63; ++c)
          if (*c != '*')
            want[w++] = (char)tolower((unsigned char)*c);
        for (const char *c = fn; *c && h < 127; ++c)
          have[h++] = (char)tolower((unsigned char)*c);
        want[w] = have[h] = 0;
        hit = strstr(have, want) != nullptr;
      }
      if (!hit || o >= (int)sizeof out - 80)
        continue;
      uint8_t *v = (uint8_t *)obj + *(uint32_t *)((uint8_t *)f + 0x50);
      if (!IsReadable(v, 16))
        continue;
      if (!strcmp(fc, "IntProperty"))
        o += snprintf(out + o, sizeof out - o, " %s=%d", fn, *(int32_t *)v);
      else if (!strcmp(fc, "BoolProperty") || !strcmp(fc, "ByteProperty") || !strcmp(fc, "EnumProperty"))
        o += snprintf(out + o, sizeof out - o, " %s=%u", fn, *v);
      else if (!strcmp(fc, "FloatProperty"))
        o += snprintf(out + o, sizeof out - o, " %s=%.2f", fn, *(float *)v);
      else if (!strcmp(fc, "NameProperty")) {
        const char *t = NameText(*(FNameMin *)v);
        o += snprintf(out + o, sizeof out - o, " %s='%s'", fn, t ? t : "?");
      } else if (!strcmp(fc, "ArrayProperty"))
        o += snprintf(out + o, sizeof out - o, " %s=[%d]", fn, *(int32_t *)(v + 8));
      else if (!strcmp(fc, "ObjectProperty") || !strcmp(fc, "ClassProperty"))
        o += snprintf(out + o, sizeof out - o, " %s=%s", fn, *(void **)v ? ClassNameOf(*(UObjectMin **)v) : "null");
      else
        o += snprintf(out + o, sizeof out - o, " %s=<%s>", fn, fc);
    }
  }
  Logf("%s", out);
}

// ---------------------------------------------------------------------------
// Havoc wave director (on by default on Havoc hosts; dn_host_no_havoc_director.txt
// / DN_HOST_NO_HAVOC_DIRECTOR=1 turns it off).
//
// Operator report 2026-10-13: Havoc loads, fighter swarms fight, but no wave
// ever starts -- no big ships, no wave timer. The server half of the wave
// loop is compiled out of this build: AYGameMode_Horde StartFirstWave /
// EndCurrentWave / CheckWaveOver and AYGameMode_Havoc OnWaveStartDelayTimerOver
// / OnWaveCompleteTimerOver all share the empty exec thunk 0x74C640 (UFunction
// Func +0xB0, runtime reflection dump 2026-10-13), and OnSwitchToState's body
// is the bare `ret` at 0x1AA9450. What survives is the game-state side:
//   AYGameState_Havoc::EnterNewState(gs, state) 0x36BC00 -- fires the BP
//     OnEnter*State events of GameState_Havoc_BP and ClientHavocStateChanged
//     (0x5E5440); 0 UpgradeSelection, 1 WaveStarting, 2 WaveInProgress,
//     3 WaveComplete, 4 GameOver.
//   AYGameState_Horde OnRep_WaveStateChanged body 0x39E010(gs) -- reads
//     m_currentWaveState and fires the level script OnHordeWaveStarting /
//     Started / Ended events, then BP HordeWaveStateChanged.
// Property offsets (UProperty Offset_Internal +0x50, reflection dump, the
// Horde ones match the disassembly of 0x39E010):
#define OFF_HVC_GM_WAVE_COMPLETE_DUR 0xA70  // float m_waveCompleteDuration
#define OFF_HVC_GM_UPGRADE_DUR 0xA74        // int   m_upgradeSelectionDuration
#define OFF_HVC_GM_WAVE_START_DELAY 0xA78   // int   m_waveStartDelayDuration
#define OFF_HVC_GM_PVE_EVENT_MANAGER 0xA10  // m_pveEventManager
#define OFF_HVC_GS_IS_MATCH_STARTED 0x569   // AYGameState::m_isMatchStarted
#define OFF_HVC_GS_NUM_ALIVE 0x1E58         // AYGameState_PVE::m_numAliveEnemies
#define OFF_HVC_GS_KILLED 0x1F40            // m_numEnemiesKilledThisWave
#define OFF_HVC_GS_MAX_WAVE_ENEMIES 0x1F44  // m_maxWaveEnemies
#define OFF_HVC_GS_MAX_WAVES 0x1F48         // m_maxNumberOfWaves
#define OFF_HVC_GS_WAVE_INDEX 0x1F4C        // m_currentWaveIndex
#define OFF_HVC_GS_WAVE_STATE 0x1F50        // m_currentWaveState (EYHordeWaveState)
#define OFF_HVC_GS_WAVE_DURATION 0x1F58     // m_currentWaveDuration
#define OFF_HVC_GS_HAVOC_STATE 0x2018       // m_currentHavocState
#define OFF_HVC_GS_WAVES_REACHED 0x2024     // m_wavesReached
#define OFF_HVC_GS_EVENT_STARTED 0x2120     // GameState_Havoc_BP EventStarted
#define OFF_HVC_GS_HORDE_MANAGER 0x2128     // GameState_Havoc_BP HordeManager
#define OFF_HVC_GM_ON_PVE_MODE_STARTED 0xA00 // AYGameMode_PVE::OnPVEModeStarted (multicast)
// AYGameMode_PVE::OnAnyNPCKilled (multicast, +0x9E0). GameState_Havoc_BP binds
// OnAnyNPCKilled_Event(KilledYPawn obj @0, KilledNPCID int @8; parms 12 B) to
// it; when EnableEnemiesDropXP has opened the gate, the event spawns
// AlliedBuffPod_HavocXP_Pickup_Standard_BP (boss: _Boss_B) at the pawn's
// transform, and picking that up calls GameMode_Havoc.AddXPForGroup(HavocXP)
// -- the per-kill XP. Nothing on the host broadcast it (the server-side
// death path went with the server build), so kills gave no XP and +0x1F40
// stayed 0 (log "killed 0" every wave, 2026-10-14 playtest).
#define OFF_HVC_GM_ON_ANY_NPC_KILLED 0x9E0
#define OFF_HVC_GS_PVE_EVENT_MANAGER 0x1E68 // AYGameState_PVE::m_pveEventManager
#define OFF_HVC_GS_ON_WAVE_STATE_CHANGED 0x1F20 // AYGameState_Horde::OnHordeWaveStateChanged
#define OFF_HVC_HM_GAME_MODE 0x4B8          // PVE_HordeManager_BP GameMode
// YPVEEventManager::m_combatSceneManager (reflection: ObjectProperty +0x3C0).
// PVE_HordeManager_BP.SpawnWaveInitialEnemies hands it to
// PVE_EnemyGroupSpawn_Component.SpawnPVEEnemyGroupEvent, whose only spawn is
// CombatSceneManager.AddNPCSpawnByDataset (bytecode via tools/bpdump +
// bpcompact.py) -- a null context would skip the call silently.
// DISPROVED as the no-spawn cause (2026-10-13 run): the field is already set
// to the world's YAICombatSceneManager before the director touches it; the
// fill below is a no-op kept only as a guard.
#define OFF_PVE_EM_COMBAT_SCENE_MANAGER 0x3C0
#define OFF_HVC_HM_WAVES 0x4A8              // PVE_HordeManager_BP Waves (TArray)
#define RVA_MULTICAST_BROADCAST_NOPARMS 0x2322A0 // (delegate*, parms*): EnterNewState's own broadcasts
#define RVA_HVC_ENTER_NEW_STATE 0x36BC00
#define RVA_HORDE_WAVE_STATE_CHANGED 0x39E010

// Calls a parameterless UFunction (BP event) by name through ProcessEvent.
static bool CallNoParms(void *obj, const wchar_t *name) {
  if (!IsReadable(obj, sizeof(UObjectMin)))
    return false;
  FNameMin n = {};
  ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&n, name, 1 /* FNAME_Add */);
  void *fn = FindFunctionSafe(obj, *(uint64_t *)&n);
  if (!fn)
    return false;
  alignas(16) uint8_t parms[256] = {};
  void **vt = *(void ***)obj;
  ((tProcessEventVirt)vt[VT_PROCESS_EVENT / 8])(obj, fn, parms);
  return true;
}

// Calls a UFunction whose parameters are a wave state (byte/enum) and a wave
// index (int), filled by type from the function's own parameter list.
static bool CallWaveEvent(void *obj, const wchar_t *name, int state, int wave) {
  if (!IsReadable(obj, sizeof(UObjectMin)))
    return false;
  FNameMin n = {};
  ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&n, name, 1 /* FNAME_Add */);
  void *fn = FindFunctionSafe(obj, *(uint64_t *)&n);
  if (!fn || !IsReadable((uint8_t *)fn + 0x92, 2))
    return false;
  alignas(16) uint8_t parms[256] = {};
  char desc[160] = "";
  int o = 0;
  UObjectMin *pp = *(UObjectMin **)((uint8_t *)fn + 0x38);
  for (int j = 0; pp && j < 8 && IsReadable(pp, 0x58); ++j) {
    uint32_t off = *(uint32_t *)((uint8_t *)pp + 0x50);
    const char *pc = ClassNameOf(pp);
    if (off < sizeof parms - 4) {
      if (!strcmp(pc, "ByteProperty") || !strcmp(pc, "EnumProperty"))
        parms[off] = (uint8_t)state;
      else if (!strcmp(pc, "IntProperty"))
        *(int32_t *)(parms + off) = wave;
    }
    const char *pn = NameText(pp->Name);
    o += snprintf(desc + o, sizeof desc - o > 0 ? sizeof desc - o : 0, " %s:%s@%x", pn ? pn : "?", pc, off);
    pp = *(UObjectMin **)((uint8_t *)pp + 0x28);
  }
  void **vt = *(void ***)obj;
  ((tProcessEventVirt)vt[VT_PROCESS_EVENT / 8])(obj, fn, parms);
  static int s_logged = 0;
  if (s_logged++ < 8)
    Logf("havoc: called %ls(%d, %d) on %s; parms%s", name, state, wave, ClassNameOf(obj), desc);
  return true;
}

static int DelegateBound(const uint8_t *p) { return IsReadable(p + 8, 4) ? *(const int32_t *)(p + 8) : -1; }

// The compiled-out natives the BPs still call. Their exec thunk 0x74C640
// (P_FINISH, then the bare `ret` 0x1AA9450) is shared by every parameterless
// stub; CalledNativeName names the UFunction being run. Each call is
// logged once per name and recorded for the director.
#define RVA_EMPTY_EXEC_THUNK 0x74C640
tExecThunk g_origEmptyExecThunk = nullptr;
static volatile LONG g_hvcBpEnd = 0; // 1 PlayersWin, 2 PlayersLose: the BP asked the run to end
static volatile LONG g_hvcEndWaveCalls = 0, g_hvcPlayersWinCalls = 0, g_hvcPlayersLoseCalls = 0;

// Which native is running. A script call hands the native the CALLER's
// FFrame, so FFrame::Node (+0x10) is the calling function (logged as
// "ExecuteUbergraph_PVE_EnemyGroupSpawn_Component" until 2026-10-14 -- that
// line never named the stub). The callee is the EX_FinalFunction / EX_CallMath
// operand, the UFunction* just before the parameters: Code (+0x20) - 8 on
// entry. Falls back to Node when that is not a readable *Function object
// (an EX_VirtualFunction carries an FName there instead).
static const char *CalledNativeName(uint8_t *stack) {
  uint8_t *code = IsReadable(stack + 0x20, 8) ? *(uint8_t **)(stack + 0x20) : nullptr;
  if (code && IsReadable(code - 8, 8)) {
    UObjectMin *fn = *(UObjectMin **)(code - 8);
    if (IsReadable(fn, sizeof(UObjectMin)) && IsReadable(fn->Class, sizeof(UObjectMin))) {
      const char *cn = NameText(fn->Class->Name);
      if (cn && strstr(cn, "Function"))
        return NameText(fn->Name);
    }
  }
  UObjectMin *node = IsReadable(stack + 0x10, 8) ? *(UObjectMin **)(stack + 0x10) : nullptr;
  return IsReadable(node, sizeof(UObjectMin)) ? NameText(node->Name) : nullptr;
}

static void __fastcall HookEmptyExecThunk(void *ctx, uint8_t *stack, void *result) {
  const char *n = CalledNativeName(stack);
  g_origEmptyExecThunk(ctx, stack, result);
  if (!n)
    return;
  if (!strcmp(n, "EndCurrentWave"))
    InterlockedIncrement(&g_hvcEndWaveCalls);
  else if (!strcmp(n, "PlayersWin") || !strcmp(n, "PlayersLose")) {
    // The run's end. Compiled out like the wave loop, so nothing ends the
    // match yet: logged on every call to learn when the BP decides it.
    LONG c = InterlockedIncrement(!strcmp(n, "PlayersWin") ? &g_hvcPlayersWinCalls : &g_hvcPlayersLoseCalls);
    InterlockedCompareExchange(&g_hvcBpEnd, !strcmp(n, "PlayersWin") ? 1 : 2, 0);
    uint8_t *gs = IsReadable((uint8_t *)ctx + OFF_GM_GAMESTATE, 8) ? *(uint8_t **)((uint8_t *)ctx + OFF_GM_GAMESTATE) : nullptr;
    bool havocGs = gs && IsReadable(gs + OFF_HVC_GS_WAVES_REACHED, 4) && ClassChainContains((UObjectMin *)gs, "YGameState_Havoc");
    Logf("havoc: BP called %s on %s (call %ld; wave %d of %d, waves reached %d) -- the director ends the run", n,
         ClassNameOf(ctx), c, havocGs ? *(int32_t *)(gs + OFF_HVC_GS_WAVE_INDEX) : -1,
         havocGs ? *(int32_t *)(gs + OFF_HVC_GS_MAX_WAVES) : -1, havocGs ? *(int32_t *)(gs + OFF_HVC_GS_WAVES_REACHED) : -1);
  }
  static char s_seen[16][48];
  static int s_nseen = 0;
  for (int i = 0; i < s_nseen; ++i)
    if (!strcmp(s_seen[i], n))
      return;
  if (s_nseen < 16)
    snprintf(s_seen[s_nseen++], sizeof s_seen[0], "%s", n);
  Logf("havoc: BP called compiled-out native %s on %s (first call)", n, ClassNameOf(ctx));
}

// AddNPCToEnemyList(int npcId), which PVE_EnemyGroupSpawn_Component calls on
// YGameMode_PVE right after AddNPCSpawnByDataset for every enemy that
// CountForWave, is compiled out too: its exec thunk 0x742E90 reads the int and
// calls the bare `ret` 0x1AA9450. That thunk is shared by 34 one-int natives
// (LEA r8 refs in the StaticRegisterNatives blocks), so the hook does the
// thunk's own work -- FFrame::Step (0xD5D250: stack, stack->Object +0x18,
// &result) when Code (+0x20) is set, else StepExplicitProperty (0xD5D280) on
// PropertyChainForCompiledIn (+0x80, advanced by Next +0x28); P_FINISH
// (Code += Code != 0) -- and then records the id when the called native
// (CalledNativeName) is AddNPCToEnemyList. Nothing else in the exe writes AYGameState_PVE
// m_numAliveEnemies (+0x1E58; the two writers are constructors), so the
// director derives it from these ids. GUESS: the server build's list
// semantics (removal on death, what else it fed) are unknown.
#define RVA_INT_PARM_EXEC_THUNK 0x742E90
#define RVA_FFRAME_STEP 0xD5D250
#define RVA_FFRAME_STEP_EXPLICIT_PROPERTY 0xD5D280
tExecThunk g_origIntParmExecThunk = nullptr;
static volatile LONG g_hvcEnemyIds[256];
static volatile LONG g_hvcEnemyCount = 0;

// /enemy <ShipId> (CommandsTick): the dataset pawn id FName that
// AddNPCSpawnByDataset (0x23A380) stores at scene entry +0x18, and the scene
// tick resolves to a class when it spawns (0x24FD00), is replaced for every
// newly listed enemy. Done here, inside the BP's AddNPCToEnemyList call that
// directly follows AddNPCSpawnByDataset, so the entry is not yet spawned.
// mmogbrain only sends ids that appear in HVC_Map1_P's own wave data.
// GUESS: other Havoc maps' NPC sets hold the same rows (unchecked).
static FNameMin g_hvcEnemyOverride = {};
static bool g_hvcEnemyOverrideOn = false;
static void HavocEnemyOverride(int id) {
  if (!g_hvcEnemyOverrideOn)
    return;
  uint8_t *scene = (uint8_t *)((tGetSceneManager)(g_base + RVA_COMBAT_SCENE_MANAGER))();
  if (!scene || !IsReadable(scene + OFF_SCENE_NPCS, 16) || id < 0 || id >= *(int32_t *)(scene + OFF_SCENE_NPCS_COUNT))
    return;
  uint8_t *e = *(uint8_t **)(scene + OFF_SCENE_NPCS) + (size_t)id * NPC_SPAWN_SIZE;
  if (e[OFF_NPCSPAWN_INSTANCED]) {
    Logf("commands: enemy override: entry %d already spawned", id);
    return;
  }
  const char *was = NameText(*(FNameMin *)(e + OFF_NPCSPAWN_DATASET_ID));
  Logf("commands: enemy override: entry %d %s -> %s", id, was ? was : "?", NameText(g_hvcEnemyOverride));
  memcpy(e + OFF_NPCSPAWN_DATASET_ID, &g_hvcEnemyOverride, sizeof(FNameMin));
}

// Havoc XP. AYGameMode_Havoc AddXPForGroup(int) (int thunk above) and
// AddXPWithContributerPlayer/Pawn(int, object) (thunk 0x740AF0, below) are
// compiled out like the wave loop, and nothing in the exe writes AYGameState
// _Havoc m_playerWaveXP (+0x201C) or m_totalXP (+0x2020) -- the only writer is
// the constructor 0x363680. The getters are plain loads: GetTotalXP 0x372730
// = +0x2020, GetPlayerWaveXP 0x371AC0 = +0x201C, GetWaveXP 0x372F60 = +0x2020
// - +0x2038 (+0x2038 has no writer at all; reads 0). Both fields are
// replicated (CPF_Net). Run 2026-10-14 18:39: GameMode_Havoc_BP called
// AddXPForGroup(750) as wave 1 cleared; the client showed 0 wave XP and 0
// upgrade currency. GUESS: the original split between the two fields is
// unknown; every grant goes to both, and m_playerWaveXP restarts each wave.
#define OFF_HVC_GS_PLAYER_WAVE_XP 0x201C
#define OFF_HVC_GS_TOTAL_XP 0x2020
// Per-player spendable XP. The upgrade panel's XP counter and the purchase
// gate both read AYPlayerReplicationInfo_Havoc m_currentXP: 0x3FC7D0 (PC
// +0x3E0 -> PRI cast -> +0xA28), called by the activate path 0x448240 which
// compares it with the upgrade's cost before SendSelectedUpgrade. Field
// offsets from the property registration 0x958150..0x958963: m_currentXP
// +0xA28, m_currentContributedXP +0xA2C, m_lastWaveContributedXP +0xA30.
// DeductXP 0x3F34F0 (pri, xp) is real code (`sub [pri+0xA28], xp`); AddXP is
// compiled out (only its log format string "AddXP | Player %s | XP %i |
// Contributer %i" survives, no LEA to it), so grants write the field here.
// GUESS: Havoc is co-op and the GS XP is team XP, so every connected human
// gets the full grant; the contributor (if any) also gets it in
// m_currentContributedXP. The original split is unknown.
#define OFF_HVC_PRI_CURRENT_XP 0xA28
#define OFF_HVC_PRI_CONTRIBUTED_XP 0xA2C
#define RVA_HVC_PRI_DEDUCT_XP 0x3F34F0
static int HavocHumanPCs(uint8_t *gs, uint8_t **out, int max) {
  if (!IsReadable(gs + OFF_GS_PLAYER_ARRAY, 16))
    return 0;
  uint8_t **arr = *(uint8_t ***)(gs + OFF_GS_PLAYER_ARRAY);
  int count = *(int32_t *)(gs + OFF_GS_PLAYER_ARRAY + 8);
  if (count < 0 || count > 256 || (count && !IsReadable(arr, (size_t)count * 8)))
    return 0;
  int n = 0;
  for (int i = 0; i < count && n < max; ++i) {
    uint8_t *pri = arr[i];
    if (!IsReadable(pri, OFF_HVC_PRI_CONTRIBUTED_XP + 4))
      continue;
    uint8_t *pc = *(uint8_t **)(pri + OFF_ACTOR_OWNER);
    if (!IsReadable(pc, OFF_PC_NETCONNECTION + 8) || !ClassChainContains((UObjectMin *)pc, "PlayerController") ||
        !*(void **)(pc + OFF_PC_NETCONNECTION) || *(uint8_t **)(pc + OFF_PC_PLAYER_STATE) != pri ||
        !ClassChainContains((UObjectMin *)pri, "YPlayerReplicationInfo_Havoc"))
      continue;
    out[n++] = pc;
  }
  return n;
}

// The PRI behind a contributor object (a pawn, a controller or a PRI).
static uint8_t *HavocContributorPri(void *obj) {
  uint8_t *o = (uint8_t *)obj;
  if (!IsReadable(o, sizeof(UObjectMin)))
    return nullptr;
  if (ClassChainIs((UObjectMin *)o, "Pawn"))
    o = IsReadable(o + OFF_PAWN_CONTROLLER, 8) ? *(uint8_t **)(o + OFF_PAWN_CONTROLLER) : nullptr;
  if (o && ClassChainIs((UObjectMin *)o, "Controller"))
    o = IsReadable(o + OFF_PC_PLAYER_STATE, 8) ? *(uint8_t **)(o + OFF_PC_PLAYER_STATE) : nullptr;
  return o && IsReadable(o, OFF_HVC_PRI_CONTRIBUTED_XP + 4) &&
                 ClassChainContains((UObjectMin *)o, "YPlayerReplicationInfo_Havoc") ? o : nullptr;
}

static void HavocAddXP(uint8_t *gm, int xp, const char *why, void *contributor = nullptr) {
  if (!IsReadable(gm + OFF_GM_GAMESTATE, 8))
    return;
  uint8_t *gs = *(uint8_t **)(gm + OFF_GM_GAMESTATE);
  if (!IsReadable(gs + OFF_HVC_GS_TOTAL_XP, 4) || !ClassChainContains((UObjectMin *)gs, "YGameState_Havoc"))
    return;
  if (xp <= 0 || xp > 1000000)
    return;
  *(int32_t *)(gs + OFF_HVC_GS_PLAYER_WAVE_XP) += xp;
  *(int32_t *)(gs + OFF_HVC_GS_TOTAL_XP) += xp;
  uint8_t *pcs[16];
  int npc = HavocHumanPCs(gs, pcs, 16);
  for (int i = 0; i < npc; ++i)
    *(int32_t *)(*(uint8_t **)(pcs[i] + OFF_PC_PLAYER_STATE) + OFF_HVC_PRI_CURRENT_XP) += xp;
  if (uint8_t *cpri = HavocContributorPri(contributor))
    *(int32_t *)(cpri + OFF_HVC_PRI_CONTRIBUTED_XP) += xp;
  static int s_logged = 0;
  if (s_logged++ < 40)
    Logf("havoc: %s(%d): wave xp %d, total xp %d, %d player(s) credited%s", why, xp,
         *(int32_t *)(gs + OFF_HVC_GS_PLAYER_WAVE_XP), *(int32_t *)(gs + OFF_HVC_GS_TOTAL_XP), npc,
         contributor ? " (+contributor)" : "");
}

// FFrame::Step for one parameter, as the generated exec thunks do it.
static void StepParm(uint8_t *stack, void *out) {
  if (*(void **)(stack + 0x20))
    ((void(__fastcall *)(void *, void *, void *))(g_base + RVA_FFRAME_STEP))(stack, *(void **)(stack + 0x18), out);
  else {
    uint8_t *prop = *(uint8_t **)(stack + 0x80);
    *(void **)(stack + 0x80) = *(void **)(prop + 0x28);
    ((void(__fastcall *)(void *, void *, void *))(g_base + RVA_FFRAME_STEP_EXPLICIT_PROPERTY))(stack, out, prop);
  }
}

// Exec thunk 0x740AF0: (int, object) -> bare `ret` 0x1AA9450; read both
// parameters the way it does (StepParm x2, P_FINISH), then act by callee.
#define RVA_INT_OBJ_PARM_EXEC_THUNK 0x740AF0
tExecThunk g_origIntObjParmExecThunk = nullptr;
static void __fastcall HookIntObjParmExecThunk(void *ctx, uint8_t *stack, void *result) {
  const char *n = CalledNativeName(stack);
  int32_t v = 0;
  void *obj = nullptr;
  StepParm(stack, &v);
  StepParm(stack, &obj);
  if (*(uint8_t **)(stack + 0x20))
    ++*(uint8_t **)(stack + 0x20);
  if (!n)
    return;
  if (!strcmp(n, "AddXPWithContributerPlayer") || !strcmp(n, "AddXPWithContributerPawn")) {
    HavocAddXP((uint8_t *)ctx, v, n, obj);
    return;
  }
  static char s_seen[16][48];
  static int s_nseen = 0;
  for (int i = 0; i < s_nseen; ++i)
    if (!strcmp(s_seen[i], n))
      return;
  if (s_nseen < 16)
    snprintf(s_seen[s_nseen++], sizeof s_seen[0], "%s", n);
  Logf("havoc: BP called compiled-out native %s(%d, %s) on %s (first call)", n, v,
       obj ? ClassNameOf((UObjectMin *)obj) : "null", ClassNameOf(ctx));
}

// Ready-up. AYPlayerController_Havoc::ServerReadyUpPlayerForNextWave's exec
// thunk 0x772650 (no parameters; _Validate at vtable +0x1068, body at
// +0x1070) is unique to the RPC. The body's effect is unknown (the game
// state's ClientUpdatePlayerReadyForNextWave is never sent), and the director
// waited the full m_upgradeSelectionDuration (60 s) regardless. The hook
// records the controller; the director ends upgrade selection early once
// every connected human has readied. GUESS: the original rule (all players?
// a vote?) is unknown.
#define RVA_HVC_READY_UP_THUNK 0x772650
static void HavocBroadcastReady(uint8_t *readyPc, bool ready);
tExecThunk g_origHavocReadyUpThunk = nullptr;
static void *volatile g_hvcReady[16];
static volatile LONG g_hvcReadyCount = 0;
static void __fastcall HookHavocReadyUpThunk(void *ctx, uint8_t *stack, void *result) {
  g_origHavocReadyUpThunk(ctx, stack, result);
  LONG n = g_hvcReadyCount;
  for (LONG i = 0; i < n && i < 16; ++i)
    if (g_hvcReady[i] == ctx)
      return;
  if (n < 16) {
    g_hvcReady[n] = ctx;
    InterlockedIncrement(&g_hvcReadyCount);
  }
  Logf("havoc: ServerReadyUpPlayerForNextWave from %p %s (%ld ready)", ctx, ClassNameOf(ctx), g_hvcReadyCount);
  __try {
    HavocBroadcastReady((uint8_t *)ctx, true);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("havoc: ready broadcast faulted");
  }
}

// Un-ready (upstream a36f046): ServerUnReadyPlayerForNextWave's exec thunk
// 0x774B40 (78 B, unique; body stub at PC vtable +0x1060 like ready-up's).
// Drops the controller from the ready list and re-sends flag + array.
#define RVA_HVC_UNREADY_THUNK 0x774B40
tExecThunk g_origHavocUnReadyThunk = nullptr;
static void __fastcall HookHavocUnReadyThunk(void *ctx, uint8_t *stack, void *result) {
  g_origHavocUnReadyThunk(ctx, stack, result);
  LONG n = g_hvcReadyCount;
  for (LONG i = 0; i < n && i < 16; ++i)
    if (g_hvcReady[i] == ctx) {
      g_hvcReady[i] = g_hvcReady[n - 1];
      InterlockedDecrement(&g_hvcReadyCount);
      break;
    }
  Logf("havoc: ServerUnReadyPlayerForNextWave from %p %s (%ld ready)", ctx, ClassNameOf(ctx), g_hvcReadyCount);
  __try {
    HavocBroadcastReady((uint8_t *)ctx, false);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("havoc: unready broadcast faulted");
  }
}

// Reflected property object `name` on obj's class chain, or null.
static UObjectMin *FindPropObj(UObjectMin *obj, const char *name) {
  if (!IsReadable(obj, sizeof(UObjectMin)))
    return nullptr;
  for (UObjectMin *cls = obj->Class; cls && IsReadable(cls, 0x40); cls = *(UObjectMin **)((uint8_t *)cls + OFF_USTRUCT_SUPER))
    for (UObjectMin *f = *(UObjectMin **)((uint8_t *)cls + 0x38); f && IsReadable(f, 0x78); f = *(UObjectMin **)((uint8_t *)f + 0x28)) {
      const char *fn = NameText(f->Name);
      if (fn && !strcmp(fn, name) && strcmp(ClassNameOf(f), "Function"))
        return f;
    }
  return nullptr;
}

// Ready display between waves. Stock clients show other players' ready state
// from two things this build never sends (operator 2026-10-14: each client
// saw only its own tick):
//  - AYPlayerReplicationInfo_Havoc.m_readyForNextWave, BoolProperty +0xA41,
//    size 1, flags 0x0020081140000220 (CPF_Net | CPF_RepNotify; OnRep_
//    ReadyForNextWaveChanged), headless 2026-10-16;
//  - GameState_Havoc ClientUpdatePlayerReadyForNextWave(
//    readyPlayersForNextWave: ArrayProperty @0), same run.
// Both are set/sent here. Disproved: "UE 4.13 layout, UBoolProperty
// FieldMask at UProperty+0x73, UArrayProperty::Inner at +0x70" -- +0x70..73
// read zeros (headless 2026-10-16 20:02), so both are now found by scanning.
// The array's Inner is IntProperty (20:10), not ObjectProperty. Client side:
// exec thunk 0x747B90 -> gs vt+0x898 = 0x369530 copies it to gs+0x2028 and
// 0x61BC00 fires the BP event OnPlayerReadyForNextWaveChanged(array) (FName
// global 0x3E107D8); no cooked Havoc widget found implementing it yet.
static void HavocSetReadyFlag(uint8_t *pri, bool ready) {
  UObjectMin *bp = FindPropObj((UObjectMin *)pri, "m_readyForNextWave");
  if (!bp || strcmp(ClassNameOf(bp), "BoolProperty"))
    return;
  uint32_t off = *(uint32_t *)((uint8_t *)bp + 0x50);
  // UBoolProperty {FieldSize, ByteOffset, ByteMask, FieldMask} follows
  // UProperty, whose size in this build is not pinned: +0x70/+0x71/+0x73
  // read all zeros (headless 2026-10-16 20:02; UProperty here has
  // Offset_Internal at +0x50, not 4.13's +0x44). Scan +0x60..+0x90 for the
  // first {1, <=7, m, m|0xFF} run and log the raw bytes once.
  uint8_t byteOff = 0, mask = 0;
  int at = -1;
  const uint8_t *q = (const uint8_t *)bp;
  if (IsReadable(q + 0x60, 0x34))
    for (int o = 0x60; o < 0x90; ++o)
      if (q[o] == 1 && q[o + 1] <= 7 && q[o + 2] && (q[o + 3] == q[o + 2] || q[o + 3] == 0xFF)) {
        byteOff = q[o + 1], mask = q[o + 3], at = o;
        break;
      }
  static int s_logged = 0;
  if (!s_logged++ && IsReadable(q + 0x60, 0x30))
    Logf("ready: m_readyForNextWave +0x%x byte +%u mask 0x%02x at prop+0x%x; prop+0x60: %016llx %016llx %016llx %016llx %016llx %016llx",
         off, byteOff, mask, at, *(unsigned long long *)(q + 0x60), *(unsigned long long *)(q + 0x68),
         *(unsigned long long *)(q + 0x70), *(unsigned long long *)(q + 0x78), *(unsigned long long *)(q + 0x80),
         *(unsigned long long *)(q + 0x88));
  // Found at prop+0x78 (headless 2026-10-16 20:18): {1, 0, ByteMask 1,
  // FieldMask 0xFF} = a native bool, so the whole byte is written 0/1.
  if (!mask || byteOff > 7)
    return;
  uint8_t *b = pri + off + byteOff;
  if (mask == 0xFF)
    *b = ready ? 1 : 0;
  else
    *b = ready ? (uint8_t)(*b | q[at + 2]) : (uint8_t)(*b & ~q[at + 2]);
}

static void HavocBroadcastReady(uint8_t *readyPc, bool ready) {
  uint8_t *pri = IsReadable(readyPc + OFF_PC_PLAYER_STATE, 8) ? *(uint8_t **)(readyPc + OFF_PC_PLAYER_STATE) : nullptr;
  void *world = ((tGetWorld)(*(void ***)readyPc)[VT_ACTOR_GET_WORLD / 8])(readyPc);
  uint8_t *gs = world && IsReadable((uint8_t *)world + OFF_WORLD_GAMESTATE, 8) ? *(uint8_t **)((uint8_t *)world + OFF_WORLD_GAMESTATE) : nullptr;
  if (!pri || !gs)
    return;
  HavocSetReadyFlag(pri, ready);
  // The array: PlayerStates of every readied controller.
  UObjectMin *fn = FindFnOnChain(gs, "ClientUpdatePlayerReadyForNextWave");
  UObjectMin *ap = fn ? *(UObjectMin **)((uint8_t *)fn + 0x38) : nullptr;
  // UArrayProperty::Inner sits right after UProperty -- same unpinned size
  // as above, so take the first pointer at +0x70/+0x78/+0x80 that is a
  // *Property object.
  UObjectMin *inner = nullptr;
  const char *innerType = "?";
  for (int o = 0x70; ap && o <= 0x80 && !inner; o += 8) {
    UObjectMin *c = IsReadable((uint8_t *)ap + o, 8) ? *(UObjectMin **)((uint8_t *)ap + o) : nullptr;
    const char *cn = c && IsReadable(c, sizeof(UObjectMin)) && IsReadable(c->Class, sizeof(UObjectMin)) ? ClassNameOf(c) : nullptr;
    if (cn && strstr(cn, "Property"))
      inner = c, innerType = cn;
  }
  // Inner is IntProperty (headless 2026-10-16 20:10), so the array carries
  // ints, not PlayerStates. GUESS: APlayerState::PlayerId of each readied
  // player -- the only per-player int a client can match against its PRIs.
  int sent = 0;
  if (ap && !strcmp(ClassNameOf(ap), "ArrayProperty") && (!strcmp(innerType, "ObjectProperty") || !strcmp(innerType, "IntProperty"))) {
    bool ints = !strcmp(innerType, "IntProperty");
    UObjectMin *pidp = ints ? FindPropObj((UObjectMin *)pri, "PlayerId") : nullptr;
    uint32_t pidOff = pidp ? *(uint32_t *)((uint8_t *)pidp + 0x50) : 0;
    void *list[16] = {};
    int32_t ids[16] = {};
    int n = 0;
    for (LONG k = 0; k < g_hvcReadyCount && k < 16; ++k) {
      uint8_t *c = (uint8_t *)g_hvcReady[k];
      uint8_t *ps = c && IsReadable(c + OFF_PC_PLAYER_STATE, 8) ? *(uint8_t **)(c + OFF_PC_PLAYER_STATE) : nullptr;
      if (!ps || (ints && (!pidOff || !IsReadable(ps + pidOff, 4))))
        continue;
      if (ints)
        ids[n++] = *(int32_t *)(ps + pidOff);
      else
        list[n++] = ps;
    }
    if (ints && n)
      Logf("havoc: ready ids: PlayerId +0x%x first %d", pidOff, ids[0]);
    struct { void *data; int32_t num, max; } arr = {ints ? (void *)ids : (void *)list, n, n}; // caller-owned; ProcessEvent does not free parms
    alignas(16) uint8_t parms[64] = {};
    memcpy(parms + *(uint32_t *)((uint8_t *)ap + 0x50), &arr, sizeof arr);
    void **vt = *(void ***)gs;
    ((tProcessEventVirt)vt[VT_PROCESS_EVENT / 8])(gs, fn, parms);
    sent = n;
  }
  Logf("havoc: ready %d for %p: flag set, array (%s) sent with %d player(s)", ready, pri, innerType, sent);
}

// Havoc next-wave countdown. AYGameState_Havoc::ClientUpdateTimeToNextWave
// (int; thunk 0x747CD0 -> GS vtable +0x890 = 0x369580) feeds the upgrade
// menu's wave info (0x5598D0 -> UI_HUD_HavocProgressionMenu_WaveInfo_WBP
// BP_SetTimeUntilNextWave: "wave begins in {seconds}") and the controller.
// The widget has no timer of its own -- it shows the number it is given and
// hides the line on -1 (BP_GetTimeLeftHideTimerValue 0x2A1FC50 = `or eax, -1`)
// -- so the server sent it every second. Never sent before: no countdown.
// GUESS: whether the original counted to WaveStarting (here) or to the
// enemies' arrival (+m_waveStartDelayDuration) is unknown.
static void FillTimeToNextWave(uint8_t *parms, const char *pc, const char *, uint32_t off, void *ctx) {
  if (!strcmp(pc, "IntProperty"))
    *(int32_t *)(parms + off) = *(int *)ctx;
}
static void HavocSendTimeToNextWave(uint8_t *gs, int secs) {
  static int s_last = -1;
  if (secs < -1)
    secs = 0;
  if (secs == s_last)
    return;
  bool first = s_last == -1;
  s_last = secs;
  bool ok = CallNamed(gs, L"ClientUpdateTimeToNextWave", FillTimeToNextWave, &secs);
  if (first || secs == -1 || !ok)
    Logf("havoc: next-wave countdown -> %d s%s", secs, ok ? "" : " -- ClientUpdateTimeToNextWave NOT SENT");
}

// Havoc end-of-match rewards list. The client's Havoc end-of-match flow
// (UYEndOfMatchManager::StartEndOfMatchFlow 0x454610 adds the pending step
// "ApplyHavocProgressionData" for game mode 17) runs 0x449470, which waits
// for TWO things, binding a delegate and returning while either is missing:
//   - the PRI match score (+0xA44 != -1): ClientUpdateHighscore, sent at
//     game over (HavocReportRuns);
//   - the game state's reward list (+0x20D0, getter 0x36E940: TArray of 12-byte
//     entries, an "available" bool at +0x20E0, a delegate at +0x20E8) --
//     "PlayerScoreData not yet available. Bind function call to delegate."
// Only AYGameState_Havoc::ClientUpdateRewards (thunk 0x747C30 -> GS vtable
// +0x8A0 -> 0x373E40) fills it and sets "available", and nothing in this exe
// sends it, so the step never completed: the end-of-match scene showed the
// captain and stopped there. Its parameter is TArray<YHavocRewardDataTableRow>
// (0x18 bytes, registration 0x9311F0: m_highscore int +0x08, m_rewardType
// EYHavocRewardType byte +0x0C, m_value int +0x10) -- the DN_HavocRewards_DT
// rows, which the server loaded from m_rewardsDataTableCfgPath
// (DefaultHavoc.ini, an mmogbrain .cfg). 0x373E40 converts m_rewardType 0 and
// 1 only (YHRT_FREEXP, YHRT_CREDITS, the first two names of the enum and the
// only two types the table uses). The screen walks the list in order against
// the weekly best and the match score, so it goes lowest score first.
// The function is an UNRELIABLE multicast (flags 0x84C40, no NetReliable), so
// it is sent at game over and twice more before EndMatch.
// GUESS: when the original server sent it (match start?) is unknown.
static const struct { int32_t highscore; uint8_t type; int32_t value; } kHavocRewardRows[] = {
    // DN_HavocRewards_DT: Beginner, 2, 3, 4, 5, 6, 7 (0 = YHRT_FREEXP, 1 = YHRT_CREDITS)
    {1000, 0, 500}, {1500, 1, 750}, {2500, 1, 1000}, {4500, 0, 2000},
    {8500, 1, 2500}, {15000, 0, 5000}, {25000, 1, 7500},
};
#define HVC_REWARD_ROW_SIZE 0x18
static void FillHavocRewards(uint8_t *parms, const char *pc, const char *, uint32_t off, void *ctx) {
  if (strcmp(pc, "ArrayProperty"))
    return;
  int n = (int)(sizeof kHavocRewardRows / sizeof kHavocRewardRows[0]);
  *(void **)(parms + off) = ctx;
  *(int32_t *)(parms + off + 8) = n;
  *(int32_t *)(parms + off + 12) = n;
}
// Checks the row layout above against reflection once (ArrayProperty Inner
// +0x78 -> StructProperty Struct +0x78 -> fields, as the team-info struct
// was found); false = do not send.
static bool HavocRewardRowLayoutOK(uint8_t *gs) {
  static int s_ok = -1;
  if (s_ok >= 0)
    return s_ok == 1;
  s_ok = 0;
  void *fn = FindFnOnChain(gs, "ClientUpdateRewards");
  UObjectMin *arr = fn && IsReadable((uint8_t *)fn + 0x40, 8) ? *(UObjectMin **)((uint8_t *)fn + 0x38) : nullptr;
  UObjectMin *inner = arr && IsReadable((uint8_t *)arr + 0x80, 8) ? *(UObjectMin **)((uint8_t *)arr + 0x78) : nullptr;
  UObjectMin *st = inner && IsReadable((uint8_t *)inner + 0x80, 8) ? *(UObjectMin **)((uint8_t *)inner + 0x78) : nullptr;
  int hs = -1, ty = -1, va = -1;
  if (st && IsReadable(st, 0x40) && !strcmp(ClassNameOf(st), "ScriptStruct"))
    for (UObjectMin *f = *(UObjectMin **)((uint8_t *)st + 0x38); f && IsReadable(f, 0x58); f = *(UObjectMin **)((uint8_t *)f + 0x28)) {
      const char *fnm = NameText(f->Name);
      int off = (int)*(uint32_t *)((uint8_t *)f + 0x50);
      if (fnm && !strcmp(fnm, "m_highscore")) hs = off;
      else if (fnm && !strcmp(fnm, "m_rewardType")) ty = off;
      else if (fnm && !strcmp(fnm, "m_value")) va = off;
    }
  s_ok = hs == 0x08 && ty == 0x0C && va == 0x10 && inner && IsReadable((uint8_t *)inner + 0x34, 4) &&
         *(int32_t *)((uint8_t *)inner + 0x34) == HVC_REWARD_ROW_SIZE;
  Logf("havoc: YHavocRewardDataTableRow by reflection (%s %s, %s): element size %d, m_highscore +0x%X, "
       "m_rewardType +0x%X, m_value +0x%X -> %s", arr ? ClassNameOf(arr) : "?", arr ? NameText(arr->Name) : "?",
       st ? NameText(st->Name) : "?", inner && IsReadable((uint8_t *)inner + 0x34, 4) ? *(int32_t *)((uint8_t *)inner + 0x34) : -1,
       hs, ty, va, s_ok ? "OK" : "MISMATCH, not sent");
  return s_ok == 1;
}

static void HavocSendRewards(uint8_t *gs, const char *why) {
  if (!HavocRewardRowLayoutOK(gs))
    return;
  static uint8_t s_rows[sizeof kHavocRewardRows / sizeof kHavocRewardRows[0]][HVC_REWARD_ROW_SIZE];
  for (size_t i = 0; i < sizeof kHavocRewardRows / sizeof kHavocRewardRows[0]; ++i) {
    memset(s_rows[i], 0, HVC_REWARD_ROW_SIZE);
    *(int32_t *)(s_rows[i] + 0x08) = kHavocRewardRows[i].highscore;
    s_rows[i][0x0C] = kHavocRewardRows[i].type;
    *(int32_t *)(s_rows[i] + 0x10) = kHavocRewardRows[i].value;
  }
  bool ok = CallNamed(gs, L"ClientUpdateRewards", FillHavocRewards, s_rows);
  uint8_t *avail = gs + 0x20E0;
  Logf("havoc: reward list (%d DN_HavocRewards_DT rows) -> clients (%s)%s; host copy available %d",
       (int)(sizeof kHavocRewardRows / sizeof kHavocRewardRows[0]), why, ok ? "" : " -- ClientUpdateRewards NOT SENT",
       IsReadable(avail, 1) ? *avail : -1);
}

// Wave start: clear every player's ready flag and tell clients the list is
// empty again.
static void HavocClearReady(uint8_t *gs) {
  if (!IsReadable(gs + OFF_GS_PLAYER_ARRAY, 16))
    return;
  uint8_t **arr = *(uint8_t ***)(gs + OFF_GS_PLAYER_ARRAY);
  int count = *(int32_t *)(gs + OFF_GS_PLAYER_ARRAY + 8);
  for (int i = 0; count > 0 && count <= 256 && IsReadable(arr, (size_t)count * 8) && i < count; ++i)
    if (IsReadable(arr[i], 0xA48))
      HavocSetReadyFlag(arr[i], false);
  UObjectMin *fn = FindFnOnChain(gs, "ClientUpdatePlayerReadyForNextWave");
  UObjectMin *ap = fn ? *(UObjectMin **)((uint8_t *)fn + 0x38) : nullptr;
  if (ap && !strcmp(ClassNameOf(ap), "ArrayProperty")) {
    alignas(16) uint8_t parms[64] = {}; // empty TArray
    void **vt = *(void ***)gs;
    ((tProcessEventVirt)vt[VT_PROCESS_EVENT / 8])(gs, fn, parms);
  }
}

// Match ready-up (pre-match lineup toggle). 2026-10-14 operator Havoc match
// 21:36: each client's own card showed its tick, the other player's an X, and
// the match began on its timer (21:38:19) -- the battle log has no ready-up
// line at all. AYPlayerController::ServerPlayerReadyUpForMatch's exec thunk is
// 0x772380 (.pdata ENTRY 0x772380-0x772436): reads one bool parm, _Validate at
// vtable +0xEC0 (fail -> "ServerPlayerReadyUpForMatch_Validate" 0x772403),
// body at vtable +0xEC8. DIAGNOSTIC ONLY: logs every call with the body's RVA
// so the next match shows whether the RPC reaches the host and what runs.
tExecThunk g_origReadyUpForMatchThunk = nullptr;
static void __fastcall HookReadyUpForMatchThunk(void *ctx, uint8_t *stack, void *result) {
  void **vt = ctx && IsReadable(ctx, 8) ? *(void ***)ctx : nullptr;
  uintptr_t body = vt && IsReadable(vt + 0xEC8 / 8, 8) ? (uintptr_t)vt[0xEC8 / 8] : 0;
  uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
  g_origReadyUpForMatchThunk(ctx, stack, result);
  Logf("ready: ServerPlayerReadyUpForMatch from %p %s, body RVA 0x%llX", ctx, ClassNameOf(ctx),
       body ? (unsigned long long)(body - base) : 0ULL);
  // Body 0x592210 (logged live 21:43, Glacier): world (vt +0x108) +0x58 cast
  // YGameState_MP -> gs vt +0x7A8 / +0x7B8; world +0xF0 cast
  // YGameMode_Multiplayer; if gs vt +0x7D8 returns true it stores the bool at
  // PC+0xC44 and calls gm vt +0x960(PC). Other clients still see the player
  // unready, so log which arm ran and every "*eady*" property afterwards.
  // FOUND (headless Havoc 23:21): the gate byte gs+0x1C3C is the reflected
  // m_useWaveSpawning, copied from the game mode's m_useWaveSpawning (gm+0x899)
  // by 0x379D60; both are 0 in GameMode_Havoc_BP. So without wave spawning the
  // body drops the ready on purpose -- PC+0xC44 is never set, gm vt+0x960 never
  // runs, the lineup ends on its timer, and only the clicking client shows its
  // own tick. Stock game-mode data, not missing server state; forcing the flag
  // would also switch the spawn rules it is named for. Not checked: the value
  // in the Glacier mode (its 21:43 calls replicated nothing either).
  uint8_t *pc = (uint8_t *)ctx;
  if (!IsReadable(pc, 0xC48))
    return;
  void *world = ((tGetWorld)(*(void ***)pc)[VT_ACTOR_GET_WORLD / 8])(pc);
  uint8_t *gs = world && IsReadable((uint8_t *)world + 0xF8, 8) ? *(uint8_t **)((uint8_t *)world + OFF_WORLD_GAMESTATE) : nullptr;
  uint8_t *gm = world && IsReadable((uint8_t *)world + 0xF8, 8) ? *(uint8_t **)((uint8_t *)world + 0xF0) : nullptr;
  auto vrva = [&](uint8_t *o, int off) -> unsigned long long {
    if (!IsReadable(o, 8) || !IsReadable(*(void ***)o + off / 8, 8))
      return 0;
    return (unsigned long long)((uintptr_t)(*(void ***)o)[off / 8] - base);
  };
  Logf("ready:   PC+0xC44 now %u; gs %p %s vt+0x7A8 0x%llX +0x7D8 0x%llX; gm %p %s vt+0x960 0x%llX",
       pc[0xC44], gs, gs ? ClassNameOf(gs) : "-", vrva(gs, 0x7A8),
       vrva(gs, 0x7D8), gm, gm ? ClassNameOf(gm) : "-", vrva(gm, 0x960));
  // The arm that stores PC+0xC44 is gated by gs vt+0x7D8 = 0x39E3D0, which is
  // just `movzx eax, byte [gs+0x1C3C]`. That byte is zeroed by the GameState
  // ctor (0x388D62) and copied from gm+0x899 by the YGameMode_Multiplayer
  // function at 0x379D60 (+0xF5); headless 22:07 left PC+0xC44 at 0, so log
  // both bytes and which reflected property (if any) owns them.
  if (gs && gm && IsReadable(gs + 0x1C3C, 1) && IsReadable(gm + 0x899, 1))
    Logf("ready:   gate gs+0x1C3C=%u (%s) gm+0x899=%u (%s) gs+0x569=%u", gs[0x1C3C], PropNameAt((UObjectMin *)gs, 0x1C3C),
         gm[0x899], PropNameAt((UObjectMin *)gm, 0x899), gs[0x569]);
  static const char *const kReady[] = {"*eady*"};
  LogProps("ready:   pc", (UObjectMin *)pc, kReady, 1);
  if (IsReadable(pc + OFF_PC_PLAYER_STATE, 8))
    LogProps("ready:   pri", *(UObjectMin **)(pc + OFF_PC_PLAYER_STATE), kReady, 1);
  if (gs)
    LogProps("ready:   gs", (UObjectMin *)gs, kReady, 1);
}

// Connected humans (player controllers with a net connection), as PlayersTick
// counts them; -1 when the player array is unreadable.
static int HavocHumanCount(uint8_t *gs, int *readyOut) {
  *readyOut = 0;
  if (!IsReadable(gs + OFF_GS_PLAYER_ARRAY, 16))
    return -1;
  uint8_t **arr = *(uint8_t ***)(gs + OFF_GS_PLAYER_ARRAY);
  int count = *(int32_t *)(gs + OFF_GS_PLAYER_ARRAY + 8);
  if (count < 0 || count > 256 || (count && !IsReadable(arr, (size_t)count * 8)))
    return -1;
  int humans = 0;
  for (int i = 0; i < count; ++i) {
    uint8_t *pri = arr[i];
    if (!IsReadable(pri, OFF_ACTOR_OWNER + 8))
      continue;
    uint8_t *pc = *(uint8_t **)(pri + OFF_ACTOR_OWNER);
    if (!IsReadable(pc, OFF_PC_NETCONNECTION + 8) || !ClassChainContains((UObjectMin *)pc, "PlayerController") ||
        !*(void **)(pc + OFF_PC_NETCONNECTION))
      continue;
    ++humans;
    for (LONG j = 0; j < g_hvcReadyCount && j < 16; ++j)
      if (g_hvcReady[j] == pc) {
        ++*readyOut;
        break;
      }
  }
  return humans;
}

// ---------------------------------------------------------------------------
// Havoc upgrade offers. Server side of the between-wave upgrade panel, all of
// it compiled out of this build:
//  - offer: AYPlayerController_Havoc::ClientReceiveRandSelectedUpgrades
//    (TArray<FName>) -- its RPC wrapper 0x7463E0 has no callers. The client
//    side is intact: _Implementation 0x3F25F0 hands the names to
//    UYHavocProgressionComponent::StoreSelectedUpgradesReceivedFromServer
//    0x3AAAE0 (PC +0x1148, slots at comp +0x30) and the HUD's
//    PrepareUpgradesForUI 0x37B2D0 loads them for UI_HUD_HavocProgressionMenu.
//  - pick: the panel's ActivateSelectedUpgrade (0x448240) checks m_currentXP
//    >= the upgrade's cost, then SendSelectedUpgrade 0x405900 turns the FName
//    back into its slot (0x39B800) and calls ServerReceiveSelectedUpgradeIndex
//    (int), whose _Implementation 0x406040 only logs "Selected slot index is
//    %d" -- hooked below.
//  - apply: UYHavocProgressionComponent::AddNewBoost and
//    OnApplyBoostAndModifiersData (0x3A25C0) only log; the boost's m_feats go
//    onto the ship here with the game's own UYFeatsComponent::AddFeat (as the
//    Turbo TDM modifiers do), again on every respawned ship.
// Boost rows: data/datatables/Progression/Havoc/DN_HavocBoosts_DT.json (row
// name, m_feats, m_cost, m_weight, m_category), verbatim. Five rows' m_feats
// differ from their row name (typos in the table, kept). Feat definitions:
// DN_Feats_Havoc_Boosts_Shared_OTS_DT.
// GUESS: the selection rule is unknown. One boost per category (offensive /
// defensive / utility), weighted by m_weight (weight-0 rows never offered);
// each slot can be bought once per selection phase. Loadout (ship tier)
// upgrades from DN_HavocLoadouts_DT are NOT offered yet.
// NOT verified against a live client: the panel showing the offer, the
// purchase result, the feats replicating to the client's ship.
struct HavocBoost {
  const wchar_t *id, *feat;
  int cost, weight;
  char cat; // 'O' offensive, 'D' defensive, 'U' utility
};
static const HavocBoost kHavocBoosts[] = {
    {L"HVC_Boost_Def_HpInc_Large", L"HVC_Boost_Def_HpInc_Large", 1000, 2, 'D'},
    {L"HVC_Boost_Def_HpInc_Small", L"HVC_Boost_Def_HpInc_Small", 300, 2, 'D'},
    {L"HVC_Boost_Def_HpInc_Medium", L"HVC_Boost_Def_HpInc_Medium", 600, 5, 'D'},
    {L"HCV_Boost_Def_RegtimeDec_Medium", L"HVC_Boost_Def_HPRegtimeDec_Medium", 600, 5, 'D'},
    {L"HVC_Boost_Def_HPRegInc_Small", L"HVC_Boost_Def_HPRegInc_Small", 350, 2, 'D'},
    {L"HVC_Boost_Def_HPRegInc_Medium", L"HVC_Boost_Def_HPRegInc_Medium", 600, 5, 'D'},
    {L"HVC_Boost_Def_HPRegInc_Large", L"HVC_Boost_Def_HPRegInc_Large", 1000, 2, 'D'},
    {L"HVC_Boost_Def_ArInc_Small", L"HVC_Boost_Def_ArInc_Small", 350, 2, 'D'},
    {L"HVC_Boost_Def_ArInc_Medium", L"HVC_Boost_Def_ArInc_Medium", 600, 5, 'D'},
    {L"HVC_Boost_Def_ArInc_Large", L"HVC_Boost_Def_ArInc_Large", 1000, 2, 'D'},
    {L"HVC_Boost_DefArIncFr_Medium", L"HVC_Boost_Def_ArIncFr_Medium", 600, 0, 'D'},
    {L"HVC_Boost_Def_ArIncSid_Medium", L"HVC_Boost_Def_ArIncSid_Medium", 600, 0, 'D'},
    {L"HVC_Boost_Def_ArincBk_Medium", L"HVC_Boost_Def_ArIncBk_Medium", 600, 0, 'D'},
    {L"HVC_Boost_Def_ShieldEnDec_Medium", L"HVC_Boost_Def_ShieldEnDec_Medium", 600, 5, 'D'},
    {L"HVC_Boost_Off_DmgInc_Small", L"HVC_Boost_Off_DmgInc_Small", 350, 2, 'O'},
    {L"HVC_Boost_Off_DmgInc_Medium", L"HVC_Boost_Off_DmgInc_Medium", 600, 5, 'O'},
    {L"HVC_Boost_Off_DmgInc_Large", L"HVC_Boost_Off_DmgInc_Large", 1000, 2, 'O'},
    {L"HVC_Boost_Off_AbiInc_Large", L"HVC_Boost_Off_AbiInc_Large", 1000, 2, 'O'},
    {L"HVC_Boost_Off_AbiInc_Small", L"HVC_Boost_Off_AbiInc_Small", 350, 2, 'O'},
    {L"HVC_Boost_Off_AbiInc_Medium", L"HVC_Boost_Off_AbiInc_Medium", 600, 5, 'O'},
    {L"HVC_Boost_Off_RldTDec_Medium", L"HVC_Boost_Off_RldTDec_Medium", 600, 5, 'O'},
    {L"HVC_Boost_Off_WpnEnDec", L"HVC_Boost_Off_WpnEnDec", 600, 5, 'O'},
    {L"HVC_Boost_Off_WpnEnRelDec_Medium", L"HVC_Boost_Off_WpnEnRelDec_Medium", 600, 2, 'D'},
    {L"HVC_Boost_Def_ThrustEnArmInc_Medium", L"HVC_Boost_Def_ThrustEnArmInc_Medium", 600, 2, 'D'},
    {L"HVC_Boost_Util_EnInc_Large", L"HVC_Boost_Util_EnInc_Large", 1000, 2, 'U'},
    {L"HVC_Boost_Util_EnInc_Medium", L"HVC_Boost_Util_EnInc_Medium", 600, 5, 'U'},
    {L"HVC_Boost_Util_EnInc_Small", L"HVC_Boost_Util_EnInc_Small", 350, 2, 'U'},
    {L"HVC_Boost_Util_SpdInc_Small", L"HVC_Boost_Util_SpdInc_Small", 350, 2, 'U'},
    {L"HVC_Boost_Util_SpdInc_Medium", L"HVC_Boost_Util_SpdInc_Medium", 600, 5, 'U'},
    {L"HVC_Boost_Util_SpdInc_Large", L"HVC_Boost_Util_SpdInc_Large", 1000, 2, 'U'},
    {L"HVC_Boost_Util_SpdIncFwd_Large", L"HVC_Boost_Util_SpdIncFwd_Large", 1000, 2, 'U'},
    {L"HVC_Boost_Util_SpdIncStr_Large", L"HVC_Boost_Util_SpdIncStr_Large", 1000, 2, 'U'},
    {L"HVC_Boost_Util_SpdIncVert_Large", L"HVC_Boost_Util_SpdIncVert_Large", 1000, 2, 'U'},
    {L"HVC_Boost_Util_SpdIncBk_Large", L"HVC_Boost_Util_SpdIncBk_Large", 1000, 2, 'U'},
    {L"HVC_Boost_Util_EnRegInc_Medium", L"HVC_Boost_Util_EnReginc_Medium", 600, 5, 'U'},
    {L"HCV_Boost_Util_EnThrustSpdInc_Medium", L"HVC_Boost_Util_EnThrustSpdInc_Medium", 600, 2, 'D'},
    {L"HVC_Boost_Util_AbiTDec_Medium", L"HVC_Boost_Util_AbiTDec_Medium", 600, 5, 'U'},
    {L"HVC_Boost_Util_ThrustEnDec", L"HVC_Boost_Util_ThrustEnDec", 600, 5, 'U'},
};
static const int kHavocBoostCount = (int)(sizeof kHavocBoosts / sizeof kHavocBoosts[0]);

struct HavocPlayer {
  void *pc;
  int offer[3];     // kHavocBoosts index, -1 = none / already bought
  int offerShip[3]; // kHavocShips index on a ship-upgrade wave, else -1
  FNameMin offerNames[3];
  int owned[128], nOwned;
  void *appliedPawn;
  int nApplied;
};
static HavocPlayer g_hvcPlayers[16];

static HavocPlayer *HavocPlayerFor(void *pc, bool create) {
  for (HavocPlayer &p : g_hvcPlayers)
    if (p.pc == pc)
      return &p;
  if (!create)
    return nullptr;
  for (HavocPlayer &p : g_hvcPlayers)
    if (!p.pc) {
      memset(&p, 0, sizeof p);
      p.pc = pc;
      p.offer[0] = p.offer[1] = p.offer[2] = -1;
      p.offerShip[0] = p.offerShip[1] = p.offerShip[2] = -1;
      return &p;
    }
  return nullptr;
}

// Calls a UFunction by name, letting `fill` write each parameter by its
// property class and offset; logs the parameter list the first few times.
typedef void (*tParmFill)(uint8_t *parms, const char *propClass, uint32_t off, void *ctx);
static bool CallFilled(void *obj, const wchar_t *name, tParmFill fill, void *ctx) {
  if (!IsReadable(obj, sizeof(UObjectMin)))
    return false;
  FNameMin n = {};
  ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&n, name, 1 /* FNAME_Add */);
  void *fn = FindFunctionSafe(obj, *(uint64_t *)&n);
  if (!fn || !IsReadable((uint8_t *)fn + 0x40, 8))
    return false;
  alignas(16) uint8_t parms[256] = {};
  char desc[160] = "";
  int o = 0;
  UObjectMin *pp = *(UObjectMin **)((uint8_t *)fn + 0x38);
  for (int j = 0; pp && j < 8 && IsReadable(pp, 0x58); ++j) {
    uint32_t off = *(uint32_t *)((uint8_t *)pp + 0x50);
    const char *pc = ClassNameOf(pp);
    if (fill && off < sizeof parms - 16)
      fill(parms, pc, off, ctx);
    const char *pn = NameText(pp->Name);
    o += snprintf(desc + o, sizeof desc - o > 0 ? sizeof desc - o : 0, " %s:%s@%x", pn ? pn : "?", pc, off);
    pp = *(UObjectMin **)((uint8_t *)pp + 0x28);
  }
  void **vt = *(void ***)obj;
  ((tProcessEventVirt)vt[VT_PROCESS_EVENT / 8])(obj, fn, parms);
  static int s_logged = 0;
  if (s_logged++ < 12)
    Logf("havoc: called %ls on %s; parms%s", name, ClassNameOf(obj), desc[0] ? desc : " (none)");
  return true;
}

static void FillBoolTrue(uint8_t *parms, const char *pc, uint32_t off, void *) {
  if (!strcmp(pc, "BoolProperty"))
    parms[off] = 1;
}

static void FillNameArray(uint8_t *parms, const char *pc, uint32_t off, void *ctx) {
  if (!strcmp(pc, "ArrayProperty")) {
    HavocPlayer *p = (HavocPlayer *)ctx;
    *(void **)(parms + off) = p->offerNames;
    *(int32_t *)(parms + off + 8) = 3;
    *(int32_t *)(parms + off + 12) = 3;
  }
}
// ClientReceiveBoostPurchaseResult(FName purchasedBoostID @0, bool isSuccessful
// @8) -- parameter list read at runtime 2026-10-14.
struct HavocPurchaseResult { FNameMin id; uint8_t ok; };
static void FillPurchaseResult(uint8_t *parms, const char *pc, uint32_t off, void *ctx) {
  HavocPurchaseResult *r = (HavocPurchaseResult *)ctx;
  if (!strcmp(pc, "NameProperty"))
    memcpy(parms + off, &r->id, sizeof r->id);
  else if (!strcmp(pc, "BoolProperty"))
    parms[off] = r->ok;
}

static int HavocPickBoost(char cat) {
  static uint32_t s_rng = 0;
  if (!s_rng)
    s_rng = GetTickCount() | 1;
  int total = 0;
  for (const HavocBoost &b : kHavocBoosts)
    if (b.cat == cat)
      total += b.weight;
  if (total <= 0)
    return -1;
  s_rng ^= s_rng << 13, s_rng ^= s_rng >> 17, s_rng ^= s_rng << 5;
  int r = (int)(s_rng % (uint32_t)total);
  for (int i = 0; i < kHavocBoostCount; ++i)
    if (kHavocBoosts[i].cat == cat && (r -= kHavocBoosts[i].weight) < 0)
      return i;
  return -1;
}

static uint32_t HavocRand();
// ---------------------------------------------------------------------------
// Havoc ship tier upgrades (stage 3, 2026-10-19; dn_host_no_havoc_ship_upgrades
// .txt turns them off).
//
// Offer. AYGameState_Havoc::PrepareUpgradesForUI (0x37B2D0) asks the data
// manager IsWaveWithLoadoutUpgrades(m_currentWaveIndex) (0x3A0830: the wave is
// one of the loadout rows' m_waveAppearances -- 3, 6, 12); on such a wave it
// reads every offered name as a DN_HavocLoadouts_DT ROW (GetLoadoutByID
// 0x39A1B0) instead of a boost, and shows the ship. The cost the panel checks
// (upgrade data +0x1A8, ActivateSelectedUpgrade 0x448240) is filled from
// m_cost for a boost and left 0 for a ship: a ship upgrade costs no XP.
// GUESS: one ship per category (offensive / defensive / utility) from the rows
// that appear on this wave, the slot of the player's own line holding its next
// tier (m_loadoutItemIDNextTier of the row whose item id is the active
// loadout's m_precastLoadoutID).
//
// Swap. AYPlayerController::SpawnPlayer (0x5998A0) has a Havoc path: with the
// last pawn weak pointer (PC +0xBB0) valid and PC +0xCA0 clear, it spawns the
// ACTIVE loadout (loadout manager *(*(PC+0x958)+0x28), +0x208) at that pawn's
// transform and destroys it ("GameState is Havoc. Respawn location should be
// the last known pawn position before it get's destroyed!"). Its own caller
// (ServerSpawnNearActorPostLoadPawnWork 0x592C40) sets the pending flag PC
// +0xB88 first. So the pick: register the ship's loadout with the player's
// manager (its class from ItemIDRegister, as the precasts are), activate it
// (ActivateLoadout 0x336C90), point +0xBB0 at the current ship, set +0xB88,
// call SpawnPlayer. Boosts and modifiers follow onto the new ship (their
// per-pawn bookkeeping sees a new pawn).
struct HavocShip {
  const wchar_t *row;
  int itemId, nextTierId, wave;
  char cat; // 'O' offensive, 'D' defensive, 'U' utility
  const char *asset;
  const wchar_t *path;
};
// DN_HavocLoadouts_DT, verbatim (generated 2026-10-19), with each m_loadoutItemID's
// asset from ItemIDRegister: row, item id, next-tier item id, m_waveAppearances
// (0 = "none", the starting T2 ships), category, asset name, class path.
static const HavocShip kHavocShips[] = {
    {L"HVCDEFDN_M(T2)", 33489354, 33489355, 0, 'D', "VH_HVC_DEF_DreadM_Loadout_T2_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_DEF_DreadM_Loadout_T2_BP.VH_HVC_DEF_DreadM_Loadout_T2_BP_C"},
    {L"HVCDEFSU_M(T2)", 33489363, 33489364, 0, 'D', "VH_HVC_DEF_SupportM_Loadout_T2_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_DEF_SupportM_Loadout_T2_BP.VH_HVC_DEF_SupportM_Loadout_T2_BP_C"},
    {L"HVCUTIAS_M(T2)", 33489370, 33489367, 0, 'U', "VH_HVC_UTI_AssaultM_Loadout_T2_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_UTI_AssaultM_Loadout_T2_BP.VH_HVC_UTI_AssaultM_Loadout_T2_BP_C"},
    {L"HVCOFFSC_L(T2)", 33489386, 33489387, 0, 'O', "VH_HVC_OFF_ScoutL_Loadout_T2_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_OFF_ScoutL_Loadout_T2_BP.VH_HVC_OFF_ScoutL_Loadout_T2_BP_C"},
    {L"HVCOFFSN_M(T2)", 33489394, 33489395, 0, 'O', "VH_HVC_OFF_SniperM_Loadout_T2_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_OFF_SniperM_Loadout_T2_BP.VH_HVC_OFF_SniperM_Loadout_T2_BP_C"},
    {L"HVCDEFAS_H(T3)", 33489432, 33489349, 3, 'D', "VH_HVC_DEF_AssaultH_Loadout_T3_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_DEF_AssaultH_Loadout_T3_BP.VH_HVC_DEF_AssaultH_Loadout_T3_BP_C"},
    {L"HVCDEFDN_M(T3)", 33489355, 33489356, 3, 'D', "VH_HVC_DEF_DreadM_Loadout_T3_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_DEF_DreadM_Loadout_T3_BP.VH_HVC_DEF_DreadM_Loadout_T3_BP_C"},
    {L"HVCDEFSU_M(T3)", 33489364, 33489365, 3, 'D', "VH_HVC_DEF_SupportM_Loadout_T3_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_DEF_SupportM_Loadout_T3_BP.VH_HVC_DEF_SupportM_Loadout_T3_BP_C"},
    {L"HVCUTIAS_M(T3)", 33489367, 33489368, 3, 'U', "VH_HVC_UTI_AssaultM_Loadout_T3_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_UTI_AssaultM_Loadout_T3_BP.VH_HVC_UTI_AssaultM_Loadout_T3_BP_C"},
    {L"HVCUTISU_L(T3)", 33489374, 33489371, 3, 'U', "VH_HVC_UTI_SupportL_Loadout_T3_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_UTI_SupportL_Loadout_T3_BP.VH_HVC_UTI_SupportL_Loadout_T3_BP_C"},
    {L"HVCUTISN_L(T3)", 33489382, 33489381, 3, 'U', "VH_HVC_UTI_SniperL_Loadout_T3_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_UTI_SniperL_Loadout_T3_BP.VH_HVC_UTI_SniperL_Loadout_T3_BP_C"},
    {L"HVCUTISC_H(T3)", 33489383, 33489384, 3, 'U', "VH_HVC_UTI_ScoutH_Loadout_T3_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_UTI_ScoutH_Loadout_T3_BP.VH_HVC_UTI_ScoutH_Loadout_T3_BP_C"},
    {L"HVCOFFSC_L(T3)", 33489387, 33489388, 3, 'O', "VH_HVC_OFF_ScoutL_Loadout_T3_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_OFF_ScoutL_Loadout_T3_BP.VH_HVC_OFF_ScoutL_Loadout_T3_BP_C"},
    {L"HVCOFFSC_M(T3)", 33489390, 33489391, 3, 'O', "VH_HVC_OFF_ScoutM_Loadout_T3_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_OFF_ScoutM_Loadout_T3_BP.VH_HVC_OFF_ScoutM_Loadout_T3_BP_C"},
    {L"HVCOFFSN_M(T3)", 33489395, 33489396, 3, 'O', "VH_HVC_OFF_SniperM_Loadout_T3_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_OFF_SniperM_Loadout_T3_BP.VH_HVC_OFF_SniperM_Loadout_T3_BP_C"},
    {L"HVCOFFDN_L(T3)", 33489404, 33489403, 3, 'O', "VH_HVC_OFF_DreadL_Loadout_T3_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_OFF_DreadL_Loadout_T3_BP.VH_HVC_OFF_DreadL_Loadout_T3_BP_C"},
    {L"HVCDEFAS_H(T4)", 33489349, 33489350, 6, 'D', "VH_HVC_DEF_AssaultH_Loadout_T4_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_DEF_AssaultH_Loadout_T4_BP.VH_HVC_DEF_AssaultH_Loadout_T4_BP_C"},
    {L"HVCDEFAS_M(T3)", 33489351, 33489352, 3, 'D', "VH_HVC_DEF_AssaultM_Loadout_T3_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_DEF_AssaultM_Loadout_T3_BP.VH_HVC_DEF_AssaultM_Loadout_T3_BP_C"},
    {L"HVCDEFAS_M(T4)", 33489352, 33489353, 6, 'D', "VH_HVC_DEF_AssaultM_Loadout_T4_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_DEF_AssaultM_Loadout_T4_BP.VH_HVC_DEF_AssaultM_Loadout_T4_BP_C"},
    {L"HVCDEFDN_M(T4)", 33489356, 33489357, 6, 'D', "VH_HVC_DEF_DreadM_Loadout_T4_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_DEF_DreadM_Loadout_T4_BP.VH_HVC_DEF_DreadM_Loadout_T4_BP_C"},
    {L"HVCDEFDN_H(T4)", 33489358, 33489359, 6, 'D', "VH_HVC_DEF_DreadH_Loadout_T4_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_DEF_DreadH_Loadout_T4_BP.VH_HVC_DEF_DreadH_Loadout_T4_BP_C"},
    {L"HVCDEFSU_H(T4)", 33489361, 33489362, 6, 'D', "VH_HVC_DEF_SupportH_Loadout_T4_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_DEF_SupportH_Loadout_T4_BP.VH_HVC_DEF_SupportH_Loadout_T4_BP_C"},
    {L"HVCDEFSU_M(T4)", 33489365, 33489366, 6, 'D', "VH_HVC_DEF_SupportM_Loadout_T4_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_DEF_SupportM_Loadout_T4_BP.VH_HVC_DEF_SupportM_Loadout_T4_BP_C"},
    {L"HVCUTIAS_M(T4)", 33489368, 33489369, 6, 'U', "VH_HVC_UTI_AssaultM_Loadout_T4_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_UTI_AssaultM_Loadout_T4_BP.VH_HVC_UTI_AssaultM_Loadout_T4_BP_C"},
    {L"HVCUTISU_L(T4)", 33489371, 33489372, 6, 'U', "VH_HVC_UTI_SupportL_Loadout_T4_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_UTI_SupportL_Loadout_T4_BP.VH_HVC_UTI_SupportL_Loadout_T4_BP_C"},
    {L"HVCUTIDN_L(T4)", 33489378, 33489379, 6, 'U', "VH_HVC_UTI_DreadL_Loadout_T4_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_UTI_DreadL_Loadout_T4_BP.VH_HVC_UTI_DreadL_Loadout_T4_BP_C"},
    {L"HVCUTISN_L(T4)", 33489381, 33489380, 6, 'U', "VH_HVC_UTI_SniperL_Loadout_T4_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_UTI_SniperL_Loadout_T4_BP.VH_HVC_UTI_SniperL_Loadout_T4_BP_C"},
    {L"HVCUTISC_H(T4)", 33489384, 33489385, 6, 'U', "VH_HVC_UTI_ScoutH_Loadout_T4_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_UTI_ScoutH_Loadout_T4_BP.VH_HVC_UTI_ScoutH_Loadout_T4_BP_C"},
    {L"HVCOFFSC_L(T4)", 33489388, 33489389, 6, 'O', "VH_HVC_OFF_ScoutL_Loadout_T4_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_OFF_ScoutL_Loadout_T4_BP.VH_HVC_OFF_ScoutL_Loadout_T4_BP_C"},
    {L"HVCOFFSC_M(T4)", 33489391, 33489392, 6, 'O', "VH_HVC_OFF_ScoutM_Loadout_T4_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_OFF_ScoutM_Loadout_T4_BP.VH_HVC_OFF_ScoutM_Loadout_T4_BP_C"},
    {L"HVCOFFSN_M(T4)", 33489396, 33489397, 6, 'O', "VH_HVC_OFF_SniperM_Loadout_T4_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_OFF_SniperM_Loadout_T4_BP.VH_HVC_OFF_SniperM_Loadout_T4_BP_C"},
    {L"HVCOFFSN_H(T4)", 33489398, 33489399, 6, 'O', "VH_HVC_OFF_SniperH_Loadout_T4_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_OFF_SniperH_Loadout_T4_BP.VH_HVC_OFF_SniperH_Loadout_T4_BP_C"},
    {L"HVCOFFAS_L(T4)", 33489401, 33489402, 6, 'O', "VH_HVC_OFF_AssaultL_Loadout_T4_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_OFF_AssaultL_Loadout_T4_BP.VH_HVC_OFF_AssaultL_Loadout_T4_BP_C"},
    {L"HVCOFFDN_L(T4)", 33489403, 33489405, 6, 'O', "VH_HVC_OFF_DreadL_Loadout_T4_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_OFF_DreadL_Loadout_T4_BP.VH_HVC_OFF_DreadL_Loadout_T4_BP_C"},
    {L"HVCOFFAS_L(T5)", 33489402, 33489402, 12, 'O', "VH_HVC_OFF_AssaultL_Loadout_T5_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_OFF_AssaultL_Loadout_T5_BP.VH_HVC_OFF_AssaultL_Loadout_T5_BP_C"},
    {L"HVCOFFDN_L(T5)", 33489405, 33489405, 12, 'O', "VH_HVC_OFF_DreadL_Loadout_T5_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_OFF_DreadL_Loadout_T5_BP.VH_HVC_OFF_DreadL_Loadout_T5_BP_C"},
    {L"HVCDEFAS_H(T5)", 33489350, 33489350, 12, 'D', "VH_HVC_DEF_AssaultH_Loadout_T5_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_DEF_AssaultH_Loadout_T5_BP.VH_HVC_DEF_AssaultH_Loadout_T5_BP_C"},
    {L"HVCDEFAS_M(T5)", 33489353, 33489353, 12, 'D', "VH_HVC_DEF_AssaultM_Loadout_T5_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_DEF_AssaultM_Loadout_T5_BP.VH_HVC_DEF_AssaultM_Loadout_T5_BP_C"},
    {L"HVCDEFDN_M(T5)", 33489357, 33489357, 12, 'D', "VH_HVC_DEF_DreadM_Loadout_T5_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_DEF_DreadM_Loadout_T5_BP.VH_HVC_DEF_DreadM_Loadout_T5_BP_C"},
    {L"HVCDEFDN_H(T5)", 33489359, 33489359, 12, 'D', "VH_HVC_DEF_DreadH_Loadout_T5_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_DEF_DreadH_Loadout_T5_BP.VH_HVC_DEF_DreadH_Loadout_T5_BP_C"},
    {L"HVCOFFSN_H(T5)", 33489399, 33489399, 12, 'O', "VH_HVC_OFF_SniperH_Loadout_T5_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_OFF_SniperH_Loadout_T5_BP.VH_HVC_OFF_SniperH_Loadout_T5_BP_C"},
    {L"HVCDEFSN_H(T5)", 33489360, 33489360, 12, 'D', "VH_HVC_DEF_SniperH_Loadout_T5_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_DEF_SniperH_Loadout_T5_BP.VH_HVC_DEF_SniperH_Loadout_T5_BP_C"},
    {L"HVCDEFSU_H(T5)", 33489362, 33489362, 12, 'D', "VH_HVC_DEF_SupportH_Loadout_T5_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_DEF_SupportH_Loadout_T5_BP.VH_HVC_DEF_SupportH_Loadout_T5_BP_C"},
    {L"HVCDEFSU_M(T5)", 33489366, 33489366, 12, 'D', "VH_HVC_DEF_SupportM_Loadout_T5_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_DEF_SupportM_Loadout_T5_BP.VH_HVC_DEF_SupportM_Loadout_T5_BP_C"},
    {L"HVCUTIAS_M(T5)", 33489369, 33489369, 12, 'U', "VH_HVC_UTI_AssaultM_Loadout_T5_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_UTI_AssaultM_Loadout_T5_BP.VH_HVC_UTI_AssaultM_Loadout_T5_BP_C"},
    {L"HVCUTISU_L(T5)", 33489372, 33489372, 12, 'U', "VH_HVC_UTI_SupportL_Loadout_T5_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_UTI_SupportL_Loadout_T5_BP.VH_HVC_UTI_SupportL_Loadout_T5_BP_C"},
    {L"HVCUTIDN_M(T5)", 33489373, 33489373, 12, 'U', "VH_HVC_UTI_DreadM_Loadout_T5_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_UTI_DreadM_Loadout_T5_BP.VH_HVC_UTI_DreadM_Loadout_T5_BP_C"},
    {L"HVCOFFSN_M(T5)", 33489397, 33489397, 12, 'O', "VH_HVC_OFF_SniperM_Loadout_T5_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_OFF_SniperM_Loadout_T5_BP.VH_HVC_OFF_SniperM_Loadout_T5_BP_C"},
    {L"HVCUTIDN_L(T5)", 33489379, 33489379, 12, 'U', "VH_HVC_UTI_DreadL_Loadout_T5_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_UTI_DreadL_Loadout_T5_BP.VH_HVC_UTI_DreadL_Loadout_T5_BP_C"},
    {L"HVCUTISN_L(T5)", 33489380, 33489380, 12, 'U', "VH_HVC_UTI_SniperL_Loadout_T5_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_UTI_SniperL_Loadout_T5_BP.VH_HVC_UTI_SniperL_Loadout_T5_BP_C"},
    {L"HVCUTISC_H(T5)", 33489385, 33489385, 12, 'U', "VH_HVC_UTI_ScoutH_Loadout_T5_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_UTI_ScoutH_Loadout_T5_BP.VH_HVC_UTI_ScoutH_Loadout_T5_BP_C"},
    {L"HVCOFFSC_L(T5)", 33489389, 33489389, 12, 'O', "VH_HVC_OFF_ScoutL_Loadout_T5_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_OFF_ScoutL_Loadout_T5_BP.VH_HVC_OFF_ScoutL_Loadout_T5_BP_C"},
    {L"HVCOFFSC_M(T5)", 33489392, 33489392, 12, 'O', "VH_HVC_OFF_ScoutM_Loadout_T5_BP", L"/Game/Generic/Loadouts/Precast/Havoc/VH_HVC_OFF_ScoutM_Loadout_T5_BP.VH_HVC_OFF_ScoutM_Loadout_T5_BP_C"},
};
static const int kHavocShipCount = (int)(sizeof kHavocShips / sizeof kHavocShips[0]);
#define RVA_SPAWN_PLAYER 0x5998A0       // AYPlayerController::SpawnPlayer(pc)
#define RVA_WEAK_PTR_SET 0xD64B60       // TWeakObjectPtr = obj (index +0xC, serial)
#define OFF_PC_LOADOUT_HOLDER 0x958     // -> +0x28 the loadout manager (0x5B7A60)
#define OFF_PC_SPAWN_PENDING 0xB88      // byte; SpawnPlayer clears it
#define OFF_PC_LAST_PAWN 0xBB0          // TWeakObjectPtr<YPawn>
#define OFF_PC_NO_INPLACE_SPAWN 0xCA0   // byte; set -> no spawn at the last pawn
typedef void(__fastcall *tSpawnPlayer)(void *pc);
typedef void(__fastcall *tWeakSet)(void *weak, void *obj);

static bool HavocShipUpgradesOn() {
  static int s_on = -1;
  if (s_on < 0)
    s_on = SwitchOn("DN_HOST_NO_HAVOC_SHIP_UPGRADES", "dn_host_no_havoc_ship_upgrades.txt") ? 0 : 1;
  return s_on == 1;
}
static bool HavocIsShipWave(int wave) {
  for (const HavocShip &s : kHavocShips)
    if (s.wave > 0 && s.wave == wave)
      return true;
  return false;
}
static int HavocShipByItem(int itemId) {
  for (int i = 0; i < kHavocShipCount; ++i)
    if (kHavocShips[i].itemId == itemId)
      return i;
  return -1;
}
static void *HavocLoadoutManager(void *pc) {
  uint8_t *h = IsReadable((uint8_t *)pc + OFF_PC_LOADOUT_HOLDER, 8) ? *(uint8_t **)((uint8_t *)pc + OFF_PC_LOADOUT_HOLDER) : nullptr;
  return h && IsReadable(h + 0x28, 8) ? *(void **)(h + 0x28) : nullptr;
}
// The kHavocShips row of the active loadout, or -1. By its
// m_precastLoadoutID when set; else by its loadout id FName (+0xB0), which on
// these classes is the default object's name, "Default__<asset>_C" (smoke
// host 2026-10-19: every Havoc CDO has m_precastLoadoutID 0 and that FName);
// else by its class name.
static int HavocCurrentShipRow(void *pc) {
  uint8_t *mgr = (uint8_t *)HavocLoadoutManager(pc);
  uint8_t *lo = mgr && IsReadable(mgr + OFF_LOADOUT_MGR_ACTIVE, 8) ? *(uint8_t **)(mgr + OFF_LOADOUT_MGR_ACTIVE) : nullptr;
  if (!lo || !IsReadable(lo, 0xB8))
    return -1;
  int off = PropOffset((UObjectMin *)lo, "m_precastLoadoutID");
  int id = off > 0 && IsReadable(lo + off, 4) ? *(int32_t *)(lo + off) : 0;
  int row = id > 0 ? HavocShipByItem(id) : -1;
  if (row >= 0)
    return row;
  const char *fname = NameText(*(FNameMin *)(lo + 0xB0));
  const char *cls = ClassNameOf(lo);
  char want[160];
  for (int i = 0; i < kHavocShipCount; ++i) {
    _snprintf_s(want, sizeof want, _TRUNCATE, "Default__%s_C", kHavocShips[i].asset);
    if (fname && !strcmp(fname, want))
      return i;
    if (cls && !strcmp(cls, want + 9))
      return i;
  }
  static int s_logged = 0;
  if (s_logged++ < 4)
    Logf("havoc: active loadout %p not a Havoc ship (precast id %d, id FName %s, class %s)", lo, id, fname ? fname : "?",
         cls ? cls : "?");
  return -1;
}
static int HavocCurrentShipItem(void *pc) {
  int row = HavocCurrentShipRow(pc);
  return row >= 0 ? kHavocShips[row].itemId : -1;
}
// One ship of the category from the rows appearing on this wave; `prefer`
// wins when it is one of them.
static int HavocPickShip(char cat, int wave, int prefer) {
  if (prefer >= 0 && kHavocShips[prefer].cat == cat && kHavocShips[prefer].wave == wave)
    return prefer;
  int n = 0;
  for (const HavocShip &s : kHavocShips)
    if (s.cat == cat && s.wave == wave)
      ++n;
  if (!n)
    return -1;
  int r = (int)(HavocRand() % (uint32_t)n);
  for (int i = 0; i < kHavocShipCount; ++i)
    if (kHavocShips[i].cat == cat && kHavocShips[i].wave == wave && r-- == 0)
      return i;
  return -1;
}

// Smoke test (dn_host_havoc_smoke.txt): every ship class loads, and its
// default object's loadout id FName is "Default__<asset>_C".
static void HavocVerifyShipClasses() {
  int ok = 0, bad = 0;
  for (const HavocShip &s : kHavocShips) {
    PrecastPath pp = {s.asset, s.path};
    UObjectMin *cdo = ResolvePrecastByName(&pp);
    const char *fname = cdo && IsReadable((uint8_t *)cdo + 0xB0, 8) ? NameText(*(FNameMin *)((uint8_t *)cdo + 0xB0)) : nullptr;
    char want[160];
    _snprintf_s(want, sizeof want, _TRUNCATE, "Default__%s_C", s.asset);
    if (cdo && fname && !strcmp(fname, want))
      ++ok;
    else {
      ++bad;
      Logf("havoc: ship class %s -> cdo %p, id FName %s (want %s)", s.asset, cdo, fname ? fname : "?", want);
    }
  }
  Logf("havoc: ship classes checked: %d OK, %d wrong or missing", ok, bad);
}

static bool HavocSwapShip(void *pc, int row) {
  const HavocShip &s = kHavocShips[row];
  void *mgr = HavocLoadoutManager(pc);
  if (!mgr) {
    Logf("havoc: ship upgrade %ls for %p -- no loadout manager", s.row, pc);
    return false;
  }
  PrecastPath pp = {s.asset, s.path};
  UObjectMin *cdo = ResolvePrecastByName(&pp);
  if (!cdo || !IsReadable((uint8_t *)cdo + 0xB8, 8)) {
    Logf("havoc: ship upgrade %ls -- class %ls did not load", s.row, s.path);
    return false;
  }
  void *loadout = g_origFindLoadoutByID ? g_origFindLoadoutByID(mgr, (void **)((uint8_t *)cdo + 0xB0), 0) : nullptr;
  if (!loadout) {
    bool added = AddLoadoutGuarded(mgr, cdo);
    loadout = g_origFindLoadoutByID ? g_origFindLoadoutByID(mgr, (void **)((uint8_t *)cdo + 0xB0), 0) : nullptr;
    Logf("havoc: ship upgrade %ls -- registered %s with manager %p -> %s, found %p", s.row, s.asset, mgr,
         added ? "ok" : "EXCEPTION", loadout);
  }
  if (!loadout)
    return false;
  uint8_t *p = (uint8_t *)pc;
  void *pawn = IsReadable(p + OFF_CONTROLLER_PAWN, 8) ? *(void **)(p + OFF_CONTROLLER_PAWN) : nullptr;
  int before = HavocCurrentShipItem(pc);
  __try {
    ((tActivateLoadout)(g_base + RVA_ACTIVATE_LOADOUT))(mgr, loadout, 1);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("havoc: ship upgrade %ls -- EXCEPTION in ActivateLoadout", s.row);
    return false;
  }
  int after = HavocCurrentShipItem(pc);
  if (!pawn) {
    Logf("havoc: ship upgrade %ls for %p -- active loadout %d -> %d; no ship now, the next spawn uses it", s.row, pc,
         before, after);
    return after == s.itemId;
  }
  uint8_t noInPlace = p[OFF_PC_NO_INPLACE_SPAWN];
  __try {
    ((tWeakSet)(g_base + RVA_WEAK_PTR_SET))(p + OFF_PC_LAST_PAWN, pawn);
    p[OFF_PC_SPAWN_PENDING] = 1;
    ((tSpawnPlayer)(g_base + RVA_SPAWN_PLAYER))(pc);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("havoc: ship upgrade %ls -- EXCEPTION in SpawnPlayer", s.row);
    return false;
  }
  void *now = IsReadable(p + OFF_CONTROLLER_PAWN, 8) ? *(void **)(p + OFF_CONTROLLER_PAWN) : nullptr;
  Logf("havoc: ship upgrade %ls (%s) for %p -- active loadout %d -> %d, ship %p %s -> %p %s (pending %u, no-in-place %u)",
       s.row, s.asset, pc, before, after, pawn, ClassNameOf(pawn), now, now ? ClassNameOf(now) : "-",
       p[OFF_PC_SPAWN_PENDING], noInPlace);
  return after == s.itemId && now && now != pawn;
}

// On entering UpgradeSelection: a fresh offer for every connected human.
static void HavocOfferUpgrades(uint8_t *gs) {
  uint8_t *pcs[16];
  int npc = HavocHumanPCs(gs, pcs, 16);
  for (int i = 0; i < npc; ++i) {
    HavocPlayer *p = HavocPlayerFor(pcs[i], true);
    if (!p)
      continue;
    static const char kCats[3] = {'O', 'D', 'U'};
    int wave = *(int32_t *)(gs + OFF_HVC_GS_WAVE_INDEX);
    bool ships = HavocShipUpgradesOn() && HavocIsShipWave(wave);
    int cur = ships ? HavocShipByItem(HavocCurrentShipItem(pcs[i])) : -1;
    int next = cur >= 0 ? HavocShipByItem(kHavocShips[cur].nextTierId) : -1;
    for (int k = 0; k < 3; ++k) {
      p->offer[k] = ships ? -1 : HavocPickBoost(kCats[k]);
      p->offerShip[k] = ships ? HavocPickShip(kCats[k], wave, next) : -1;
      p->offerNames[k] = {};
      const wchar_t *nm = p->offer[k] >= 0 ? kHavocBoosts[p->offer[k]].id
                          : p->offerShip[k] >= 0 ? kHavocShips[p->offerShip[k]].row : nullptr;
      if (nm)
        ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&p->offerNames[k], nm, 1);
    }
    bool ok = CallFilled(pcs[i], L"ClientReceiveRandSelectedUpgrades", FillNameArray, p);
    uint8_t *pri = *(uint8_t **)(pcs[i] + OFF_PC_PLAYER_STATE);
    if (ships) {
      Logf("havoc: wave %d is a ship-upgrade wave -- offered %ls / %ls / %ls to %p (now %ls, next tier %ls)%s", wave,
           p->offerShip[0] >= 0 ? kHavocShips[p->offerShip[0]].row : L"-",
           p->offerShip[1] >= 0 ? kHavocShips[p->offerShip[1]].row : L"-",
           p->offerShip[2] >= 0 ? kHavocShips[p->offerShip[2]].row : L"-", pcs[i],
           cur >= 0 ? kHavocShips[cur].row : L"unknown", next >= 0 ? kHavocShips[next].row : L"-", ok ? "" : " -- RPC NOT FOUND");
      continue;
    }
    Logf("havoc: offered %ls / %ls / %ls to %p (xp %d)%s", p->offer[0] >= 0 ? kHavocBoosts[p->offer[0]].id : L"-",
         p->offer[1] >= 0 ? kHavocBoosts[p->offer[1]].id : L"-", p->offer[2] >= 0 ? kHavocBoosts[p->offer[2]].id : L"-",
         pcs[i], *(int32_t *)(pri + OFF_HVC_PRI_CURRENT_XP), ok ? "" : " -- RPC NOT FOUND");
  }
}

#define RVA_HVC_SERVER_SELECTED_UPGRADE_IMPL 0x406040
typedef void(__fastcall *tHvcSelectedUpgrade)(void *pc, int32_t slot);
tHvcSelectedUpgrade g_origHvcSelectedUpgrade = nullptr;
static void __fastcall HookHvcSelectedUpgrade(void *pc, int32_t slot) {
  g_origHvcSelectedUpgrade(pc, slot);
  HavocPlayer *p = HavocPlayerFor(pc, false);
  uint8_t *pri = IsReadable((uint8_t *)pc + OFF_PC_PLAYER_STATE, 8) ? *(uint8_t **)((uint8_t *)pc + OFF_PC_PLAYER_STATE) : nullptr;
  int b = p && slot >= 0 && slot < 3 ? p->offer[slot] : -1;
  int ship = p && slot >= 0 && slot < 3 ? p->offerShip[slot] : -1;
  if (ship >= 0) {
    // A ship costs no XP (see HavocSwapShip); one ship per upgrade phase.
    bool swapped = HavocSwapShip(pc, ship);
    if (swapped)
      p->offerShip[0] = p->offerShip[1] = p->offerShip[2] = -1;
    Logf("havoc: %p picked slot %d (ship %ls) -> %s", pc, slot, kHavocShips[ship].row, swapped ? "swapped" : "FAILED");
    HavocPurchaseResult r = {};
    r.ok = swapped ? 1 : 0;
    ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&r.id, kHavocShips[ship].row, 1);
    CallFilled(pc, L"ClientReceiveBoostPurchaseResult", FillPurchaseResult, &r);
    return;
  }
  uint8_t ok = 0;
  int xp = -1;
  if (b >= 0 && pri && IsReadable(pri, OFF_HVC_PRI_CONTRIBUTED_XP + 4) &&
      ClassChainContains((UObjectMin *)pri, "YPlayerReplicationInfo_Havoc")) {
    xp = *(int32_t *)(pri + OFF_HVC_PRI_CURRENT_XP);
    if (xp >= kHavocBoosts[b].cost && p->nOwned < (int)(sizeof p->owned / sizeof p->owned[0])) {
      ((void(__fastcall *)(void *, int32_t))(g_base + RVA_HVC_PRI_DEDUCT_XP))(pri, kHavocBoosts[b].cost);
      p->owned[p->nOwned++] = b;
      p->offer[slot] = -1;
      ok = 1;
    }
  }
  Logf("havoc: %p picked slot %d (%ls, cost %d) with xp %d -> %s", pc, slot, b >= 0 ? kHavocBoosts[b].id : L"none",
       b >= 0 ? kHavocBoosts[b].cost : 0, xp, ok ? "bought" : "refused");
  // AYPlayerReplicationInfo_Havoc::ClientXPDeducted ("Update upgrades panel",
  // 0x4030F2) is NOT a reflected UFunction: FindFunctionChecked on it killed
  // the host (run 2026-10-14 19:45, "Failed to find function ClientXPDeducted
  // in YPlayerReplicationInfo_Havoc"). m_currentXP replicates by itself.
  HavocPurchaseResult r = {};
  r.ok = ok;
  if (b >= 0)
    ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&r.id, kHavocBoosts[b].id, 1);
  CallFilled(pc, L"ClientReceiveBoostPurchaseResult", FillPurchaseResult, &r);
}

// Feats components by ship, recorded at UYFeatsComponent begin-play.
// vid: a per-process counter given when the slot is filled, the viewer's ship
// identity (bots have no pid, and keying them by class+team+order shifted every
// later bot's history onto its neighbour when one died -- DeepSeek round 10).
// modGen/modsOn/permApplied: the Havoc wave and permanent enemy modifiers
// already on this ship (HavocApplyModifiers).
struct FeatsByPawn {
  void *pawn, *comp;
  int vid;
  LONG modGen;
  int modsOn[2];
  int permApplied;
};
static int g_nextVid = 1;
// 256: Onslaught after /bots 3 had 55 enemy ships at once (onsfun2), plus
// fighters; stale entries (component gone or re-owned) are reused first.
#define FEATS_SLOTS 256
static FeatsByPawn g_featsByPawn[FEATS_SLOTS];
static void RecordFeatsComp(void *pawn, void *comp) {
  int freeAt = -1;
  for (int i = 0; i < FEATS_SLOTS; ++i) {
    if (g_featsByPawn[i].pawn == pawn) {
      // A pawn's feats component begins play once, so a different one here
      // is a new pawn at a recycled address: new viewer id, or it would
      // inherit the dead ship's history and teleport (DeepSeek round 11).
      if (g_featsByPawn[i].comp != comp)
        g_featsByPawn[i] = {pawn, comp, g_nextVid++, -1, {-1, -1}, 0};
      g_featsByPawn[i].comp = comp;
      return;
    }
    void *c = g_featsByPawn[i].comp;
    if (freeAt < 0 && (!IsReadable(c, OFF_COMPONENT_OWNER + 8) ||
                       *(void **)((uint8_t *)c + OFF_COMPONENT_OWNER) != g_featsByPawn[i].pawn))
      freeAt = i;
  }
  static int s_next = 0;
  if (freeAt < 0)
    freeAt = s_next++ % FEATS_SLOTS;
  g_featsByPawn[freeAt] = {pawn, comp, g_nextVid++, -1, {-1, -1}, 0};
}
static bool ObjectLive(void *p);
// Both ends must still be live objects and the pawn still a pawn: a dead
// ship's address gets reused (hvc matrix: "added 7 from SkeletalMeshComponent",
// then an access violation 15 s later).
static void *FeatsCompOf(void *pawn) {
  for (FeatsByPawn &e : g_featsByPawn)
    if (e.pawn == pawn && ObjectLive(pawn) && ObjectLive(e.comp) &&
        ClassChainContains((UObjectMin *)pawn, "Pawn") &&
        *(void **)((uint8_t *)e.comp + OFF_COMPONENT_OWNER) == pawn)
      return e.comp;
  return nullptr;
}

// Every director tick: put each player's bought boosts on their current ship.
static void HavocApplyBoosts() {
  for (HavocPlayer &p : g_hvcPlayers) {
    if (!p.pc || !p.nOwned || !IsReadable((uint8_t *)p.pc + OFF_CONTROLLER_PAWN, 8))
      continue;
    void *pawn = *(void **)((uint8_t *)p.pc + OFF_CONTROLLER_PAWN);
    if (!pawn)
      continue;
    if (pawn != p.appliedPawn)
      p.appliedPawn = pawn, p.nApplied = 0;
    if (p.nApplied >= p.nOwned)
      continue;
    void *comp = FeatsCompOf(pawn);
    if (!comp)
      continue;
    for (; p.nApplied < p.nOwned; ++p.nApplied) {
      const HavocBoost &b = kHavocBoosts[p.owned[p.nApplied]];
      FNameMin name{};
      ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&name, b.feat, 1);
      bool ok = false;
      __try {
        ok = AddFeatStackable(comp, name, b.feat);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("havoc: AddFeat(%ls) faulted on %p", b.feat, comp);
      }
      Logf("havoc: boost feat %ls on %s for %p -> %s", b.feat, ClassNameOf(pawn), p.pc, ok ? "added" : "FAILED");
    }
  }
}

static void __fastcall HookIntParmExecThunk(void *ctx, uint8_t *stack, void *result) {
  const char *n = CalledNativeName(stack);
  int32_t v = 0;
  if (*(void **)(stack + 0x20))
    ((void(__fastcall *)(void *, void *, void *))(g_base + RVA_FFRAME_STEP))(stack, *(void **)(stack + 0x18), &v);
  else {
    uint8_t *prop = *(uint8_t **)(stack + 0x80);
    *(void **)(stack + 0x80) = *(void **)(prop + 0x28);
    ((void(__fastcall *)(void *, void *, void *))(g_base + RVA_FFRAME_STEP_EXPLICIT_PROPERTY))(stack, &v, prop);
  }
  if (*(uint8_t **)(stack + 0x20))
    ++*(uint8_t **)(stack + 0x20);
  if (!n)
    return;
  if (!strcmp(n, "AddNPCToEnemyList")) {
    LONG i = InterlockedIncrement(&g_hvcEnemyCount) - 1;
    if (i < 256)
      g_hvcEnemyIds[i] = v;
    else
      InterlockedDecrement(&g_hvcEnemyCount);
    Logf("havoc: AddNPCToEnemyList(%d) on %s (list %ld)", v, ClassNameOf(ctx), g_hvcEnemyCount);
    HavocEnemyOverride(v);
    return;
  }
  if (!strcmp(n, "AddXPForGroup")) {
    HavocAddXP((uint8_t *)ctx, v, "AddXPForGroup");
    return;
  }
  static char s_seen[16][48];
  static int s_nseen = 0;
  for (int i = 0; i < s_nseen; ++i)
    if (!strcmp(s_seen[i], n))
      return;
  if (s_nseen < 16)
    snprintf(s_seen[s_nseen++], sizeof s_seen[0], "%s", n);
  Logf("havoc: BP called compiled-out native %s(%d) on %s (first call)", n, v, ClassNameOf(ctx));
}

// ---------------------------------------------------------------------------
// Havoc, stage 2 (2026-10-19): the rules the PR's director guessed or left
// out, from GameState_Havoc_BP / PVE_HordeManager_BP bytecode (tools/bpdump),
// the client's own code and the Havoc data tables.
//
// Wave over. GameState_Havoc_BP CheckWaveOver: when GetNumberOfRemainingWave
// Enemies <= 0 AND m_numEnemiesKilledThisWave >= m_maxWaveEnemies + the
// bridgehead spawns, it sets god mode and -- once every XP drop is picked up
// or gone (AllXPDropsPickedUp, set by the pickups' CompleteBuffPodObjective
// IfAllRetrieved) and 2 s later -- calls SetSpecialWaveConditionsFulfilled
// (true). That native is compiled out (bool exec thunk 0x777000 -> the bare
// `ret` 0x1AA9450, same shape as the int thunk 0x742E90), so the hook below
// records it and the director ends the wave on it.
tExecThunk g_origBoolParmExecThunk = nullptr;
static volatile LONG g_hvcWaveConditions = 0;
static const char *CalledNativeName(uint8_t *stack);
static void StepParm(uint8_t *stack, void *out);

static void __fastcall HookBoolParmExecThunk(void *ctx, uint8_t *stack, void *result) {
  const char *n = CalledNativeName(stack);
  int32_t v = 0;
  StepParm(stack, &v);
  if (*(uint8_t **)(stack + 0x20))
    ++*(uint8_t **)(stack + 0x20);
  if (!n)
    return;
  if (!strcmp(n, "SetSpecialWaveConditionsFulfilled")) {
    LONG before = InterlockedExchange(&g_hvcWaveConditions, v ? 1 : 0);
    if (before != (v ? 1 : 0))
      Logf("havoc: BP SetSpecialWaveConditionsFulfilled(%d) on %s", v ? 1 : 0, ClassNameOf(ctx));
    return;
  }
  static char s_seen[16][48];
  static int s_nseen = 0;
  for (int i = 0; i < s_nseen; ++i)
    if (!strcmp(s_seen[i], n))
      return;
  if (s_nseen < 16)
    snprintf(s_seen[s_nseen++], sizeof s_seen[0], "%s", n);
  Logf("havoc: BP called compiled-out native %s(%d) on %s (first call)", n, v, ClassNameOf(ctx));
}

// Kill XP. GameMode_Havoc_BP m_monsterKillXP = 25 (DefaultHavoc.ini says 10;
// the BP default wins), and the scoring table's HavocXPKill row scores the
// same 25. The native that paid it is compiled out with the rest of the
// server's death path. GUESS: every listed wave enemy's death pays it to the
// group (AddXPForGroup's rule); the killer is not known here.
static int HavocMonsterKillXP(uint8_t *gm) {
  static void *s_for = nullptr;
  static int s_xp = 0;
  if (s_for != gm) {
    s_for = gm;
    int off = PropOffset((UObjectMin *)gm, "m_monsterKillXP");
    s_xp = off > 0 && IsReadable(gm + off, 4) ? *(int32_t *)(gm + off) : 0;
    if (s_xp < 0 || s_xp > 10000)
      s_xp = 0;
    Logf("havoc: kill XP %d per enemy (m_monsterKillXP)", s_xp);
  }
  return s_xp;
}
static void HavocAddXP(uint8_t *gm, int xp, const char *why, void *contributor);
static void HavocOnEnemyKilled(uint8_t *gm) {
  int xp = HavocMonsterKillXP(gm);
  if (xp > 0)
    HavocAddXP(gm, xp, "enemy kill (m_monsterKillXP)", nullptr);
}
// Enemies on the list still alive: entry not yet spawned (the scene tick will
// spawn it), or its pawn live and not in state 2/3 (OFF_PAWN_STATE, the AI's
// own dead/dying filter 0x57BD10). Dead ids drop off the list.
static int HavocCountAliveEnemies(uint8_t *gm, uint8_t *gs) {
  uint8_t *scene = (uint8_t *)((tGetSceneManager)(g_base + RVA_COMBAT_SCENE_MANAGER))();
  if (!scene || !IsReadable(scene + OFF_SCENE_NPCS, 16))
    return -1;
  uint8_t *arr = *(uint8_t **)(scene + OFF_SCENE_NPCS);
  int cnt = *(int32_t *)(scene + OFF_SCENE_NPCS_COUNT);
  int n = (int)g_hvcEnemyCount, alive = 0, keep = 0;
  for (int i = 0; i < n && i < 256; ++i) {
    int id = (int)g_hvcEnemyIds[i];
    bool live = false;
    if (arr && id >= 0 && id < cnt && IsReadable(arr + (size_t)id * NPC_SPAWN_SIZE, NPC_SPAWN_SIZE)) {
      uint8_t *e = arr + (size_t)id * NPC_SPAWN_SIZE;
      if (!e[OFF_NPCSPAWN_INSTANCED])
        live = true;
      else {
        uint8_t *pawn = (uint8_t *)((tWeakGet)(g_base + RVA_WEAK_PTR_GET))(e + OFF_NPCSPAWN_PAWN);
        live = pawn && IsReadable(pawn + OFF_PAWN_STATE, 1) && pawn[OFF_PAWN_STATE] != 2 && pawn[OFF_PAWN_STATE] != 3;
        if (!live) {
          // A listed id leaves the list exactly once, so this counts each kill
          // once. A pawn already destroyed (weak ptr null) is counted but not
          // broadcast: the BP spawns the pickup at KilledYPawn's transform and
          // a null pawn would drop it at the origin.
          *(int32_t *)(gs + OFF_HVC_GS_KILLED) += 1;
          HavocOnEnemyKilled(gm);
          if (pawn) {
            struct { void *pawn; int32_t npcId; int32_t pad; } parms = {pawn, id, 0};
            // GUESS: KilledNPCID is the combat-scene spawn index (the id the
            // enemy-list hook records); the BP only forwards it, the XP pickup
            // path does not read it.
            ((void (*)(void *, void *))(g_base + RVA_MULTICAST_BROADCAST_NOPARMS))(gm + OFF_HVC_GM_ON_ANY_NPC_KILLED,
                                                                                  &parms);
          }
          Logf("havoc: enemy %d died (pawn %p) -> killed %d%s", id, pawn, *(int32_t *)(gs + OFF_HVC_GS_KILLED),
               pawn ? ", OnAnyNPCKilled broadcast" : ", pawn gone: no XP drop");
        }
      }
    }
    if (live) {
      g_hvcEnemyIds[keep++] = id;
      ++alive;
    }
  }
  // ids appended by the hook while we pruned stay at the tail
  LONG now = g_hvcEnemyCount;
  for (LONG i = n; i < now && i < 256; ++i)
    g_hvcEnemyIds[keep++] = g_hvcEnemyIds[i];
  InterlockedExchange(&g_hvcEnemyCount, keep);
  return alive;
}

static bool HostIsHavoc() {
  const wchar_t *cmd = GetCommandLineW();
  return cmd && wcsstr(cmd, L"-GameMode=Havoc ") != nullptr;
}

// PvE Horde episodes (S1E1-3, S3E1-4): GameMode_Horde_BP, the same horde
// manager / wave-state machinery as Havoc minus the upgrade phase.
static bool HostIsHorde() {
  const wchar_t *cmd = GetCommandLineW();
  return cmd && wcsstr(cmd, L"-GameMode=Horde ") != nullptr;
}

// /wave <n> (CommandsTick): the next wave start uses wave n instead of the
// next one. The horde manager picks the wave's content from the index the
// director passes to OnWaveStateChanged (HVC_Map1_P: Waves[22], with
// WaveToLoopBackTo for later ones). 0 = no jump pending.
static volatile int g_hvcWaveJump = 0;

// Havoc forced respawn (on unless dn_host_no_havoc_respawn.txt /
// DN_HOST_NO_HAVOC_RESPAWN=1). Headless 10-16 21:10-21:36: after a death the
// respawn screen readies ("RESPAWNING IN", tick) but shows no number and the
// player is never respawned, mid-wave or at wave end, although the pool is
// unlimited (GameState m_playerTeamRespawnsLeft -1) and the GameMode has
// m_respawnTime 15 and m_maxTimeBeforeForcedRespawn 40. The death path
// (chunk around 0x58400B) sends ClientSetRespawntime (wrapper 0x5E5C80) from
// GameMode +0x6B8 (m_respawnTime); the client never got it, so whatever
// server code was meant to run the timer is not running here. This applies
// the stock forced-respawn rule: a human who had a ship and has been without
// one for m_maxTimeBeforeForcedRespawn seconds gets the engine's own
// ServerRestartPlayer, which spawns the loadout manager's active loadout
// (0x3827D0 -> 0x370970 with a null ship argument), i.e. the respawn-screen
// pick (ServerPlayerClickedShipLoadout -> ActivateLoadout sets it).
// GUESS: the original timer respawned at m_respawnTime once readied and
// forced at the max; we only do the forced one.
static int CollectHumanPCs(uint8_t *gm, uint8_t **out, int max);
static float CallFloatGetter(void *obj, const char *name);
static void HavocForcedRespawnTick(uint8_t *gm) {
  static int s_off = -1; // no guarded static init: mingw would pull in pthread
  static void *s_gm = nullptr;
  if (s_off < 0)
    s_off = SwitchOn("DN_HOST_NO_HAVOC_RESPAWN", "dn_host_no_havoc_respawn.txt");
  static struct { uint8_t *pc; bool hadPawn; uint64_t since; } s_pcs[32];
  if (s_off)
    return;
  if (s_gm != gm)
    s_gm = gm, memset(s_pcs, 0, sizeof s_pcs);
  int off = PropOffset((UObjectMin *)gm, "m_maxTimeBeforeForcedRespawn");
  int maxSecs = off > 0 ? *(int32_t *)(gm + off) : 40;
  if (maxSecs <= 0 || maxSecs > 600)
    maxSecs = 40;
  uint8_t *pcs[32];
  int n = CollectHumanPCs(gm, pcs, 32);
  uint64_t now = GetTickCount64();
  for (int i = 0; i < n; ++i) {
    int k = 0;
    while (k < 32 && s_pcs[k].pc && s_pcs[k].pc != pcs[i])
      ++k;
    if (k == 32)
      continue;
    s_pcs[k].pc = pcs[i];
    void *pawn = IsReadable(pcs[i] + OFF_CONTROLLER_PAWN, 8) ? *(void **)(pcs[i] + OFF_CONTROLLER_PAWN) : nullptr;
    if (pawn) {
      // The controller keeps the wreck as its pawn ~16 s after the kill
      // (respawnship5: kill 21:44:52, pawn gone 21:45:08), so the clock
      // starts at zero health and the respawn waits for the pawn to go.
      float hp = CallFloatGetter(pawn, "GetCurrentHealth");
      if (hp != hp)
        hp = CallFloatGetter(pawn, "GetHealth");
      if (s_pcs[k].hadPawn && hp <= 0.f && !s_pcs[k].since) {
        s_pcs[k].since = now;
        Logf("havoc: controller %p ship at %.0f health; forced respawn in %d s unless it respawns", pcs[i], hp,
             maxSecs);
      } else if (hp > 0.f) {
        s_pcs[k].since = 0;
      }
      s_pcs[k].hadPawn = true;
      continue;
    }
    if (!s_pcs[k].hadPawn) // not spawned yet: lobby / orbit intro
      continue;
    if (!s_pcs[k].since) {
      s_pcs[k].since = now;
      Logf("havoc: controller %p lost its ship; forced respawn in %d s unless it respawns", pcs[i], maxSecs);
      continue;
    }
    if (now - s_pcs[k].since < (uint64_t)maxSecs * 1000)
      continue;
    if (HavocIsEliminated(pcs[i])) // out until the wave ends (HavocReleaseEliminated)
      continue;
    s_pcs[k].since = now; // retry after another full wait if this one fails
    UObjectMin *fn = FindFnOnChain(pcs[i], "ServerRestartPlayer");
    if (fn && g_origProcessEvent)
      g_origProcessEvent(pcs[i], fn, nullptr);
    Logf("havoc: forced respawn of %p (%s)", pcs[i], fn ? "ServerRestartPlayer called" : "no ServerRestartPlayer");
  }
}

// A float seconds property of the Horde game mode (GameMode_Horde_BP CDO:
// m_waveStartDuration 10, m_waveEndDuration 10), 0 when absent.
static int HordeGmSecs(uint8_t *gm, const char *name) {
  int off = PropOffset((UObjectMin *)gm, name);
  return off > 0 && IsReadable(gm + off, 4) ? (int)*(float *)(gm + off) : 0;
}

// The team the human players are on (1 when none has a live ship). GUESS.
static int HumanTeam(uint8_t *gm) {
  uint8_t *pcs[64];
  int n = CollectHumanPCs(gm, pcs, 64);
  for (int i = 0; i < n; ++i) {
    uint8_t *hp = IsReadable(pcs[i] + OFF_CONTROLLER_PAWN, 8) ? *(uint8_t **)(pcs[i] + OFF_CONTROLLER_PAWN) : nullptr;
    if (hp && IsReadable(hp, OFF_SHIP_TEAM + 1) && (hp[OFF_SHIP_TEAM] == 1 || hp[OFF_SHIP_TEAM] == 2))
      return hp[OFF_SHIP_TEAM];
  }
  return 1;
}

// Ends a PvE episode: winner, the game state's native-called
// PVEMatchEnded(playersWon) (GameState_Horde_BP / _Escort_BP multicast the
// outro from it; its caller went with the server code), then EndMatch
// (vtable +0x668) as the Onslaught rule does.
static void PveEndMatch(uint8_t *gm, uint8_t *gs, bool playersWon, const char *why) {
  int team = HumanTeam(gm);
  gs[OFF_GS_WINNING_TEAM] = (uint8_t)(playersWon ? team : 3 - team);
  bool told = FindFnOnChain(gs, "PVEMatchEnded") &&
              CallFilled(gs, L"PVEMatchEnded", playersWon ? FillBoolTrue : nullptr, nullptr);
  Logf("pve: %s -- players %s (team %d), PVEMatchEnded %d, ending the match", why, playersWon ? "win" : "lose",
       team, told);
  void **vt = *(void ***)gm;
  ((void(__fastcall *)(void *))vt[VT_GM_END_MATCH / 8])(gm);
}

// The PvE start the server build would have run, for a map's event manager
// (PVE_HordeManager_BP, PVE_EscortManager_BP: both YPVEEventManager).
static void PveStartEventManager(uint8_t *gm, uint8_t *gs, UObjectMin *hm, UObjectMin *em, int esOff) {
  uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
  void **csm = (void **)((uint8_t *)hm + OFF_PVE_EM_COMBAT_SCENE_MANAGER);
  void *scene = ((tGetSceneManager)(g_base + RVA_COMBAT_SCENE_MANAGER))();
  Logf("havoc: hm.m_combatSceneManager %p, world scene manager %p %s", *csm, scene,
       scene ? ClassNameOf((UObjectMin *)scene) : "");
  if (!*csm && scene)
    *csm = scene;
  // PvE difficulty. PVE_EnemyGroupSpawn_Component picks the enemy name by
  // YGameState_PVE::GetPVEDifficulty (1 Recruit / 2 Veteran / 3 Legendary);
  // 0 has no case, so PawnIdToSpawn stays None and the group stalls with
  // GroupSpawnInProgress set (run 2026-10-13: m_pveDifficulty 0, index 0
  // of 3). First the manager's own native InitializePVEDifficulty (real
  // code, 0x75D7A0 -> vtable +0x108); if that leaves 0, the manager's
  // m_previewDifficulty -- the map's own value, 1 on HVC_Map1.
  // GUESS: the server build's source of the lobby-chosen difficulty is unknown.
  int dOff = PropOffset((UObjectMin *)gs, "m_pveDifficulty");
  int pOff = PropOffset(hm, "m_previewDifficulty");
  if (dOff > 0 && !gs[dOff]) {
    bool called = CallNoParms(hm, L"InitializePVEDifficulty");
    int after = gs[dOff];
    if (!gs[dOff] && pOff > 0 && ((uint8_t *)hm)[pOff])
      gs[dOff] = ((uint8_t *)hm)[pOff];
    Logf("havoc: pve difficulty: InitializePVEDifficulty called %d -> %d; now %d (preview %d)", called, after,
         gs[dOff], pOff > 0 ? ((uint8_t *)hm)[pOff] : -1);
  }
  // Start the combat scene manager. Its tick (0x2693A0) spawns entries only
  // while +0x444 is 0; BeginPlay (0x23B83D) sets it to 1 when the net mode
  // is 1, and only AYAICombatSceneManager::StartCombat (0x2681A0) clears it
  // -- it also schedules each queued entry's spawn time (+0x00 = world time
  // + the summed +0x80 delays). The game calls it only from the game mode's
  // team fill (0x381FA0, at 0x3822C6, notify=false), which Havoc never
  // runs, so AddNPCSpawnByDataset entries (time 0, pawn class resolved by
  // the tick from the dataset id at +0x18 via 0x24FD00) sat unspawned
  // (run 2026-10-13 23:17: 3 entries, cls null, inst 0, no tick errors).
  // GUESS: where the server build started combat in PvE is unknown; this
  // makes the same call the game makes, once, at PvE start.
  if (scene && IsReadable((uint8_t *)scene + OFF_SCENE_COMBAT_NOT_STARTED, 1)) {
    uint8_t before = ((uint8_t *)scene)[OFF_SCENE_COMBAT_NOT_STARTED];
    int queued = *(int32_t *)((uint8_t *)scene + OFF_SCENE_NPCS_COUNT);
    if (before)
      ((tSceneStartCombat)(g_base + RVA_SCENE_START_COMBAT))(scene, 0);
    Logf("havoc: scene manager StartCombat: not-started flag %u -> %u (%d entries queued)", before,
         ((uint8_t *)scene)[OFF_SCENE_COMBAT_NOT_STARTED], queued);
  }
  if (!em)
    *(UObjectMin **)(gm + OFF_HVC_GM_PVE_EVENT_MANAGER) = hm;
  if (!*(void **)(gs + OFF_HVC_GS_PVE_EVENT_MANAGER))
    *(UObjectMin **)(gs + OFF_HVC_GS_PVE_EVENT_MANAGER) = hm;
  if (DelegateBound(gm + OFF_HVC_GM_ON_PVE_MODE_STARTED) > 0) {
    ((void (*)(void *, void *))(base + RVA_MULTICAST_BROADCAST_NOPARMS))(gm + OFF_HVC_GM_ON_PVE_MODE_STARTED, nullptr);
    Logf("havoc: OnPVEModeStarted broadcast");
  } else {
    bool a = CallNoParms(hm, L"OnPVEModeStarted");
    bool b = CallNoParms(gs, L"OnPVEModeStarted_Event");
    Logf("havoc: OnPVEModeStarted unbound -- called directly: manager %d, game state %d", a, b);
  }
  if (ClassChainContains((UObjectMin *)gs, "YGameState_Horde"))
    Logf("havoc: after start: EventStarted %d, bound OnHordeWaveStateChanged %d, hm.Waves %d",
         esOff > 0 ? gs[esOff] : -1, DelegateBound(gs + OFF_HVC_GS_ON_WAVE_STATE_CHANGED),
         DelegateBound((uint8_t *)hm + OFF_HVC_HM_WAVES));
}

// ---------------------------------------------------------------------------
// Havoc wave modifiers (stage 2). The wave modifier component's server half is
// compiled out; the client half is intact: AYGameState_Havoc::
// ClientUpdateActiveWaveModifier(activeModifierNames, activePermanentEnemy
// ModifierNames) (thunk 0x7479B0, two TArray<FName>; parameter names from the
// UFunction construction 0x8D5071) fills the HUD's modifier list.
// Rules from the data: YHavocWaveModifierComponent m_desiredModifierSlotsNum
// = 2 (DefaultHavoc.ini), "NoModifier" fills an empty slot, "BossWave" marks
// a boss wave (DN_HavocBossWaves_DT 7/12/17/22). A row is eligible when
// m_minWave <= wave <= m_maxWave (-1 = open; 999 = never), weight > 0, and no
// chosen row lists it in m_excludes (or it a chosen row). Its m_feats go on
// the ships of m_affectedTeam for the wave and come off at its end.
// Permanent enemy modifiers (DN_HavocPermanentEnemyModifiers_DT): every row
// with m_wave <= the wave applies to every enemy ship; AddFeat (0x337850)
// stacks a feat the ship already has when the feat allows it (existing +0x51,
// 0x14033F060), which is how the repeated HVC_PEM_EnemyDamageBoost5 rows add up.
// GUESS: the selection is weighted random without repeats; boss waves get only
// "BossWave". The original's selection code is not in this exe.
struct HavocModifier {
  const wchar_t *id, *feats, *excludes;
  int minWave, maxWave, weight;
  char team; // 'A' all, 'P' players, 'E' enemies, 'N' none
};
struct HavocEnemyBoost {
  const wchar_t *id, *feat;
  int wave;
};
// DN_HavocModifiers_DT, verbatim (generated 2026-10-19): row, m_feats, m_excludes,
// m_minWave, m_maxWave, m_weight, m_affectedTeam (A all / P players / E enemies /
// N none). Feats and excludes are ";"-separated, as in the table.
static const HavocModifier kHavocModifiers[] = {
    {L"NoModifier", L"", L"", 0, 0, 1, 'N'},
    {L"BossWave", L"", L"", 999, 1, 0, 'A'},
    {L"HVC_enemies_damageresistanceFrontFull_damageresistanceBackNegative", L"HVC_damageresistanceFrontFull_damageresistanceBackNegative_1;HVC_damageresistanceFrontFull_damageresistanceBackNegative_2", L"", 1, -1, 1, 'A'},
    {L"HVC_global_abilitiesDamageHigh_abilitiesCooldownHigh", L"HVC_abilitiesDamageHigh_abilitiesCooldownHigh", L"HVC_global_abilitiesDamageLow_abilitiesCooldownLow", 1, -1, 1, 'A'},
    {L"HVC_global_abilitiesDamageLow_abilitiesCooldownLow", L"HVC_abilitiesDamageLow_abilitiesCooldownLow", L"HVC_global_abilitiesDamageHigh_abilitiesCooldownHigh", 1, -1, 1, 'A'},
    {L"HVC_global_damageresistanceBackNegative", L"HVC_damageresistanceBackNegative", L"", 1, -1, 1, 'A'},
    {L"HVC_global_energyNone", L"HVC_energyNone", L"HVC_global_energyRegenFast_energyCooldownLow_energyMaxLow;HVC_global_energyRegenHit_abilitiesCooldownHit;HVC_global_energyRegenNone_energyMaxHigh;HVC_players_energyFullKill_energyRegenNone;HVC_players_movementNone_movementFast;", 1, -1, 1, 'A'},
    {L"HVC_global_energyRegenFast_energyCooldownLow_energyMaxLow", L"HVC_energyRegenFast_energyCooldownLow_energyMaxLow", L"HVC_global_energyNone;HVC_players_energyFullKill_energyRegenNone;HVC_global_energyRegenNone_energyMaxHigh;", 1, -1, 1, 'A'},
    {L"HVC_global_energyRegenHit_abilitiesCooldownHit", L"HVC_energyRegenHit_abilitiesCooldownHit", L"HVC_global_energyNone", 1, -1, 1, 'A'},
    {L"HVC_global_energyRegenNone_energyMaxHigh", L"HVC_energyRegenNone_energyMaxHigh", L"HVC_global_energyNone;HVC_global_energyRegenFast_energyCooldownLow_energyMaxLow;HVC_players_energyFullKill_energyRegenNone;", 1, -1, 1, 'A'},
    {L"HVC_global_energyUnlimted_healthDecreaseHit", L"HVC_energyUnlimted_healthDecreaseHit_1;HVC_energyUnlimted_healthDecreaseHit_2;", L"", 999, -1, 1, 'A'},
    {L"HVC_global_healthRegenFast_healthCooldownHigh", L"HVC_healthRegenFast_healthCooldownHigh", L"HVC_global_healthRegenHit_healthRegenSlow;HVC_global_healthRegenNone_healthMaxHigh;HVC_players_healthFullKill_healthRegenNone;", 1, -1, 1, 'A'},
    {L"HVC_global_healthRegenHit_healthRegenSlow", L"HVC_healthRegenHit_healthRegenSlow_1;HVC_healthRegenHit_healthRegenSlow_2", L"HVC_global_healthRegenFast_healthCooldownHigh;HVC_players_healthFullKill_healthRegenNone", 1, -1, 1, 'A'},
    {L"HVC_global_healthRegenNone_healthMaxHigh", L"HVC_healthRegenNone_healthMaxHigh", L"HVC_global_healthRegenFast_healthCooldownHigh;HVC_players_healthFullKill_healthRegenNone", 1, -1, 1, 'A'},
    {L"HVC_global_weaponFirerateLow_weaponDamageHigh", L"HVC_weaponFirerateLow_weaponDamageHigh", L"", 1, -1, 1, 'A'},
    {L"HVC_players_abilitiesCooldownResetKill", L"HVC_abilitiesCooldownResetKill", L"", 1, -1, 1, 'A'},
    {L"HVC_players_aimLowWhileMoving_movementForwardFast", L"HVC_aimLowWhileMoving_movementForwardFast", L"", 1, -1, 1, 'A'},
    {L"HVC_players_movementNone_movementFast", L"HVC_movementNone_movementFast_1;HVC_movementNone_movementFast_2;", L"HVC_global_energyNone", 1, -1, 1, 'P'},
    {L"HVC_players_energyFullKill_energyRegenNone", L"HVC_energyFullKill_energyRegenNone_1;HVC_energyFullKill_energyRegenNone_2;", L"HVC_global_energyRegenNone_energyMaxHigh;HVC_global_energyRegenFast_energyCooldownLow_energyMaxLow;HVC_global_energyNone;", 1, -1, 1, 'A'},
    {L"HVC_players_energyMaxDecreaseKill", L"HVC_energyMaxDecreaseKill", L"", 999, -1, 1, 'A'},
    {L"HVC_players_energyUnlimted_wheelRestrictions_healthRegenNone", L"HVC_energyUnlimted_wheelRestrictions", L"", 999, -1, 1, 'A'},
    {L"HVC_players_healingNone_healingDamageresistanceFull", L"HVC_healingNone_healingDamageresistanceFull_1;HVC_healingNone_healingDamageresistanceFull_2;HVC_healingNone_healingDamageresistanceFull_3;", L"", 1, -1, 1, 'P'},
    {L"HVC_players_healthFullKill_healthRegenNone", L"HVC_healthFullKill_healthRegenNone_1;HVC_healthFullKill_healthRegenNone_2;", L"HVC_global_healthRegenNone_healthMaxHigh;HVC_global_healthRegenHit_healthRegenSlow;HVC_global_healthRegenFast_healthCooldownHigh;", 1, -1, 1, 'A'},
    {L"HVC_players_healthMaxDecreaseKill", L"HVC_healthMaxDecreaseKill", L"", 999, -1, 1, 'A'},
    {L"HVC_players_healthMaxIncreaseHeal", L"HVC_healthMaxIncreaseHeal", L"", 999, -1, 1, 'A'},
    {L"HVC_players_movementVerticalNone", L"HVC_movementVerticalNone", L"", 999, -1, 1, 'A'},
};
// DN_HavocPermanentEnemyModifiers_DT, verbatim: row, m_feats, m_wave.
static const HavocEnemyBoost kHavocEnemyBoosts[] = {
    {L"EnemyBoost_01_Damage", L"HVC_PEM_EnemyDamageBoost5", 3},
    {L"EnemyBoost_01_Health", L"HVC_PEM_EnemyHealthBoost5", 3},
    {L"EnemyBoost_02_Damage", L"HVC_PEM_EnemyDamageBoost5", 4},
    {L"EnemyBoost_02_Health", L"HVC_PEM_EnemyHealthBoost5", 4},
    {L"EnemyBoost_03_Damage", L"HVC_PEM_EnemyDamageBoost5", 5},
    {L"EnemyBoost_03_Health", L"HVC_PEM_EnemyHealthBoost5", 5},
    {L"EnemyBoost_04_Damage", L"HVC_PEM_EnemyDamageBoost5", 6},
    {L"EnemyBoost_04_Health", L"HVC_PEM_EnemyHealthBoost5", 6},
    {L"EnemyBoost_05_Damage", L"HVC_PEM_EnemyDamageBoost5", 8},
    {L"EnemyBoost_05_Health", L"HVC_PEM_EnemyHealthBoost5", 8},
    {L"EnemyBoost_06_Damage", L"HVC_PEM_EnemyDamageBoost5", 9},
    {L"EnemyBoost_06_Health", L"HVC_PEM_EnemyHealthBoost5", 9},
    {L"EnemyBoost_07_Damage", L"HVC_PEM_EnemyDamageBoost10", 10},
    {L"EnemyBoost_07_Health", L"HVC_PEM_EnemyHealthBoost10", 10},
    {L"EnemyBoost_08_Damage", L"HVC_PEM_EnemyDamageBoost10", 11},
    {L"EnemyBoost_08_Health", L"HVC_PEM_EnemyHealthBoost10", 11},
    {L"EnemyBoost_09_Damage", L"HVC_PEM_EnemyDamageBoost10", 13},
    {L"EnemyBoost_09_Health", L"HVC_PEM_EnemyHealthBoost10", 13},
    {L"EnemyBoost_10_Damage", L"HVC_PEM_EnemyDamageBoost10", 14},
    {L"EnemyBoost_10_Health", L"HVC_PEM_EnemyHealthBoost10", 14},
    {L"EnemyBoost_11_Damage", L"HVC_PEM_EnemyDamageBoost10", 15},
    {L"EnemyBoost_11_Health", L"HVC_PEM_EnemyHealthBoost10", 15},
    {L"EnemyBoost_12_Damage", L"HVC_PEM_EnemyDamageBoost10", 16},
    {L"EnemyBoost_12_Health", L"HVC_PEM_EnemyHealthBoost10", 16},
    {L"EnemyBoost_13_Damage", L"HVC_PEM_EnemyDamageBoost15", 18},
    {L"EnemyBoost_13_Health", L"HVC_PEM_EnemyHealthBoost15", 18},
    {L"EnemyBoost_14_Damage", L"HVC_PEM_EnemyDamageBoost15", 19},
    {L"EnemyBoost_14_Health", L"HVC_PEM_EnemyHealthBoost15", 19},
    {L"EnemyBoost_15_Damage", L"HVC_PEM_EnemyDamageBoost15", 20},
    {L"EnemyBoost_15_Health", L"HVC_PEM_EnemyHealthBoost15", 20},
    {L"EnemyBoost_16_Damage", L"HVC_PEM_EnemyDamageBoost15", 21},
    {L"EnemyBoost_16_Health", L"HVC_PEM_EnemyHealthBoost15", 21},
    {L"EnemyBoost_17_Damage", L"HVC_PEM_EnemyDamageBoost15", 23},
    {L"EnemyBoost_17_Health", L"HVC_PEM_EnemyHealthBoost15", 23},
};
// DN_HavocBossWaves_DT m_waveNumber.
static const int kHavocBossWaves[] = {7, 12, 17, 22};
static const int kHavocModifierCount = (int)(sizeof kHavocModifiers / sizeof kHavocModifiers[0]);
static const int kHavocEnemyBoostCount = (int)(sizeof kHavocEnemyBoosts / sizeof kHavocEnemyBoosts[0]);
#define RVA_FEATS_REMOVE_FEAT 0x359770 // (featsComp, FName): RemoveFeat thunk 0x76ECE0's body
typedef void(__fastcall *tRemoveFeat)(void *featsComp, uint64_t featName);

static int g_hvcActiveMods[2] = {-1, -1}; // kHavocModifiers index
static volatile LONG g_hvcModGen = 0;      // bumped per wave: pawns re-check their feats
static int g_hvcModWave = 0;               // the wave the active modifiers are for

static int HavocModifierIndex(const wchar_t *id) {
  for (int i = 0; i < kHavocModifierCount; ++i)
    if (!wcscmp(kHavocModifiers[i].id, id))
      return i;
  return -1;
}
static bool HavocListHas(const wchar_t *list, const wchar_t *id) {
  size_t n = wcslen(id);
  for (const wchar_t *p = list; p && *p;) {
    const wchar_t *e = wcschr(p, L';');
    size_t len = e ? (size_t)(e - p) : wcslen(p);
    if (len == n && !wcsncmp(p, id, n))
      return true;
    p = e ? e + 1 : nullptr;
  }
  return false;
}
static bool HavocIsBossWave(int wave) {
  for (int w : kHavocBossWaves)
    if (w == wave)
      return true;
  return false;
}
static uint32_t HavocRand() {
  static uint32_t s = 0;
  if (!s)
    s = GetTickCount() | 1;
  s ^= s << 13, s ^= s >> 17, s ^= s << 5;
  return s;
}

struct HavocNameArrays {
  FNameMin active[4];
  int nActive;
  FNameMin permanent[64];
  int nPermanent;
};
static void FillModifierArrays(uint8_t *parms, const char *pc, const char *pn, uint32_t off, void *ctx) {
  HavocNameArrays *a = (HavocNameArrays *)ctx;
  if (strcmp(pc, "ArrayProperty"))
    return;
  bool perm = strstr(pn, "Permanent") != nullptr;
  *(void **)(parms + off) = perm ? (void *)a->permanent : (void *)a->active;
  *(int32_t *)(parms + off + 8) = perm ? a->nPermanent : a->nActive;
  *(int32_t *)(parms + off + 12) = perm ? a->nPermanent : a->nActive;
}

// The upgrade panel (UpgradeSelection) is headed "Wave N+1" and lists the
// game state's active wave modifiers and permanent enemy boosts, so they are
// picked and sent when the panel opens (HavocPrepareWaveModifiers) and put on
// the ships at the wave's start. Picking only at the start showed the wave
// just played under the next wave's title (headless havoc-hud 2026-10-23: the
// "Wave 3" panel listed wave 2's energyNone + healthRegenHit). GUESS: the
// original server sent the preview at the same moment.
static int g_hvcPreparedMods[2] = {-1, -1};
static int g_hvcPreparedWave = 0;

static void HavocPickWaveModifiers(int wave, int mods[2]) {
  mods[0] = mods[1] = -1;
  if (HavocIsBossWave(wave)) {
    mods[0] = HavocModifierIndex(L"BossWave");
  } else {
    for (int slot = 0; slot < 2; ++slot) {
      int total = 0;
      int ok[64], nok = 0;
      for (int i = 0; i < kHavocModifierCount && nok < 64; ++i) {
        const HavocModifier &m = kHavocModifiers[i];
        if (m.team == 'N' || m.weight <= 0 || wave < m.minWave || (m.maxWave >= 0 && wave > m.maxWave))
          continue;
        bool clash = false;
        for (int s = 0; s < slot; ++s) {
          int c = mods[s];
          if (c == i || (c >= 0 && (HavocListHas(kHavocModifiers[c].excludes, m.id) ||
                                    HavocListHas(m.excludes, kHavocModifiers[c].id))))
            clash = true;
        }
        if (!clash)
          ok[nok++] = i, total += m.weight;
      }
      if (total <= 0)
        break;
      int r = (int)(HavocRand() % (uint32_t)total);
      for (int k = 0; k < nok; ++k)
        if ((r -= kHavocModifiers[ok[k]].weight) < 0) {
          mods[slot] = ok[k];
          break;
        }
    }
  }
}

static void HavocSendWaveModifiers(uint8_t *gs, int wave, const int mods[2], const char *what) {
  HavocNameArrays a = {};
  for (int s = 0; s < 2; ++s) {
    const wchar_t *id = mods[s] >= 0 ? kHavocModifiers[mods[s]].id : L"NoModifier";
    ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&a.active[a.nActive++], id, 1);
  }
  for (int i = 0; i < kHavocEnemyBoostCount && a.nPermanent < 64; ++i)
    if (kHavocEnemyBoosts[i].wave <= wave)
      ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&a.permanent[a.nPermanent++], kHavocEnemyBoosts[i].id, 1);
  bool sent = CallNamed(gs, L"ClientUpdateActiveWaveModifier", FillModifierArrays, &a);
  Logf("havoc: wave %d modifiers %ls / %ls, %d permanent enemy modifier(s) (%s)%s", wave,
       mods[0] >= 0 ? kHavocModifiers[mods[0]].id : L"NoModifier",
       mods[1] >= 0 ? kHavocModifiers[mods[1]].id : L"NoModifier", a.nPermanent, what,
       sent ? "" : " -- ClientUpdateActiveWaveModifier NOT FOUND");
}

// Upgrade selection opens: pick wave `wave`'s modifiers and show them.
static void HavocPrepareWaveModifiers(uint8_t *gs, int wave) {
  HavocPickWaveModifiers(wave, g_hvcPreparedMods);
  g_hvcPreparedWave = wave;
  HavocSendWaveModifiers(gs, wave, g_hvcPreparedMods, "preview");
}

// The wave starts: its modifiers (the previewed ones unless /wave moved it)
// go live and every client is told again.
static void HavocStartWaveModifiers(uint8_t *gs, int wave) {
  bool prepared = g_hvcPreparedWave == wave;
  if (prepared) {
    g_hvcActiveMods[0] = g_hvcPreparedMods[0];
    g_hvcActiveMods[1] = g_hvcPreparedMods[1];
  } else {
    HavocPickWaveModifiers(wave, g_hvcActiveMods);
  }
  g_hvcPreparedWave = 0;
  g_hvcModWave = wave;
  InterlockedIncrement(&g_hvcModGen);
  HavocSendWaveModifiers(gs, wave, g_hvcActiveMods, prepared ? "start, as previewed" : "start");
}

// Every feat of a ";" list, one call each.
static int HavocForEachFeat(const wchar_t *list, void *comp, bool add, const char *what, void *pawn) {
  int done = 0;
  wchar_t buf[128];
  for (const wchar_t *p = list; p && *p;) {
    const wchar_t *e = wcschr(p, L';');
    size_t len = e ? (size_t)(e - p) : wcslen(p);
    if (len > 0 && len < 128) {
      memcpy(buf, p, len * sizeof(wchar_t));
      buf[len] = 0;
      FNameMin name{};
      ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&name, buf, 1);
      bool ok = true;
      __try {
        if (add)
          ok = ((tAddFeat)(g_base + RVA_FEATS_ADD_FEAT))(comp, *(uint64_t *)&name);
        else
          ((tRemoveFeat)(g_base + RVA_FEATS_REMOVE_FEAT))(comp, *(uint64_t *)&name);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
      }
      static int s_logged = 0;
      if (s_logged++ < 60 || !ok)
        Logf("havoc: %s feat %ls %s %s -> %s", what, buf, add ? "on" : "off", ClassNameOf(pawn), ok ? "ok" : "FAILED");
      ++done;
    }
    p = e ? e + 1 : nullptr;
  }
  return done;
}

// Puts the wave's modifiers and the permanent enemy modifiers on every ship
// that does not have them yet (new enemies, respawned players). Each director
// tick, so a ship spawned mid-wave is covered within a second.
static bool HavocPawnIsPlayer(void *pawn) {
  uint8_t *ctrl = IsReadable((uint8_t *)pawn + OFF_PAWN_CONTROLLER, 8) ? *(uint8_t **)((uint8_t *)pawn + OFF_PAWN_CONTROLLER) : nullptr;
  return ctrl && IsReadable(ctrl, sizeof(UObjectMin)) && ClassChainContains((UObjectMin *)ctrl, "PlayerController");
}
// A recorded ship that still exists: its feats component is still owned by it
// and it is a living YPawn. A destroyed ship's memory gets reused (live log
// 2026-10-19: AddFeat "on None" / on a weapon actor -> FAILED), so all three.
static bool HavocLiveShip(const FeatsByPawn &e) {
  if (!e.pawn || !IsReadable(e.comp, OFF_COMPONENT_OWNER + 8) || *(void **)((uint8_t *)e.comp + OFF_COMPONENT_OWNER) != e.pawn)
    return false;
  uint8_t *pw = (uint8_t *)e.pawn;
  return IsReadable(pw + OFF_PAWN_STATE, 1) && pw[OFF_PAWN_STATE] != 2 && pw[OFF_PAWN_STATE] != 3 &&
         ClassChainContains((UObjectMin *)pw, "YPawn");
}
static void HavocApplyModifiers() {
  LONG gen = g_hvcModGen;
  for (FeatsByPawn &e : g_featsByPawn) {
    if (!HavocLiveShip(e))
      continue;
    uint8_t *ctrl = IsReadable((uint8_t *)e.pawn + OFF_PAWN_CONTROLLER, 8) ? *(uint8_t **)((uint8_t *)e.pawn + OFF_PAWN_CONTROLLER) : nullptr;
    if (!ctrl)
      continue; // not possessed yet: its side is not known
    bool player = HavocPawnIsPlayer(e.pawn);
    if (e.modGen != gen) {
      e.modGen = gen;
      for (int s = 0; s < 2; ++s) {
        e.modsOn[s] = -1;
        int m = g_hvcActiveMods[s];
        if (m < 0)
          continue;
        char t = kHavocModifiers[m].team;
        if (t == 'A' || (t == 'P' && player) || (t == 'E' && !player)) {
          HavocForEachFeat(kHavocModifiers[m].feats, e.comp, true, "wave modifier", e.pawn);
          e.modsOn[s] = m;
        }
      }
    }
    if (!player)
      for (; e.permApplied < kHavocEnemyBoostCount && kHavocEnemyBoosts[e.permApplied].wave <= g_hvcModWave; ++e.permApplied)
        HavocForEachFeat(kHavocEnemyBoosts[e.permApplied].feat, e.comp, true, "permanent enemy modifier", e.pawn);
  }
}
// At a wave's end the wave modifiers come off every ship that has them.
static void HavocEndWaveModifiers() {
  for (FeatsByPawn &e : g_featsByPawn) {
    if (!HavocLiveShip(e)) {
      e.modsOn[0] = e.modsOn[1] = -1;
      continue;
    }
    for (int s = 0; s < 2; ++s)
      if (e.modsOn[s] >= 0) {
        HavocForEachFeat(kHavocModifiers[e.modsOn[s]].feats, e.comp, false, "wave modifier", e.pawn);
        e.modsOn[s] = -1;
      }
  }
  g_hvcActiveMods[0] = g_hvcActiveMods[1] = -1;
  InterlockedIncrement(&g_hvcModGen);
}

// ---------------------------------------------------------------------------
// Havoc game over (stage 2). PlayersWin / PlayersLose are compiled out (empty
// thunk 0x74C640). PVE_HordeManager_BP calls PlayersWin 4 s after the last
// wave of Waves when the map has no WaveToLoopBackTo; Havoc maps loop
// (SetWaveIndex back to that wave), so a run ends only when it is lost.
// Nothing in the cooked BPs calls PlayersLose: the rule was server code.
// YHavocHUDTeamInfo carries m_eliminated per player, and the team refills
// respawns only after a wave (m_refillPlayerTeamRespawnsAfterWave = 1,
// m_respawnsToRefill = 0): a ship lost during a wave is out until it ends.
// GUESS: the run is lost when no connected player has a living ship for 3 s
// during a wave. Respawns during a wave are NOT blocked yet (the host's
// respawn request is not traced), so in a group a destroyed player can come
// back early; solo, the rule is exact.
//
// At game over the director: EnterNewState(GameOver) (GameState_Havoc_BP plays
// its outro), reports each player's run to mmogbrain (GET /battle/havoc: the
// run's score is the team's total XP, m_totalXP -- YEOMHavocProgressionData
// m_teamTotalXP), sends the answer to the player's client --
// AYPlayerReplicationInfo_Havoc::ClientUpdateHighscore(lifetimePlayerScore,
// weeklyPlayerScore, matchScore) and ClientUpdateWeeklyScoreResetTime -- which
// the end-of-match screen reads (+0xA4C / +0xA48 / +0xA44 / +0xA58, 0x449470;
// it waits while +0xA44 is -1), marks the player's Havoc EOM data finalized
// (m_isInEndOfMatchDataFinalized, the byte AYPlayerController_Havoc::
// IsInEndOfMatchDataFinalized 0x401D20 checks), and ends the match 6 s later
// through EndMatch (vtable +0x668) with the enemy team as the winner.

static int64_t HavocField(const char *body, const char *key) {
  char k[32];
  _snprintf_s(k, sizeof k, _TRUNCATE, "%s=", key);
  for (const char *p = body; p && *p;) {
    if (!strncmp(p, k, strlen(k)))
      return _atoi64(p + strlen(k));
    p = strchr(p, '\n');
    if (p)
      ++p;
  }
  return -1;
}
struct HavocHighscore { int32_t lifetime, weekly, match; int64_t reset; };
static void FillHighscore(uint8_t *parms, const char *pc, const char *pn, uint32_t off, void *ctx) {
  HavocHighscore *h = (HavocHighscore *)ctx;
  if (!strcmp(pn, "lifetimePlayerScore"))
    *(int32_t *)(parms + off) = h->lifetime;
  else if (!strcmp(pn, "weeklyPlayerScore"))
    *(int32_t *)(parms + off) = h->weekly;
  else if (!strcmp(pn, "matchScore"))
    *(int32_t *)(parms + off) = h->match;
  else if (!strcmp(pn, "weeklyScoreResetTime")) {
    if (!strcmp(pc, "Int64Property"))
      *(int64_t *)(parms + off) = h->reset;
    else
      *(int32_t *)(parms + off) = (int32_t)h->reset;
  }
}

static void HavocReportRuns(uint8_t *gs) {
  int score = *(int32_t *)(gs + OFF_HVC_GS_TOTAL_XP);
  int waves = *(int32_t *)(gs + OFF_HVC_GS_WAVES_REACHED);
  char match[96];
  if (!MatchIDFromCommandLine(match, sizeof match)) {
    Logf("havoc: game over -- no -MatchID= on the command line; runs not reported");
    return;
  }
  uint8_t *pcs[16];
  int npc = HavocHumanPCs(gs, pcs, 16);
  for (int i = 0; i < npc; ++i) {
    uint8_t *pri = *(uint8_t **)(pcs[i] + OFF_PC_PLAYER_STATE);
    char pid[80];
    if (!PlayerPIDForController(pcs[i], pid, sizeof pid) && !RememberedControllerPID(pcs[i], pid, sizeof pid)) {
      Logf("havoc: game over -- controller %p has no DNPID; run not reported", pcs[i]);
      continue;
    }
    char path[512], body[1024];
    _snprintf_s(path, sizeof path, _TRUNCATE, "/battle/havoc?match=%s&pid=%s&score=%d&waves=%d", match, pid, score, waves);
    bool ok = HttpGetLoopback(path, body, sizeof body);
    HavocHighscore h = {0, 0, score, 0};
    if (ok) {
      int64_t v;
      if ((v = HavocField(body, "lifetime")) >= 0) h.lifetime = (int32_t)v;
      if ((v = HavocField(body, "weekly")) >= 0) h.weekly = (int32_t)v;
      if ((v = HavocField(body, "reset")) >= 0) h.reset = v;
    }
    bool sentScore = CallNamed(pri, L"ClientUpdateHighscore", FillHighscore, &h);
    bool sentReset = CallNamed(pri, L"ClientUpdateWeeklyScoreResetTime", FillHighscore, &h);
    int fin = PropOffset((UObjectMin *)pri, "m_isInEndOfMatchDataFinalized");
    if (fin > 0 && IsReadable(pri + fin, 1))
      pri[fin] = 1;
    for (char *c = body; *c; ++c)
      if (*c == '\n') *c = ' ';
    Logf("havoc: game over -- %s score %d waves %d -> %s%s; highscore sent %d, reset sent %d, EOM finalized byte +0x%X",
         pid, score, waves, ok ? "mmogbrain: " : "mmogbrain UNREACHABLE", ok ? body : "", sentScore, sentReset, fin);
  }
}

// Living ships of connected players; -1 when there is no connected player or
// one of them has not had a living ship yet this run (still choosing one, or
// in the orbit intro) -- the run cannot be lost before everyone is in.
static void *g_hvcSeenAlive[16];
static int HavocPlayersAlive(uint8_t *gs, int *humansOut) {
  uint8_t *pcs[16];
  int npc = HavocHumanPCs(gs, pcs, 16), alive = 0;
  bool allSeen = true;
  *humansOut = npc;
  for (int i = 0; i < npc; ++i) {
    uint8_t *pawn = IsReadable(pcs[i] + OFF_CONTROLLER_PAWN, 8) ? *(uint8_t **)(pcs[i] + OFF_CONTROLLER_PAWN) : nullptr;
    bool up = pawn && IsReadable(pawn + OFF_PAWN_STATE, 1) && pawn[OFF_PAWN_STATE] != 2 &&
              pawn[OFF_PAWN_STATE] != 3 && ClassChainContains((UObjectMin *)pawn, "YPawn");
    int slot = -1, freeSlot = -1;
    for (int k = 0; k < 16; ++k) {
      if (g_hvcSeenAlive[k] == pcs[i])
        slot = k;
      else if (!g_hvcSeenAlive[k] && freeSlot < 0)
        freeSlot = k;
    }
    if (up) {
      ++alive;
      if (slot < 0 && freeSlot >= 0)
        g_hvcSeenAlive[freeSlot] = pcs[i];
    } else if (slot < 0)
      allSeen = false;
  }
  return npc && allSeen ? alive : -1;
}

// ---------------------------------------------------------------------------
// Havoc respawn blocking (stage 3; dn_host_no_havoc_respawn_block.txt turns it
// off). A ship lost during a wave stays out until the wave ends:
// YHavocHUDTeamInfo m_eliminated, m_refillPlayerTeamRespawnsAfterWave 1 with
// m_respawnsToRefill 0. The gate: AYPlayerController::SpawnPlayer (0x5998A0)
// first asks the game mode (vtable +0x870 -- on GameMode_Havoc_BP 0x368380,
// read live 2026-10-19: allowed while the player's respawn count is under
// gm+0x6A4, 0 = unlimited) and returns WITHOUT spawning, leaving the pending
// flag PC +0xB88 set, when it says no. Nothing retries, so at the wave's end
// the director calls SpawnPlayer for every eliminated player still pending.
// GUESS: a player is eliminated when their ship is destroyed while a wave is
// in progress; one destroyed between waves respawns at once.
tSpawnGate g_origSpawnGate = nullptr;
static void *volatile g_hvcEliminated[16];

static bool HavocRespawnBlockOn() {
  static int s_on = -1;
  if (s_on < 0)
    s_on = SwitchOn("DN_HOST_NO_HAVOC_RESPAWN_BLOCK", "dn_host_no_havoc_respawn_block.txt") ? 0 : 1;
  return s_on == 1;
}
static bool HavocIsEliminated(void *pc) {
  for (void *e : g_hvcEliminated)
    if (e && e == pc)
      return true;
  return false;
}

static uint32_t __fastcall HookSpawnGate(void *gm, void *pc) {
  if (pc && HavocIsEliminated(pc)) {
    static void *s_lastLogged = nullptr;
    if (s_lastLogged != pc) {
      s_lastLogged = pc;
      Logf("havoc: respawn of %p held -- eliminated until the wave ends", pc);
    }
    return 0;
  }
  return g_origSpawnGate(gm, pc);
}

// During a wave: a player seen with a living ship who now has none is out.
static void HavocTrackEliminations(uint8_t *gs) {
  if (!HavocRespawnBlockOn())
    return;
  uint8_t *pcs[16];
  int npc = HavocHumanPCs(gs, pcs, 16);
  for (int i = 0; i < npc; ++i) {
    uint8_t *pawn = IsReadable(pcs[i] + OFF_CONTROLLER_PAWN, 8) ? *(uint8_t **)(pcs[i] + OFF_CONTROLLER_PAWN) : nullptr;
    bool up = pawn && IsReadable(pawn + OFF_PAWN_STATE, 1) && pawn[OFF_PAWN_STATE] != 2 && pawn[OFF_PAWN_STATE] != 3;
    bool seen = false;
    for (void *s : g_hvcSeenAlive)
      seen |= s == pcs[i];
    if (up || !seen || HavocIsEliminated(pcs[i]))
      continue;
    for (auto &e : g_hvcEliminated)
      if (!e) {
        e = pcs[i];
        Logf("havoc: %p eliminated -- ship lost during the wave, no respawn until it ends", pcs[i]);
        break;
      }
  }
}

// At the wave's end: everyone is back in; held spawns go through now.
static void HavocReleaseEliminated() {
  for (auto &e : g_hvcEliminated) {
    void *pc = e;
    e = nullptr;
    if (!pc || !IsReadable((uint8_t *)pc + OFF_PC_SPAWN_PENDING, 1))
      continue;
    uint8_t *p = (uint8_t *)pc;
    void *pawn = IsReadable(p + OFF_CONTROLLER_PAWN, 8) ? *(void **)(p + OFF_CONTROLLER_PAWN) : nullptr;
    bool pending = p[OFF_PC_SPAWN_PENDING] != 0;
    if (pending) {
      __try {
        ((tSpawnPlayer)(g_base + RVA_SPAWN_PLAYER))(pc);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("havoc: EXCEPTION releasing the held respawn of %p", pc);
      }
    }
    void *now = IsReadable(p + OFF_CONTROLLER_PAWN, 8) ? *(void **)(p + OFF_CONTROLLER_PAWN) : nullptr;
    Logf("havoc: %p back in -- %s (ship %p -> %p)", pc,
         pending ? "held respawn released" : "no respawn was requested yet", pawn, now);
  }
}

// ---------------------------------------------------------------------------
// Havoc team HUD (stage 4, 2026-10-19). AYGameState_Havoc::
// ClientHavocPlayersChanged(teamsInfo) (thunk 0x745410, NetMulticast) carries
// ONE YHavocHUDTeamInfo of parallel arrays (0x38 bytes: +0x08 TArray<FString>
// m_playerID, +0x18 TArray<int32> m_classID, +0x28 TArray<uint8>
// m_eliminated -- the thunk's copy); its _Implementation (GS vtable +0x888 =
// 0x3689E0) hands the class and eliminated arrays to the HUD's team list
// (0x5598B0). The client's own refresh (AYHUD::UpdateHavocTeamState 0x562C00)
// builds the same two arrays from the player states (class = PRI +0x941;
// eliminated when it is 0x10 or 0x13-0x15), but nothing on a login-less host
// marks a player eliminated there, so the director sends the list itself:
// one entry per connected player in PlayerArray order, the class the client
// would read, eliminated from HavocTrackEliminations. Offsets are read from the
// struct's reflection when found (logged once), else the thunk's.
// GUESS: m_playerID is left empty -- the implementation does not read it.
static void HavocTeamInfoLayout(UObjectMin *structProp, int *offId, int *offClass, int *offElim) {
  static int s_id = -1, s_class = -1, s_elim = -1, s_done = 0;
  if (!s_done && structProp) {
    s_done = 1;
    for (int k = 0x70; k <= 0x80; k += 8) {
      UObjectMin *st = IsReadable((uint8_t *)structProp + k, 8) ? *(UObjectMin **)((uint8_t *)structProp + k) : nullptr;
      if (!st || !IsReadable(st, 0x40) || strcmp(ClassNameOf(st), "ScriptStruct"))
        continue;
      for (UObjectMin *f = *(UObjectMin **)((uint8_t *)st + 0x38); f && IsReadable(f, 0x58);
           f = *(UObjectMin **)((uint8_t *)f + 0x28)) {
        const char *n = NameText(f->Name);
        int off = (int)*(uint32_t *)((uint8_t *)f + 0x50);
        if (n && !strcmp(n, "m_playerID")) s_id = off;
        else if (n && !strcmp(n, "m_classID")) s_class = off;
        else if (n && !strcmp(n, "m_eliminated")) s_elim = off;
      }
      Logf("havoc: YHavocHUDTeamInfo (struct at UStructProperty +0x%X): m_playerID +0x%X, m_classID +0x%X, "
           "m_eliminated +0x%X", k, s_id, s_class, s_elim);
      break;
    }
  }
  *offId = s_id >= 0 ? s_id : 0x08;
  *offClass = s_class >= 0 ? s_class : 0x18;
  *offElim = s_elim >= 0 ? s_elim : 0x28;
}

struct HavocTeamInfoArgs {
  int32_t classes[16];
  uint8_t elim[16];
  int n;
};
static void FillTeamInfo(uint8_t *parms, const char *pc, const char *pn, uint32_t off, void *ctx) {
  HavocTeamInfoArgs *a = (HavocTeamInfoArgs *)ctx;
  if (strcmp(pc, "StructProperty") || off + 0x38 > 256)
    return;
  extern UObjectMin *g_hvcTeamInfoProp;
  int oId, oClass, oElim;
  HavocTeamInfoLayout(g_hvcTeamInfoProp, &oId, &oClass, &oElim);
  uint8_t *s = parms + off;
  *(void **)(s + oId) = nullptr; // TArray<FString>: empty
  *(int32_t *)(s + oId + 8) = 0;
  *(int32_t *)(s + oId + 12) = 0;
  *(void **)(s + oClass) = a->classes;
  *(int32_t *)(s + oClass + 8) = a->n;
  *(int32_t *)(s + oClass + 12) = a->n;
  *(void **)(s + oElim) = a->elim;
  *(int32_t *)(s + oElim + 8) = a->n;
  *(int32_t *)(s + oElim + 12) = a->n;
}
UObjectMin *g_hvcTeamInfoProp = nullptr;

// Sends the team list when it changed (players, classes or eliminations).
static void HavocSendTeamInfo(uint8_t *gs, bool force) {
  if (!IsReadable(gs + OFF_GS_PLAYER_ARRAY, 16))
    return;
  uint8_t **arr = *(uint8_t ***)(gs + OFF_GS_PLAYER_ARRAY);
  int count = *(int32_t *)(gs + OFF_GS_PLAYER_ARRAY + 8);
  if (count < 0 || count > 256 || (count && !IsReadable(arr, (size_t)count * 8)))
    return;
  HavocTeamInfoArgs a = {};
  for (int i = 0; i < count && a.n < 16; ++i) {
    uint8_t *pri = arr[i];
    if (!IsReadable(pri, 0x948) || !ClassChainContains((UObjectMin *)pri, "YPlayerReplicationInfo"))
      continue;
    uint8_t *pc = IsReadable(pri + OFF_ACTOR_OWNER, 8) ? *(uint8_t **)(pri + OFF_ACTOR_OWNER) : nullptr;
    if (!pc || !IsReadable(pc, OFF_PC_NETCONNECTION + 8) || !ClassChainContains((UObjectMin *)pc, "PlayerController") ||
        !*(void **)(pc + OFF_PC_NETCONNECTION))
      continue;
    a.classes[a.n] = pri[0x941];
    a.elim[a.n] = HavocIsEliminated(pc) ? 1 : 0;
    ++a.n;
  }
  static uint32_t s_lastSig = 0xFFFFFFFF;
  uint32_t sig = (uint32_t)a.n * 2654435761u;
  for (int i = 0; i < a.n; ++i)
    sig = (sig ^ (uint32_t)(a.classes[i] << 1 | a.elim[i])) * 16777619u;
  if (!force && sig == s_lastSig)
    return;
  s_lastSig = sig;
  if (!g_hvcTeamInfoProp) {
    FNameMin n = {};
    ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&n, L"ClientHavocPlayersChanged", 1);
    void *fn = ((tFindFunctionChecked)(g_base + RVA_FIND_FUNCTION_CHECKED))(gs, *(uint64_t *)&n);
    g_hvcTeamInfoProp = fn && IsReadable((uint8_t *)fn + 0x40, 8) ? *(UObjectMin **)((uint8_t *)fn + 0x38) : nullptr;
  }
  bool sent = CallNamed(gs, L"ClientHavocPlayersChanged", FillTeamInfo, &a);
  int out = 0;
  for (int i = 0; i < a.n; ++i)
    out += a.elim[i];
  Logf("havoc: team HUD -> %d player(s), %d eliminated%s", a.n, out, sent ? "" : " -- ClientHavocPlayersChanged NOT FOUND");
}

// One-time check of every reflected offset the director uses against the
// engine's own reflection (PropOffset: UProperty Offset_Internal). The native
// ones were also read off their property registration in the exe
// (2026-10-19); the Blueprint ones (GameState_Havoc_BP, PVE_HordeManager_BP)
// exist only in the cooked assets, so this is their only check. A MISMATCH
// means the constant is wrong for this build: the director writes there.
static void HavocCheckOffsets(uint8_t *gm, uint8_t *gs) {
  struct Want { int which; const char *name; int off; };
  static const Want kWant[] = {
      {0, "m_waveCompleteDuration", OFF_HVC_GM_WAVE_COMPLETE_DUR}, {0, "m_upgradeSelectionDuration", OFF_HVC_GM_UPGRADE_DUR},
      {0, "m_waveStartDelayDuration", OFF_HVC_GM_WAVE_START_DELAY}, {0, "m_pveEventManager", OFF_HVC_GM_PVE_EVENT_MANAGER},
      {0, "OnPVEModeStarted", OFF_HVC_GM_ON_PVE_MODE_STARTED}, {0, "OnAnyNPCKilled", OFF_HVC_GM_ON_ANY_NPC_KILLED},
      {1, "m_isMatchStarted", OFF_HVC_GS_IS_MATCH_STARTED}, {1, "m_numAliveEnemies", OFF_HVC_GS_NUM_ALIVE},
      {1, "m_numEnemiesKilledThisWave", OFF_HVC_GS_KILLED}, {1, "m_maxWaveEnemies", OFF_HVC_GS_MAX_WAVE_ENEMIES},
      {1, "m_maxNumberOfWaves", OFF_HVC_GS_MAX_WAVES}, {1, "m_currentWaveIndex", OFF_HVC_GS_WAVE_INDEX},
      {1, "m_currentWaveState", OFF_HVC_GS_WAVE_STATE}, {1, "m_currentWaveDuration", OFF_HVC_GS_WAVE_DURATION},
      {1, "m_currentHavocState", OFF_HVC_GS_HAVOC_STATE}, {1, "m_wavesReached", OFF_HVC_GS_WAVES_REACHED},
      {1, "m_playerWaveXP", OFF_HVC_GS_PLAYER_WAVE_XP}, {1, "m_totalXP", OFF_HVC_GS_TOTAL_XP},
      {1, "m_pveEventManager", OFF_HVC_GS_PVE_EVENT_MANAGER}, {1, "OnHordeWaveStateChanged", OFF_HVC_GS_ON_WAVE_STATE_CHANGED},
      {1, "EventStarted", OFF_HVC_GS_EVENT_STARTED}, {1, "HordeManager", OFF_HVC_GS_HORDE_MANAGER},
      {2, "GameMode", OFF_HVC_HM_GAME_MODE}, {2, "Waves", OFF_HVC_HM_WAVES},
      {2, "m_combatSceneManager", OFF_PVE_EM_COMBAT_SCENE_MANAGER},
  };
  int hmOff = PropOffset((UObjectMin *)gs, "HordeManager");
  uint8_t *hm = hmOff > 0 && IsReadable(gs + hmOff, 8) ? *(uint8_t **)(gs + hmOff) : nullptr;
  uint8_t *objs[3] = {gm, gs, hm};
  int ok = 0, bad = 0, missing = 0;
  for (const Want &w : kWant) {
    uint8_t *o = objs[w.which];
    int got = o ? PropOffset((UObjectMin *)o, w.name) : -2;
    if (got == w.off)
      ++ok;
    else if (got < 0) {
      ++missing;
      Logf("havoc: offsets: %s.%s not reflected%s (constant 0x%X unchecked)", o ? ClassNameOf(o) : "?", w.name,
           o ? "" : " -- object missing", w.off);
    } else {
      ++bad;
      Logf("havoc: offsets: MISMATCH %s.%s is +0x%X, constant +0x%X", ClassNameOf(o), w.name, got, w.off);
    }
  }
  Logf("havoc: offsets checked against reflection: %d OK, %d MISMATCH, %d not reflected", ok, bad, missing);
}

// One-time dump of the game mode's / game state's own Havoc rule settings
// (reflected names from the class registrations; values are the BP defaults
// of GameMode_Havoc_BP / GameState_Havoc_BP plus DefaultHavoc.ini). The
// director should follow these rather than its own guesses.
static void HavocLogRules(uint8_t *gm, uint8_t *gs) {
  static const char *const kGm[] = {"m_firstWaveDelay", "m_automaticallyStartFirstWave", "m_waveStartDelayDuration",
                                    "m_waveStartDuration", "m_waveEndDuration", "m_waveCompleteDuration",
                                    "m_upgradeSelectionDuration", "m_monsterKillXP", "m_waveScoreXPMultiplier",
                                    "m_killFighterJetsOnWaveEnd", "m_refillPlayerTeamRespawnsAfterWave",
                                    "m_respawnsToRefill", "m_forceRespawnInWave", "m_respawningWaveTime",
                                    "m_respawningWaveGracePeriodTime", "m_canPlayersRespawnAsCapitalShipsAfterWave",
                                    "m_maxPlayersPerTeam"};
  static const char *const kGs[] = {"m_maxNumberOfWaves", "m_maxWaveEnemies", "m_waveRespawningTime",
                                    "m_allowCancelWaveSpawning", "m_useWaveSpawning", "m_endOfMatchDuration",
                                    "m_maxModifiers", "m_pveDifficulty"};
  for (int pass = 0; pass < 2; ++pass) {
    uint8_t *o = pass ? gs : gm;
    const char *const *names = pass ? kGs : kGm;
    int n = pass ? (int)(sizeof kGs / sizeof *kGs) : (int)(sizeof kGm / sizeof *kGm);
    for (int i = 0; i < n; ++i) {
      int off = PropOffset((UObjectMin *)o, names[i]);
      if (off < 0 || !IsReadable(o + off, 4)) {
        Logf("havoc: rule %s.%s not reflected", ClassNameOf(o), names[i]);
        continue;
      }
      Logf("havoc: rule %s.%s +0x%X = int %d / float %g / byte %u", ClassNameOf(o), names[i], off,
           *(int32_t *)(o + off), *(float *)(o + off), o[off]);
    }
  }
}

// PvE Escort (S1E4). PVE_EscortManager_BP (GameState_Escort_BP.EscortModeManager)
// runs the whole mission from SpawnVipShip: it spawns the VIP through the
// combat scene manager (AddNPCSpawnByDataset), hands it to YGameMode_Escort
// (EscortShipSpawned), registers the waypoints, starts the initial objective
// and after DelayFromSpawnUntilTravel cruises to waypoint 0; waypoints spawn
// the ambushes. Nothing in the BPs calls SpawnVipShip -- native code did.
// GUESS: the server build called it at PvE start; this does the same PvE start
// as the horde director, then calls SpawnVipShip 20 s after the match starts
// (as the horde director's first wave).
static void EscortDirectorTick(uint8_t *gm) {
  static void *s_for = nullptr;
  static int s_on = 0, s_secs = 0, s_mgrOff = 0, s_vipOff = 0, s_wpOff = 0;
  static bool s_started = false, s_spawned = false;
  if (s_for != gm) {
    s_for = gm;
    s_on = ClassChainContains((UObjectMin *)gm, "YGameMode_Escort") &&
           !SwitchOn("DN_HOST_NO_HAVOC_DIRECTOR", "dn_host_no_havoc_director.txt");
    s_secs = 0, s_mgrOff = 0, s_started = false, s_spawned = false;
    if (s_on)
      Logf("escort: director on for %p (%s)", gm, ClassNameOf((UObjectMin *)gm));
  }
  if (!s_on || !IsReadable(gm + OFF_GM_GAMESTATE, 8))
    return;
  uint8_t *gs = *(uint8_t **)(gm + OFF_GM_GAMESTATE);
  if (!IsReadable(gs, OFF_HVC_GS_IS_MATCH_STARTED + 1) || !gs[OFF_HVC_GS_IS_MATCH_STARTED])
    return;
  if (!s_mgrOff) {
    s_mgrOff = PropOffset((UObjectMin *)gs, "EscortModeManager");
    Logf("escort: game state %s: EscortModeManager +0x%x", ClassNameOf((UObjectMin *)gs), s_mgrOff);
    if (s_mgrOff <= 0) {
      s_on = 0;
      return;
    }
  }
  UObjectMin *mgr = IsReadable(gs + s_mgrOff, 8) ? *(UObjectMin **)(gs + s_mgrOff) : nullptr;
  if (!mgr || !IsReadable(mgr, sizeof(UObjectMin)))
    return;
  if (!s_vipOff) {
    s_vipOff = PropOffset(mgr, "VIPShipId");
    s_wpOff = PropOffset(mgr, "CurrentWaypointId");
    Logf("escort: manager %p %s: VIPShipId +0x%x (%d), CurrentWaypointId +0x%x", mgr, ClassNameOf(mgr), s_vipOff,
         s_vipOff > 0 ? *(int32_t *)((uint8_t *)mgr + s_vipOff) : 0, s_wpOff);
  }
  ++s_secs;
  if (!s_started) {
    s_started = true;
    UObjectMin *em = IsReadable(gm + OFF_HVC_GM_PVE_EVENT_MANAGER, 8) ? *(UObjectMin **)(gm + OFF_HVC_GM_PVE_EVENT_MANAGER) : nullptr;
    Logf("escort: match started; event manager %p %s", em, ClassNameOf(em));
    if (ClassChainContains(mgr, "YPVEEventManager"))
      PveStartEventManager(gm, gs, mgr, em, -1);
  }
  int vip = s_vipOff > 0 ? *(int32_t *)((uint8_t *)mgr + s_vipOff) : -2;
  int wp = s_wpOff > 0 ? *(int32_t *)((uint8_t *)mgr + s_wpOff) : -2;
  if (!s_spawned && s_secs >= 20) {
    s_spawned = true;
    if (vip < 0) {
      bool ok = CallNoParms(mgr, L"SpawnVipShip");
      Logf("escort: SpawnVipShip called %d -> VIPShipId %d", ok,
           s_vipOff > 0 ? *(int32_t *)((uint8_t *)mgr + s_vipOff) : -2);
    } else {
      Logf("escort: VIP already spawned (VIPShipId %d)", vip);
    }
  }
  if (s_secs % 30 == 0)
    Logf("escort: t+%d VIPShipId %d, waypoint %d", s_secs, vip, wp);
  // Mission end. The manager broadcasts OnEscortDone when the VIP leaves the
  // last waypoint (WaypointLeft on Waypoints' last index); only native code
  // bound it. Win: the last waypoint's WayPointReached, then 10 s for the
  // outro. Lose: GameState_Escort_BP.GetEscortShipHealthPercent <= 0 once the
  // VIP has spawned. GUESS: both rules and the 10 s are ours.
  static void *s_endedGs = nullptr;
  static int s_wonAt = 0;
  if (s_endedGs == gs || vip < 0)
    return;
  // YGameState_Escort keeps the VIP pawn at +0x1F20: GetEscortShipPawn
  // (0x36EE60) returns it and GetEscortShipHealthPercent (0x36EE10) divides
  // its health (pawn +0x9A8 -> +0x1D8) by the max, 0 when unset. Only the
  // server build filled it: YGameMode_Escort.EscortShipSpawned, which the
  // manager calls with the VIP's scene NPC id, is an empty stub here (exec
  // 0x742E90 -> 0x1AA9450 `ret 0`), so escort2 read 0.000 for 15 min while the
  // VIP flew its route. Fill it from that scene entry's weak pawn, and clear
  // it ourselves once the weak pointer stops resolving (pending kill) so the
  // raw field never outlives the actor.
  static void *s_aliveGs = nullptr;
  uint8_t *vipPawn = nullptr;
  {
    uint8_t *scene = (uint8_t *)((tGetSceneManager)(g_base + RVA_COMBAT_SCENE_MANAGER))();
    if (scene && IsReadable(scene + OFF_SCENE_NPCS, 16) && vip < *(int32_t *)(scene + OFF_SCENE_NPCS_COUNT)) {
      uint8_t *e = *(uint8_t **)(scene + OFF_SCENE_NPCS) + (size_t)vip * NPC_SPAWN_SIZE;
      if (IsReadable(e, NPC_SPAWN_SIZE) && e[OFF_NPCSPAWN_INSTANCED])
        vipPawn = (uint8_t *)((tWeakGet)(g_base + RVA_WEAK_PTR_GET))(e + OFF_NPCSPAWN_PAWN);
    }
  }
  if (ClassChainContains((UObjectMin *)gs, "YGameState_Escort") && IsReadable(gs + 0x1F20, 8)) {
    void **slot = (void **)(gs + 0x1F20);
    if (vipPawn && *slot != vipPawn) {
      Logf("escort: VIP pawn %p %s -> game state +0x1F20 (was %p)", vipPawn, ClassNameOf((UObjectMin *)vipPawn), *slot);
      *slot = vipPawn;
    } else if (!vipPawn && *slot && s_aliveGs == gs) {
      Logf("escort: VIP pawn gone from scene entry %d", vip);
      *slot = nullptr;
      s_endedGs = gs;
      PveEndMatch(gm, gs, false, "escort: VIP ship destroyed");
      return;
    }
  }
  float hp = CallFloatGetter(gs, "GetEscortShipHealthPercent");
  if (s_secs % 30 == 0)
    Logf("escort: VIP health %.3f%s", hp, s_aliveGs == gs ? "" : " (not seen alive yet)");
  if (hp == hp && hp > 0.0f)
    s_aliveGs = gs;
  else if (s_aliveGs == gs && hp == hp && hp <= 0.0f) {
    s_endedGs = gs;
    PveEndMatch(gm, gs, false, "escort: VIP ship destroyed");
    return;
  }
  static int s_wpsOff = 0;
  if (!s_wpsOff)
    s_wpsOff = PropOffset(mgr, "Waypoints");
  if (s_wpsOff <= 0 || !IsReadable((uint8_t *)mgr + s_wpsOff, 12))
    return;
  UObjectMin **wps = *(UObjectMin ***)((uint8_t *)mgr + s_wpsOff);
  int nw = *(int32_t *)((uint8_t *)mgr + s_wpsOff + 8);
  if (!wps || nw <= 0 || nw > 256 || !IsReadable(wps, (size_t)nw * 8))
    return;
  UObjectMin *last = wps[nw - 1];
  int rOff = last && IsReadable(last, sizeof(UObjectMin)) ? PropOffset(last, "WayPointReached") : 0;
  if (rOff > 0 && IsReadable((uint8_t *)last + rOff, 1) && ((uint8_t *)last)[rOff]) {
    if (!s_wonAt) {
      s_wonAt = s_secs;
      Logf("escort: last waypoint %d of %d reached", nw - 1, nw);
    } else if (s_secs - s_wonAt >= 10) {
      s_endedGs = gs, s_wonAt = 0;
      PveEndMatch(gm, gs, true, "escort: VIP reached the last waypoint");
    }
  }
}

static void HavocDirectorTick(uint8_t *gm) {
  static void *s_for = nullptr;
  static int s_on = 0, s_phase = -1, s_wave = 0, s_secs = 0, s_waitStart = 0, s_peakAlive = 0;
  if (s_for != gm) {
    s_for = gm;
    s_on = (ClassChainContains((UObjectMin *)gm, "YGameMode_Havoc") ||
            ClassChainContains((UObjectMin *)gm, "YGameMode_Horde")) &&
           !SwitchOn("DN_HOST_NO_HAVOC_DIRECTOR", "dn_host_no_havoc_director.txt");
    s_phase = -1, s_wave = 0, s_secs = 0, s_waitStart = 0, s_peakAlive = 0;
    if (s_on && !ClassChainContains((UObjectMin *)gm, "YGameMode_Havoc"))
      Logf("havoc: director on for %p (PvE horde %s)", gm, ClassNameOf((UObjectMin *)gm));
    else if (s_on)
      Logf("havoc: director on for %p (start delay %d s, complete %.0f s, upgrade %d s)", gm,
           *(int32_t *)(gm + OFF_HVC_GM_WAVE_START_DELAY), *(float *)(gm + OFF_HVC_GM_WAVE_COMPLETE_DUR),
           *(int32_t *)(gm + OFF_HVC_GM_UPGRADE_DUR));
  }
  if (!s_on || !IsReadable(gm + OFF_GM_GAMESTATE, 8))
    return;
  uint8_t *gs = *(uint8_t **)(gm + OFF_GM_GAMESTATE);
  // PvE horde: GameState_Horde_BP declares its own HordeManager / EventStarted
  // (offsets resolved by name); no havoc state, upgrades or forced respawn.
  static void *s_gsFor = nullptr;
  static int s_horde = 0, s_hmOff = OFF_HVC_GS_HORDE_MANAGER, s_esOff = OFF_HVC_GS_EVENT_STARTED;
  if (s_gsFor != gs && IsReadable(gs, sizeof(UObjectMin))) {
    s_gsFor = gs;
    s_horde = !ClassChainContains((UObjectMin *)gs, "YGameState_Havoc");
    s_hmOff = s_horde ? PropOffset((UObjectMin *)gs, "HordeManager") : OFF_HVC_GS_HORDE_MANAGER;
    s_esOff = s_horde ? PropOffset((UObjectMin *)gs, "EventStarted") : OFF_HVC_GS_EVENT_STARTED;
    if (s_horde)
      Logf("havoc: horde game state %s: HordeManager +0x%x, EventStarted +0x%x", ClassNameOf((UObjectMin *)gs),
           s_hmOff, s_esOff);
  }
  if (s_hmOff <= 0 || s_esOff <= 0 || !IsReadable(gs, s_hmOff + 8) ||
      !ClassChainContains((UObjectMin *)gs, s_horde ? "YGameState_Horde" : "YGameState_Havoc"))
    return;
  if (!s_horde)
    HavocForcedRespawnTick(gm);
  static void *s_checked = nullptr;
  if (!s_horde && s_checked != gs) {
    s_checked = gs;
    HavocCheckOffsets(gm, gs);
    HavocLogRules(gm, gs);
    void **vt = *(void ***)gm;
    Logf("havoc: game mode vtable +0x870 (SpawnPlayer's spawn gate) -> RVA 0x%llX",
         (unsigned long long)((uintptr_t)vt[0x870 / 8] - g_base));
  }
  uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
  auto enterState = (void (*)(uint8_t *, uint8_t))(base + RVA_HVC_ENTER_NEW_STATE);
  auto waveChanged = (void (*)(uint8_t *))(base + RVA_HORDE_WAVE_STATE_CHANGED);
  {
    int listed = HavocCountAliveEnemies(gm, gs);
    if (listed >= 0)
      *(int32_t *)(gs + OFF_HVC_GS_NUM_ALIVE) = listed;
  }
  int alive = *(int32_t *)(gs + OFF_HVC_GS_NUM_ALIVE);
  auto go = [&](int havoc, int horde) {
    s_phase = havoc, s_secs = 0;
    if (!s_horde)
      gs[OFF_HVC_GS_HAVOC_STATE] = (uint8_t)havoc;
    if (horde >= 0) {
      *(int32_t *)(gs + OFF_HVC_GS_WAVE_INDEX) = s_wave;
      gs[OFF_HVC_GS_WAVE_STATE] = (uint8_t)horde;
      waveChanged(gs);
      // The horde manager's YHordeEventManager::OnWaveStateChanged is a
      // native-called event; its caller went with the server code.
      CallWaveEvent(*(void **)(gs + s_hmOff), L"OnWaveStateChanged", horde, s_wave);
    }
    if (!s_horde)
      enterState(gs, (uint8_t)havoc);
    void *hmGm = IsReadable(*(uint8_t **)(gs + s_hmOff), OFF_HVC_HM_GAME_MODE + 8)
                     ? *(void **)(*(uint8_t **)(gs + s_hmOff) + OFF_HVC_HM_GAME_MODE) : nullptr;
    Logf("havoc: (hm.GameMode %p)", hmGm);
    Logf("havoc: wave %d -> havoc state %d, horde wave state %d (alive %d, killed %d, max %d, EventStarted %d)",
         s_wave, havoc, horde, *(int32_t *)(gs + OFF_HVC_GS_NUM_ALIVE), *(int32_t *)(gs + OFF_HVC_GS_KILLED),
         *(int32_t *)(gs + OFF_HVC_GS_MAX_WAVE_ENEMIES), gs[s_esOff]);
  };
  ++s_secs;
  if (!s_horde) {
    HavocApplyBoosts();
    HavocApplyModifiers();
  }
  // Havoc game over (upstream stage 2): the run is reported, the clients get
  // their scores, and the match ends 6 s later (GameState_Havoc_BP's outro is 4 s).
  if (!s_horde && s_phase == 4) {
    if (s_secs == 2 || s_secs == 4)
      HavocSendRewards(gs, "game over, resend");
    if (s_secs == 6) {
      int win = g_hvcBpEnd == 1;
      gs[OFF_GS_WINNING_TEAM] = (uint8_t)(win ? 1 : 2);
      Logf("havoc: ending the match (%s, wave %d, waves reached %d, team XP %d)", win ? "won" : "lost", s_wave,
           *(int32_t *)(gs + OFF_HVC_GS_WAVES_REACHED), *(int32_t *)(gs + OFF_HVC_GS_TOTAL_XP));
      void **vt = *(void ***)gm;
      ((void(__fastcall *)(void *))vt[VT_GM_END_MATCH / 8])(gm);
    }
    return;
  }
  auto gameOver = [&](const char *why) {
    Logf("havoc: GAME OVER at wave %d (%s)", s_wave, why);
    HavocSendTimeToNextWave(gs, -1);
    HavocEndWaveModifiers();
    for (auto &e : g_hvcEliminated)
      e = nullptr;
    s_phase = 4, s_secs = 0;
    gs[OFF_HVC_GS_HAVOC_STATE] = 4;
    enterState(gs, 4);
    HavocReportRuns(gs);
    HavocSendRewards(gs, "game over");
  };
  if (!s_horde) {
    static int s_smokeEnd = -1;
    if (s_smokeEnd < 0)
      s_smokeEnd = SwitchOn("DN_HOST_HAVOC_SMOKE", "dn_host_havoc_smoke.txt") ? 1 : 0;
    if (s_smokeEnd && s_wave >= 3 && s_phase == 0 && s_secs >= 5) {
      HavocVerifyShipClasses();
      gameOver("smoke test switch after wave 3");
      return;
    }
    if (s_phase >= 0 && g_hvcBpEnd) {
      gameOver(g_hvcBpEnd == 1 ? "the horde manager called PlayersWin" : "the BP called PlayersLose");
      return;
    }
    // Lost: no connected player has a living ship for 3 s during a wave.
    static int s_downSecs = 0;
    if (s_phase == 2)
      HavocTrackEliminations(gs);
    if (s_phase >= 0)
      HavocSendTeamInfo(gs, false);
    if (s_phase == 1 || s_phase == 2) {
      int humans = 0, up = HavocPlayersAlive(gs, &humans);
      s_downSecs = up == 0 ? s_downSecs + 1 : 0;
      if (s_downSecs == 1)
        Logf("havoc: no living player ship (%d connected) -- game over in 3 s unless one comes back", humans);
      if (s_downSecs >= 3) {
        s_downSecs = 0;
        gameOver("every player's ship was destroyed during the wave");
        return;
      }
    } else
      s_downSecs = 0;
  }
  if (s_phase < 0) {
    // GUESS: start once the match has started and the players have had 20 s
    // to leave the orbit intro; the real trigger (OnPVEModeStarted /
    // m_firstWaveDelay) is unverified.
    // Test switch for a headless host with no player (dn_host_havoc_smoke.txt):
    // start without m_isMatchStarted and, after wave 3, force game over --
    // exercises modifiers, enemy boosts, the multicast and EndMatch. Use with
    // dn_host_havoc_fast_waves.txt.
    static int s_smoke = -1;
    if (s_smoke < 0)
      s_smoke = SwitchOn("DN_HOST_HAVOC_SMOKE", "dn_host_havoc_smoke.txt") ? 1 : 0;
    if (!gs[OFF_HVC_GS_IS_MATCH_STARTED] && !(s_smoke && !s_horde))
      return;
    if (++s_waitStart == 1) {
      UObjectMin *hm = *(UObjectMin **)(gs + s_hmOff);
      UObjectMin *em = IsReadable(gm + OFF_HVC_GM_PVE_EVENT_MANAGER, 8) ? *(UObjectMin **)(gm + OFF_HVC_GM_PVE_EVENT_MANAGER) : nullptr;
      Logf("havoc: match started; HordeManager %p %s, event manager %p %s", hm, ClassNameOf(hm), em, ClassNameOf(em));
      // Lineup-ready gate (see HookReadyUpForMatchThunk): logged here too so a
      // run without a READY click still records it.
      Logf("havoc: ready gate gs+0x1C3C=%u (%s) gm+0x899=%u (%s)", gs[0x1C3C], PropNameAt((UObjectMin *)gs, 0x1C3C),
           gm[0x899], PropNameAt((UObjectMin *)gm, 0x899));
      g_peTraceObj[0] = hm;
      g_peTraceObj[1] = gs;
      DumpReflection(hm);
      DumpReflection(em);
      // The PvE start the server build would have run: register the map's
      // horde manager as the event manager, then OnPVEModeStarted. GameState
      // _Havoc_BP's EventStarted and the manager's wave spawning hang off it.
      Logf("havoc: before start: hm.GameMode %p, hm.Waves %d, bound OnPVEModeStarted %d, "
           "OnHordeWaveStateChanged %d", IsReadable(hm, OFF_HVC_HM_GAME_MODE + 8) ? *(void **)((uint8_t *)hm + OFF_HVC_HM_GAME_MODE) : nullptr,
           DelegateBound((uint8_t *)hm + OFF_HVC_HM_WAVES), DelegateBound(gm + OFF_HVC_GM_ON_PVE_MODE_STARTED),
           DelegateBound(gs + OFF_HVC_GS_ON_WAVE_STATE_CHANGED));
      if (hm && ClassChainContains(hm, "YPVEEventManager"))
        PveStartEventManager(gm, gs, hm, em, s_esOff);
    }
    // Wave 1 starts m_firstWaveDelay after the match starts: the game mode's
    // own value, 18 s in GameMode_Havoc_BP (upstream read it by reflection
    // 2026-10-19; 20 s was a guess). GUESS: the trigger.
    static int s_firstDelay = -1;
    if (s_firstDelay < 0) {
      int off = PropOffset((UObjectMin *)gm, "m_firstWaveDelay");
      float fd = off > 0 && IsReadable(gm + off, 4) ? *(float *)(gm + off) : -1.0f;
      s_firstDelay = fd >= 1.0f && fd <= 300.0f ? (int)(fd + 0.5f) : 20;
      Logf("havoc: first wave %d s after the match start (m_firstWaveDelay %g)", s_firstDelay, fd);
    }
    if (s_waitStart < s_firstDelay) {
      if (!s_horde)
        HavocSendTimeToNextWave(gs, s_firstDelay - s_waitStart);
      return;
    }
    s_wave = g_hvcWaveJump > 0 ? g_hvcWaveJump : 1;
    g_hvcWaveJump = 0;
    if (!s_horde)
      HavocStartWaveModifiers(gs, s_wave);
    go(1, 1);
    return;
  }
  if (alive > s_peakAlive)
    s_peakAlive = alive;
  if (s_phase == 2 && (s_secs == 1 || s_secs == 5 || s_secs == 20) && s_wave == 1) {
    static const char *const kComp[] = {"GroupSpawnInProgress", "CurrentEnemies", "CurrentIndex", "PawnIdToSpawn",
                                        "CurrentNPCId", "CombatSceneManager", "PVEModeManager", "OwningActor",
                                        "SpawnpointReference", "SpawnName", "Queued*", "SpawnDelay"};
    static const char *const kGs[] = {"*ifficulty*"};
    uint8_t *hm = *(uint8_t **)(gs + s_hmOff);
    if (IsReadable(hm + 0x450, 8))
      LogProps(s_secs == 1 ? "havoc: t+1 spawn comp" : s_secs == 5 ? "havoc: t+5 spawn comp" : "havoc: t+20 spawn comp",
               *(UObjectMin **)(hm + 0x450), kComp, 12);
    LogProps("havoc: game state", (UObjectMin *)gs, kGs, 1);
    // Did AddNPCSpawnByDataset land in the scene manager's NPC array, and was
    // any entry instanced (+0x88) with a live pawn (+0x8C weak ptr)?
    uint8_t *scene = (uint8_t *)((tGetSceneManager)(g_base + RVA_COMBAT_SCENE_MANAGER))();
    if (scene && IsReadable(scene + OFF_SCENE_NPCS, 16)) {
      uint8_t *arr = *(uint8_t **)(scene + OFF_SCENE_NPCS);
      int cnt = *(int32_t *)(scene + OFF_SCENE_NPCS_COUNT);
      char b[600];
      int o = snprintf(b, sizeof b, "havoc: t+%d scene npcs %d (not-started %u):", s_secs, cnt,
                       scene[OFF_SCENE_COMBAT_NOT_STARTED]);
      for (int i = 0; arr && i < cnt && i < 12 && IsReadable(arr + i * NPC_SPAWN_SIZE, NPC_SPAWN_SIZE); ++i) {
        uint8_t *e = arr + i * NPC_SPAWN_SIZE;
        void *cls = *(void **)(e + OFF_NPCSPAWN_PAWN_CLASS);
        o += snprintf(b + o, sizeof b - o, " [%d type %d t %.1f cls %s inst %u pawnidx %d]", i,
                      *(int32_t *)(e + OFF_NPCSPAWN_TYPE), *(float *)(e + OFF_NPCSPAWN_TIME),
                      cls ? ClassNameOf((UObjectMin *)cls) : "null", e[OFF_NPCSPAWN_INSTANCED], *(int32_t *)(e + OFF_NPCSPAWN_PAWN));
        if (o >= (int)sizeof b - 90)
          break;
      }
      Logf("%s", b);
    }
    LogProps("havoc: manager", (UObjectMin *)hm, kGs, 1);
  }
  // Respawn pool, for the respawn-ship bug: the HUD shows 3 red crossed
  // slots from the first spawn, and a player killed mid-wave sits on
  // "RESPAWNING IN" with no countdown (headless 10-16 21:19). GUESS: the
  // team pool (m_playerTeamRespawnsLeft) is 0 and only wave end respawns.
  if ((s_phase == 2 && s_secs % 30 == 2) || (s_phase != 2 && s_secs % 10 == 2)) {
    static const char *const kRsp[] = {"*espawn*"};
    char tag[48];
    snprintf(tag, sizeof tag, "havoc: w%d s%d t+%d respawn gs", s_wave, s_phase, s_secs);
    LogProps(tag, (UObjectMin *)gs, kRsp, 1);
    snprintf(tag, sizeof tag, "havoc: w%d s%d t+%d respawn gm", s_wave, s_phase, s_secs);
    LogProps(tag, (UObjectMin *)gm, kRsp, 1);
  }
  if (s_secs % 10 == 0)
    Logf("havoc: wave %d state %d t+%d s: alive %d (peak %d), killed %d, max %d", s_wave, s_phase, s_secs, alive,
         s_peakAlive, *(int32_t *)(gs + OFF_HVC_GS_KILLED), *(int32_t *)(gs + OFF_HVC_GS_MAX_WAVE_ENEMIES));
  switch (s_phase) {
  case 1: // WaveStarting
    if (s_secs >= std::max(3, s_horde ? HordeGmSecs(gm, "m_waveStartDuration") : *(int32_t *)(gm + OFF_HVC_GM_WAVE_START_DELAY))) {
      s_peakAlive = 0;
      *(int32_t *)(gs + OFF_HVC_GS_KILLED) = 0;
      if (!s_horde)
        *(int32_t *)(gs + OFF_HVC_GS_PLAYER_WAVE_XP) = 0;
      InterlockedExchange(&g_hvcWaveConditions, 0);
      go(2, 2);
    }
    break;
  case 2: { // WaveInProgress: over once everything that spawned is dead
    // HUD wave clock ("Wave N 00:00"). Disproved first theory: it is NOT
    // m_currentWaveDuration (+0x1F58, GetCurrentWaveDuration 0x750E70) --
    // writing that every second left the clock at 00:00 (headless wavetimer2).
    // Real path: 0x562AC0 (HUD tick) calls gs vtable +0x6B0 (GetMatchTimer;
    // exec 0x753DE0) = 0x371340 on the Horde GS (vtable rva 0x2E9C9A0, logged
    // at runtime) = load of m_matchTime +0x1E70, then 0x492BB0 -> manager
    // widget 0x463C40 -> BP_EventSetTime (thunk 0x5E1D60) ->
    // UI_HUD_GameModeManager_WBP -> FormatSecondsToCountdownString ->
    // UI_HUD_GameModeHavoc_WBP.Set Time. Only writers in this binary are the
    // ctor (0x389093) and the Horde reset to 0 at 0x3A9A1B, so the counter
    // itself went with the server build. Seconds since the wave started:
    // GUESS (per-wave rather than per-match) from that per-state reset.
    static int s_matchTimeOff = -2;
    if (s_matchTimeOff == -2)
      s_matchTimeOff = PropOffset((UObjectMin *)gs, "m_matchTime");
    if (s_matchTimeOff > 0)
      *(int32_t *)(gs + s_matchTimeOff) = s_secs;
    // HUD wave progress bar. The client only redraws it in two places: the
    // wave-state handler (HUD 0x536820 -> 0x563410, once per state change)
    // and AYGameState_Horde::ClientUpdateWaveProgress (impl 0x369600, vtable
    // +0x8A8, exec thunk 0x747D60), which packs {m_wavesReached +0x2024,
    // m_maxWaveEnemies +0x1F44, m_numEnemiesKilledThisWave +0x1F40, +0x1F54,
    // boss flag} into AYHUD 0x5598F0 -> 0x492C50 -> 0x463EE0 ->
    // BP_SetWaveProgress (thunk 0x5E4930). Its only caller in this binary is
    // the no-parm RPC stub 0x5E63A0 (ProcessEvent with the FName at
    // 0x3E10370), itself never called: the per-kill push went with the
    // server build, so the bar stayed empty. Push it whenever kills/max change.
    // GUESS: the RPC is NetMulticast (a GameState has no owning connection
    // for a Client RPC).
    static int s_fastWavesBar = -1; // test switch below; nobody kills in an unattended run
    if (s_fastWavesBar < 0)
      s_fastWavesBar = SwitchOn("DN_HOST_HAVOC_FAST_WAVES", "dn_host_havoc_fast_waves.txt") ? 1 : 0;
    if (s_fastWavesBar && s_secs % 10 == 5 && *(int32_t *)(gs + OFF_HVC_GS_KILLED) < *(int32_t *)(gs + OFF_HVC_GS_MAX_WAVE_ENEMIES))
      *(int32_t *)(gs + OFF_HVC_GS_KILLED) += 1;
    static int s_pushedKilled = -1, s_pushedMax = -1;
    int killedNow = *(int32_t *)(gs + OFF_HVC_GS_KILLED), maxNow = *(int32_t *)(gs + OFF_HVC_GS_MAX_WAVE_ENEMIES);
    // The stub looks the UFunction up with FindFunctionChecked, which is fatal
    // when the class lacks it: GameState_Horde_BP does (pveend1 08:31:03,
    // "Failed to find function ClientUpdateWaveProgress"), so only Havoc.
    if (!s_horde && (s_secs <= 1 || killedNow != s_pushedKilled || maxNow != s_pushedMax)) {
      s_pushedKilled = killedNow, s_pushedMax = maxNow;
      ((void (*)(void *))(g_base + 0x5E63A0 /* RVA_HVC_CLIENT_UPDATE_WAVE_PROGRESS_RPC */))(gs);
    }
    // GUESS: 15 s grace for the spawn-in; 120 s with nothing alive = nothing spawns.
    // Test switch for unattended runs (nobody kills the enemies): the wave
    // ends after 40 s with the 750 XP GameMode_Havoc_BP grants at a clear.
    static int s_fastWaves = -1;
    if (s_fastWaves < 0)
      s_fastWaves = SwitchOn("DN_HOST_HAVOC_FAST_WAVES", "dn_host_havoc_fast_waves.txt") ? 1 : 0;
    bool fastEnd = s_fastWaves && s_secs >= 40;
    if (fastEnd) {
      Logf("havoc: fast-waves test switch: ending wave %d with %d alive", s_wave, alive);
      HavocAddXP(gm, 750, "fast-waves test switch");
    }
    // Havoc: over when GameState_Havoc_BP says so -- SetSpecialWaveConditions
    // Fulfilled(true) after its CheckWaveOver: enemies dead and every XP drop
    // picked up or expired (HookBoolParmExecThunk). GUESS fallbacks: 60 s
    // after the last listed enemy died (a drop lives 40 s), or 120 s with
    // nothing spawned. m_wavesReached is NOT written: IncrementWavesReached
    // is real code, called by the BP's OnEnterWaveStartingState.
    // Horde (no such BP call seen): 15 s grace, then everything spawned dead.
    static int s_clearSecs = 0;
    s_clearSecs = s_peakAlive > 0 && alive == 0 ? s_clearSecs + 1 : 0;
    bool bpOver = !s_horde && g_hvcWaveConditions != 0;
    bool cleared = s_horde ? s_peakAlive > 0 && alive == 0 && s_secs >= 15 : s_clearSecs >= 60;
    if (fastEnd || bpOver || cleared || (s_peakAlive == 0 && s_secs >= 120)) {
      Logf("havoc: wave %d over after %d s (%s)", s_wave, s_secs,
           fastEnd ? "fast-waves test switch" : bpOver ? "BP: SetSpecialWaveConditionsFulfilled"
           : cleared ? (s_horde ? "everything spawned is dead" : "fallback: 60 s after the last kill")
           : "fallback: nothing spawned");
      s_clearSecs = 0;
      *(int32_t *)(gs + OFF_HVC_GS_WAVE_DURATION) = s_secs;
      if (!s_horde) {
        HavocEndWaveModifiers();
        HavocReleaseEliminated();
      }
      go(3, 3);
    }
    break;
  }
  case 3: // WaveComplete
    if (s_horde) {
      // GUESS: Horde goes straight to the next wave after m_waveEndDuration;
      // after the last wave (m_maxNumberOfWaves) the players' team wins and
      // the match ends through EndMatch (vtable +0x668), as the Onslaught
      // rule does. The original's own end path (OnHordeMatchEndedReplicated)
      // is not driven.
      int maxW = *(int32_t *)(gs + OFF_HVC_GS_MAX_WAVES);
      // m_maxNumberOfWaves reads 0 on PVE_S1E1 (pveend2: waves ran past 50),
      // so fall back to the map horde manager's Waves array (Num 4 there).
      // GUESS: one Waves element per horde wave.
      if (maxW <= 0) {
        uint8_t *hm = *(uint8_t **)(gs + s_hmOff);
        if (hm && IsReadable(hm + OFF_HVC_HM_WAVES, 16))
          maxW = *(int32_t *)(hm + OFF_HVC_HM_WAVES + 8);
      }
      if (s_secs >= std::max(3, HordeGmSecs(gm, "m_waveEndDuration"))) {
        if (maxW > 0 && s_wave >= maxW && !g_hvcWaveJump) {
          static void *s_endedGs = nullptr;
          if (s_endedGs == gs)
            break;
          s_endedGs = gs;
          char why[64];
          snprintf(why, sizeof why, "horde: last wave %d of %d complete", s_wave, maxW);
          PveEndMatch(gm, gs, true, why);
          break;
        }
        s_wave = g_hvcWaveJump > 0 ? g_hvcWaveJump : s_wave + 1;
        g_hvcWaveJump = 0;
        go(1, 1);
      }
      break;
    }
    if (s_secs >= std::max(3, (int)*(float *)(gm + OFF_HVC_GM_WAVE_COMPLETE_DUR))) {
      InterlockedExchange(&g_hvcReadyCount, 0);
      go(0, -1);
      HavocOfferUpgrades(gs);
      // The next wave as the UpgradeSelection end computes it (gs index + 1,
      // or a pending /wave jump).
      HavocPrepareWaveModifiers(gs, g_hvcWaveJump > 0 ? g_hvcWaveJump : *(int32_t *)(gs + OFF_HVC_GS_WAVE_INDEX) + 1);
    }
    break;
  case 0: { // UpgradeSelection
    int ready = 0, humans = HavocHumanCount(gs, &ready);
    bool allReady = humans > 0 && ready >= humans && s_secs >= 3;
    if (allReady)
      Logf("havoc: all %d human(s) ready after %d s -- starting wave %d", humans, s_secs, s_wave + 1);
    if (allReady || s_secs >= std::max(5, *(int32_t *)(gm + OFF_HVC_GM_UPGRADE_DUR))) {
      __try {
        HavocClearReady(gs);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("havoc: ready clear faulted");
      }
      if (g_hvcWaveJump > 0) {
        Logf("havoc: /wave jump: wave %d -> %d", s_wave + 1, g_hvcWaveJump);
        s_wave = g_hvcWaveJump - 1;
        g_hvcWaveJump = 0;
      } else {
        // The next wave follows the game state's index: at the last wave of
        // Waves, PVE_HordeManager_BP sets it back to WaveToLoopBackTo - 1
        // (AYGameState_Havoc::SetWaveIndex, real code) and the run loops.
        int gsWave = *(int32_t *)(gs + OFF_HVC_GS_WAVE_INDEX);
        if (gsWave != s_wave)
          Logf("havoc: the horde manager moved the wave index %d -> %d (loop)", s_wave, gsWave);
        s_wave = gsWave;
      }
      ++s_wave;
      HavocStartWaveModifiers(gs, s_wave);
      go(1, 1);
    }
    break;
  }
  }
  // The countdown to the next WaveStarting (upstream a36f046): WaveComplete +
  // UpgradeSelection; -1 (hidden) otherwise.
  if (!s_horde) {
    int upgradeSecs = HvcMax(5, *(int32_t *)(gm + OFF_HVC_GM_UPGRADE_DUR));
    int completeSecs = HvcMax(3, (int)*(float *)(gm + OFF_HVC_GM_WAVE_COMPLETE_DUR));
    HavocSendTimeToNextWave(gs, s_phase == 0   ? upgradeSecs - s_secs
                                : s_phase == 3 ? completeSecs - s_secs + upgradeSecs
                                               : -1);
  }
}

// ---------------------------------------------------------------------------
// Chat commands for this match (always on; nothing happens until one arrives)
//
// mmogbrain queues admin chat commands (/turbo, /npc, /wave) per match
// (chat_commands.go); GET /battle/commands?match=<-MatchID=>&since=<seq>
// returns "<seq> <command> <arg>" lines. Polled every 2 game-mode timer ticks
// on the game thread, so the handlers may touch actors directly.
//
//   turbo on|off   the Turbo TDM feats (kTurboTdmFeats) on every ship, and on
//                  every ship spawned while on (HookFeatsBegin).
//   npc normal|hard|brutal
//                  Havoc boost feats on every NPC ship (any pawn whose
//                  controller is not a PlayerController), re-applied to new
//                  spawns each poll. GUESS: the boost feats act on NPC ships
//                  as on players; only player ships were seen to take them.
//   wave skip      Havoc: K2_DestroyActor on every listed live enemy; the
//                  director then sees 0 alive and completes the wave (no XP
//                  drops for those).
//   wave <n>       Havoc: the next wave to start is wave n (1..50); combine
//                  with "wave skip" to get there now.
//   bots 1..4      Havoc: every enemy the wave lists (AddNPCToEnemyList) is
//                  queued N times -- copies of its scene entry through the
//                  game's own AddNPCSpawn, listed like the original so the
//                  wave waits for them and they drop XP. Takes effect on the
//                  next spawns.
//   enemy <ShipId>|off
//                  Havoc: every new enemy spawns as this NPC dataset row
//                  (HavocEnemyOverride).
//   difficulty recruit|veteran|legendary
//                  Havoc: YGameState_PVE m_pveDifficulty 1/2/3, which the
//                  group spawn component reads per enemy to pick
//                  EnemyNameRecruit/Veteran/Legendary from the map's waves.
//                  Takes effect on the next spawns.
// Removal is UYFeatsComponent::RemoveFeat(FName) through ProcessEvent.
// NOT verified live: RemoveFeat's effect, turbo replication, NPC feats.
static const wchar_t *const kNpcTierFeats[3][5] = {
    {nullptr},
    {L"HVC_Boost_Def_HpInc_Large", L"HVC_Boost_Def_ArInc_Large", L"HVC_Boost_Off_DmgInc_Medium", nullptr},
    {L"HVC_Boost_Def_HpInc_Large", L"HVC_Boost_Def_ArInc_Large", L"HVC_Boost_Off_DmgInc_Medium",
     L"HVC_Boost_Def_HPRegInc_Large", L"HVC_Boost_Off_DmgInc_Large"},
};
static int g_npcTier = 0;
struct NpcTierApplied { void *pawn; int tier; };
static NpcTierApplied g_npcApplied[FEATS_SLOTS];

static void FillFName(uint8_t *parms, const char *pc, uint32_t off, void *ctx) {
  if (!strcmp(pc, "NameProperty"))
    memcpy(parms + off, ctx, sizeof(FNameMin));
}

// faulted: set when the call faulted (callers stop retrying on that pawn: a
// fault can leave the component half-updated, and repeating it compounds that).
static bool FeatOp(void *comp, const wchar_t *feat, bool add, bool *faulted = nullptr) {
  FNameMin name{};
  ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&name, feat, 1);
  bool ok = false;
  uintptr_t at = 0;
  __try {
    ok = add ? AddFeatStackable(comp, name, feat)
             : CallFilled(comp, L"RemoveFeat", FillFName, &name);
  } __except (CaptureFault(GetExceptionInformation(), &at)) {
    Logf("commands: %s(%ls) faulted on %p at exe+0x%llX", add ? "AddFeat" : "RemoveFeat", feat, comp,
         (unsigned long long)(at - g_base));
    if (faulted)
      *faulted = true;
  }
  return ok;
}

static bool PawnIsNpc(void *pawn) {
  if (!IsReadable((uint8_t *)pawn + OFF_PAWN_CONTROLLER, 8))
    return false;
  void *ctrl = *(void **)((uint8_t *)pawn + OFF_PAWN_CONTROLLER);
  return ctrl && !ClassChainContains((UObjectMin *)ctrl, "PlayerController");
}

// The object is still the live entry of its GObjects slot and not pending
// kill. A destroyed ship's memory can be reused before our tables notice
// (hvcfun2: AddFeat faulted x5 on a pawn whose class read "None").
static bool ObjectLive(void *p) {
  FUObjectArrayMin *arr = GObjects();
  if (!arr || !arr->Objects || !IsReadable(p, sizeof(UObjectMin)))
    return false;
  int32_t i = ((UObjectMin *)p)->InternalIndex;
  return i >= 0 && i < arr->NumElements && IsReadable(&arr->Objects[i], sizeof(FUObjectItemMin)) &&
         arr->Objects[i].Object == p && !(arr->Objects[i].Flags & OBJ_PENDING_KILL);
}

// Onslaught creeps reject every /npc, /turbo and elite feat (onsfun5, 0/5):
// "FYS_FeatsDataTableRow::GetByName() - no data row found for name:
// HVC_Boost_Def_HpInc_Large" / "AddFeatInternal() : could not retrieve the
// feat named ...". AddFeatInternal 0x337960 falls back to the component's own
// table list at +0xF8 (TArray, Num +0x100, Max +0x104; it memcpys the 16-byte
// elements and reads elem[0]->+0x18 as the table's name, so elem[0] is the
// UDataTable*). Player ship pawn BPs list DN_Feats_Havoc_Boosts_*,
// DN_Feats_Custom_Modifiers_OTS_DT etc. there; creep pawns do not. So copy any
// entry a live human ship's component has and this one lacks.
// GUESS: the second qword of an element is plain data (the engine copies the
// array with memcpy, so it owns nothing). The old buffer is leaked, not freed.
static void EnsureFeatTables(void *comp) {
  uint8_t *c = (uint8_t *)comp;
  if (!IsReadable(c + 0xF8, 16))
    return;
  for (FeatsByPawn &d : g_featsByPawn) {
    if (!d.pawn || d.comp == comp || !ObjectLive(d.pawn) || FeatsCompOf(d.pawn) != d.comp || PawnIsNpc(d.pawn))
      continue;
    uint8_t *dc = (uint8_t *)d.comp;
    int dn = IsReadable(dc + 0xF8, 16) ? *(int32_t *)(dc + 0x100) : 0;
    uint8_t *de = dn > 0 && dn <= 64 ? *(uint8_t **)(dc + 0xF8) : nullptr;
    if (!de || !IsReadable(de, (size_t)dn * 16))
      continue;
    int n = *(int32_t *)(c + 0x100);
    uint8_t *e = *(uint8_t **)(c + 0xF8);
    if (n < 0 || n > 64 || (n && (!e || !IsReadable(e, (size_t)n * 16))))
      return;
    alignas(16) uint8_t add[64 * 16];
    int na = 0;
    for (int i = 0; i < dn; ++i) {
      void *tbl = *(void **)(de + i * 16);
      bool have = !tbl;
      for (int j = 0; j < n && !have; ++j)
        have = *(void **)(e + j * 16) == tbl;
      for (int j = 0; j < na && !have; ++j)
        have = *(void **)(add + j * 16) == tbl;
      if (!have && n + na < 64)
        memcpy(add + na++ * 16, de + i * 16, 16);
    }
    if (!na)
      return;
    uint8_t *buf = (uint8_t *)((tFMemoryMalloc)(g_base + RVA_FMEMORY_MALLOC))((size_t)(n + na) * 16, 0);
    if (!buf)
      return;
    if (n)
      memcpy(buf, e, (size_t)n * 16);
    memcpy(buf + n * 16, add, (size_t)na * 16);
    *(uint8_t **)(c + 0xF8) = buf;
    *(int32_t *)(c + 0x100) = n + na;
    *(int32_t *)(c + 0x104) = n + na;
    static int s_logged = 0;
    if (s_logged++ < 6)
      Logf("commands: feat tables: %s had %d, added %d from %s", ClassNameOf((UObjectMin *)((UObjectMin *)comp)->Outer),
           n, na, ClassNameOf(d.pawn));
    return;
  }
}

static void CommandTurbo(bool on) {
  if (g_turboOn == on)
    return;
  g_turboOn = on;
  int ships = 0, failed = 0;
  for (FeatsByPawn &e : g_featsByPawn) {
    if (!e.pawn || !ObjectLive(e.pawn) || FeatsCompOf(e.pawn) != e.comp)
      continue;
    if (on && PawnIsNpc(e.pawn))
      EnsureFeatTables(e.comp);
    for (const wchar_t *feat : kTurboTdmFeats)
      failed += !FeatOp(e.comp, feat, on);
    ++ships;
  }
  Logf("commands: turbo %s on %d ships (%d feat ops failed)", on ? "on" : "off", ships, failed);
}

static void NpcTierTick() {
  for (FeatsByPawn &e : g_featsByPawn) {
    if (!e.pawn || !ObjectLive(e.pawn) || FeatsCompOf(e.pawn) != e.comp || !PawnIsNpc(e.pawn))
      continue;
    NpcTierApplied *slot = nullptr, *freeSlot = nullptr;
    for (NpcTierApplied &a : g_npcApplied) {
      if (a.pawn == e.pawn)
        slot = &a;
      else if (!freeSlot && (!a.pawn || !ObjectLive(a.pawn) || !FeatsCompOf(a.pawn)))
        freeSlot = &a;
    }
    int had = slot ? slot->tier : 0;
    if (had == g_npcTier)
      continue;
    if (!slot) {
      if (!freeSlot)
        continue;
      slot = freeSlot;
      *slot = {e.pawn, 0};
    }
    for (const wchar_t *f : kNpcTierFeats[had])
      if (f)
        FeatOp(e.comp, f, false);
    if (g_npcTier)
      EnsureFeatTables(e.comp);
    int ok = 0, n = 0;
    for (const wchar_t *f : kNpcTierFeats[g_npcTier])
      if (f)
        ok += FeatOp(e.comp, f, true), ++n;
    slot->tier = g_npcTier;
    // Diagnostic: Onslaught creeps reject every AddFeat (onsfun5, 0/5).
    // AddFeatInternal 0x337960 returns false at once when the name is in the
    // component's set at +0x138 (0x18-byte elements, FName first; Num +0x140,
    // NumFree +0x16C) -- log what that set holds on a rejecting ship's class.
    if (!ok && n) {
      static void *s_diagCls[8];
      void *cls = ((UObjectMin *)e.pawn)->Class;
      int k = 0;
      while (k < 8 && s_diagCls[k] && s_diagCls[k] != cls)
        ++k;
      if (k < 8 && !s_diagCls[k]) {
        s_diagCls[k] = cls;
        uint8_t *c = (uint8_t *)e.comp;
        int num = IsReadable(c + 0x16C, 4) ? *(int32_t *)(c + 0x140) : -1;
        int nfree = num >= 0 ? *(int32_t *)(c + 0x16C) : -1;
        uint8_t *els = num > 0 ? *(uint8_t **)(c + 0x138) : nullptr;
        char names[400] = "";
        int o = 0;
        __try {
          for (int i = 0; els && i < num && i < 24 && IsReadable(els + i * 0x18, 0x18); ++i) {
            const char *nm = NameText(*(FNameMin *)(els + i * 0x18)); // free slots may read "?"
            o += snprintf(names + o, o < (int)sizeof names ? sizeof names - o : 0, " %s", nm ? nm : "?");
          }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
          o += snprintf(names + o, o < (int)sizeof names ? sizeof names - o : 0, " (faulted)");
        }
        Logf("commands: npc feats rejected on %s: comp+0x138 set num %d free %d:%s", ClassNameOf(e.pawn), num,
             nfree, names);
      }
    }
    static int s_logged = 0;
    if (s_logged++ < 24)
      Logf("commands: npc tier %d -> %d on %s %p (%d/%d feats added)", had, g_npcTier, ClassNameOf(e.pawn), e.pawn, ok, n);
  }
}

static UObjectMin *FindFnOnChain(void *obj, const char *name);

// Offset of a UFunction parameter by name (UProperty+0x50), or -1.
static int FnParmOffset(UObjectMin *f, const char *name) {
  for (UObjectMin *pp = *(UObjectMin **)((uint8_t *)f + 0x38); pp && IsReadable(pp, 0x58);
       pp = *(UObjectMin **)((uint8_t *)pp + 0x28)) {
    const char *pn = NameText(pp->Name);
    if (pn && !strcmp(pn, name))
      return (int)*(uint32_t *)((uint8_t *)pp + 0x50);
  }
  return -1;
}

// Every human player controller in the match (a PRI whose owner is a
// PlayerController with a net connection -- bots have none). Shared by the
// camera commands below.
static int CollectHumanPCs(uint8_t *gm, uint8_t **out, int max) {
  int n = 0;
  __try {
    uint8_t *gs = IsReadable(gm + OFF_GM_GAMESTATE, 8) ? *(uint8_t **)(gm + OFF_GM_GAMESTATE) : nullptr;
    if (!gs || !IsReadable(gs + OFF_GS_PLAYER_ARRAY, 16))
      return 0;
    uint8_t **arr = *(uint8_t ***)(gs + OFF_GS_PLAYER_ARRAY);
    int count = *(int32_t *)(gs + OFF_GS_PLAYER_ARRAY + 8);
    if (count <= 0 || count > 256 || !IsReadable(arr, (size_t)count * 8))
      return 0;
    for (int i = 0; i < count && n < max; ++i) {
      uint8_t *pri = arr[i];
      if (!IsReadable(pri, OFF_ACTOR_OWNER + 8))
        continue;
      uint8_t *pc = *(uint8_t **)(pri + OFF_ACTOR_OWNER);
      if (!IsReadable(pc, OFF_PC_NETCONNECTION + 8) || !ClassChainContains((UObjectMin *)pc, "PlayerController") ||
          !*(void **)(pc + OFF_PC_NETCONNECTION))
        continue;
      out[n++] = pc;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("commands: player scan faulted after %d", n);
  }
  return n;
}

// /shake N: APlayerController::ClientPlayCameraShake(Shake, Scale, PlaySpace,
// UserPlaySpaceRot) on every human controller. The shake is a stock ability
// camshake class from the client's paks (_SHARED/Camshakes), loaded once with
// the same StaticLoadObject(UClass, ...) path the precast loadouts use; the
// client plays it locally from the replicated class reference.
// GUESS: Scale multiplies the BP's oscillation amplitude (engine default);
// how strong N=10 feels is not measured.
static void CommandShake(uint8_t *gm, float scale) {
  static UObjectMin *s_cls = nullptr;
  if (!s_cls) {
    UObjectMin *uclassClass = ResolveUClassClass();
    __try {
      if (uclassClass)
        s_cls = (UObjectMin *)((tStaticLoadClass)(g_base + RVA_STATIC_LOAD_CLASS))(
            uclassClass, nullptr,
            L"/Game/Generic/Abilities/_SHARED/Camshakes/AB_AbilityLarge_CamShake01_BP.AB_AbilityLarge_CamShake01_BP_C",
            nullptr, 0, nullptr, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      s_cls = nullptr;
    }
    if (!s_cls)
      return (void)Logf("commands: shake class did not load");
  }
  uint8_t *pcs[64];
  int n = CollectHumanPCs(gm, pcs, 64), sent = 0;
  __try {
    for (int i = 0; i < n; ++i) {
      uint8_t *pc = pcs[i];
      UObjectMin *fn = FindFnOnChain(pc, "ClientPlayCameraShake");
      int oShake = fn ? FnParmOffset(fn, "Shake") : -1, oScale = fn ? FnParmOffset(fn, "Scale") : -1;
      if (oShake < 0 || oScale < 0 || oShake > 56 || oScale > 56)
        continue;
      alignas(16) uint8_t parms[64] = {}; // PlaySpace 0 = CameraLocal, rotator zero
      *(UObjectMin **)(parms + oShake) = s_cls;
      *(float *)(parms + oScale) = scale;
      void **vt = *(void ***)pc;
      ((tProcessEventVirt)vt[VT_PROCESS_EVENT / 8])(pc, fn, parms);
      ++sent;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("commands: shake faulted after %d players", sent);
  }
  Logf("commands: shake x%.0f sent to %d players", scale, sent);
}

// /flash <colour>: APlayerController::ClientSetCameraFade(bEnableFading,
// FadeColor, FadeAlpha, FadeTime, bFadeAudio) on every human controller --
// the screen jumps to the colour and fades back out over 1.5 s. Parameter
// names are in the exe's UTF-16 FName table; offsets come from reflection
// (FnParmOffset), not assumed. FColor is stored B,G,R,A (UE4 little-endian
// layout). bFadeAudio stays 0 so sound is untouched.
// GUESS: FadeAlpha (1,0) = from opaque to clear (engine 4.13
// StartCameraFade(FromAlpha, ToAlpha, ...)); not seen on a real client yet.
static void CommandFlash(uint8_t *gm, const char *colour) {
  static const struct { const char *name; uint8_t r, g, b; } kColours[] = {
      {"white", 255, 255, 255}, {"red", 255, 30, 30}, {"pink", 255, 150, 200},
      {"black", 0, 0, 0},       {"gold", 255, 200, 40}, {"blue", 40, 120, 255}};
  int c = 0;
  for (int i = 0; i < (int)(sizeof kColours / sizeof *kColours); ++i)
    if (!strcmp(colour, kColours[i].name))
      c = i;
  uint8_t *pcs[64];
  int n = CollectHumanPCs(gm, pcs, 64), sent = 0;
  __try {
    for (int i = 0; i < n; ++i) {
      uint8_t *pc = pcs[i];
      UObjectMin *fn = FindFnOnChain(pc, "ClientSetCameraFade");
      int oEn = fn ? FnParmOffset(fn, "bEnableFading") : -1, oCol = fn ? FnParmOffset(fn, "FadeColor") : -1,
          oAlpha = fn ? FnParmOffset(fn, "FadeAlpha") : -1, oTime = fn ? FnParmOffset(fn, "FadeTime") : -1;
      if (oEn < 0 || oCol < 0 || oAlpha < 0 || oTime < 0 || oEn > 56 || oCol > 56 || oAlpha > 52 || oTime > 56) {
        Logf("commands: flash: ClientSetCameraFade parms not found (%d %d %d %d)", oEn, oCol, oAlpha, oTime);
        break;
      }
      alignas(16) uint8_t parms[64] = {};
      parms[oEn] = 1;
      uint8_t *col = parms + oCol;
      col[0] = kColours[c].b, col[1] = kColours[c].g, col[2] = kColours[c].r, col[3] = 255;
      ((float *)(parms + oAlpha))[0] = 1.f, ((float *)(parms + oAlpha))[1] = 0.f;
      *(float *)(parms + oTime) = 1.5f;
      void **vt = *(void ***)pc;
      ((tProcessEventVirt)vt[VT_PROCESS_EVENT / 8])(pc, fn, parms);
      ++sent;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("commands: flash faulted after %d players", sent);
  }
  Logf("commands: flash %s sent to %d players", kColours[c].name, sent);
}

// /slowmo F (0.2-1): AWorldSettings::TimeDilation, a replicated float, so
// every client slows with the server. Reached by reflection only:
// UWorld.PersistentLevel -> ULevel.WorldSettings -> TimeDilation (all three
// are UPROPERTYs in 4.13). 1 = normal speed. Not reset automatically; a new
// match starts at 1 again.
// GUESS: client movement prediction copes with a dilated server; not seen on
// a real client yet.
// /kill <pid|all>. First try was the game's own debug RPC ServerKillMe
// (UYMPCheatManager natives 0x6253F0, StaticClass 0x603100, thunk 0x771F60):
// RETRACTED 10-16 20:56 -- in this shipping build its _Validate is
// "mov al,1; ret" (0x15FBF00) and the implementation a bare "ret" (0x1AA9450),
// cheats compiled out; called on a constructed cheat manager, nothing died.
// Now a real death: UGameplayStatics::ApplyDamage(DamagedActor, BaseDamage,
// EventInstigator, DamageCauser, DamageTypeClass) on its class default object,
// so the pawn's own TakeDamage runs and the normal death/respawn flow follows.
// GUESS: AYPawn's TakeDamage accepts a generic (null DamageType, no
// instigator) point of damage this large; the headless run decides.
static UObjectMin *FindObjectNamed(const char *want) {
  FUObjectArrayMin *arr = GObjects();
  if (!arr || !arr->Objects)
    return nullptr;
  for (int i = 0; i < arr->NumElements; ++i) {
    UObjectMin *o = arr->Objects[i].Object;
    const char *n = o ? NameText(o->Name) : nullptr;
    if (n && !strcmp(n, want))
      return o;
  }
  return nullptr;
}

// probefn <object> <function>: logs every parameter of a UFunction -- name,
// property class, offset (+0x50), ElementSize (+0x34), PropertyFlags (+0x38;
// 0x80 Parm, 0x100 OutParm, 0x400 ReturnParm). Debug aid for driving
// Blueprint-callable natives (UYBlueprintLibrary::SpawnProjectile) through
// CallFilled. GUESS: +0x34/+0x38 follow the UE 4.13 UProperty layout; only
// +0x50 is proven here (CallFilled).
// "gm" / "gs" name the live game mode / game state. The lookup walks the class
// chain (FindFnOnChain): FindFunctionChecked is fatal on a missing name.
static UObjectMin *ProbeObject(uint8_t *gm, const char *objName) {
  if (!_stricmp(objName, "gm"))
    return (UObjectMin *)gm;
  if (!_stricmp(objName, "gs"))
    return gm && IsReadable(gm + OFF_GM_GAMESTATE, 8) ? *(UObjectMin **)(gm + OFF_GM_GAMESTATE) : nullptr;
  return FindObjectNamed(objName);
}

static void ProbeFn(uint8_t *gm, const char *objName, const char *fnName) {
  UObjectMin *obj = ProbeObject(gm, objName);
  if (!obj)
    return (void)Logf("probefn: object %s not found", objName);
  void *f = FindFnOnChain(obj, fnName);
  if (!f)
    return (void)Logf("probefn: %s has no function %s", objName, fnName);
  Logf("probefn: %s.%s = %p (class %s)", objName, fnName, f, ClassNameOf(obj));
  UObjectMin *pp = *(UObjectMin **)((uint8_t *)f + 0x38);
  for (int j = 0; pp && j < 32 && IsReadable(pp, 0x58); ++j) {
    const char *pn = NameText(pp->Name);
    Logf("probefn:   %-28s %-18s off 0x%03x size 0x%x flags 0x%llx", pn ? pn : "?", ClassNameOf(pp),
         *(uint32_t *)((uint8_t *)pp + 0x50), *(uint32_t *)((uint8_t *)pp + 0x34),
         (unsigned long long)*(uint64_t *)((uint8_t *)pp + 0x38));
    pp = *(UObjectMin **)((uint8_t *)pp + 0x28);
  }
}

static bool StrIStr(const char *h, const char *n) {
  for (; *h; ++h) {
    size_t i = 0;
    while (n[i] && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)n[i]))
      ++i;
    if (!n[i])
      return true;
  }
  return false;
}

// props <object|pawn> [substring]: every reflected property of an object (or
// of the first human ship) whose name contains substring, with offset and,
// for object properties, the value's class. For finding fields by name.
static void *PropObjValue(void *obj, const char *name);
// The first pointer in prop+0x70..+0x98 to an object of class `want`
// (UStructProperty::Struct, UArrayProperty::Inner). GUESS: the scan window;
// only Offset_Internal (+0x50) is proven for this build's UProperty.
static UObjectMin *PropRef(UObjectMin *prop, const char *want) {
  for (int o = 0x70; o <= 0x98; o += 8) {
    UObjectMin *v = IsReadable((uint8_t *)prop + o, 8) ? *(UObjectMin **)((uint8_t *)prop + o) : nullptr;
    if (IsReadable(v, sizeof(UObjectMin)) && IsReadable(v->Class, 0x40)) {
      const char *cn = ClassNameOf(v);
      if (cn && (!strcmp(cn, want) || (!strcmp(want, "*Property") && strstr(cn, "Property"))))
        return v;
    }
  }
  return nullptr;
}
static void ProbeStructFields(UObjectMin *st, int depth) {
  int shown = 0;
  for (UObjectMin *s2 = st; s2 && IsReadable(s2, 0x40) && shown < 80; s2 = *(UObjectMin **)((uint8_t *)s2 + OFF_USTRUCT_SUPER))
    for (UObjectMin *f = *(UObjectMin **)((uint8_t *)s2 + 0x38); f && IsReadable(f, 0x78) && shown < 80;
         f = *(UObjectMin **)((uint8_t *)f + 0x28), ++shown) {
      const char *fc = ClassNameOf(f);
      UObjectMin *sub = !strcmp(fc, "StructProperty") ? PropRef(f, "ScriptStruct") : nullptr;
      Logf("props: %*s+0x%03x size 0x%x %-18s %s%s%s", depth * 2, "", *(uint32_t *)((uint8_t *)f + 0x50),
           *(uint32_t *)((uint8_t *)f + 0x34), fc, NameText(f->Name), sub ? " : " : "", sub ? NameText(sub->Name) : "");
      if (sub && depth < 2)
        ProbeStructFields(sub, depth + 1);
    }
}
static void ProbeStructProp(UObjectMin *obj, const char *name) {
  UObjectMin *f = FindPropObj(obj, name);
  if (!f)
    return (void)Logf("props: %s has no property %s", ClassNameOf(obj), name);
  UObjectMin *p = f;
  if (!strcmp(ClassNameOf(f), "ArrayProperty") && !(p = PropRef(f, "*Property")))
    return (void)Logf("props: %s: array inner property not found", name);
  UObjectMin *st = !strcmp(ClassNameOf(p), "StructProperty") ? PropRef(p, "ScriptStruct") : nullptr;
  Logf("props: %s.%s +0x%x (%s, element %s) struct %s", ClassNameOf(obj), name, *(uint32_t *)((uint8_t *)f + 0x50),
       ClassNameOf(f), ClassNameOf(p), st ? NameText(st->Name) : "NOT FOUND");
  if (st)
    ProbeStructFields(st, 1);
}
static void ProbeProps(uint8_t *gm, const char *objName, const char *filter) {
  UObjectMin *obj = nullptr;
  if (!_stricmp(objName, "pawn")) {
    uint8_t *pcs[64];
    int n = CollectHumanPCs(gm, pcs, 64);
    for (int i = 0; i < n && !obj; ++i)
      obj = IsReadable(pcs[i] + OFF_CONTROLLER_PAWN, 8) ? *(UObjectMin **)(pcs[i] + OFF_CONTROLLER_PAWN) : nullptr;
  } else if (!_strnicmp(objName, "pawn.", 5)) {
    uint8_t *pcs[64];
    int n = CollectHumanPCs(gm, pcs, 64);
    for (int i = 0; i < n && !obj; ++i) {
      void *p = IsReadable(pcs[i] + OFF_CONTROLLER_PAWN, 8) ? *(void **)(pcs[i] + OFF_CONTROLLER_PAWN) : nullptr;
      obj = p ? (UObjectMin *)PropObjValue(p, objName + 5) : nullptr;
    }
  } else
    obj = ProbeObject(gm, objName);
  if (!IsReadable(obj, sizeof(UObjectMin)))
    return (void)Logf("props: %s not found", objName);
  // "struct:<property>" lists the fields of a struct property (or of an
  // array property's struct elements), one nested level deep.
  if (!strncmp(filter, "struct:", 7))
    return ProbeStructProp(obj, filter + 7);
  // "fn:<substring>" lists functions instead of properties.
  bool fns = !strncmp(filter, "fn:", 3);
  if (fns)
    filter += 3;
  Logf("props: %s = %p (class %s), filter \"%s\"", objName, obj, ClassNameOf(obj), filter);
  int shown = 0;
  for (UObjectMin *cls = obj->Class; cls && IsReadable(cls, 0x40); cls = *(UObjectMin **)((uint8_t *)cls + OFF_USTRUCT_SUPER))
    for (UObjectMin *f = *(UObjectMin **)((uint8_t *)cls + 0x38); f && IsReadable(f, 0x78) && shown < 200;
         f = *(UObjectMin **)((uint8_t *)f + 0x28)) {
      const char *fn = NameText(f->Name), *fc = ClassNameOf(f);
      if (!fn || !strcmp(fc, "Function") != fns || (*filter && !StrIStr(fn, filter)))
        continue;
      uint32_t off = *(uint32_t *)((uint8_t *)f + 0x50);
      ++shown;
      if (!strcmp(fc, "ObjectProperty") || !strcmp(fc, "WeakObjectProperty")) {
        void *v = IsReadable((uint8_t *)obj + off, 8) ? *(void **)((uint8_t *)obj + off) : nullptr;
        Logf("props:   %-40s %-20s +0x%x (%s) = %p %s", fn, fc, off, ClassNameOf(cls), v,
             !strcmp(fc, "ObjectProperty") && IsReadable(v, sizeof(UObjectMin)) ? ClassNameOf((UObjectMin *)v) : "");
      } else
        Logf("props:   %-40s %-20s +0x%x (%s) flags %llx", fn, fc, off, ClassNameOf(cls),
             (unsigned long long)*(uint64_t *)((uint8_t *)f + 0x38));
    }
}

// CallFilled with the parameter name passed to `fill` and the parameter block
// handed back (ReturnValue and out params live in it). parms must hold 256.
static bool CallNamed(void *obj, const wchar_t *name, tParmFillNamed fill, void *ctx, uint8_t *parms) {
  if (!IsReadable(obj, sizeof(UObjectMin)))
    return false;
  FNameMin n = {};
  ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&n, name, 1 /* FNAME_Add */);
  void *fn = FindFunctionSafe(obj, *(uint64_t *)&n);
  if (!fn || !IsReadable((uint8_t *)fn + 0x40, 8))
    return false;
  memset(parms, 0, 256);
  UObjectMin *pp = *(UObjectMin **)((uint8_t *)fn + 0x38);
  for (int j = 0; pp && j < 24 && IsReadable(pp, 0x58); ++j) {
    uint32_t off = *(uint32_t *)((uint8_t *)pp + 0x50);
    const char *pn = NameText(pp->Name);
    if (fill && off < 256 - 48)
      fill(parms, ClassNameOf(pp), pn ? pn : "", off, ctx);
    pp = *(UObjectMin **)((uint8_t *)pp + 0x28);
  }
  void **vt = *(void ***)obj;
  ((tProcessEventVirt)vt[VT_PROCESS_EVENT / 8])(obj, fn, parms);
  return true;
}

// Fire-and-forget form (Havoc stage 2 RPCs): own parameter block, a fault in
// the call is logged instead of taking the tick down.
static bool CallNamed(void *obj, const wchar_t *name, tParmFillNamed fill, void *ctx) {
  alignas(16) uint8_t parms[256];
  __try {
    return CallNamed(obj, name, fill, ctx, parms);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("havoc: EXCEPTION in %ls on %s", name, ClassNameOf(obj));
    return false;
  }
}

// Actor location through the Blueprint getter K2_GetActorLocation (FVector
// ReturnValue). The mod had no root-component offset to read it directly.
static bool ActorLocation(void *actor, float out[3]) {
  alignas(16) uint8_t parms[256];
  struct Ctx { uint32_t off; } c = {0xFFFFFFFF};
  auto fill = [](uint8_t *, const char *, const char *pn, uint32_t off, void *ctx) {
    if (!strcmp(pn, "ReturnValue"))
      ((Ctx *)ctx)->off = off;
  };
  __try {
    if (!CallNamed(actor, L"K2_GetActorLocation", fill, &c, parms) || c.off == 0xFFFFFFFF)
      return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  memcpy(out, parms + c.off, 12);
  return true;
}

// /meteor [n]: n Ultra torpedoes (Assault "Pri_Torpedo_Ultra") rain on every
// human ship. UYBlueprintLibrary::SpawnProjectile (native 0x3125B0) params,
// from probefn in headless meteorprobe 10-16 23:14:
//   0x00 WorldContextObject, 0x08 projectileBlueprint (UBlueprint),
//   0x10 projectileDataRowName (FName), 0x18 loadContent (bool),
//   0x20 projectileTransform (FTransform 0x30), 0x50 firingWeapon,
//   0x58 projectileID (int), 0x5C damageModifierData (0xC),
//   0x68 randomSeed (int), 0x70 ReturnValue (projectile).
// Blueprint and row come from AB_AS_Pri_Torpedo_Ultra_Weapon_BP's package:
// m_projectileBaseBlueprint WP_YProjectileMissile_BP, m_projectileDataTableRowName
// AB_AS_Pri_Torpedo_Ultra_Projectile_BP (a row of YProjectileMissile_Offline_DT).
// First GObjects entry with this name whose class name is `cls`.
static UObjectMin *FindObjectNamedOfClass(const char *want, const char *cls) {
  FUObjectArrayMin *arr = GObjects();
  for (int i = 0; arr && arr->Objects && i < arr->NumElements; ++i) {
    UObjectMin *o = arr->Objects[i].Object;
    const char *n = o && IsReadable(o, sizeof(UObjectMin)) ? NameText(o->Name) : nullptr;
    const char *cn = n && !strcmp(n, want) ? ClassNameOf(o) : nullptr;
    if (cn && !strcmp(cn, cls))
      return o;
  }
  return nullptr;
}

struct MeteorShot {
  void *world, *cls, *weapon;
  FNameMin row;
  float quat[4], pos[3];
  int id, seed;
};
static void FillMeteor(uint8_t *parms, const char *, const char *pn, uint32_t off, void *ctx) {
  MeteorShot *m = (MeteorShot *)ctx;
  if (!strcmp(pn, "WorldContextObject"))
    *(void **)(parms + off) = m->world;
  else if (!strcmp(pn, "projectileBlueprint"))
    *(void **)(parms + off) = m->cls;
  else if (!strcmp(pn, "projectileDataRowName"))
    memcpy(parms + off, &m->row, 8);
  else if (!strcmp(pn, "loadContent"))
    parms[off] = 1;
  else if (!strcmp(pn, "projectileTransform")) {
    // FTransform: FQuat Rotation @0, FVector Translation @0x10, Scale3D @0x20.
    float *t = (float *)(parms + off);
    memcpy(t, m->quat, 16);
    memcpy(t + 4, m->pos, 12);
    t[8] = t[9] = t[10] = 1.f;
  } else if (!strcmp(pn, "firingWeapon"))
    *(void **)(parms + off) = m->weapon;
  else if (!strcmp(pn, "projectileID"))
    *(int32_t *)(parms + off) = m->id;
  else if (!strcmp(pn, "damageModifierData")) {
    // GUESS: three float multipliers; 1.0 is neutral if so. Struct layout
    // not read from the binary.
    float *d = (float *)(parms + off);
    d[0] = d[1] = d[2] = 1.f;
  } else if (!strcmp(pn, "randomSeed"))
    *(int32_t *)(parms + off) = m->seed;
}

// Object-valued property `name` of obj (Offset_Internal at +0x50 in this
// build, see the m_readyForNextWave note), or null.
static void *PropObjValue(void *obj, const char *name) {
  UObjectMin *f = FindPropObj((UObjectMin *)obj, name);
  if (!f || strcmp(ClassNameOf(f), "ObjectProperty"))
    return nullptr;
  void *v = *(void **)((uint8_t *)obj + *(uint32_t *)((uint8_t *)f + 0x50));
  return IsReadable(v, sizeof(UObjectMin)) ? v : nullptr;
}

static void *ShipBaseClassOf(void *pawn);

// firingWeapon for /meteor. SpawnProjectile (0x3125B0) dereferences it
// unconditionally: 0x14171CCC0 reads its RootComponent (+0x198) scale, so
// null faults (headless onsfun2 10-16 23:35: "SpawnProjectile faulted",
// projectiles left without offline data because AYProjectile init 0x513460
// runs after that read). It is also the damage source, so an enemy ship's
// weapon makes the torpedoes hurt players; the player's own weapon is the
// fallback (GUESS: then they hit only enemies, not tested).
// Weapon actors are not direct fields of YWeaponManagerComponent (hvcfun2: the
// props walk found none), so scan GObjects instead. A weapon's owning ship is
// at +0x550: FUN_1405119b0 (called by SpawnProjectile on firingWeapon) reads
// *(weapon+0x550)+0x868 = AYPawn::m_weaponManager.
#define OFF_WEAPON_OWNER_PAWN 0x550
static void *MeteorWeapon(uint8_t *gm, void *preferOwner = nullptr, bool onlyPreferred = false) {
  uint8_t *pcs[64];
  int n = CollectHumanPCs(gm, pcs, 64), humanTeams = 0;
  void *own = nullptr, *ownPawn = nullptr;
  for (int i = 0; i < n; ++i) {
    uint8_t *hp = IsReadable(pcs[i] + OFF_CONTROLLER_PAWN, 8) ? *(uint8_t **)(pcs[i] + OFF_CONTROLLER_PAWN) : nullptr;
    if (!hp || !IsReadable(hp, OFF_SHIP_TEAM + 1))
      continue;
    if (!ownPawn)
      ownPawn = hp;
    int t = hp[OFF_SHIP_TEAM];
    if (t == 1 || t == 2)
      humanTeams |= 1 << t;
  }
  FUObjectArrayMin *arr = GObjects();
  if (!arr || !arr->Objects)
    return nullptr;
  int seen = 0;
  __try {
    for (int i = 0; i < arr->NumElements; ++i) {
      UObjectMin *o = arr->Objects[i].Object;
      if (!o || (o->Flags & RF_CDO_OR_ARCHETYPE) || (arr->Objects[i].Flags & OBJ_PENDING_KILL) ||
          !IsReadable(o, OFF_WEAPON_OWNER_PAWN + 8) || !IsReadable(o->Class, 0x40) ||
          !ClassChainContains(o, "Weapon") || !ClassChainIs(o, "Actor") || !*(void **)((uint8_t *)o + 0x198))
        continue;
      uint8_t *owner = *(uint8_t **)((uint8_t *)o + OFF_WEAPON_OWNER_PAWN);
      if (!IsReadable(owner, 0x870) || *(void **)(owner + 0x868) == nullptr)
        continue;
      ++seen;
      if (preferOwner && owner == preferOwner) {
        Logf("commands: meteor -- herald weapon %s", ClassNameOf(o));
        return o;
      }
      if (onlyPreferred) // a herald's own weapon or nothing: the first enemy
        continue;        // weapon was the team-2 herald's, twice (skymeteor5)
      if (owner == ownPawn && !own)
        own = o;
      int t = owner[OFF_SHIP_TEAM];
      if ((t == 1 || t == 2) && !(humanTeams & (1 << t))) {
        Logf("commands: meteor -- enemy weapon %s on %s (team %d)", ClassNameOf(o), ClassNameOf((UObjectMin *)owner), t);
        return o;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("commands: meteor -- weapon scan faulted");
  }
  if (onlyPreferred)
    return nullptr;
  Logf("commands: meteor -- %d weapon actors, no enemy one; own %s", seen, own ? ClassNameOf((UObjectMin *)own) : "none");
  return own;
}

// Registration with the weapon's projectile tracking -- the tail of a real
// shot that /meteor never ran, and the reason its missiles were spawned on the
// host and streamed to nobody.
//
// Delivery of projectiles to a client is NOT actor replication: AYProjectileWeapon
// registers each projectile with a manager (FYProjectileTracking), and a
// per-client tick (0x592720) queries that manager for the projectiles in range
// (0x511B80) and sends them as a stream to AYPawn::ClientReceiveProjectilesInRange
// (native 0x746340). UYProjectileReplicator carries only the steering/destroy
// multicast events, so a projectile absent from the manager is invisible on
// every client. AYProjectileWeapon::ExecuteFiring (0x50EA93) does this right
// after SpawnProjectile returns; SpawnProjectile's only other caller is the
// Blueprint VM thunk (0x77F020), which is how the engine's own BP nodes miss it too.
//
// Calling conventions, read off ExecuteFiring (all three take the weapon or the
// manager in rcx, which is where the compiler put the only argument):
//   0x50ED0E  mov rcx, rsi (the weapon) -> 0x5126E0 -> rax = manager
//   0x50ED1B  mov rdx, r15 (the projectile); mov rcx, rax -> 0x51E140(mgr, proj)
//   0x50EAAE  rax = 0x5119B0(weapon) = the weapon manager component (kept in r12)
//   0x50ED2B  mov rax, [r12]; call [rax+0x238](r12, projectile) -- its notify
// The projectile itself is r15 there, the value SpawnProjectile returned.
// GUESS: that running this tail is enough for the client to render the
// missile. The missiles /meteor spawns already log "AYProjectile::Tick(): No
// ProjectileOfflineData available", so they may still be missing whatever the
// client needs to reconstruct them; a live /meteor is what tells us.
typedef void *(*tProjTrackingForWeapon)(void *weapon);
typedef void (*tRegisterProjectile)(void *manager, void *projectile);
typedef void *(*tWeaponManagerForWeapon)(void *weapon);
#define RVA_PROJ_TRACKING_FOR_WEAPON 0x5126E0
#define RVA_REGISTER_PROJECTILE 0x51E140
#define RVA_WEAPON_MANAGER_FOR_WEAPON 0x5119B0
#define OFF_VT_WEAPON_MANAGER_NOTIFY 0x238

// Registers `proj` with `weapon`'s projectile tracking and notifies the weapon
// manager, exactly as ExecuteFiring does. True when RegisterProjectile ran with
// a real manager (the manager getter returning null is a silent no-op in the
// original too: 0x50ED16 test rax,rax / je).
static bool RegisterMeteorProjectile(void *weapon, void *proj) {
  bool registered = false;
  __try {
    void *mgr = ((tProjTrackingForWeapon)(g_base + RVA_PROJ_TRACKING_FOR_WEAPON))(weapon);
    if (mgr) {
      ((tRegisterProjectile)(g_base + RVA_REGISTER_PROJECTILE))(mgr, proj);
      registered = true;
    }
    // The notify the original makes whether or not it registered (it is
    // guarded by r12, the weapon manager, not by the manager getter).
    void *wm = ((tWeaponManagerForWeapon)(g_base + RVA_WEAPON_MANAGER_FOR_WEAPON))(weapon);
    if (wm && IsReadable(wm, 8) && *(void ***)wm)
      ((void (*)(void *, void *))(*(void ***)wm)[OFF_VT_WEAPON_MANAGER_NOTIFY / 8])(wm, proj);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("commands: meteor -- projectile registration faulted");
    return false;
  }
  return registered;
}

// from: launch point for every missile (the sky ship's position) instead of
// a random point 9000 above each target; skyShip: whose weapon fires them.
struct MeteorAssetsT { UObjectMin *lib, *cls, *cdo, *dt, *bp; FNameMin row; };
static int s_meteorId = 0x5EED0000; // GUESS: any id unused by real shooters
// > 0: MeteorFire writes it into each shot's YProjectile::m_abilityDamage
// (+0x6a0, 24000 on the Ultra row, probe 2026-10-21) -- /rain's weaker drops.
static float g_meteorDamage = 0;
// > 0: AActor::SetLifeSpan on each shot, so a miss is destroyed instead of
// flying on forever (rain1: 37 of 51 still alive, far out, when it ended).
static float g_meteorLifeSpan = 0;
static UObjectMin *g_nukeWeaponCls = nullptr; // AB_DN_Pri_WarpM_Dmg_Weapon_T5_BP_C, MeteorAssets

static bool MeteorAssets(MeteorAssetsT &a) {
  static UObjectMin *s_lib = nullptr, *s_cls = nullptr;
  if (!s_lib)
    s_lib = FindObjectNamed("Default__YBlueprintLibrary");
  if (!s_cls) {
    UObjectMin *uc = ResolveUClassClass();
    __try {
      s_cls = uc ? (UObjectMin *)((tStaticLoadClass)(g_base + RVA_STATIC_LOAD_CLASS))(
                       uc, nullptr, L"/Game/Generic/Weapons/_SHARED/WP_YProjectileMissile_BP.WP_YProjectileMissile_BP_C",
                       nullptr, 0, nullptr, false)
                 : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      s_cls = nullptr;
    }
    if (IsReadable(s_cls, sizeof(UObjectMin)))
      PinToRootSet(s_cls);
    else
      s_cls = nullptr;
  }
  // SpawnProjectile reads the offline data table from the projectile class
  // default object (+0x5D8; 0x312707, null -> "no offline data table
  // specified", meteor1 23:21). The base WP_YProjectileMissile_BP leaves it
  // null -- weapons carry it -- so the missile table is lent to the CDO for
  // the call and put back after.
  static UObjectMin *s_dt = nullptr, *s_cdo = nullptr;
  if (s_cls && !s_cdo) {
    UObjectMin *classes[1] = {s_cls}, *cdos[1] = {};
    FindCDOsForClasses(classes, cdos, 1);
    s_cdo = cdos[0];
  }
  if (!s_dt) {
    UObjectMin *dtClass = FindObjectNamedOfClass("DataTable", "Class");
    __try {
      s_dt = dtClass ? (UObjectMin *)((tStaticLoadClass)(g_base + RVA_STATIC_LOAD_CLASS))(
                           dtClass, nullptr,
                           L"/Game/Generic/DataTables/Projectiles/YProjectileMissile_Offline_DT.YProjectileMissile_Offline_DT",
                           nullptr, 0, nullptr, false)
                     : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      s_dt = nullptr;
    }
    if (IsReadable(s_dt, sizeof(UObjectMin)))
      PinToRootSet(s_dt);
    else
      s_dt = nullptr;
  }
  // projectileBlueprint is the UBlueprint asset, not its class: the native
  // takes [arg+0x30] (UBlueprintCore::GeneratedClass) as the class whose CDO
  // it reads (0x3126A6). Passing the _C class made it read the native parent
  // YProjectileMissile's CDO. Dreadnought cooks the UBlueprint objects
  // (weapons hold m_projectileBaseBlueprint); loading the class loads it.
  static UObjectMin *s_bp = nullptr;
  if (!s_bp && s_cls) {
    s_bp = FindObjectNamedOfClass("WP_YProjectileMissile_BP", "Blueprint");
    if (s_bp && *(UObjectMin **)((uint8_t *)s_bp + 0x30) != s_cls) {
      Logf("commands: meteor -- blueprint %p GeneratedClass %p is not %p", s_bp, *(void **)((uint8_t *)s_bp + 0x30), s_cls);
      s_bp = nullptr;
    }
  }
  // The real nuke (operator 10-17: "the projectiles are NOT nukes ... my t5
  // medium dreadnought has the nuke, it's the first module"): blueprint and
  // row read from AB_DN_Pri_WarpM_Dmg_Weapon_T5_BP's CDO, so the shot is the
  // "Nuclear Missile" ability's own projectile. Its row
  // AB_DN_Pri_WarpM_Dmg_Projectile_T5_BP is in YProjectileMissile_Offline_DT
  // too. Falls back to the base missile blueprint with that row.
  static bool s_nukeTried = false;
  static UObjectMin *s_nBp = nullptr, *s_nCdo = nullptr;
  static FNameMin s_row = {};
  if (!s_nukeTried && s_cls) {
    s_nukeTried = true;
    UObjectMin *uc = ResolveUClassClass(), *wcls = nullptr;
    __try {
      wcls = uc ? (UObjectMin *)((tStaticLoadClass)(g_base + RVA_STATIC_LOAD_CLASS))(
                      uc, nullptr,
                      L"/Game/Generic/Abilities/Dreadnought/Pri_WarpM_Dmg/T5/"
                      L"AB_DN_Pri_WarpM_Dmg_Weapon_T5_BP.AB_DN_Pri_WarpM_Dmg_Weapon_T5_BP_C",
                      nullptr, 0, nullptr, false)
                : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      wcls = nullptr;
    }
    UObjectMin *wcdo = nullptr;
    if (IsReadable(wcls, sizeof(UObjectMin))) {
      PinToRootSet(wcls);
      g_nukeWeaponCls = wcls;
      UObjectMin *classes[1] = {wcls}, *cdos[1] = {};
      FindCDOsForClasses(classes, cdos, 1);
      wcdo = cdos[0];
    }
    int bo = wcdo ? PropOffset(wcdo, "m_projectileBaseBlueprint") : -1;
    int ro = wcdo ? PropOffset(wcdo, "m_projectileDataTableRowName") : -1;
    UObjectMin *bp = bo > 0 && IsReadable((uint8_t *)wcdo + bo, 8) ? *(UObjectMin **)((uint8_t *)wcdo + bo) : nullptr;
    if (ro > 0 && IsReadable((uint8_t *)wcdo + ro, 8))
      s_row = *(FNameMin *)((uint8_t *)wcdo + ro);
    UObjectMin *gen = IsReadable(bp, 0x38) ? *(UObjectMin **)((uint8_t *)bp + 0x30) : nullptr;
    UObjectMin *gcdo = nullptr;
    if (IsReadable(gen, sizeof(UObjectMin))) {
      UObjectMin *classes[1] = {gen}, *cdos[1] = {};
      FindCDOsForClasses(classes, cdos, 1);
      gcdo = cdos[0];
    }
    // +0x5D8 is YProjectileMissile's table slot; any other class is unsafe.
    if (gcdo && ClassChainContains(gcdo, "YProjectileMissile")) {
      PinToRootSet(bp);
      s_nBp = bp, s_nCdo = gcdo;
    }
    Logf("commands: meteor -- nuke weapon class %p cdo %p: blueprint %s (%s) row %s -> %s", wcls, wcdo,
         bp ? NameText(bp->Name) : "-", gen ? NameText(gen->Name) : "-", s_row.ComparisonIndex ? NameText(s_row) : "-",
         s_nBp ? "using it" : "base missile blueprint");
    if (!s_row.ComparisonIndex)
      ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&s_row, L"AB_DN_Pri_WarpM_Dmg_Projectile_T5_BP", 1);
  }
  a = {s_lib, s_cls, s_cdo, s_dt, s_bp, s_row};
  if (s_nBp)
    a.bp = s_nBp, a.cdo = s_nCdo;
  if (!s_lib || !s_cls || !s_cdo || !s_dt || !s_bp || !s_row.ComparisonIndex)
    return Logf("commands: meteor -- library %p / projectile class %p / cdo %p / table %p / blueprint %p missing",
                      s_lib, s_cls, s_cdo, s_dt, s_bp), false;
  return true;
}

// The nuke row arms after m_armingTime 5 s and does m_unarmedDamage 10
// before that (YProjectileMissile_Offline_DT); a shot from overhead lands in
// ~1 s, so it hit unarmed: no blast, no damage (19:27 Gorge). Zeroes a
// per-instance arming time if the projectile has one; the first call logs
// every reflected field whose name looks related, to find it if not.
static bool SetBoolProp(void *obj, const char *name, bool v);
// Follows one shot per /meteor once a second until it is gone: the viewer
// showed every trail ending in mid-air (operator 10-17 20:07) with
// m_maxTravelDistance 900000 in the row, so something else ends them.
static struct { void *p; DWORD since, lastLog; float at[3]; uint8_t armed; } g_meteorTrack;
static void MeteorTrackTick() {
  if (!g_meteorTrack.p)
    return;
  DWORD now = GetTickCount(), age = now - g_meteorTrack.since;
  if (!ObjectLive(g_meteorTrack.p)) {
    Logf("commands: meteor -- tracked shot %p gone after %.1f s, last seen at %.0f %.0f %.0f (m_armed 0x%02x)",
         g_meteorTrack.p, age / 1000.f, g_meteorTrack.at[0], g_meteorTrack.at[1], g_meteorTrack.at[2],
         g_meteorTrack.armed);
    g_meteorTrack.p = nullptr;
    return;
  }
  ActorLocation(g_meteorTrack.p, g_meteorTrack.at);
  g_meteorTrack.armed = ((uint8_t *)g_meteorTrack.p)[0x6db];
  if (now - g_meteorTrack.lastLog >= 1000) {
    g_meteorTrack.lastLog = now;
    Logf("commands: meteor -- tracked shot %p t+%.1f s at %.0f %.0f %.0f, m_armed 0x%02x", g_meteorTrack.p,
         age / 1000.f, g_meteorTrack.at[0], g_meteorTrack.at[1], g_meteorTrack.at[2], g_meteorTrack.armed);
  }
  if (age > 120000)
    g_meteorTrack.p = nullptr;
}

static void MeteorArm(void *proj) {
  static bool s_probed = false;
  UObjectMin *o = (UObjectMin *)proj;
  if (!IsReadable(o, sizeof(UObjectMin)))
    return;
  if (!s_probed) {
    s_probed = true;
    static const char *needles[] = {"arm", "Arm", "ime", "ffline", "arget", "xplo", "eton", "mpact", "Damage"};
    for (UObjectMin *cls = o->Class; cls && IsReadable(cls, 0x40); cls = *(UObjectMin **)((uint8_t *)cls + OFF_USTRUCT_SUPER))
      for (UObjectMin *f = *(UObjectMin **)((uint8_t *)cls + 0x38); f && IsReadable(f, 0x58); f = *(UObjectMin **)((uint8_t *)f + 0x28)) {
        const char *fn = NameText(f->Name), *fc = ClassNameOf(f);
        bool hit = false;
        for (const char *nd : needles)
          hit |= fn && strstr(fn, nd) != nullptr;
        if (!hit)
          continue;
        uint32_t off = *(uint32_t *)((uint8_t *)f + 0x50);
        if (!strcmp(fc, "Function"))
          Logf("commands: meteor -- probe %s: fn %s", NameText(cls->Name), fn);
        else if (!strcmp(fc, "FloatProperty"))
          Logf("commands: meteor -- probe %s: %s %s +0x%x = %g", NameText(cls->Name), fc, fn, off, *(float *)((uint8_t *)o + off));
        else
          Logf("commands: meteor -- probe %s: %s %s +0x%x", NameText(cls->Name), fc, fn, off);
      }
  }
  // YProjectile::m_armed (BoolProperty +0x6db, probe 19:44 nuke2); the
  // projectile has no per-instance m_armingTime.
  // Read back on the next call: does the projectile's own tick clear it?
  static void *s_last = nullptr;
  static float s_lastAt[3];
  static int s_backLogged = 0;
  if (s_last && s_backLogged < 3 && ObjectLive(s_last)) {
    float at[3] = {};
    ActorLocation(s_last, at);
    Logf("commands: meteor -- previous shot %p: m_armed byte 0x%02x, at %.0f %.0f %.0f (spawned at %.0f %.0f %.0f)",
         s_last, ((uint8_t *)s_last)[0x6db], at[0], at[1], at[2], s_lastAt[0], s_lastAt[1], s_lastAt[2]);
    ++s_backLogged;
  }
  s_last = proj;
  ActorLocation(proj, s_lastAt);
  if (!g_meteorTrack.p) {
    g_meteorTrack.p = proj;
    g_meteorTrack.since = g_meteorTrack.lastLog = GetTickCount();
    memcpy(g_meteorTrack.at, s_lastAt, 12);
  }
  static int s_armLogged = 0;
  bool ok = SetBoolProp(o, "m_armed", true);
  if (!s_armLogged++)
    Logf("commands: meteor -- m_armed %s", ok ? "set on spawn" : "NOT SET (property/layout not found)");
}

// One torpedo at pos flying at tgt, firingWeapon `weapon`, registered with its
// projectile tracking. The caller lends the offline table to the CDO (+0x5D8)
// around the calls. 1 spawned, 2 spawned and registered, 0 failed, -1 faulted.
// Lock-on, as a player's nuke has it (operator 10-17: "for me it is locked
// on ... set it for whatever is targeted"): YProjectileMissile::SetTarget with
// the target ship, and m_targetLocation (+0x7b8 on WP_YProjectileWarpMissile_BP)
// set to where it is now. GUESS: SetTarget's object parameter that is not a
// socket/component is the target actor; its parameter list is logged once.
static void MeteorLockOn(void *proj, void *target) {
  static int s_logged = 0;
  float at[3];
  if (!target || !ObjectLive(target) || !ActorLocation(target, at))
    return;
  struct Ctx { void *t; bool log; } c = {target, s_logged == 0};
  alignas(16) uint8_t parms[256];
  bool called = CallNamed(proj, L"SetTarget", [](uint8_t *p, const char *pc, const char *pn, uint32_t off, void *ctx) {
    Ctx *k = (Ctx *)ctx;
    bool obj = !strcmp(pc, "ObjectProperty") && !StrIStr(pn, "ocket") && !StrIStr(pn, "omponent");
    if (obj)
      *(void **)(p + off) = k->t;
    if (k->log)
      Logf("commands: meteor -- SetTarget param %s %s +0x%x%s", pc, pn, off, obj ? " = target ship" : "");
  }, &c, parms);
  int lo = PropOffset((UObjectMin *)proj, "m_targetLocation");
  if (lo > 0)
    memcpy((uint8_t *)proj + lo, at, 12);
  if (!s_logged++) {
    void *got = ((tWeakGet)(g_base + RVA_WEAK_PTR_GET))((uint8_t *)proj + 0x7a8);
    Logf("commands: meteor -- lock-on: SetTarget %s, m_target now %p (want %p %s), m_targetLocation +0x%x", called ? "called" : "NOT called",
         got, target, ClassNameOf((UObjectMin *)target), lo);
  }
}

static int MeteorFire(const MeteorAssetsT &A, uint8_t *gm, void *weapon, const float pos[3], const float tgt[3],
                      int seed, void *target) {
  MeteorShot m = {};
  m.world = gm;
  m.cls = A.bp;
  m.weapon = weapon;
  m.row = A.row;
  memcpy(m.pos, pos, 12);
  float dx = tgt[0] - pos[0], dy = tgt[1] - pos[1], dz = tgt[2] - pos[2];
  float yaw = atan2f(dy, dx), pitch = atan2f(dz, sqrtf(dx * dx + dy * dy));
  float sp = sinf(pitch / 2), cp = cosf(pitch / 2), sy = sinf(yaw / 2), cy = cosf(yaw / 2);
  // FRotator(pitch, yaw, 0) -> FQuat, UE4 FRotator::Quaternion.
  m.quat[0] = sp * sy;
  m.quat[1] = -sp * cy;
  m.quat[2] = cp * sy;
  m.quat[3] = cp * cy;
  m.id = s_meteorId++;
  m.seed = seed;
  alignas(16) uint8_t parms[256];
  __try {
    if (!CallNamed(A.lib, L"SpawnProjectile", FillMeteor, &m, parms) || !*(void **)(parms + 0x70))
      return 0;
    void *proj = *(void **)(parms + 0x70);
    MeteorArm(proj);
    MeteorLockOn(proj, target);
    if (g_meteorDamage > 0) {
      static int s_dmgLogged = 0;
      int off = PropOffset((UObjectMin *)proj, "m_abilityDamage");
      if (off > 0) {
        if (!s_dmgLogged++)
          Logf("commands: meteor -- m_abilityDamage +0x%x %g -> %g", off, *(float *)((uint8_t *)proj + off), g_meteorDamage);
        *(float *)((uint8_t *)proj + off) = g_meteorDamage;
      }
    }
    if (g_meteorLifeSpan > 0) {
      static int s_lsLogged = 0;
      float ls = g_meteorLifeSpan;
      bool ok = CallNamed(proj, L"SetLifeSpan", [](uint8_t *p, const char *pc, const char *, uint32_t off, void *ctx) {
        if (!strcmp(pc, "FloatProperty"))
          *(float *)(p + off) = *(float *)ctx;
      }, &ls);
      if (!s_lsLogged++)
        Logf("commands: meteor -- SetLifeSpan(%g) %s", ls, ok ? "called" : "NOT called");
    }
    // Without this the missile exists only on the host (see
    // RegisterMeteorProjectile above).
    return RegisterMeteorProjectile(weapon, proj) ? 2 : 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("commands: meteor -- SpawnProjectile faulted");
    return -1;
  }
}

static void CommandMeteor(uint8_t *gm, int count) {
  MeteorAssetsT A;
  if (!MeteorAssets(A))
    return;
  void *weapon = MeteorWeapon(gm);
  if (!weapon || !IsReadable((uint8_t *)weapon + 0x198, 8) || !*(void **)((uint8_t *)weapon + 0x198))
    return (void)Logf("commands: meteor -- no firing weapon actor (%p)", weapon);
  void *savedDt = *(void **)((uint8_t *)A.cdo + 0x5D8);
  *(void **)((uint8_t *)A.cdo + 0x5D8) = A.dt;
  uint8_t *pcs[64];
  int n = CollectHumanPCs(gm, pcs, 64), shot = 0, ok = 0, registered = 0;
  for (int i = 0; i < n; ++i) {
    void *pawn = IsReadable(pcs[i] + OFF_CONTROLLER_PAWN, 8) ? *(void **)(pcs[i] + OFF_CONTROLLER_PAWN) : nullptr;
    float at[3];
    if (!pawn || !ActorLocation(pawn, at))
      continue;
    for (int k = 0; k < count; ++k) {
      // GUESS distances (uu): impacts within 6000 of the ship, launched 9000
      // above. Picked to land around a dreadnought, not measured.
      float tgt[3] = {at[0] + (float)(rand() % 12001 - 6000), at[1] + (float)(rand() % 12001 - 6000),
                      at[2] + (float)(rand() % 2001 - 1000)};
      float pos[3] = {tgt[0], tgt[1], tgt[2] + 9000.f}; // straight down, as the nuke falls
      ++shot;
      int r = MeteorFire(A, gm, weapon, pos, tgt, rand(), pawn);
      if (r < 0)
        break;
      ok += r > 0, registered += r == 2;
    }
  }
  *(void **)((uint8_t *)A.cdo + 0x5D8) = savedDt;
  Logf("commands: meteor -- %d of %d projectiles spawned around %d players", ok, shot, n);
  Logf("commands: meteor -- registered %d of %d with projectile tracking", registered, shot);
}

// /meteor as a visible event (operator 2026-10-16: "custom NPC spawned in the
// sky with the weapon to spawn from? it *must* be visual"; 10-17: cloaked, in
// the middle of the map, one per team, every ship targeted, and one meteor
// should hurt both sides of a duel). Two Onslaught assault ships
// (VH_ONS_AITargetM_Pawn_BP), one on each team, spawned SKY_SHIP_UP above the
// map centre (MapCentre: the largest LevelBoundaries volume BoundsProbe read)
// and cloaked (CloakPawn). Each CommandsTick fires one volley: for every live
// ship but the heralds, perVolley strike points around it, and at each point
// one Heavy Nuclear Torpedo per herald weapon from the same spot along the
// same line -- drawn as one meteor. Team filtering is the weapon's
// (DeepSeek 10-17: IsFriendlyTeam / IsTargetFriendly in the weapon layer,
// TakeDamage itself has no team test), so the team-1 herald's copy can hurt
// team 2 and the team-2 copy team 1: whoever is under it takes the hit,
// either side. GUESS: that a projectile only hits the firing weapon's enemies
// is inferred from the original /meteor note (an enemy weapon "makes the
// torpedoes hurt players"), not tested for both copies at once.
// Launched SKY_SHIP_UP above the strike point, not from the herald: the nuke
// travels up into warp and comes down on its target (operator 10-17), and
// DeepSeek found no warp-in step in the projectile data, so the torpedo is
// spawned as the descending half. Module cooldown is not involved: the
// torpedoes are spawned directly, the ability never runs.
// Parking: not registered with the scene manager and no route, the state in
// which Onslaught assault ships "sat still" (host 2026-10-08 00:51, see
// SpawnOnslaughtAIShip). The heralds stay afterwards, cloaked targets.
// Launch height above the strike point. The nuke arms 5 s after spawning
// (m_armingTime), and from 9000 up it landed unarmed in ~1 s (19:27, 19:53:
// no damage). Operator 10-17: "spawn it from further up, nukes can fall for
// dozens of km". /meteor N [height] overrides it per command. From 200000 up
// (nuke5 20:04) the warp torpedo fell ~1450 uu/s (205618 -> 204172 in the
// first second): ~140 s to land, and no hit or sight of one in the run.
#define SKY_SHIP_UP 12000.f // GUESS: ~8 s at that speed if it does not accelerate; armed at 5 s
static float g_skyUp = SKY_SHIP_UP;
// Heralds: medium AI dreadnoughts (VH_DreadM_AI_Pawn_T3_BP, the nuke-carrying
// class -- operator 10-17: "dreadnoughts have the nukes, can just be left to
// be spawned far high above"), parked overhead instead of cloaked: the ONS
// hull had no UYCloakingComponent (skymeteor5 17:59). GUESS: 8000 uu over the
// highest ship stays inside net relevancy (30000 never showed on the client, 18:30); a herald killed by
// a boundary just ends the volleys ("heralds gone").
#define HERALD_UP 8000.f
#define SKY_VOLLEYS 8
#define SKY_STRIKES_MAX 24 // GUESS: points per volley (x2 torpedoes), a cap on spawns per tick
struct SkyShip { void *pawn[2], *weapon[2]; int volleys, perVolley; ULONGLONG nextScan, giveUpAt; };
static SkyShip g_sky = {};

static bool MapCentre(float out[3]);

// Heavy cloak on a ship. There is no "apply status effect" native (DeepSeek
// 10-17: no AddStatusEffect/ApplyGameplayEffect among 6,892 natives); cloak is
// a state machine entered through the ActivateCloaking UFunction (native
// 0x73CE50 -> 0x3B5680). The function's owning class is read off the UFunction
// object's Outer, then the pawn or its component of that class is called.
// GUESS: zeroed parameters -- the leaf's first int lands at this+0x120 and its
// meaning (cloak kind? the CloakH modifier id?) is not traced; the log shows
// the parameter list the first few calls.
static void NoFill(uint8_t *, const char *, uint32_t, void *) {}
static bool CloakPawn(void *pawn) {
  static UObjectMin *s_fn = nullptr;
  if (!s_fn)
    s_fn = FindObjectNamedOfClass("ActivateCloaking", "Function");
  UObjectMin *owner = s_fn && IsReadable(s_fn->Outer, sizeof(UObjectMin)) ? s_fn->Outer : nullptr;
  if (!owner)
    return Logf("commands: meteor -- no ActivateCloaking UFunction"), false;
  void *target = ClassIsA(((UObjectMin *)pawn)->Class, owner) ? pawn : nullptr;
  FUObjectArrayMin *arr = GObjects();
  for (int i = 0; !target && arr && arr->Objects && i < arr->NumElements; ++i) {
    UObjectMin *o = arr->Objects[i].Object;
    if (o && o->Outer == pawn && IsReadable(o, sizeof(UObjectMin)) && ClassIsA(o->Class, owner))
      target = o;
  }
  if (!target) {
    // VH_ONS_AITargetM_Pawn_BP has none (skymeteor5 17:59). Name the ship
    // classes that do, once, to pick a herald hull that can cloak.
    static bool s_listed = false;
    for (int i = 0, k = 0; !s_listed && arr && arr->Objects && i < arr->NumElements && k < 6; ++i) {
      UObjectMin *o = arr->Objects[i].Object;
      if (o && IsReadable(o, sizeof(UObjectMin)) && IsReadable(o->Outer, sizeof(UObjectMin)) &&
          ClassIsA(o->Class, owner) && strncmp(NameText(o->Name) ? NameText(o->Name) : "", "Default__", 9))
        ++k, Logf("commands: meteor --   cloak component %s on %s", NameText(o->Name), ClassNameOf(o->Outer));
    }
    s_listed = true;
    return Logf("commands: meteor -- herald %p has nothing of class %s to cloak", pawn, NameText(owner->Name)), false;
  }
  // DeepSeek 10-17: the owner is UYCloakingComponent; leaf 0x3B5680 enters
  // the cloaked state (+0x108 = 1) only when the weak pointer at +0x100 is
  // valid, and +0x100 is not among the four parameters (int, int, object ->
  // +0xF8 weak, entry list -> +0x110). So a zeroed call can return cleanly
  // and cloak nothing; the before/after bytes say which happened.
  // GUESS: +0x100 is set by the ability path (AbilityActivated 0x73C760).
  uint8_t *cc = (uint8_t *)target;
  bool probe = target != pawn && IsReadable(cc + 0x100, 0x10);
  uint64_t w0 = probe ? *(uint64_t *)(cc + 0x100) : 0;
  int st0 = probe ? cc[0x108] : -1;
  bool ok = false;
  __try {
    ok = CallFilled(target, L"ActivateCloaking", NoFill, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("commands: meteor -- ActivateCloaking faulted");
  }
  Logf("commands: meteor -- cloak %s on %s (%s); +0x100 weak %016llx, state +0x108 %d -> %d", ok ? "called" : "failed",
       ClassNameOf((UObjectMin *)target), NameText(owner->Name), (unsigned long long)w0, st0,
       probe ? cc[0x108] : -1);
  return ok;
}

// Sets reflected bool `name` on obj (bitfield or native), with the
// UBoolProperty {FieldSize, ByteOffset, ByteMask, FieldMask} scan
// HavocSetReadyFlag uses. False when the property or its layout is not found.
static bool SetBoolProp(void *obj, const char *name, bool v) {
  UObjectMin *bp = FindPropObj((UObjectMin *)obj, name);
  if (!bp || strcmp(ClassNameOf(bp), "BoolProperty"))
    return false;
  const uint8_t *q = (const uint8_t *)bp;
  if (!IsReadable(q + 0x60, 0x34))
    return false;
  for (int o = 0x60; o < 0x90; ++o)
    if (q[o] == 1 && q[o + 1] <= 7 && q[o + 2] && (q[o + 3] == q[o + 2] || q[o + 3] == 0xFF)) {
      uint8_t *b = (uint8_t *)obj + *(uint32_t *)(q + 0x50) + q[o + 1];
      *b = v ? (uint8_t)(*b | q[o + 2]) : (uint8_t)(*b & ~q[o + 2]);
      return true;
    }
  return false;
}

// Heralds sit high over the map centre and were never sent to a player far
// across Gorge (19:27: no herald, no shot seen). GUESS: always-relevant plus
// a huge cull distance is what puts them (and their weapon's projectile
// stream) on every client.
static void HeraldAlwaysRelevant(void *actor) {
  bool rel = SetBoolProp(actor, "bAlwaysRelevant", true);
  int cull = PropOffset((UObjectMin *)actor, "NetCullDistanceSquared");
  if (cull > 0)
    *(float *)((uint8_t *)actor + cull) = 1e12f; // GUESS: (1e6 uu)^2, far past any map
  Logf("commands: meteor -- %s: bAlwaysRelevant %s, NetCullDistanceSquared at %d", ClassNameOf((UObjectMin *)actor),
       rel ? "set" : "NOT FOUND", cull);
}

// A real AB_DN_Pri_WarpM_Dmg_Weapon_T5_BP actor for `herald`: spawned at the
// herald, its owning ship (+0x550, which SpawnProjectile and the projectile
// registration read) set to the herald. GUESS: that the weapon needs nothing
// else of the loadout path to fire SpawnProjectile shots.
// The herald's own weapons (actors whose owning ship +0x550 is the herald)
// and its weapon manager's group count. Clients draw a ship's shots from its
// weapon groups firing (YWeaponManagerComponent m_weaponGroupFireInfos +0xF8,
// m_repTargetInfo +0x160, both replicated); the projectile stream
// (ClientReceiveProjectilesInRange) carries only {id, distance} pairs
// (0x511B80), so a SpawnProjectile shot is never drawn on a client.
static void LogHeraldWeapons(void *herald) {
  FUObjectArrayMin *arr = GObjects();
  uint8_t *wm = *(uint8_t **)((uint8_t *)herald + 0x868);
  int groups = wm && IsReadable(wm + 0x1b8, 4) ? *(int32_t *)(wm + 0x1b8) : -1, n = 0;
  __try {
    for (int i = 0; arr && arr->Objects && i < arr->NumElements; ++i) {
      UObjectMin *o = arr->Objects[i].Object;
      if (!o || (o->Flags & RF_CDO_OR_ARCHETYPE) || !IsReadable(o, OFF_WEAPON_OWNER_PAWN + 8) ||
          *(void **)((uint8_t *)o + OFF_WEAPON_OWNER_PAWN) != herald || !ClassChainContains(o, "Weapon"))
        continue;
      Logf("commands: meteor -- herald %p own weapon %s", herald, ClassNameOf(o));
      ++n;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  Logf("commands: meteor -- herald %p: %d weapon actors, weapon manager %p with %d weapon groups", herald, n, wm, groups);
}

static void *HeraldWeapon(uint8_t *gm, void *herald) {
  float loc[3], rot[3] = {0, 0, 0};
  LogHeraldWeapons(herald);
  if (!g_nukeWeaponCls || !ActorLocation(herald, loc))
    return Logf("commands: meteor -- no nuke weapon class (%p) or herald location", g_nukeWeaponCls), nullptr;
  void *world = ((tGetWorld)(*(void ***)gm)[VT_ACTOR_GET_WORLD / 8])(gm);
  void *w = nullptr;
  __try {
    alignas(16) uint8_t params[0x80] = {};
    ((tSpawnParamsCtor)(g_base + RVA_SPAWN_PARAMS_CTOR))(params);
    params[0x28] = 1; // AlwaysSpawn, as SpawnHerald
    w = world ? ((tSpawnActor)(g_base + RVA_WORLD_SPAWN_ACTOR))(world, g_nukeWeaponCls, loc, rot, params) : nullptr;
    if (!w)
      return Logf("commands: meteor -- nuke weapon spawn failed"), nullptr;
    *(void **)((uint8_t *)w + OFF_WEAPON_OWNER_PAWN) = herald;
    int ow = PropOffset((UObjectMin *)w, "Owner"); // AActor::Owner, for relevancy
    if (ow > 0)
      *(void **)((uint8_t *)w + ow) = herald;
    HeraldAlwaysRelevant(w);
    ((tActorVoidFn)(*(void ***)w)[0x580 / 8])(w); // ForceNetUpdate
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return Logf("commands: meteor -- nuke weapon setup faulted (%p)", w), nullptr;
  }
  void *root = *(void **)((uint8_t *)w + 0x198), *wm = *(void **)((uint8_t *)herald + 0x868);
  Logf("commands: meteor -- nuke weapon %p (%s) on herald %p: root %p, herald weapon manager %p", w,
       ClassNameOf((UObjectMin *)w), herald, root, wm);
  if (!root || !wm) // SpawnProjectile reads both; null faults
    return nullptr;
  return w;
}

static void *SpawnHerald(uint8_t *gm, UObjectMin *cls, const float at[3], uint8_t team) {
  float rot[3] = {0, 0, 0};
  void *world = ((tGetWorld)(*(void ***)gm)[VT_ACTOR_GET_WORLD / 8])(gm);
  void *pawn = nullptr, *ctrl = nullptr;
  __try {
    alignas(16) uint8_t params[0x80] = {};
    ((tSpawnParamsCtor)(g_base + RVA_SPAWN_PARAMS_CTOR))(params);
    // FActorSpawnParameters +0x28 SpawnCollisionHandlingOverride = AlwaysSpawn
    // (GUESS: UE 4.13 layout, as GiveNpcPlayerState sets it). A second
    // /meteor on the same spot failed without it (headless 17:42).
    params[0x28] = 1;
    float p[3] = {at[0], at[1], at[2]};
    pawn = world ? ((tSpawnActor)(g_base + RVA_WORLD_SPAWN_ACTOR))(world, cls, p, rot, params) : nullptr;
    if (!pawn)
      return Logf("commands: meteor -- herald spawn failed (world %p)", world), nullptr;
    ((tPawnSetTeam)(g_base + RVA_PAWN_SET_TEAM))(pawn, &team);
    ctrl = *(void **)((uint8_t *)pawn + OFF_PAWN_CONTROLLER);
    if (!ctrl) {
      void *ctrlCls = *(void **)((uint8_t *)pawn + OFF_PAWN_AI_CONTROLLER_CLASS);
      if (ctrlCls) {
        alignas(16) uint8_t cparams[0x80] = {};
        ((tSpawnParamsCtor)(g_base + RVA_SPAWN_PARAMS_CTOR))(cparams);
        ctrl = ((tSpawnActor)(g_base + RVA_WORLD_SPAWN_ACTOR))(world, ctrlCls, p, rot, cparams);
      }
      if (ctrl) {
        void **cvt = *(void ***)ctrl;
        ((tPossess)cvt[VT_CONTROLLER_POSSESS / 8])(ctrl, pawn);
        ((tPossess)cvt[VT_CONTROLLER_POSSESS2 / 8])(ctrl, pawn);
      }
    }
    if (!SwitchOn("DN_HOST_NO_NPC_NAMES", "dn_host_no_npc_names.txt"))
      GiveNpcPlayerState(world, pawn, ctrl, L"Comet");
    ((tActorVoidFn)(*(void ***)pawn)[0x580 / 8])(pawn); // ForceNetUpdate
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return Logf("commands: meteor -- herald setup faulted (pawn %p)", pawn), nullptr;
  }
  CloakPawn(pawn);
  HeraldAlwaysRelevant(pawn);
  Logf("commands: meteor -- herald %p team %d at %.0f %.0f %.0f, controller %s", pawn, team, at[0], at[1], at[2],
       ctrl ? ClassNameOf((UObjectMin *)ctrl) : "none");
  return pawn;
}

// Live ship pawns (players and bots) other than the heralds, from the same
// slots and liveness checks as the viewer snapshot.
static int SkyTargets(void **out, float (*at)[3], int max) {
  int n = 0;
  for (FeatsByPawn &e : g_featsByPawn) {
    void *pawn = e.pawn;
    if (n >= max)
      break;
    if (!pawn || pawn == g_sky.pawn[0] || pawn == g_sky.pawn[1] || !ObjectLive(pawn) || !ObjectLive(e.comp) ||
        FeatsCompOf(pawn) != e.comp || !ClassChainIs((UObjectMin *)pawn, "Pawn") || !ActorLocation(pawn, at[n]))
      continue;
    out[n++] = pawn;
  }
  // Humans first, the rest shuffled each volley: the per-volley cap went to
  // the first 5 of 29 ships in g_featsByPawn order (18:30 test, none on the player).
  int h = 0;
  for (int i = 0; i < n; ++i)
    if (!PawnIsNpc(out[i])) {
      std::swap(out[i], out[h]);
      std::swap(at[i][0], at[h][0]), std::swap(at[i][1], at[h][1]), std::swap(at[i][2], at[h][2]);
      ++h;
    }
  for (int i = n - 1; i > h; --i) {
    int j = h + rand() % (i - h + 1);
    std::swap(out[i], out[j]);
    std::swap(at[i][0], at[j][0]), std::swap(at[i][1], at[j][1]), std::swap(at[i][2], at[j][2]);
  }
  return n;
}


static void CommandSkyMeteor(uint8_t *gm, int count, float up) {
  g_skyUp = up > 0 ? up : SKY_SHIP_UP;
  MeteorAssetsT A;
  if (!MeteorAssets(A)) // also loads g_nukeWeaponCls for HeraldWeapon
    return;
  static UObjectMin *s_cls = nullptr;
  if (!s_cls) {
    UObjectMin *uc = ResolveUClassClass();
    __try {
      s_cls = uc ? (UObjectMin *)((tStaticLoadClass)(g_base + RVA_STATIC_LOAD_CLASS))(
                       uc, nullptr, L"/Game/Generic/AI/AIPawns/VH_DreadM_AI_Pawn_T3_BP.VH_DreadM_AI_Pawn_T3_BP_C",
                       nullptr, 0, nullptr, false)
                 : nullptr;
      if (uc && !IsReadable(s_cls, sizeof(UObjectMin))) // the first herald hull
        s_cls = (UObjectMin *)((tStaticLoadClass)(g_base + RVA_STATIC_LOAD_CLASS))(
            uc, nullptr, L"/Game/Generic/AI/Onslaught/Targets/VH_ONS_AITargetM_Pawn_BP.VH_ONS_AITargetM_Pawn_BP_C",
            nullptr, 0, nullptr, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      s_cls = nullptr;
    }
    if (IsReadable(s_cls, sizeof(UObjectMin)))
      PinToRootSet(s_cls), Logf("commands: meteor -- herald hull %s", NameText(s_cls->Name));
    else
      s_cls = nullptr;
  }
  void *pawns[64];
  float pts[64][3];
  int n = SkyTargets(pawns, pts, 64);
  if (!s_cls || !n) {
    Logf("commands: meteor -- herald class %p / %d target ships, plain shower instead", s_cls, n);
    return CommandMeteor(gm, count);
  }
  float c[3] = {0, 0, -1e30f}, top = -1e30f;
  for (int i = 0; i < n; ++i)
    top = std::max(top, pts[i][2]);
  if (!MapCentre(c)) { // no boundary volume: the ships' mean
    c[0] = c[1] = 0;
    for (int i = 0; i < n; ++i)
      c[0] += pts[i][0] / n, c[1] += pts[i][1] / n;
  }
  c[2] = top + HERALD_UP;
  // Heralds already up stay; a repeat /meteor just queues more volleys.
  for (int t = 0; t < 2; ++t)
    if (!g_sky.pawn[t] || !ObjectLive(g_sky.pawn[t])) {
      // GUESS: 3000 uu apart, so the two do not sit inside each other.
      float at[3] = {c[0] + (t ? 1500.f : -1500.f), c[1], c[2]};
      g_sky.pawn[t] = SpawnHerald(gm, s_cls, at, (uint8_t)(t + 1));
      g_sky.weapon[t] = g_sky.pawn[t] ? HeraldWeapon(gm, g_sky.pawn[t]) : nullptr;
    }
  g_sky.volleys = SKY_VOLLEYS;
  g_sky.nextScan = g_sky.giveUpAt = 0;
  g_sky.perVolley = std::max(1, count / SKY_VOLLEYS);
  Logf("commands: meteor -- heralds %p %p over %.0f %.0f %.0f; %d volleys of %d per ship, %d ships, launched %.0f up",
       g_sky.pawn[0], g_sky.pawn[1], c[0], c[1], c[2], g_sky.volleys, g_sky.perVolley, n, g_skyUp);
}

// One volley per CommandsTick (~1 s).
static void SkyMeteorTick(uint8_t *gm) {
  MeteorTrackTick();
  if (g_sky.volleys <= 0)
    return;
  void *weapons[2];
  int nw = 0;
  for (int t = 0; t < 2; ++t) {
    if (!g_sky.pawn[t] || !ObjectLive(g_sky.pawn[t])) {
      g_sky.pawn[t] = g_sky.weapon[t] = nullptr;
      continue;
    }
    if (g_sky.weapon[t] && !ObjectLive(g_sky.weapon[t]))
      g_sky.weapon[t] = nullptr;
  }
  if (!g_sky.pawn[0] && !g_sky.pawn[1]) {
    Logf("commands: meteor -- heralds gone, %d volleys unfired", g_sky.volleys);
    g_sky.volleys = 0;
    return;
  }
  // Each herald fires from its own nuke weapon (operator 10-17: "you
  // shouldn't rely on existing NPC weapons, just spawn it in with the proper
  // one"); borrowed weapons gave shots no client ever saw.
  ULONGLONG nowMs = GetTickCount64();
  for (int t = 0; t < 2; ++t)
    if (g_sky.pawn[t] && !g_sky.weapon[t] && nowMs >= g_sky.nextScan)
      g_sky.weapon[t] = HeraldWeapon(gm, g_sky.pawn[t]);
  for (int t = 0; t < 2; ++t)
    if (g_sky.weapon[t])
      weapons[nw++] = g_sky.weapon[t];
  int own = nw;
  if (!own) {
    g_sky.nextScan = nowMs + 3000; // spawn retry throttle
    if (!g_sky.giveUpAt)
      g_sky.giveUpAt = nowMs + 20000;
    if (nowMs >= g_sky.giveUpAt) {
      Logf("commands: meteor -- no herald weapon in 20 s, %d volleys unfired", g_sky.volleys);
      g_sky.volleys = 0;
    }
    return;
  }
  --g_sky.volleys;
  MeteorAssetsT A;
  if (!MeteorAssets(A))
    return;
  void *pawns[64];
  float pts[64][3];
  int n = SkyTargets(pawns, pts, 64), points = 0, shot = 0, ok = 0, registered = 0;
  void *savedDt = *(void **)((uint8_t *)A.cdo + 0x5D8);
  *(void **)((uint8_t *)A.cdo + 0x5D8) = A.dt;
  int per = n ? std::max(1, std::min(g_sky.perVolley, SKY_STRIKES_MAX / n)) : 0;
  for (int i = 0; i < n && points < SKY_STRIKES_MAX; ++i)
    for (int k = 0; k < per && points < SKY_STRIKES_MAX; ++k, ++points) {
      // GUESS distances (uu), as the plain shower: within 3000 of the ship
      // so most volleys land on or beside it.
      float tgt[3] = {pts[i][0] + (float)(rand() % 6001 - 3000), pts[i][1] + (float)(rand() % 6001 - 3000),
                      pts[i][2] + (float)(rand() % 1001 - 500)};
      // Directly above: the nuke falls straight down (operator 10-17).
      float pos[3] = {tgt[0], tgt[1], tgt[2] + g_skyUp};
      int seed = rand();
      for (int w = 0; w < own; ++w) {
        ++shot;
        int r = MeteorFire(A, gm, weapons[w], pos, tgt, seed, pawns[i]);
        if (r < 0)
          goto done;
        ok += r > 0, registered += r == 2;
      }
    }
done:
  *(void **)((uint8_t *)A.cdo + 0x5D8) = savedDt;
  Logf("commands: meteor -- volley: %d points over %d ships, %d of %d torpedoes (%d registered), %d herald weapons, %d volleys left",
       points, n, ok, shot, registered, own, g_sky.volleys);
}

struct DamageFill { void *actor; float damage; int objs, floats; void *instigator, *causer; };
static void FillApplyDamage(uint8_t *parms, const char *cls, uint32_t off, void *ctx) {
  DamageFill *d = (DamageFill *)ctx;
  if (!strcmp(cls, "ObjectProperty")) {
    void *v = d->objs == 0 ? d->actor        // DamagedActor
            : d->objs == 1 ? d->instigator   // EventInstigator (AController)
            : d->objs == 2 ? d->causer       // DamageCauser (AActor)
                           : nullptr;
    if (d->objs++ < 3)
      *(void **)(parms + off) = v;
  } else if (!strcmp(cls, "FloatProperty") && d->floats++ == 0)
    *(float *)(parms + off) = d->damage; // BaseDamage
}

// instigator/causer: credit the kill to that controller and its ship (nuke
// kill XP). UGameplayStatics::ApplyDamage(DamagedActor, BaseDamage,
// EventInstigator, DamageCauser, DamageTypeClass) is the engine signature;
// whether the game's kill XP follows EventInstigator here is GUESS until a
// nuke run shows XP.
static bool KillPawn(void *pawn, void *instigator = nullptr, void *causer = nullptr) {
  static UObjectMin *s_statics = nullptr;
  if (!s_statics)
    s_statics = FindObjectNamed("Default__GameplayStatics");
  if (!s_statics)
    return Logf("commands: Default__GameplayStatics not found"), false;
  DamageFill d = {pawn, 1.0e7f, 0, 0, instigator, causer};
  __try {
    return CallFilled(s_statics, L"ApplyDamage", FillApplyDamage, &d);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("commands: ApplyDamage faulted");
    return false;
  }
}

static void CommandKill(uint8_t *gm, const char *who) {
  uint8_t *pcs[64];
  int n = CollectHumanPCs(gm, pcs, 64), hit = 0;
  for (int i = 0; i < n; ++i) {
    char pid[80] = "";
    UrlOptionForController(pcs[i], L"DNPID=", pid, sizeof(pid));
    if (_stricmp(who, "all") && _stricmp(who, pid))
      continue;
    void *pawn = IsReadable(pcs[i] + OFF_CONTROLLER_PAWN, 8) ? *(void **)(pcs[i] + OFF_CONTROLLER_PAWN) : nullptr;
    if (pawn && KillPawn(pawn)) // no pawn: dead or not spawned yet
      ++hit;
  }
  Logf("commands: kill %s -> %d players", who, hit);
}

static UObjectMin *FindPropObj(UObjectMin *obj, const char *name);
static void CommandSlowmo(uint8_t *gm, float k) {
  __try {
    void *world = ((tGetWorld)(*(void ***)gm)[VT_ACTOR_GET_WORLD / 8])(gm);
    UObjectMin *pl = world ? FindPropObj((UObjectMin *)world, "PersistentLevel") : nullptr;
    uint8_t *level = pl ? *(uint8_t **)((uint8_t *)world + *(uint32_t *)((uint8_t *)pl + 0x50)) : nullptr;
    UObjectMin *wsp = level && IsReadable(level, sizeof(UObjectMin)) ? FindPropObj((UObjectMin *)level, "WorldSettings") : nullptr;
    uint8_t *ws = wsp ? *(uint8_t **)(level + *(uint32_t *)((uint8_t *)wsp + 0x50)) : nullptr;
    UObjectMin *td = ws && IsReadable(ws, sizeof(UObjectMin)) ? FindPropObj((UObjectMin *)ws, "TimeDilation") : nullptr;
    if (!td || strcmp(ClassNameOf(td), "FloatProperty"))
      return (void)Logf("commands: slowmo: TimeDilation not found (level %p ws %p)", level, ws);
    uint32_t off = *(uint32_t *)((uint8_t *)td + 0x50);
    float *v = (float *)(ws + off);
    Logf("commands: slowmo %s +0x%x %.2f -> %.2f", ClassNameOf((UObjectMin *)ws), off, *v, k);
    *v = k;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("commands: slowmo faulted");
  }
}

// /wave skip destroys every live wave enemy outright (no death, no XP);
// /nuke (CommandNuke below) applies lethal damage instead, so each dies its
// normal death (explosion, kill credit) -- GUESS as for KillPawn above.
static void CommandWaveSkip() {
  uint8_t *scene = (uint8_t *)((tGetSceneManager)(g_base + RVA_COMBAT_SCENE_MANAGER))();
  if (!scene || !IsReadable(scene + OFF_SCENE_NPCS, 16)) {
    Logf("commands: wave skip -- no combat scene manager");
    return;
  }
  uint8_t *arr = *(uint8_t **)(scene + OFF_SCENE_NPCS);
  int cnt = *(int32_t *)(scene + OFF_SCENE_NPCS_COUNT), destroyed = 0;
  for (int i = 0; i < (int)g_hvcEnemyCount && i < 256; ++i) {
    int id = (int)g_hvcEnemyIds[i];
    if (!arr || id < 0 || id >= cnt || !IsReadable(arr + (size_t)id * NPC_SPAWN_SIZE, NPC_SPAWN_SIZE))
      continue;
    uint8_t *e = arr + (size_t)id * NPC_SPAWN_SIZE;
    if (!e[OFF_NPCSPAWN_INSTANCED])
      continue;
    void *pawn = ((tWeakGet)(g_base + RVA_WEAK_PTR_GET))(e + OFF_NPCSPAWN_PAWN);
    if (pawn && CallFilled(pawn, L"K2_DestroyActor", nullptr, nullptr))
      ++destroyed;
  }
  Logf("commands: wave skip destroyed %d enemies", destroyed);
}

// /nuke in any mode: lethal damage (KillPawn) to every NPC ship not on a human
// player's team. Pawns are found by class: the human pawn's class chain gives
// the ship base (deepest-up ancestor whose name contains "ShipPawn", the class
// OFF_SHIP_TEAM belongs to; Onslaught creeps read their team at the same
// offset, CountOnslaughtCreeps), then one GObjects pass compares class
// pointers only (no per-object name lookups, README: a 90 s freeze). Command
// ships (AITargetH, the Onslaught objective) are spared so /nuke never ends a
// match. The 2026-10-16 22:36 Onslaught test hit 0 with the wave-only version.
// GUESS: an NPC ship's controller is never a human PC (checked anyway).
// The ship base class from one pawn's class chain: the most generic ancestor
// whose name contains "ShipPawn".
static void *ShipBaseClassOf(void *pawn) {
  void *found = nullptr;
  if (!pawn || !IsReadable(pawn, sizeof(UObjectMin)))
    return nullptr;
  for (UObjectMin *c = ((UObjectMin *)pawn)->Class; c && IsReadable(c, OFF_USTRUCT_SUPER + 8);
       c = *(UObjectMin **)((uint8_t *)c + OFF_USTRUCT_SUPER)) {
    // Every ship -- human, Havoc wave NPC, Onslaught AITarget{L,M,H} (YCreepPawn
    // or plain) -- descends from YPawn (AYPawn). The first version matched
    // "*ShipPawn*", which no class in the chain is named: Onslaught headless
    // 10-16 22:53 logged "ship class unknown" for every /nuke.
    const char *cn = NameText(c->Name);
    if (cn && !strcmp(cn, "YPawn"))
      found = c;
  }
  return found;
}

static void CommandNuke(uint8_t *gm) {
  static void *s_shipCls = nullptr;
  uint8_t *pcs[64];
  int n = CollectHumanPCs(gm, pcs, 64);
  void *humanPawns[64];
  int humanTeams = 0; // bit 1 = team 1, bit 2 = team 2
  for (int i = 0; i < n; ++i) {
    humanPawns[i] = IsReadable(pcs[i] + OFF_CONTROLLER_PAWN, 8) ? *(void **)(pcs[i] + OFF_CONTROLLER_PAWN) : nullptr;
    uint8_t *hp = (uint8_t *)humanPawns[i];
    if (!hp || !IsReadable(hp, OFF_SHIP_TEAM + 1))
      continue;
    if (!s_shipCls)
      s_shipCls = ShipBaseClassOf(hp);
    int t = hp[OFF_SHIP_TEAM];
    if (t == 1 || t == 2)
      humanTeams |= 1 << t;
  }
  // The team human players are on; with no live human ship (all dead) the PRI
  // team is not read here, so fall back to Havoc's player team 1. GUESS.
  if (!humanTeams)
    humanTeams = 1 << 1;
  // No live human ship (dead, or still in the orbit intro, headless 10-16
  // 22:54): take the class from an NPC ship -- Onslaught's own, else any live
  // pawn in the combat scene manager's NPC table (Havoc waves, bots).
  if (!s_shipCls)
    for (int k = 0; k < 2 && !s_shipCls; ++k)
      s_shipCls = ShipBaseClassOf(g_onsBigShip[k] ? g_onsBigShip[k] : g_onsMidShip[k]);
  if (!s_shipCls) {
    uint8_t *scene = (uint8_t *)((tGetSceneManager)(g_base + RVA_COMBAT_SCENE_MANAGER))();
    if (scene && IsReadable(scene + OFF_SCENE_NPCS, 16)) {
      uint8_t *a = *(uint8_t **)(scene + OFF_SCENE_NPCS);
      int cnt = *(int32_t *)(scene + OFF_SCENE_NPCS_COUNT);
      for (int i = 0; a && i < cnt && i < 512 && !s_shipCls; ++i) {
        uint8_t *e = a + (size_t)i * NPC_SPAWN_SIZE;
        if (IsReadable(e, NPC_SPAWN_SIZE) && e[OFF_NPCSPAWN_INSTANCED])
          s_shipCls = ShipBaseClassOf(((tWeakGet)(g_base + RVA_WEAK_PTR_GET))(e + OFF_NPCSPAWN_PAWN));
      }
    }
  }
  if (!s_shipCls)
    return (void)Logf("commands: nuke -- ship class unknown (no live ship found)");
  {
    static bool s_logged = false;
    const char *cn = NameText(((UObjectMin *)s_shipCls)->Name);
    if (!s_logged && (s_logged = true))
      Logf("commands: nuke -- ship base class %s", cn ? cn : "?");
  }
  void *victims[256];
  int nv = 0;
  FUObjectArrayMin *arr = GObjects();
  if (!arr || !arr->Objects)
    return;
  __try {
    for (int i = 0; i < arr->NumElements && nv < 256; ++i) {
      UObjectMin *o = arr->Objects[i].Object;
      if (!o || (o->Flags & RF_CDO_OR_ARCHETYPE) || (arr->Objects[i].Flags & OBJ_PENDING_KILL) ||
          !ClassIsAncestor(o->Class, s_shipCls))
        continue;
      uint8_t *p = (uint8_t *)o;
      int t = p[OFF_SHIP_TEAM];
      if ((t == 1 || t == 2) && (humanTeams & (1 << t)))
        continue;
      bool human = false;
      void *ctrl = *(void **)(p + OFF_PAWN_CONTROLLER);
      for (int k = 0; k < n && !human; ++k)
        human = humanPawns[k] == o || (void *)pcs[k] == ctrl;
      if (human)
        continue;
      const char *cn = NameText(o->Class->Name);
      if (cn && strstr(cn, "AITargetH"))
        continue;
      victims[nv++] = o;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("commands: nuke scan faulted after %d", nv);
  }
  // Kill credit round-robin over the human players with a live ship.
  int credit[64], nc = 0;
  for (int i = 0; i < n; ++i)
    if (humanPawns[i])
      credit[nc++] = i;
  int hit = 0;
  for (int i = 0; i < nv; ++i) {
    int k = nc ? credit[i % nc] : -1;
    hit += KillPawn(victims[i], k >= 0 ? pcs[k] : nullptr, k >= 0 ? humanPawns[k] : nullptr) ? 1 : 0;
  }
  Logf("commands: nuke hit %d of %d enemy ships (human teams mask %d)", hit, nv, humanTeams);
}

// FYNPCSpawn copy for /bots. AddNPCSpawn (0x23A280) takes the struct by
// value: it copy-constructs it into the scene array (0x234480: PODs, plus a
// deep copy of the TArray at +0x40 whose elements are 0x28-byte PODs), then
// FMemory::Free()s the argument's +0x40 data (0xBFC9C0) and overwrites +0x08
// from 0x273A70. So the copy gets its own +0x40 buffer, and the weak pawn
// pointers (+0x8C, +0x94) and the instanced flag are reset as in
// RegisterOnslaughtNPC's defaults, so the scene tick spawns it fresh from the
// dataset pawn id at +0x18. Each copy spawns 1.5 s after the previous one.
// GUESS: the stagger is ours (the original's spawn point is shared; whether
// two ships spawned on one point at once collide or fail is unknown).
static uint8_t g_botsSeen[1024]; // scene ids already handled (originals and copies)

static int DuplicateSceneEntry(uint8_t *scene, int id, float delay) {
  uint8_t *src = *(uint8_t **)(scene + OFF_SCENE_NPCS) + (size_t)id * NPC_SPAWN_SIZE;
  alignas(16) uint8_t spawn[NPC_SPAWN_SIZE];
  memcpy(spawn, src, NPC_SPAWN_SIZE);
  int32_t n = *(int32_t *)(src + 0x48);
  *(void **)(spawn + 0x40) = nullptr;
  *(int32_t *)(spawn + 0x48) = 0;
  *(int32_t *)(spawn + 0x4C) = 0;
  if (n > 0 && n < 64 && *(void **)(src + 0x40)) {
    void *buf = ((tFMemoryMalloc)(g_base + RVA_FMEMORY_MALLOC))((size_t)n * 0x28, 0);
    if (!buf)
      return -1;
    memcpy(buf, *(void **)(src + 0x40), (size_t)n * 0x28);
    *(void **)(spawn + 0x40) = buf;
    *(int32_t *)(spawn + 0x48) = n;
    *(int32_t *)(spawn + 0x4C) = n;
  }
  spawn[OFF_NPCSPAWN_INSTANCED] = 0;
  *(int32_t *)(spawn + 0x8C) = -1;
  *(int32_t *)(spawn + 0x90) = 0;
  *(int32_t *)(spawn + 0x94) = -1;
  *(int32_t *)(spawn + 0x98) = 0;
  *(float *)(spawn + OFF_NPCSPAWN_TIME) += delay;
  uint8_t randomClass = scene[OFF_SCENE_RANDOM_CLASS];
  scene[OFF_SCENE_RANDOM_CLASS] = 0;
  int nid = ((tSceneAddNpcSpawn)(g_base + RVA_SCENE_ADD_NPC_SPAWN))(scene, spawn);
  scene[OFF_SCENE_RANDOM_CLASS] = randomClass;
  return nid;
}

static void BotsTick() {
  if (!HostIsHavoc() && !HostIsHorde())
    return;
  uint8_t *scene = (uint8_t *)((tGetSceneManager)(g_base + RVA_COMBAT_SCENE_MANAGER))();
  if (!scene || !IsReadable(scene + OFF_SCENE_NPCS, 16))
    return;
  int n = (int)g_hvcEnemyCount;
  for (int i = 0; i < n && i < 256; ++i) {
    int id = (int)g_hvcEnemyIds[i];
    int cnt = *(int32_t *)(scene + OFF_SCENE_NPCS_COUNT);
    if (id < 0 || id >= cnt || id >= (int)sizeof g_botsSeen || g_botsSeen[id])
      continue;
    g_botsSeen[id] = 1;
    int made = 0;
    for (int k = 1; k < g_botsMult; ++k) {
      int nid = DuplicateSceneEntry(scene, id, 1.5f * k);
      if (nid < 0 || nid >= (int)sizeof g_botsSeen)
        break;
      g_botsSeen[nid] = 1;
      LONG j = InterlockedIncrement(&g_hvcEnemyCount) - 1;
      if (j < 256)
        g_hvcEnemyIds[j] = nid;
      else
        InterlockedDecrement(&g_hvcEnemyCount);
      ++made;
    }
    if (made)
      Logf("commands: bots x%d: enemy %d -> %d copies (scene npcs %d)", g_botsMult, id, made,
           *(int32_t *)(scene + OFF_SCENE_NPCS_COUNT));
  }
}

// /spawn ... xN: elite copies. Once the scene has spawned an entry's pawn,
// its feats component gets stacks of the Havoc boost feats players buy
// (DN_Feats_Havoc_Boosts_Shared_OTS_DT, all m_stackOnAdding, Stacks(99);
// AddFeat 0x337850 adds a stack to an existing stackable feat via 0x33F060):
// HVC_Boost_Def_HpInc_Large = MaxHealth +10% (+ Health +100%, i.e. full heal)
// per stack; HVC_Boost_Off_DmgInc_Large = PawnDamageModifier +10% per stack.
// Armor (DamageResistance +10%/stack) is left alone: resistance near 100%
// would make the ship unkillable.
// The modifiers compound per stack (measured, see EliteTick), they do not add
// -- the former GUESS of linear stacking was disproved; damage compounding is
// assumed to match health (not measured).
// Calls a no-argument float getter UFunction (e.g. GetMaxHealth) if the
// object's class chain declares it; NaN when it does not. Walks the class's
// own Children instead of FindFunctionChecked, which must not miss.
static float CallFloatGetter(void *obj, const char *name) {
  if (!IsReadable(obj, sizeof(UObjectMin)))
    return NAN;
  for (UObjectMin *cls = ((UObjectMin *)obj)->Class; cls && IsReadable(cls, 0x40);
       cls = *(UObjectMin **)((uint8_t *)cls + OFF_USTRUCT_SUPER)) {
    UObjectMin *f = *(UObjectMin **)((uint8_t *)cls + 0x38);
    for (int i = 0; f && i < 400 && IsReadable(f, 0x58); ++i, f = *(UObjectMin **)((uint8_t *)f + 0x28)) {
      const char *fn = NameText(f->Name);
      if (!fn || strcmp(fn, name) || strcmp(ClassNameOf(f), "Function"))
        continue;
      UObjectMin *rp = *(UObjectMin **)((uint8_t *)f + 0x38);
      if (!rp || !IsReadable(rp, 0x58) || strcmp(ClassNameOf(rp), "FloatProperty"))
        return NAN;
      uint32_t off = *(uint32_t *)((uint8_t *)rp + 0x50);
      alignas(16) uint8_t parms[64] = {};
      if (off > sizeof parms - 4)
        return NAN;
      void **vt = *(void ***)obj;
      ((tProcessEventVirt)vt[VT_PROCESS_EVENT / 8])(obj, f, parms);
      return *(float *)(parms + off);
    }
  }
  return NAN;
}

// Finds a UFunction by name on obj's class chain (no FindFunctionChecked:
// that one must not miss).
static UObjectMin *FindFnOnChain(void *obj, const char *name) {
  if (!IsReadable(obj, sizeof(UObjectMin)))
    return nullptr;
  for (UObjectMin *cls = ((UObjectMin *)obj)->Class; cls && IsReadable(cls, 0x40);
       cls = *(UObjectMin **)((uint8_t *)cls + OFF_USTRUCT_SUPER))
    for (UObjectMin *f = *(UObjectMin **)((uint8_t *)cls + 0x38); f && IsReadable(f, 0x58);
         f = *(UObjectMin **)((uint8_t *)f + 0x28)) {
      const char *fn = NameText(f->Name);
      if (fn && !strcmp(fn, name) && !strcmp(ClassNameOf(f), "Function"))
        return f;
    }
  return nullptr;
}

// Calls a UFunction whose parameters are FStrings (or none): every input
// StrProperty gets `in`; a return/out StrProperty (CPF_OutParm 0x100 |
// CPF_ReturnParm 0x400, UProperty+0x38) is copied to `out`. The engine-built
// return string is not freed (a few bytes per call). Logs the parameter list.
static bool CallStrFn(void *obj, const char *name, const wchar_t *in, wchar_t *out, int outCap) {
  UObjectMin *f = FindFnOnChain(obj, name);
  if (!f)
    return false;
  alignas(16) uint8_t parms[256] = {};
  FStringMin arg = {const_cast<wchar_t *>(in ? in : L""), in ? (int32_t)wcslen(in) + 1 : 1, 0};
  arg.max = arg.num;
  uint32_t outOff = UINT32_MAX;
  char desc[200] = "";
  int o = 0;
  for (UObjectMin *pp = *(UObjectMin **)((uint8_t *)f + 0x38); pp && IsReadable(pp, 0x58);
       pp = *(UObjectMin **)((uint8_t *)pp + 0x28)) {
    uint32_t off = *(uint32_t *)((uint8_t *)pp + 0x50);
    uint64_t flags = *(uint64_t *)((uint8_t *)pp + 0x38);
    const char *pc = ClassNameOf(pp);
    const char *pn = NameText(pp->Name);
    o += snprintf(desc + o, sizeof desc - o > 0 ? sizeof desc - o : 0, " %s:%s@%x/%llx", pn ? pn : "?", pc, off,
                  (unsigned long long)flags);
    if (strcmp(pc, "StrProperty") || off > sizeof parms - sizeof(FStringMin))
      return Logf("appearance: %s has a non-string parameter (%s)", name, desc), false;
    if (flags & 0x500)
      outOff = off;
    else
      memcpy(parms + off, &arg, sizeof arg);
  }
  void **vt = *(void ***)obj;
  ((tProcessEventVirt)vt[VT_PROCESS_EVENT / 8])(obj, f, parms);
  if (out && outCap > 0) {
    out[0] = 0;
    FStringMin *r = outOff != UINT32_MAX ? (FStringMin *)(parms + outOff) : nullptr;
    if (r && r->data && r->num > 0 && IsReadable(r->data, r->num * 2))
      wcsncpy(out, r->data, outCap - 1), out[outCap - 1] = 0;
  }
  static int s_logged = 0;
  if (s_logged++ < 6)
    Logf("appearance: called %s; parms%s", name, desc[0] ? desc : " (none)");
  return true;
}

// Elite look (custom bosses also set emblem, pattern and decal; 0 = keep): the NPC pawn carries the same UYShipCustomizationComponent a
// player ship does (YPawn.m_shipCustomizationComponent, +0x470 by reflection,
// headless 2026-10-16), whose ImportFromDisplayInfo / BroadcastApperance /
// ExportToDisplayInfo UFunctions are registered together at 0x6353A0. The
// display string is "mesh#mesh#mesh#mesh;emblem;paint;pattern;decal"
// (categories 0x14..0x18, mmogbrain loadout_ownership.go). Only the paint
// group is replaced: mesh parts and patterns are ship-specific, paints are
// shared (data/assets/ItemIDRegister.json, VanityItems/_Shared/Paints).
// The component is UYShipCustomizationComponent -- the S79 crash came from
// calling the import impl on a UYShipLoadout instead (see above).
static void PaintPawn(void *pawn, int32_t paint, int32_t emblem = 0, int32_t pattern = 0, int32_t decal = 0,
                      const int32_t *parts = nullptr) {
  int off = PropOffset((UObjectMin *)pawn, "m_shipCustomizationComponent");
  void *comp = off > 0 && IsReadable((uint8_t *)pawn + off, 8) ? *(void **)((uint8_t *)pawn + off) : nullptr;
  if (!comp || !IsReadable(comp, sizeof(UObjectMin)))
    return Logf("appearance: %s has no customization component", ClassNameOf(pawn));
  wchar_t cur[256] = L"", next[256] = L"";
  CallStrFn(comp, "ExportToDisplayInfo", nullptr, cur, 256);
  // Rebuild with group 2 (paint) swapped; missing groups are filled with -1.
  wchar_t groups[5][96] = {L"-1#-1#-1#-1", L"-1", L"-1", L"-1", L"-1"};
  int g = 0;
  for (wchar_t *p = cur, *start = cur; g < 5; ++p)
    if (*p == L';' || !*p) {
      bool end = !*p;
      *p = 0;
      if (*start)
        wcsncpy(groups[g], start, 95), groups[g][95] = 0;
      ++g, start = p + 1;
      if (end)
        break;
    }
  // GUESS: an AI pawn takes a pattern or decal made for a player hull of its
  // class (patterns are _Shared assets, VanityItems/Patterns/_Shared).
  const int32_t want[5] = {0, emblem, paint, pattern, decal};
  for (int i = 1; i < 5; ++i)
    if (want[i])
      swprintf(groups[i], 96, L"%d", want[i]);
  // Hero ship parts (admin_bosses.go Parts): group 0 is the four
  // m_heroShipParts ids '#'-joined, in the blueprints' order Forecastle, Hull,
  // Bridge, Stern (HeroLoadouts_cooked.jsonl, every row). 0 keeps that slot.
  // GUESS: an AI pawn imports and shows player hero parts of its own hull
  // line like a player ship does; the after-export line shows what took.
  if (parts && (parts[0] || parts[1] || parts[2] || parts[3])) {
    wchar_t cur0[4][24] = {L"-1", L"-1", L"-1", L"-1"};
    int k = 0;
    for (wchar_t *p = groups[0], *start = groups[0]; k < 4; ++p)
      if (*p == L'#' || !*p) {
        bool end = !*p;
        *p = 0;
        if (*start)
          wcsncpy(cur0[k], start, 23), cur0[k][23] = 0;
        ++k, start = p + 1;
        if (end)
          break;
      }
    for (int i = 0; i < 4; ++i)
      if (parts[i])
        swprintf(cur0[i], 24, L"%d", parts[i]);
    swprintf(groups[0], 96, L"%ls#%ls#%ls#%ls", cur0[0], cur0[1], cur0[2], cur0[3]);
  }
  swprintf(next, 256, L"%ls;%ls;%ls;%ls;%ls", groups[0], groups[1], groups[2], groups[3], groups[4]);
  bool imp = CallStrFn(comp, "ImportFromDisplayInfo", next, nullptr, 0);
  // BroadcastApperance(m_appearance StructProperty @0, CPF_Parm): hand it the
  // component's own appearance after the import. Logged layout:
  // YShipCustomizationComponent.m_appearance +0x2c0, 48 bytes, struct
  // YShipAppeareaceContainer {m_heroShipParts TArray +0, m_emblem +0x10,
  // m_paint +0x18, m_pattern +0x20, m_decal +0x28 (object pointers)}.
  // Called fine on the host; whether remote clients repaint is NOT verified.
  bool bc = false;
  {
    UObjectMin *fn = FindFnOnChain(comp, "BroadcastApperance");
    int aoff = PropOffset((UObjectMin *)comp, "m_appearance");
    if (aoff <= 0)
      aoff = PropOffset((UObjectMin *)comp, "m_shipAppearance");
    UObjectMin *pp = fn ? *(UObjectMin **)((uint8_t *)fn + 0x38) : nullptr;
    uint32_t psz = pp && IsReadable(pp, 0x58) ? *(uint32_t *)((uint8_t *)pp + 0x34) : 0;
    alignas(16) uint8_t parms[512] = {};
    if (fn && aoff > 0 && psz && psz <= sizeof parms && !*(uint32_t *)((uint8_t *)pp + 0x50)) {
      memcpy(parms, (uint8_t *)comp + aoff, psz);
      void **vt = *(void ***)comp;
      ((tProcessEventVirt)vt[VT_PROCESS_EVENT / 8])(comp, fn, parms);
      bc = true;
    } else {
      Logf("appearance: BroadcastApperance not called (fn %p, appearance prop +0x%x, parm size %u)", fn, aoff, psz);
    }
  }
  wchar_t after[256] = L"";
  CallStrFn(comp, "ExportToDisplayInfo", nullptr, after, 256);
  Logf("appearance: %s \"%ls\" -> \"%ls\" (import %d, broadcast %d), now \"%ls\"", ClassNameOf(pawn), cur, next, imp,
       bc, after);
}

// A custom boss (/boss <id>, mmogbrain admin_bosses.go), fetched from
// GET /battle/boss?id=N by CommandBossDef. A ring: a queued elite keeps
// pointing at its definition for its 120 s spawn window.
struct BossDef {
  int id;
  char ship[48];
  wchar_t name[32];
  int32_t paint, pattern, decal, emblem;
  int nAbil, nFeat;
  wchar_t abil[8][192];
  wchar_t feat[12][64];
  int stacks[12];
  int32_t parts[4];          // hero ship parts, Forecastle/Hull/Bridge/Stern, 0 = keep
  int nOfficer;
  wchar_t officer[4][32];    // DN_Officers_OTS_DT row names (MODULERECYCLER)
  bool hunter; // /hunters pool (admin_hunters.go): an elite without the boss treatment
  // HUD marker (admin "Marker"): an EYUIQuestMarker value 1..10, MARKER_DEFAULT
  // (HostileTarget, as before), MARKER_NONE, or MARKER_SPOTTED (the pawn's
  // replicated m_isSpotted, the flag a player's ServerSetPawnToSpotted sets).
  int marker;
};
enum { MARKER_DEFAULT = -1, MARKER_NONE = -2, MARKER_SPOTTED = 100 };
static BossDef g_bossDefs[32];
static int g_bossDefNext;
static const BossDef *g_spawnDef = nullptr; // CommandBossDef's, while it runs CommandSpawn
struct EliteSpawn { int nid, mult; DWORD since; const BossDef *def; };
static EliteSpawn g_elite[64];
// Boss marker (operator 10-19): a /boss behind terrain is invisible, while the
// Onslaught command ship carries a HUD marker. Candidates from the exe's
// reflection names: MulticastShowHUDMarkerOnActor / ShowHUDMarkerOnActor
// (log text names AYGameState_PVE::ShowHUDMarker), SetGenericMissionMarker
// (AYLevelScriptActor), marker type enum EYUIQuestMarker. Owners and
// parameters are logged once; each one the game state or the level script
// actor has is called with the boss as the actor.
// GUESS: UEnum Names TArray<TPair<FName,int64>> at +0x40 (UE4.1x layout); the
// names are logged, and the HostileTarget value is used only if found there.
struct MarkerApi {
  bool done;
  FNameMin fn[3];
  void *lsa;      // level script actor instance
  int64_t hostile; // EYUIQM_HostileTarget value, -1 unknown
};
static MarkerApi g_marker = {false, {}, nullptr, -1};
static void MarkerFind() {
  MarkerApi &m = g_marker;
  m.done = true;
  static const wchar_t *names[3] = {L"MulticastShowHUDMarkerOnActor", L"ShowHUDMarkerOnActor", L"SetGenericMissionMarker"};
  for (int i = 0; i < 3; ++i)
    ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&m.fn[i], names[i], 1 /* FNAME_Add */);
  FNameMin lsaN, enumN, propN[3];
  static const wchar_t *props[3] = {L"m_showQuestMarker", L"m_targetQuestMarker", L"m_targetQuestMarkerPosition"};
  for (int i = 0; i < 3; ++i)
    ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&propN[i], props[i], 1);
  ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&lsaN, L"YLevelScriptActor", 1);
  ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&enumN, L"EYUIQuestMarker", 1);
  FUObjectArrayMin *arr = GObjects();
  if (!arr || !arr->Objects)
    return;
  void *lsaCls = nullptr;
  for (int pass = 0; pass < 2; ++pass)
    for (int i = 0; i < arr->NumElements; ++i) {
      FUObjectItemMin &it = arr->Objects[i];
      UObjectMin *o = it.Object;
      if (!o || (it.Flags & OBJ_PENDING_KILL) || !o->Class)
        continue;
      if (pass == 1) {
        if (lsaCls && !m.lsa && o->Class != lsaCls && ClassIsAncestor(o->Class, lsaCls)) {
          const char *nm = NameText(o->Name);
          if (nm && strncmp(nm, "Default__", 9)) {
            m.lsa = o;
            Logf("boss marker: level script actor %p %s", o, ClassNameOf(o));
          }
        }
        continue;
      }
      if (o->Name.ComparisonIndex == lsaN.ComparisonIndex && !strcmp(ClassNameOf(o), "Class"))
        lsaCls = o;
      if (o->Name.ComparisonIndex == enumN.ComparisonIndex && !strncmp(ClassNameOf(o), "Enum", 4)) {
        uint8_t *names = IsReadable((uint8_t *)o + 0x48, 12) ? *(uint8_t **)((uint8_t *)o + 0x40) : nullptr;
        int n = names ? *(int32_t *)((uint8_t *)o + 0x48) : 0;
        for (int k = 0; k < n && k < 32 && IsReadable(names + k * 16, 16); ++k) {
          const char *t = NameText(*(FNameMin *)(names + k * 16));
          int64_t v = *(uint8_t *)(names + k * 16 + 8); // one byte: the upper bits were garbage (18:20 log)
          Logf("boss marker: enum %s = %lld", t ? t : "?", (long long)v);
          if (t && strstr(t, "EYUIQM_HostileTarget"))
            m.hostile = v;
        }
      }
      // Every marker function and its owner, for a route the Onslaught game
      // state has (it lacks the PVE ones). GUESS: UFunction FunctionFlags at
      // +0x88 (UE4.1x); FUNC_Net 0x40, NetMulticast 0x4000, NetClient 0x1000000.
      if (!strcmp(ClassNameOf(o), "Function")) {
        const char *fnm = NameText(o->Name);
        UObjectMin *owner = o->Outer;
        const char *on = owner ? NameText(owner->Name) : nullptr;
        if (fnm && strstr(fnm, "Marker") && on && strncmp(on, "YHUD", 4) && strncmp(on, "WBP_", 4))
          Logf("boss marker: fn %s.%s flags %x", on, fnm,
               IsReadable((uint8_t *)o + 0x88, 4) ? *(uint32_t *)((uint8_t *)o + 0x88) : 0);
      }
      // GUESS: UProperty PropertyFlags at +0x38 (after ArrayDim +0x30, ElementSize
      // +0x34); CPF_Net is 0x20. Logged raw.
      for (int f = 0; f < 3; ++f)
        if (o->Name.ComparisonIndex == propN[f].ComparisonIndex && strcmp(ClassNameOf(o), "Function") &&
            strcmp(ClassNameOf(o), "Class") && IsReadable(o, 0x58)) {
          UObjectMin *owner = o->Outer;
          const char *on = owner ? NameText(owner->Name) : nullptr;
          Logf("boss marker: property %S %s owned by %s +0x%x flags %llx", props[f], ClassNameOf(o), on ? on : "?",
               *(uint32_t *)((uint8_t *)o + 0x50), (unsigned long long)*(uint64_t *)((uint8_t *)o + 0x38));
        }
      for (int f = 0; f < 3; ++f)
        if (o->Name.ComparisonIndex == m.fn[f].ComparisonIndex && o->Name.Number == m.fn[f].Number &&
            !strcmp(ClassNameOf(o), "Function")) {
          UObjectMin *owner = o->Outer;
          const char *on = owner ? NameText(owner->Name) : nullptr;
          Logf("boss marker: %S owned by %s", names[f], on ? on : "?");
          UObjectMin *pp = *(UObjectMin **)((uint8_t *)o + 0x38);
          for (int j = 0; pp && j < 12 && IsReadable(pp, 0x58); ++j, pp = *(UObjectMin **)((uint8_t *)pp + 0x28)) {
            const char *pn = NameText(pp->Name);
            // GUESS: UByteProperty::Enum at +0x70 (UE4.1x, right after UProperty).
            UObjectMin *en = !strcmp(ClassNameOf(pp), "ByteProperty") && IsReadable((uint8_t *)pp + 0x70, 8)
                                 ? *(UObjectMin **)((uint8_t *)pp + 0x70) : nullptr;
            const char *enn = en && IsReadable(en, sizeof(UObjectMin)) ? NameText(en->Name) : nullptr;
            Logf("boss marker:   param %s %s +0x%x%s%s", ClassNameOf(pp), pn ? pn : "?", *(uint32_t *)((uint8_t *)pp + 0x50),
                 enn ? " enum " : "", enn ? enn : "");
          }
        }
    }
}
static void BossMarker(uint8_t *gm, void *pawn, int want) {
  __try {
    if (want == MARKER_NONE)
      return;
    if (want == MARKER_SPOTTED) {
      // Player marking: the client's ServerSetPawnToSpotted(pawn, bool) RPC
      // (exec thunk 0x773460 -> _Implementation at vt+0xDF8) ends in the
      // pawn's replicated m_isSpotted, whose OnRep_Spotted draws the mark on
      // every client. GUESS: m_isSpotted lives on the ship pawn and the
      // server does not clear it on its own (owner and offset are logged).
      UObjectMin *bp = FindPropObj((UObjectMin *)pawn, "m_isSpotted");
      bool ok = SetBoolProp(pawn, "m_isSpotted", true);
      Logf("boss marker: spotted %s on %s (m_isSpotted %s, owner %s +%d)", ok ? "set" : "NOT SET", ClassNameOf(pawn),
           bp ? ClassNameOf(bp) : "missing", bp && bp->Outer ? NameText(bp->Outer->Name) : "-",
           PropOffset((UObjectMin *)pawn, "m_isSpotted"));
      return;
    }
    if (!g_marker.done)
      MarkerFind();
    uint8_t *gs = gm && IsReadable(gm + OFF_GM_GAMESTATE, 8) ? *(uint8_t **)(gm + OFF_GM_GAMESTATE) : nullptr;
    int64_t type = want > 0 && want < 11 ? want : g_marker.hostile;
    struct Ctx { void *pawn; int64_t type; bool text, obj; } c = {pawn, type, false, false};
    auto fill = [](uint8_t *p, const char *pc, const char *, uint32_t off, void *ctx) {
      Ctx *c = (Ctx *)ctx;
      if (!strcmp(pc, "ObjectProperty") && !c->obj) {
        *(void **)(p + off) = c->pawn;
        c->obj = true;
      } else if ((!strcmp(pc, "ByteProperty") || !strcmp(pc, "EnumProperty")) && c->type >= 0)
        p[off] = (uint8_t)c->type;
      else if (!strcmp(pc, "BoolProperty"))
        p[off] = 1;
      else if (!strcmp(pc, "TextProperty") || !strcmp(pc, "StrProperty"))
        c->text = true; // GUESS: a zeroed FString is a valid empty one; FText is not, so skip those calls
    };
    // The pawn's own quest marker fields, if its class has them.
    int so = PropOffset((UObjectMin *)pawn, "m_showQuestMarker"), to = PropOffset((UObjectMin *)pawn, "m_targetQuestMarker");
    Logf("boss marker: pawn %s m_showQuestMarker +%d m_targetQuestMarker +%d", ClassNameOf(pawn), so, to);
    // m_showQuestMarker is left alone: if it is a bitfield bool, writing the
    // byte would clobber its neighbours (mask offset unknown in this build).
    if (to > 0 && type >= 0)
      ((uint8_t *)pawn)[to] = (uint8_t)type;
    alignas(16) uint8_t parms[256];
    static const wchar_t *calls[3] = {L"MulticastShowHUDMarkerOnActor", L"ShowHUDMarkerOnActor", L"SetGenericMissionMarker"};
    for (int f = 0; f < 3; ++f) {
      void *target = f < 2 ? (void *)gs : g_marker.lsa;
      if (!target || !FindFunctionSafe(target, *(uint64_t *)&g_marker.fn[f]))
        continue;
      // Dry run to see the parameter kinds before calling for real.
      c.text = c.obj = false;
      UObjectMin *fn = (UObjectMin *)FindFunctionSafe(target, *(uint64_t *)&g_marker.fn[f]);
      for (UObjectMin *pp = *(UObjectMin **)((uint8_t *)fn + 0x38); pp && IsReadable(pp, 0x58); pp = *(UObjectMin **)((uint8_t *)pp + 0x28))
        if (!strcmp(ClassNameOf(pp), "TextProperty"))
          c.text = true;
      if (c.text) {
        Logf("boss marker: %S has an FText parameter, not called", calls[f]);
        continue;
      }
      c.obj = false;
      bool ok = CallNamed(target, calls[f], fill, &c, parms);
      Logf("boss marker: %S on %s(%p) for %p type %lld -> %s", calls[f], ClassNameOf(target), target, pawn,
           (long long)c.type, ok ? "called" : "failed");
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("boss marker: fault");
  }
}

static void InstallAbilityClasses(void *pawn, const char *tag, const wchar_t *const *paths, int n);
// Officer briefings are feats of DN_Officers_OTS_DT (each PRK_*_BP names it in
// m_featDataTable and its row in m_perkFeatName). No ship pawn lists that
// table on its feats component (VH_AssaultH_Pawn_BP lists the DN_Feats_* ones
// only), so AddFeatInternal could not find MODULERECYCLER. Append the loaded
// table to the component's list (+0xF8, see EnsureFeatTables).
// GUESS: an element is {UDataTable*, 0}; and the table is only in memory once
// something loaded a briefing (a human ship flying one) -- false otherwise.
static bool EnsureOfficerTable(void *comp) {
  uint8_t *c = (uint8_t *)comp;
  UObjectMin *tbl = FindObjectNamedOfClass("DN_Officers_OTS_DT", "DataTable");
  if (!tbl || !IsReadable(c + 0xF8, 16))
    return false;
  int n = *(int32_t *)(c + 0x100);
  uint8_t *e = *(uint8_t **)(c + 0xF8);
  if (n < 0 || n >= 64 || (n && (!e || !IsReadable(e, (size_t)n * 16))))
    return false;
  for (int j = 0; j < n; ++j)
    if (*(void **)(e + j * 16) == tbl)
      return true;
  uint8_t *buf = (uint8_t *)((tFMemoryMalloc)(g_base + RVA_FMEMORY_MALLOC))((size_t)(n + 1) * 16, 0);
  if (!buf)
    return false;
  if (n)
    memcpy(buf, e, (size_t)n * 16);
  memset(buf + n * 16, 0, 16);
  *(void **)(buf + n * 16) = tbl;
  *(uint8_t **)(c + 0xF8) = buf;
  *(int32_t *)(c + 0x100) = n + 1;
  *(int32_t *)(c + 0x104) = n + 1;
  return true;
}

static void ApplyElite(uint8_t *gm, void *pawn, void *comp, int mult, const char *tag, const BossDef *def = nullptr) {
  float hp0 = CallFloatGetter(pawn, "GetMaxHealth");
  EnsureFeatTables(comp);
  // Placeholder tiers until the operator's designs: x5 and up red-gold
  // (VAN_PN_Red_Gold_DA), below that gold (VAN_PN_Dev_Gold_DA).
  bool boss = def && !def->hunter;
  if (def && def->hunter) {
    if (def->paint || def->emblem || def->pattern || def->decal || def->parts[0] || def->parts[1] || def->parts[2] ||
        def->parts[3])
      PaintPawn(pawn, def->paint, def->emblem, def->pattern, def->decal, def->parts);
  } else if (def)
    PaintPawn(pawn, def->paint ? def->paint : mult >= 5 ? 385810647 : 385810633, def->emblem, def->pattern, def->decal,
              def->parts);
  else
    PaintPawn(pawn, mult >= 5 ? 385810647 : 385810633);
  // Custom modules replace the ship's own. GUESS: the controller fires them --
  // FixNpcFireAbilities maps its fire slots by class stem, and a slot whose
  // class is not among them falls back to the pawn ability at its index.
  if (def && def->nAbil) {
    const wchar_t *paths[8];
    for (int i = 0; i < def->nAbil; ++i)
      paths[i] = def->abil[i];
    InstallAbilityClasses(pawn, def->ship, paths, def->nAbil);
  }
  // Stacks compound: 40 HpInc stacks took a boss's GetMaxHealth from
  // 236000 to 10681192 (= 1.1^40, headless 2026-10-16). So n = log(x)/log(1.1):
  // health xN, damage x(1 + (N-1)/4) (x2 at N=5, x3.25 at N=10).
  int hp = std::min(99, (int)lround(log((double)mult) / log(1.1)));
  int dmg = std::min(99, (int)lround(log(1.0 + (mult - 1) / 4.0) / log(1.1))), ok = 0;
  // One fault ends it for this pawn (onsboss 16:12: 21 faulting adds on an
  // Onslaught AITargetM, then the server died 18 s later; cause not shown).
  bool bad = false;
  for (int i = 0; i < hp && !bad; ++i)
    ok += FeatOp(comp, L"HVC_Boost_Def_HpInc_Large", true, &bad);
  for (int i = 0; i < dmg && !bad; ++i)
    ok += FeatOp(comp, L"HVC_Boost_Off_DmgInc_Large", true, &bad);
  int want = hp + dmg;
  for (int f = 0; def && f < def->nFeat && !bad; ++f)
    for (int i = 0; i < def->stacks[f] && !bad; ++i, ++want)
      ok += FeatOp(comp, def->feat[f], true, &bad);
  if (def && def->nOfficer && !bad) {
    bool tbl = EnsureOfficerTable(comp);
    int got = 0;
    for (int i = 0; i < def->nOfficer && !bad; ++i)
      got += FeatOp(comp, def->officer[i], true, &bad);
    Logf("commands: elite officers: %d/%d briefing feats added (officer table %s)", got, def->nOfficer,
         tbl ? "listed" : "not loaded");
  }
  Logf("commands: elite x%d: %s pawn %p %s%s%ls -- %d/%d feat stacks added, GetMaxHealth %.0f -> %.0f", mult, tag, pawn,
       ClassNameOf(pawn), boss ? " boss " : def ? " hunter " : "", def ? def->name : L"", ok, want, hp0, CallFloatGetter(pawn, "GetMaxHealth"));
  // Boss arrival (any elite, /boss is x2 and up): every human gets a shake
  // and a red flash when the elite is actually in the world, not when it is
  // queued; once per 3 s so /spawn 8 x2 is one arrival, not eight.
  static DWORD lastArrival;
  if ((mult >= 2 || boss) && gm && GetTickCount() - lastArrival > 3000) {
    lastArrival = GetTickCount();
    CommandShake(gm, 4.f);
    CommandFlash(gm, "red");
  }
  if (mult >= 2 || boss)
    BossMarker(gm, pawn, def ? def->marker : MARKER_DEFAULT);
}
static void EliteTick(uint8_t *gm) {
  uint8_t *scene = nullptr;
  for (EliteSpawn &el : g_elite) {
    if (!el.mult)
      continue;
    if (GetTickCount() - el.since > 120000) { // never spawned
      el.mult = 0;
      continue;
    }
    if (!scene && !(scene = (uint8_t *)((tGetSceneManager)(g_base + RVA_COMBAT_SCENE_MANAGER))()))
      return;
    if (el.nid >= *(int32_t *)(scene + OFF_SCENE_NPCS_COUNT))
      continue;
    uint8_t *e = *(uint8_t **)(scene + OFF_SCENE_NPCS) + (size_t)el.nid * NPC_SPAWN_SIZE;
    if (!e[OFF_NPCSPAWN_INSTANCED])
      continue;
    void *pawn = ((tWeakGet)(g_base + RVA_WEAK_PTR_GET))(e + OFF_NPCSPAWN_PAWN);
    void *comp = pawn ? FeatsCompOf(pawn) : nullptr;
    if (!comp)
      continue;
    char tag[32];
    snprintf(tag, sizeof tag, "scene entry %d", el.nid);
    ApplyElite(gm, pawn, comp, el.mult, tag, el.def);
    el.mult = 0;
  }
}

// Elite game bots. GUESS: a game bot's pawn carries its bot list entry's
// replication info (weak gm+0x9A0[entry], YNPCReplicationInfo, made by
// AddReplicationInfoForNpc) as APawn::PlayerState (+0x658), as the scene
// manager's NPCs carry theirs, or its AI controller does (checked too). Each new pawn of the entry (respawns too) gets
// ApplyElite once. When no pawn matches within 3 min the info and the
// team's pawns' +0x658 are logged once, to find the real link.
static void PlayerStateName(void *psv, char *out, int cap); // viewer snapshot
// YNPCState::m_NPCName (TextProperty, CPF_Net; gamebot-name3: +0x3C8 on a
// /spawn'd bot's YNPCReplicationInfo) is the name of creeps and game bots --
// their info has no PlayerName. Read through the engine's
// KismetTextLibrary.Conv_TextToString, cached per (info, text data) since the
// returned FStrings are never freed; written with the Conv_StringToText body
// (RVA_TEXT_FROM_STRING), the old FText left unreleased (once per bot).
static int NpcNameOffset(UObjectMin *info) {
  static UObjectMin *s_cls;
  static int s_off = -1;
  if (info->Class != s_cls) {
    int off = PropOffset(info, "m_NPCName");
    s_off = off, s_cls = info->Class;
  }
  return s_off;
}

static void EncodeWideName(const wchar_t *w, int len, char *out, int cap) {
  int n = 0;
  for (int i = 0; i < len && w[i] && n < cap - 4; ++i) {
    wchar_t c = w[i];
    if (c > 32 && c < 127 && c != '%')
      out[n++] = (char)c;
    else if (c < 256)
      n += _snprintf_s(out + n, cap - n, _TRUNCATE, "%%%02X", (unsigned)c);
  }
  out[n] = 0;
}

// m_NPCName of a YNPCReplicationInfo, percent-encoded like PlayerStateName;
// false (out untouched) when there is none.
static bool NpcInfoName(void *infov, char *out, int cap) {
  static struct { void *info, *data; char name[64]; } s_cache[32];
  static int s_next;
  static UObjectMin *s_lib, *s_fn;
  static uint32_t s_inOff, s_retOff;
  static bool s_tried;
  __try {
    UObjectMin *info = (UObjectMin *)infov;
    if (!info || !ObjectLive(info))
      return false;
    int off = NpcNameOffset(info);
    if (off < 0)
      return false;
    uint8_t *t = (uint8_t *)info + off;
    void *data = *(void **)t;
    if (!data)
      return false;
    for (auto &c : s_cache)
      if (c.info == info && c.data == data) {
        if (!c.name[0])
          return false;
        strcpy_s(out, cap, c.name);
        return true;
      }
    if (!s_tried) {
      s_tried = true;
      s_lib = FindObjectNamed("Default__KismetTextLibrary");
      s_fn = s_lib ? FindFnOnChain(s_lib, "Conv_TextToString") : nullptr;
      int got = 0;
      for (UObjectMin *pp = s_fn ? *(UObjectMin **)((uint8_t *)s_fn + 0x38) : nullptr; pp && IsReadable(pp, 0x58);
           pp = *(UObjectMin **)((uint8_t *)pp + 0x28)) {
        const char *pc = ClassNameOf(pp);
        if (!strcmp(pc, "TextProperty"))
          s_inOff = *(uint32_t *)((uint8_t *)pp + 0x50), got |= 1;
        else if (!strcmp(pc, "StrProperty"))
          s_retOff = *(uint32_t *)((uint8_t *)pp + 0x50), got |= 2;
      }
      if (got != 3 || s_inOff > 32 || s_retOff > 40)
        s_fn = nullptr;
      Logf("bots: m_NPCName +0x%X; KismetTextLibrary %p Conv_TextToString %p (InText +0x%X, ReturnValue +0x%X)", off,
           s_lib, s_fn, s_inOff, s_retOff);
    }
    if (!s_fn)
      return false;
    alignas(16) uint8_t parms[64] = {};
    memcpy(parms + s_inOff, t, 24); // bitwise: the caller owns the parms, nothing is released
    ((tProcessEventVirt)(*(void ***)s_lib)[VT_PROCESS_EVENT / 8])(s_lib, s_fn, parms);
    FStringMin *fs = (FStringMin *)(parms + s_retOff);
    auto &c = s_cache[s_next++ & 31];
    c.info = info, c.data = data, c.name[0] = 0;
    if (fs->data && fs->num > 1 && fs->num <= 64 && IsReadable(fs->data, fs->num * 2))
      EncodeWideName(fs->data, fs->num - 1, c.name, sizeof c.name);
    if (!c.name[0])
      return false;
    strcpy_s(out, cap, c.name);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static bool NpcInfoSetName(void *infov, const char *name) {
  __try {
    UObjectMin *info = (UObjectMin *)infov;
    int off = info && ObjectLive(info) ? NpcNameOffset(info) : -1;
    if (off < 0)
      return false;
    int len = (int)strlen(name) + 1;
    wchar_t *buf = (wchar_t *)((tOperatorNew)(g_base + RVA_OPERATOR_NEW))((size_t)len * sizeof(wchar_t));
    if (!buf)
      return false;
    for (int i = 0; i < len; ++i)
      buf[i] = (unsigned char)name[i];
    FStringMin str = {buf, len, len};
    alignas(16) uint8_t text[24] = {};
    ((tTextFromString)(g_base + RVA_TEXT_FROM_STRING))(text, &str);
    memcpy((uint8_t *)info + off, text, 24);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// gamebot-name1/2: SetPlayerName (vt+0x5E0) did nothing -- the info is a
// YNPCReplicationInfo, and its vt+0x5E0 (0x2E2260) takes no string. Once per
// info, a /spawn'd entry's m_NPCName becomes the entry's name.
static void GameBotNameTick(uint8_t *gm, uint8_t *infos, int ninfo, uint8_t *list, int nlist) {
  static void *s_done[128];
  for (int i = 0; i < 128 && i < nlist && i < ninfo; ++i) {
    if (!g_extraUsed[i] || !IsReadable(list + (size_t)i * 0x38, 8) || !IsReadable(infos + (size_t)i * 8, 8))
      continue;
    void *ps = ((tWeakGet)(g_base + RVA_WEAK_PTR_GET))(infos + (size_t)i * 8);
    if (!ps || s_done[i] == ps || !ObjectLive(ps))
      continue;
    s_done[i] = ps;
    const char *en = NameText(*(FNameMin *)(list + (size_t)i * 0x38));
    if (!en || !*en || !strcmp(en, "None"))
      continue;
    char was[64] = "-", now[64] = "-";
    NpcInfoName(ps, was, sizeof was);
    bool ok = strcmp(was, en) && NpcInfoSetName(ps, en);
    NpcInfoName(ps, now, sizeof now);
    Logf("bots: entry %d (/spawn'd) m_NPCName %s -> %s (%s)", i, was, now, ok ? "set" : "not set");
  }
}

static void GameBotEliteTick(uint8_t *gm) {
  // State once a minute while an elite entry is set: gamebot-spawn9, -boss10
  // and -boss11 logged neither a match nor the 3-minute dump below (boss11: the
  // in-loop heartbeat logged once, before the fill, then never again).
  static DWORD s_beat = 0;
  if (GetTickCount() - s_beat > 60000) {
    int first = -1;
    for (int i = 0; i < 128 && first < 0; ++i)
      if (g_gbElite[i].mult)
        first = i;
    if (first >= 0) {
      s_beat = GetTickCount();
      GameBotElite &g = g_gbElite[first];
      uint8_t *infos = IsReadable(gm + 0x9A0, 16) ? *(uint8_t **)(gm + 0x9A0) : nullptr;
      int ninfo = infos ? *(int32_t *)(gm + 0x9A8) : 0;
      void *info = first < ninfo && IsReadable(infos + (size_t)first * 8, 8)
                       ? ((tWeakGet)(g_base + RVA_WEAK_PTR_GET))(infos + (size_t)first * 8) : nullptr;
      int npcs = 0;
      for (FeatsByPawn &fp : g_featsByPawn)
        npcs += fp.pawn && ObjectLive(fp.pawn) && PawnIsNpc(fp.pawn);
      Logf("bots: elite tick: gm %s, entry %d x%d info %p (infos %d, list %d) last %p told %d age %lu s; %d NPC pawns with feats",
           g_extraBotsGm == gm ? "ours" : "OTHER", first, g.mult, info, ninfo,
           IsReadable(gm + OFF_GM_NPC_COUNT, 4) ? *(int32_t *)(gm + OFF_GM_NPC_COUNT) : -1, g.last, (int)g.told,
           (unsigned long)((GetTickCount() - g.since) / 1000), npcs);
    }
  }
  if (g_extraBotsGm != gm || !IsReadable(gm + 0x9A0, 16) || !IsReadable(gm + 0x968, 8))
    return;
  uint8_t *infos = *(uint8_t **)(gm + 0x9A0);
  int ninfo = *(int32_t *)(gm + 0x9A8);
  uint8_t *list = *(uint8_t **)(gm + 0x968);
  int nlist = IsReadable(gm + OFF_GM_NPC_COUNT, 4) ? *(int32_t *)(gm + OFF_GM_NPC_COUNT) : 0;
  if (list && infos)
    GameBotNameTick(gm, infos, ninfo, list, nlist);
  for (int i = 0; i < 128 && i < nlist; ++i) {
    GameBotElite &g = g_gbElite[i];
    if (!g.mult || !list || !IsReadable(list + (size_t)i * 0x38, 0x38))
      continue;
    int team = list[(size_t)i * 0x38 + 8];
    void *info = i < ninfo && IsReadable(infos + (size_t)i * 8, 8)
                     ? ((tWeakGet)(g_base + RVA_WEAK_PTR_GET))(infos + (size_t)i * 8) : nullptr;
    uint8_t *hit = nullptr;
    void *hitComp = nullptr;
    const char *how = "";
    for (FeatsByPawn &fp : g_featsByPawn) {
      uint8_t *p = (uint8_t *)fp.pawn;
      if (!p || !ObjectLive(p) || !ObjectLive(fp.comp) || FeatsCompOf(p) != fp.comp ||
          !IsReadable(p + OFF_PAWN_NPC_PLAYERSTATE, 8))
        continue;
      uint8_t *ctl = IsReadable(p + OFF_PAWN_CONTROLLER, 8) ? *(uint8_t **)(p + OFF_PAWN_CONTROLLER) : nullptr;
      if (info && *(void **)(p + OFF_PAWN_NPC_PLAYERSTATE) == info)
        hit = p, hitComp = fp.comp, how = "pawn PlayerState";
      else if (info && ctl && IsReadable(ctl + OFF_PC_PLAYER_STATE, 8) && *(void **)(ctl + OFF_PC_PLAYER_STATE) == info)
        hit = p, hitComp = fp.comp, how = "controller PlayerState";
      if (hit)
        break;
    }
    // Only /spawn'd bots on the map: any NPC ship of the entry's team that no
    // other entry has taken is this one.
    if (!hit && g_extraOnly)
      for (FeatsByPawn &fp : g_featsByPawn) {
        uint8_t *p = (uint8_t *)fp.pawn;
        if (!p || !ObjectLive(p) || !ObjectLive(fp.comp) || FeatsCompOf(p) != fp.comp || !PawnIsNpc(p) ||
            ShipTeam(p) != team)
          continue;
        bool taken = false;
        for (int j = 0; j < 128 && !taken; ++j)
          taken = j != i && g_gbElite[j].last == p;
        if (!taken && p != g.last) {
          bool otherLive = false; // keep a live ship of ours rather than move to another
          for (FeatsByPawn &fq : g_featsByPawn)
            otherLive |= fq.pawn == g.last && g.last && ObjectLive(g.last);
          if (!otherLive)
            hit = p, hitComp = fp.comp, how = "only /spawn'd bots on its team";
        }
        if (hit)
          break;
      }
    if (hit && hit != g.last) {
      char tag[64];
      snprintf(tag, sizeof tag, "game bot entry %d (%s)", i, how);
      ApplyElite(gm, hit, hitComp, g.mult, tag, g.def);
      g.last = hit, g.told = true;
      continue;
    }
    if (hit || g.told || GetTickCount() - g.since < 180000)
      continue;
    g.told = true;
    Logf("bots: elite entry %d (team %d) -- no ship found in 3 min; info %p (weak infos %d); NPC pawns:", i, team, info, ninfo);
    for (FeatsByPawn &fp : g_featsByPawn) {
      uint8_t *p = (uint8_t *)fp.pawn;
      if (!p || !ObjectLive(p) || !IsReadable(p + OFF_PAWN_NPC_PLAYERSTATE, 8) || !PawnIsNpc(p))
        continue;
      uint8_t *ctl = IsReadable(p + OFF_PAWN_CONTROLLER, 8) ? *(uint8_t **)(p + OFF_PAWN_CONTROLLER) : nullptr;
      Logf("bots:   pawn %p %s team %d ps %p, controller %p ps %p", p, ClassNameOf(p), ShipTeam(p),
           *(void **)(p + OFF_PAWN_NPC_PLAYERSTATE), ctl,
           ctl && IsReadable(ctl + OFF_PC_PLAYER_STATE, 8) ? *(void **)(ctl + OFF_PC_PLAYER_STATE) : nullptr);
    }
  }
}

// /boss outside Havoc: one enemy bot -- a normal player-class ship with the
// bot AI the PvP modes fill teams with -- boosted like a Havoc elite. The
// assault and command ships it used to be were "a floating glass bottle" and
// "flying bricks ... moving sinks for damage" (operator 10-17).
// /boss outside Havoc first went through the game's own bot system (boss1-5,
// 2026-10-17), and that cannot work in Onslaught. Kept for whoever tries again:
// bot list gm+0x968 (count +0x970, stride 0x38: team byte +0x8, hull ids
// array +0x10 count +0x18, hull index +0x30); per-bot arrays gm+0x990 (ship
// request handle, -1 none), gm+0x9A0 (weak YNPCReplicationInfo, owned by the
// game mode -- not a controller), gm+0x9B0 (weak helper actor). The fill
// 0x381FA0 trims bots over the team targets (gm+0x980/+0x984), spawns infos
// via 0x3671C0 per entry in order, and sets up +0x990/+0x9B0 only when +0x990
// is empty. The ship spawner 0x3822F0(gm, flag, useEntryHull) -> 0x374550
// picks the hull via GetRandSpawnForNPC (0x371B50: "no dataset in the gamemode
// or combat manager for the training match") or, flagged, entry+0x30; either
// way the combat scene manager resolves the ShipId through its dataset
// (scene+0x3D0, lookup 0x2CF110), and the Onslaught map's has only the
// Onslaught ships ("ShipId=SupportMediumT2, no base pawn in dataAsset",
// retried every frame for the rest of the match, boss5).

// The AI pawn class for a Havoc ShipId (chat_commands.go havocEnemyIDs):
// "<Class><Weight>...Boss01" -> AIPawns/Havoc/Bosses/HVC_VH_<C><W>_Boss_01_Pawn_BP,
// anything else -> AIPawns/VH_<C><W>_AI_Pawn_T<n>_BP, then the untiered
// VH_<C><W>_AI_Pawn_BP. Class Dreadnought is "Dread"; weight H/M/L. Titans
// and unknown ids give null. GUESS: these pawns carry their own loadout and
// fly under the Onslaught AI controller like the assault ships.
// Havoc pairs pawn and behaviour in its dataset rows; the Havoc boss pawns
// for Assault Heavy/Light have no AIControllerClass of their own (live
// 2026-10-17 22:21: "got no controller", the ship sat at its spawn point).
// Fallback: the Horde "Hard" behaviour controller for the class and weight
// (Generic/AI/PVE/Horde/Difficult, names from the pak listing). GUESS: the
// pairing; the DreadH boss's own was BunBunBoss_Hard (DreadnoughtMedium).
static void *ShipIdControllerClass(const char *cw) {
  static const struct { const char *cw, *name; } tbl[] = {
      {"AssaultHeavy", "VH_AiHordeAssaultHeavy_Behavior_Goliath_Hard_YAICtrl_BP"},
      {"AssaultMedium", "VH_AIHordeAssaultMedium_Behavior_MissileBoat_Hard_YAICtrl_BP"},
      {"AssaultLight", "VH_AiHordeAssaultLight_Behavior_Rammer_Hard_YAICtrl_BP"},
      {"DreadnoughtHeavy", "VH_AiHordeDreadnoughtHeavy_Behavior_ArmoredNuker_Hard_YAICtrl_BP"},
      {"DreadnoughtMedium", "VH_AiHordeDreadnoughtMedium_Behavior_BunBunBoss_Hard_YAICtrl_BP"},
      {"DreadnoughtLight", "VH_AiHordeDreadnoughtLight_Behavior_Broadside_Hard_YAICtrl_BP"},
      {"ScoutHeavy", "VH_AiHordeScoutHeavy_Behavior_Ambusher_Hard_YAICtrl_BP"},
      {"ScoutMedium", "VH_AiHordeScoutMedium_Behavior_Fetch_Hard_YAICtrl_BP"},
      {"ScoutLight", "VH_AiHordeScoutLight_Behavior_Aggressive_Hard_YAICtrl_BP"},
      {"SniperHeavy", "VH_AiHordeSniperHeavy_Behavior_Fixed_Hard_YAICtrl_BP"},
      {"SniperMedium", "VH_AiHordeSniperMedium_Behavior_Standard_Hard_YAICtrl_BP"},
      {"SniperLight", "VH_AiHordeSniperLight_Behavior_Hunter_Hard_YAICtrl_BP"},
      {"SupportHeavy", "VH_AiHordeSupportHeavy_Behavior_Standard_Hard_YAICtrl_BP"},
      {"SupportMedium", "VH_AiHordeSupportMedium_Behavior_JumpHealer_Hard_YAICtrl_BP"},
      {"SupportLight", "VH_AiHordeSupportLight_Behavior_StasisBeam_Hard_YAICtrl_BP"},
  };
  for (auto &t : tbl) {
    if (strcmp(t.cw, cw))
      continue;
    wchar_t path[256];
    swprintf(path, 256, L"/Game/Generic/AI/PVE/Horde/Difficult/%hs.%hs_C", t.name, t.name);
    UObjectMin *uc = ResolveUClassClass(), *k = nullptr;
    __try {
      k = uc ? (UObjectMin *)((tStaticLoadClass)(g_base + RVA_STATIC_LOAD_CLASS))(uc, nullptr, path, nullptr, 0, nullptr, false)
             : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      k = nullptr;
    }
    if (!IsReadable(k, sizeof(UObjectMin)))
      return Logf("commands: spawn -- controller %hs did not load", t.name), nullptr;
    PinToRootSet(k);
    return k;
  }
  return nullptr;
}

// The Havoc dataset row's abilities for `ship` (npc_abilities.h, from
// HVC_DeadHorde_EnemyShips), installed on a /spawn pawn. A directly spawned
// pawn's controller logs "fail ability index=N : ability set to trigger
// inexistent pawn ability index=-1" every half second, so the ship only shoots
// its guns (live 2026-10-17, DreadnoughtHeavyNukerT5). CORRECTION: the
// manager is NOT empty (boss9: 6 entries as spawned), and installing the
// dataset row before (boss10) or after (boss9) possess leaves the errors
// unchanged. The -1 is the controller's slot table (ctrl+0x860, stride
// 0xD0, +8 = pawn ability index; or the map lookup 0x288A10 on ctrl+0x870
// when ctrl+0x710 is set), read by the trigger chunk at 0x2AAAA4. What fills
// it is still unknown. Kept because the dataset abilities are right. AYPawn::InitAbilities (0x57CB70) turns the item
// IDs at pawn+0x8E0 into ability classes and hands the class array to
// 0x259DC0(pawn->m_abilityManager (+0x888), const TArray<UClass*>&), then
// frees its own array, so the callee copies. The manager's ability list is
// +0xF8 (count +0x100), what the controller indexes (0x2AD9CC).
#define RVA_ABILITY_MGR_SET_ABILITIES 0x259DC0
typedef void(__fastcall *tSetAbilities)(void *mgr, const TArrayIntMin *classes);
static void InstallNpcAbilities(void *pawn, const char *ship) {
  const NpcAbilities *row = nullptr;
  for (const NpcAbilities &r : kNpcAbilities)
    if (!strcmp(r.ship, ship))
      row = &r;
  if (!row)
    return (void)Logf("commands: spawn %s -- no ability row", ship);
  InstallAbilityClasses(pawn, ship, row->path, 8);
}
// Loads each class path (empty ones skipped) and hands the classes to the
// pawn's ability manager in that order.
static void InstallAbilityClasses(void *pawn, const char *ship, const wchar_t *const *paths, int npaths) {
  void *mgr = pawn && IsReadable((uint8_t *)pawn + OFF_PAWN_ABILITY_MANAGER, 8)
                  ? *(void **)((uint8_t *)pawn + OFF_PAWN_ABILITY_MANAGER)
                  : nullptr;
  if (!mgr)
    return (void)Logf("commands: spawn %s -- no ability manager", ship);
  void *cls[8] = {};
  int n = 0;
  UObjectMin *uc = ResolveUClassClass();
  for (int pi = 0; pi < npaths && n < 8; ++pi) {
    const wchar_t *path = paths[pi];
    if (!path || !path[0] || !uc)
      continue;
    UObjectMin *k = nullptr;
    __try {
      k = (UObjectMin *)((tStaticLoadClass)(g_base + RVA_STATIC_LOAD_CLASS))(uc, nullptr, path, nullptr, 0, nullptr, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      k = nullptr;
    }
    if (!IsReadable(k, sizeof(UObjectMin))) {
      Logf("commands: spawn %s -- ability %ls did not load", ship, path);
      continue;
    }
    PinToRootSet(k);
    cls[n++] = k;
  }
  int before = IsReadable((uint8_t *)mgr + 0x100, 4) ? *(int32_t *)((uint8_t *)mgr + 0x100) : -1, after = -1;
  // TArray<UClass*> header: data, num, max (same layout as TArrayIntMin).
  TArrayIntMin arr = {(int32_t *)cls, n, n};
  __try {
    ((tSetAbilities)(g_base + RVA_ABILITY_MGR_SET_ABILITIES))(mgr, &arr);
    after = *(int32_t *)((uint8_t *)mgr + 0x100);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return (void)Logf("commands: spawn %s -- installing abilities faulted", ship);
  }
  Logf("commands: spawn %s -- %d ability classes installed, ability list %d -> %d", ship, n, before, after);
}

static void *ShipIdPawnClass(const char *ship, bool havocBoss = true) {
  static const char *cls[] = {"Assault", "Dreadnought", "Scout", "Sniper", "Support"};
  static const char *wts[] = {"Heavy", "Medium", "Light"};
  const char *c = nullptr, *rest = ship;
  for (const char *k : cls)
    if (!strncmp(ship, k, strlen(k))) {
      c = k, rest = ship + strlen(k);
      break;
    }
  char w = 0;
  for (const char *k : wts)
    if (c && !strncmp(rest, k, strlen(k))) {
      w = k[0], rest += strlen(k);
      break;
    }
  if (!c || !w)
    return nullptr;
  const char *cs = !strcmp(c, "Dreadnought") ? "Dread" : c;
  int tier = 0;
  for (const char *q = rest; *q; ++q)
    if (*q == 'T' && q[1] >= '1' && q[1] <= '9' && !q[2])
      tier = q[1] - '0';
  wchar_t paths[3][192];
  int np = 0;
  if (strstr(rest, "Boss") && !havocBoss)
    tier = 5; // the class's T5 AI pawn stands in (CommandSpawn)
  if (strstr(rest, "Boss") && havocBoss)
    swprintf(paths[np++], 192, L"/Game/Generic/AI/AIPawns/Havoc/Bosses/HVC_VH_%hs%c_Boss_01_Pawn_BP.HVC_VH_%hs%c_Boss_01_Pawn_BP_C",
             cs, w, cs, w);
  if (tier)
    swprintf(paths[np++], 192, L"/Game/Generic/AI/AIPawns/VH_%hs%c_AI_Pawn_T%d_BP.VH_%hs%c_AI_Pawn_T%d_BP_C", cs, w, tier, cs, w,
             tier);
  swprintf(paths[np++], 192, L"/Game/Generic/AI/AIPawns/VH_%hs%c_AI_Pawn_BP.VH_%hs%c_AI_Pawn_BP_C", cs, w, cs, w);
  UObjectMin *uc = ResolveUClassClass();
  for (int i = 0; uc && i < np; ++i) {
    UObjectMin *k = nullptr;
    __try {
      k = (UObjectMin *)((tStaticLoadClass)(g_base + RVA_STATIC_LOAD_CLASS))(uc, nullptr, paths[i], nullptr, 0, nullptr, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      k = nullptr;
    }
    if (IsReadable(k, sizeof(UObjectMin))) {
      PinToRootSet(k);
      Logf("commands: spawn %s -> %ls", ship, paths[i]);
      return k;
    }
  }
  Logf("commands: spawn %s -- no AI pawn class (%d paths tried)", ship, np);
  return nullptr;
}

// /spawn <ShipId> [n] (CommandsTick): n enemies of one dataset ship now. Each
// is a DuplicateSceneEntry copy of a scene entry the map itself registered
// (newest listed enemy, else the newest scene entry), so it keeps that entry's
// spawn point and AI setup; only the dataset pawn id FName at +0x18 is
// swapped, as /enemy does before the first spawn. Copies are listed as wave
// enemies (kill count, XP drop, wave end waits for them) and m_maxWaveEnemies
// grows with them so the progress bar stays a fraction.
// GUESS: an entry whose time (+OFF_NPCSPAWN_TIME) is already past spawns on
// the next scene tick; the 1.5 s stagger is DuplicateSceneEntry's.
// Outside Havoc (operator 10-16: "don't lock fun stuff to just havoc"):
// Onslaught gets n extra enemy ships at its assault-ship spawn points: the
// ShipId's AI pawn (ShipIdPawnClass) through SpawnOnslaughtAIShip, or with no
// such pawn an assault ship from its own spawner (SpawnOnslaughtCreep size 1).
// Elites (/boss) are named "Boss <ShipId>". Copying a scene entry there
// failed -- the map's NPC data asset has no Havoc ship rows ("invalid spawn
// Pawn by dataset: ShipId=AssaultHeavyT4, no base pawn in dataAsset", retried
// every frame, onsfun1 23:27). Other modes have no NPC spawner to use.
static struct { void *pawn; DWORD at; } g_lastBoss = {}; // /swarm and /squad gather here within 20 s (/wave)
static void *g_lastSpawnPawn = nullptr;               // Onslaught /spawn's last AI ship (the herald reads it)
static const wchar_t *g_spawnLabel = nullptr;         // overrides the /spawn name tag
// TDM, Pod TDM, Conquest and TE have no Onslaught spawn points; /spawn uses a
// player start there (class name containing "PlayerStart"), the one farthest
// from every human ship.
static int FindPlayerStarts(void **out, int max) {
  FUObjectArrayMin *arr = GObjects();
  int n = 0;
  if (!arr || !arr->Objects)
    return 0;
  for (int i = 0; i < arr->NumElements && n < max; ++i) {
    UObjectMin *o = arr->Objects[i].Object;
    if (!o || (o->Flags & RF_CDO_OR_ARCHETYPE) || (arr->Objects[i].Flags & OBJ_PENDING_KILL) || !o->Class)
      continue;
    const char *cn = NameText(o->Class->Name);
    if (cn && strstr(cn, "PlayerStart") && *(void **)((uint8_t *)o + OFF_ACTOR_ROOT_COMPONENT))
      out[n++] = o;
  }
  return n;
}
// nearest: dn_host_spawn_near.txt, a test switch -- does a player-start AI ship
// fight once a target is in range? (spawn-fight 2026-10-20: at the farthest
// start, 4.3-6.8 km out, it never moved or fired in TDM/PodTDM/TER/Havoc.)
static void *FarthestStart(void **starts, int ns, uint8_t **pcs, int np, bool nearest = false) {
  void *best = starts[rand() % ns];
  double bestD = nearest ? 1e300 : -1;
  for (int s = 0; s < ns; ++s) {
    float a[3];
    if (!ActorLocation(starts[s], a))
      continue;
    double d = 1e30;
    for (int i = 0; i < np; ++i) {
      void *hp = IsReadable(pcs[i] + OFF_CONTROLLER_PAWN, 8) ? *(void **)(pcs[i] + OFF_CONTROLLER_PAWN) : nullptr;
      float h[3];
      if (hp && ActorLocation(hp, h))
        d = std::min(d, (double)(a[0] - h[0]) * (a[0] - h[0]) + (double)(a[1] - h[1]) * (a[1] - h[1]) + (double)(a[2] - h[2]) * (a[2] - h[2]));
    }
    if (nearest ? d < bestD : d > bestD)
      bestD = d, best = starts[s];
  }
  return best;
}

// PvP /spawn: one more bot of the game's own kind, flying ShipId `ship`.
// Player-start /spawn ships never moved or fired there (spawn-fight
// 2026-10-20); the game's bots fight. The fill 0x381FA0 calls SpawnNPC
// (0x3671C0, gm, entry, &count1, &count2) for every bot list entry in order;
// an entry is spawned when its weak info (gm+0x9A0) is dead and its team's
// count is under the target (gm+0x980/+0x984), so the first free entry of the
// team is the one the next bot takes: its ShipId list (+0x10) is overwritten
// with `ship` (see botship above), its name (+0x00 FName) set to `label`, and
// the target raised through SetTeamSizeAI (0x381550). SpawnNPC names the info
// from the captain preset (+0x28) when there is one, else from +0x00
// (0x367446-0x36747C), so the preset is cleared. GUESS: nothing else needs
// the preset (it also gives the captain's portrait). The entry keeps the ship
// for the rest of the match, so the bot respawns as it, as fill bots respawn.
// The bot's tier must match the ship's (tier block in HookGameModeTimer): TDM
// 2026-10-20 runs 7b/8 got random ships at T1-2, run 9 AssaultHeavyT4 at T4.
static void ExtraBotsFor(uint8_t *gm) {
  if (g_extraBotsGm != gm) {
    g_extraBotsGm = gm, g_extraOnly = false, g_extraTier = 0;
    memset(g_extraBots, 0, sizeof g_extraBots);
    memset(g_extraUsed, 0, sizeof g_extraUsed);
    memset(g_gbElite, 0, sizeof g_gbElite);
  }
}

// The /spawn'd bots' tier follows the ShipId's T<n> (see the tier block in
// HookGameModeTimer).
static void NoteExtraTier(const char *used) {
  size_t len = strlen(used), p = len;
  while (p > 0 && isdigit((unsigned char)used[p - 1]))
    --p;
  int t = p < len && p > 0 && (used[p - 1] == 'T' || used[p - 1] == 't') ? atoi(used + p) : 0;
  if (t > 0) {
    t = std::min(t, 5);
    if (g_extraTier && g_extraTier != t)
      Logf("commands: spawn %s -- tier %d replaces %d for /spawn'd bots; the earlier ones' ships need tier %d", used, t,
           g_extraTier, g_extraTier);
    g_extraTier = t;
  }
}

// /spawn before the pre-match fill. The fill REBUILDS the bot list
// (gamebot-boss12, TDM 2026-10-20: SpawnGameBot took entry 21 as the first
// free team 2 entry at 21:42:21; after the fill entry 7 was a live team 2 bot
// with the full T5 list, ours gone), so an entry rewritten before it is lost --
// boss10/11's wrong ships, and why spawn9 (after the fill) worked. Such
// spawns wait here; HookGmSetupAI places them right after the fill, through
// the after-the-fill path (SpawnGameBot raising the target).
struct PendingGameBot {
  char ship[64];
  wchar_t label[64];
  int team, mult;
  const BossDef *def;
};
static PendingGameBot g_pendingGB[8];
static int g_pendingGBn = 0;
static void *g_pendingGBGm = nullptr;

static bool SpawnGameBot(uint8_t *gm, int team, const char *ship, const wchar_t *label, bool swap = false, bool raise = true) {
  int n = IsReadable(gm + OFF_GM_NPC_COUNT, 4) ? *(int32_t *)(gm + OFF_GM_NPC_COUNT) : 0;
  uint8_t *list = n > 0 && IsReadable(gm + 0x968, 8) ? *(uint8_t **)(gm + 0x968) : nullptr;
  if (!list || !IsReadable(gm + 0x9A0, 16) || !IsReadable(gm + OFF_GM_AI_TARGET_T1, 8))
    return false;
  NpcIdsSnapshot(gm);
  ExtraBotsFor(gm);
  uint8_t *infos = *(uint8_t **)(gm + 0x9A0);
  int ninfo = *(int32_t *)(gm + 0x9A8);
  int idx = -1;
  for (int i = 0; i < n && i < 128 && idx < 0; ++i) {
    uint8_t *e = list + (size_t)i * 0x38;
    if (IsReadable(e, 0x38) && e[8] == team && !g_extraUsed[i] &&
        (i >= ninfo || !((tWeakGet)(g_base + RVA_WEAK_PTR_GET))(infos + (size_t)i * 8)))
      idx = i;
  }
  if (idx < 0)
    return Logf("commands: spawn %s -- no free team %d entry in the bot list (%d entries)", ship, team, n), false;
  uint8_t *e = list + (size_t)idx * 0x38;
  FNameMin id = {}, nm = {};
  char used[64];
  if (!NpcIdResolve(ship, &id, used, sizeof used, swap))
    return Logf("commands: spawn %s -- no ship of that class in this mode's bot lists (%d ShipIds)", ship, g_npcIdCount), false;
  if (_stricmp(used, ship))
    Logf("commands: spawn %s -- not in this mode's bot lists, using %s", ship, used);
  NoteExtraTier(used);
  ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&nm, label, 1 /* FNAME_Add */);
  uint64_t *ids = *(uint64_t **)(e + 0x10);
  int k = *(int32_t *)(e + 0x18);
  if (!ids || k < 1 || !IsReadable(ids, (size_t)k * 8))
    return Logf("commands: spawn %s -- bot list entry %d has no ShipId list", ship, idx), false;
  for (int j = 0; j < k; ++j)
    ids[j] = *(uint64_t *)&id;
  *(FNameMin *)e = nm;
  *(void **)(e + 0x28) = nullptr;
  g_extraUsed[idx] = true;
  g_lastGameBotIdx = idx;
  ++g_extraBots[team];
  if (!gm[OFF_GM_ENABLE_SPAWN_AI])
    gm[OFF_GM_ENABLE_SPAWN_AI] = 1, g_extraOnly = true;
  int32_t *target = (int32_t *)(gm + OFF_GM_AI_TARGET_T1 + (team - 1) * 4);
  if (!raise)
    return Logf("commands: spawn %s -> game bot, list entry %d (team %d), for the pre-match fill%s", used, idx, team,
                g_extraOnly ? " (only /spawn'd bots)" : ""), true;
  int was = *target, want = g_extraOnly ? g_extraBots[team] : was + 1;
  __try {
    if (g_extraOnly)
      ((tSetTeamSizeAI)(g_base + RVA_SET_TEAM_SIZE_AI))(gm, 3 - team, g_extraBots[3 - team]);
    ((tSetTeamSizeAI)(g_base + RVA_SET_TEAM_SIZE_AI))(gm, team, want);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return Logf("commands: spawn %s -- SetTeamSizeAI faulted", ship), false;
  }
  Logf("commands: spawn %s -> game bot, list entry %d (team %d), team %d bots %d -> %d%s", used, idx, team, team, was, want,
       g_extraOnly ? " (only /spawn'd bots)" : "");
  return true;
}

static void PlayerStateName(void *psv, char *out, int cap); // viewer snapshot
static void __fastcall HookGmSetupAI(void *gameMode, uint64_t arg) {
  uint8_t *gm = (uint8_t *)gameMode;
  bool pend = g_pendingGBGm == gm && g_pendingGBn > 0;
  if (!pend && (g_extraBotsGm != gm || !g_extraOnly))
    return g_origGmSetupAI(gameMode, arg);
  if (!IsReadable(gm + 0x97C, 12))
    return g_origGmSetupAI(gameMode, arg);
  int32_t *size = (int32_t *)(gm + 0x97C), was = *size;
  bool only = g_extraBotsGm == gm && g_extraOnly;
  if (only) {
    *size = 0;
    for (int t = 1; t <= 2; ++t)
      size[t] = g_extraBots[t];
  }
  static int s_told = 0;
  if (s_told++ < 4)
    Logf("bots: setup (0x3678F0) with only /spawn'd bots: targets %d/%d, not team size %d", g_extraBots[1],
         g_extraBots[2], was);
  __try {
    g_origGmSetupAI(gameMode, arg);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("bots: setup (0x3678F0) faulted");
  }
  if (only)
    *size = was;
  if (pend) {
    int n = g_pendingGBn;
    g_pendingGBn = 0;
    for (int i = 0; i < n; ++i) {
      PendingGameBot &q = g_pendingGB[i];
      if (!SpawnGameBot(gm, q.team, q.ship, q.label, q.def || q.mult > 1))
        continue;
      if (q.mult > 1 && g_lastGameBotIdx >= 0 && g_lastGameBotIdx < 128)
        g_gbElite[g_lastGameBotIdx] = {q.mult, q.def, nullptr, GetTickCount(), false};
    }
    Logf("bots: setup (0x3678F0): %d queued /spawn(s) placed after the fill", n);
  }
  // Which entries hold a live info now, and their first ShipId.
  if (s_told <= 4 && IsReadable(gm + 0x9A0, 16) && IsReadable(gm + 0x968, 16)) {
    uint8_t *infos = *(uint8_t **)(gm + 0x9A0), *list = *(uint8_t **)(gm + 0x968);
    int ninfo = *(int32_t *)(gm + 0x9A8), n = *(int32_t *)(gm + 0x970);
    for (int i = 0; i < ninfo && i < n && i < 128; ++i) {
      uint8_t *e = list + (size_t)i * 0x38;
      if (!IsReadable(e, 0x38) || !IsReadable(infos + (size_t)i * 8, 8) ||
          !((tWeakGet)(g_base + RVA_WEAK_PTR_GET))(infos + (size_t)i * 8))
        continue;
      uint64_t *ids = *(uint64_t **)(e + 0x10);
      const char *id0 = ids && *(int32_t *)(e + 0x18) > 0 && IsReadable(ids, 8) ? NameText(*(FNameMin *)ids) : nullptr;
      // The name check (2026-10-20, gamebot-boss15: the viewer showed no
      // bot name): the entry's FName (SpawnNPC passes it to vt+0x5E0) and
      // the info's PlayerName as the viewer reads it.
      char psName[80] = "-";
      PlayerStateName(((tWeakGet)(g_base + RVA_WEAK_PTR_GET))(infos + (size_t)i * 8), psName, sizeof psName);
      const char *en = NameText(*(FNameMin *)e);
      Logf("bots:   entry %d team %d live, ShipId[0] %s%s, entry name %s, PlayerName %s", i, e[8], id0 ? id0 : "?",
           g_extraUsed[i] ? " (/spawn'd)" : "", en ? en : "?", psName);
    }
  }
}

static void CommandSpawn(uint8_t *gm, const char *ship, int n, int mult) {
  g_lastSpawnPawn = nullptr;
  uint8_t *scene = (uint8_t *)((tGetSceneManager)(g_base + RVA_COMBAT_SCENE_MANAGER))();
  if (!scene || !IsReadable(scene + OFF_SCENE_NPCS, 16)) {
    Logf("commands: spawn %s -- no combat scene", ship);
    return;
  }
  bool havoc = HostIsHavoc() || HostIsHorde(); // both have the wave scene entries to copy
  int cnt = *(int32_t *)(scene + OFF_SCENE_NPCS_COUNT);
  int listed = std::min((int)g_hvcEnemyCount, 256);
  int tpl = listed > 0 ? (int)g_hvcEnemyIds[listed - 1] : cnt - 1;
  // Before wave 1 the scene is empty ("no scene entry to copy (count 0)",
  // Havoc/Horde matrix 2026-10-19): spawn at a player start like TDM.
  if (havoc && (tpl < 0 || tpl >= cnt))
    Logf("commands: spawn %s -- no scene entry to copy (count %d), using the player-start spawner", ship, cnt);
  if (!havoc || tpl < 0 || tpl >= cnt) {
    uint8_t *gs = IsReadable(gm + OFF_GM_GAMESTATE, 8) ? *(uint8_t **)(gm + OFF_GM_GAMESTATE) : nullptr;
    // With dn_host_no_onslaught_creeps.txt OnslaughtCreepTick never runs, so
    // nothing scanned the spawn points ("/boss did not work", 2026-10-17).
    if (gs && InvasionGameState(gm) && (g_onsCreeps.gm != gm || !g_onsCreeps.nPoints)) {
      if (g_onsCreeps.gm != gm) {
        memset(&g_onsCreeps, 0, sizeof(g_onsCreeps));
        g_onsCreeps.gm = gm;
        g_onsCreeps.firstSeen = GetTickCount();
      }
      FindOnslaughtSpawnPoints();
      Logf("commands: spawn -- scanned %d Onslaught spawn points", g_onsCreeps.nPoints);
    }
    void *starts[64];
    int ns = gs && !g_onsCreeps.nPoints ? FindPlayerStarts(starts, 64) : 0;
    if ((!g_onsCreeps.nPoints && !ns) || !gs)
      return (void)Logf("commands: spawn %s -- no NPC spawner in this mode (no spawn points, no player starts)", ship);
    int humanTeam = 1; // GUESS as in CommandNuke when no human ship is alive
    uint8_t *pcs[64];
    int np = CollectHumanPCs(gm, pcs, 64);
    for (int i = 0; i < np; ++i) {
      uint8_t *hp = IsReadable(pcs[i] + OFF_CONTROLLER_PAWN, 8) ? *(uint8_t **)(pcs[i] + OFF_CONTROLLER_PAWN) : nullptr;
      if (hp && IsReadable(hp, OFF_SHIP_TEAM + 1) && (hp[OFF_SHIP_TEAM] == 1 || hp[OFF_SHIP_TEAM] == 2)) {
        humanTeam = hp[OFF_SHIP_TEAM];
        break;
      }
    }
    int made = 0;
    void *hull0 = ShipIdPawnClass(ship, false);
    if (hull0 && !InvasionGameState(gm) && !havoc && !g_spawnNoGameBot) {
      wchar_t lbl[64];
      swprintf(lbl, 64, mult >= 4 ? L"Boss %hs" : L"%hs", ship);
      // Before the NPC set has loaded (GameState+0x1D90 m_cachedNPCSet null)
      // an added info never got a ship: the chooser 0x371B50 logged "there is
      // no dataset in the gamemode or combat manager for the training match"
      // every second until LoadNPCSet ran at the pre-match fill, and nothing
      // spawned after it (TDM 2026-10-20). The set loads at the fill, which
      // runs only with the spawner on (with bc_only nothing loaded). Raising
      // the target after the fill lost to a fill bot spawned in the fill's
      // own tick (SupportMediumT1 from another entry took the one slot). So
      // before the fill only the entries are set and counted: the fill takes
      // free entries in list order, ours first, and bot balance trims to them.
      // WRONG (gamebot-boss10-12): the fill rebuilds the list, the rewritten
      // entry is lost. Now they wait in g_pendingGB and HookGmSetupAI places
      // them right after the fill (see PendingGameBot).
      uint8_t *gs2 = IsReadable(gm + OFF_GM_GAMESTATE, 8) ? *(uint8_t **)(gm + OFF_GM_GAMESTATE) : nullptr;
      bool early = !gs2 || !IsReadable(gs2 + 0x1D90, 8) || !*(void **)(gs2 + 0x1D90);
      if (early) {
        FNameMin rid = {};
        char used[64];
        NpcIdsSnapshot(gm);
        if (!NpcIdResolve(ship, &rid, used, sizeof used, g_spawnDef || mult > 1)) {
          Logf("commands: spawn %s -- no ship of that class in this mode's bot lists (%d ShipIds)", ship, g_npcIdCount);
          return;
        }
        ExtraBotsFor(gm);
        if (g_pendingGBGm != gm)
          g_pendingGBGm = gm, g_pendingGBn = 0;
        for (int k = 0; k < n && g_pendingGBn < 8; ++k, ++made) {
          PendingGameBot &q = g_pendingGB[g_pendingGBn++];
          strncpy_s(q.ship, sizeof q.ship, ship, _TRUNCATE);
          wcsncpy_s(q.label, 64, g_spawnLabel ? g_spawnLabel : lbl, _TRUNCATE);
          q.team = 3 - humanTeam, q.mult = mult, q.def = g_spawnDef;
        }
        NoteExtraTier(used);
        if (IsReadable(gm + OFF_GM_ENABLE_SPAWN_AI, 1) && !gm[OFF_GM_ENABLE_SPAWN_AI])
          gm[OFF_GM_ENABLE_SPAWN_AI] = 1, g_extraOnly = true;
        Logf("commands: spawn %s (%s) x%d for team %d -- waits for the pre-match fill (%d queued)%s", ship, used, made,
             3 - humanTeam, g_pendingGBn, g_extraOnly ? " (only /spawn'd bots)" : "");
        return;
      }
      for (int k = 0; k < n && SpawnGameBot(gm, 3 - humanTeam, ship, g_spawnLabel ? g_spawnLabel : lbl, g_spawnDef || mult > 1); ++k) {
        ++made;
        if (mult > 1 && g_lastGameBotIdx >= 0 && g_lastGameBotIdx < 128)
          g_gbElite[g_lastGameBotIdx] = {mult, g_spawnDef, nullptr, GetTickCount(), false};
      }
      if (made) {
        Logf("commands: spawn (game bots) %d of %d %s for team %d%s", made, n, ship, 3 - humanTeam,
             mult > 1 ? ", elite once each ship is up (GameBotEliteTick)" : "");
        return;
      }
    }
    // Outside Havoc a *Boss01 id flies its class's T5 AI pawn: loading the
    // Havoc boss pawn class there returned nothing and logged nothing, the
    // rest of the spawn included (Derelict Onslaught 14:03 and 14:47,
    // 2026-10-18).
    Logf("commands: spawn %s x%d (elite x%d) -- %s", ship, n, mult, ns ? "player start" : "Onslaught");
    void *hull = ShipIdPawnClass(ship, false);
    wchar_t label[64];
    swprintf(label, 64, mult >= 4 ? L"Boss %hs" : L"%hs", ship);
    for (int k = 0; k < n; ++k) {
      g_onsLastNpcId = -1;
      if (hull) {
        int pts[256], np2 = 0;
        for (int i = 0; i < g_onsCreeps.nPoints && np2 < 256; ++i)
          if (g_onsCreeps.points[i].team == 3 - humanTeam && g_onsCreeps.points[i].size == 1)
            pts[np2++] = i;
        void *pawn = nullptr;
        void *at = np2 ? g_onsCreeps.points[pts[rand() % np2]].actor : ns ? FarthestStart(starts, ns, pcs, np, SwitchOn("DN_HOST_SPAWN_NEAR", "dn_host_spawn_near.txt")) : nullptr;
        if (at) {
          g_onsNextName = g_spawnLabel ? g_spawnLabel : label;
          char cw[32] = {};
          static const char *cs[] = {"Assault", "Dreadnought", "Scout", "Sniper", "Support"}, *ws[] = {"Heavy", "Medium", "Light"};
          for (const char *c : cs)
            for (const char *w : ws)
              if (!cw[0] && !strncmp(ship, c, strlen(c)) && !strncmp(ship + strlen(c), w, strlen(w)))
                snprintf(cw, sizeof cw, "%s%s", c, w);
          g_onsNextCtrlCls = ShipIdControllerClass(cw); // used only if the pawn names none
          g_onsNextShip = ship;
          __try {
            g_spawnAtStart = !np2;
            pawn = SpawnOnslaughtAIShip(gm, hull, at, 3 - humanTeam, false);
          } __except (EXCEPTION_EXECUTE_HANDLER) {
            Logf("commands: spawn %s -- AI ship spawn faulted", ship);
          }
          g_spawnAtStart = false;
          g_onsNextName = nullptr;
          g_onsNextCtrlCls = nullptr;
          g_onsNextShip = nullptr;
        }
        Logf("commands: spawn %s -> %p (%s) at one of %d assault points / %d player starts, scene NPC id %d", ship, pawn,
             pawn ? ClassNameOf(pawn) : "-", np2, ns, g_onsLastNpcId);
        if (!pawn)
          break;
        g_lastSpawnPawn = pawn;
        // /boss: team 3, its own side (purple on the client), fights both.
        // Was mult >= 4 only: the custom boss Gorgon (x3) and a bare /boss
        // (x2) came in on the enemy team (operator 2026-10-21).
        size_t sl = strlen(ship);
        if (mult >= 4 || (g_spawnDef && !g_spawnDef->hunter) || (sl > 6 && !strcmp(ship + sl - 6, "Boss01"))) {
          uint8_t t0 = 3;
          __try {
            ((tPawnSetTeam)(g_base + RVA_PAWN_SET_TEAM))(pawn, &t0);
          } __except (EXCEPTION_EXECUTE_HANDLER) {
            Logf("commands: spawn %s -- team 3 faulted", ship);
          }
          g_lastBoss = {pawn, GetTickCount()};
        }
      } else if (!SpawnOnslaughtCreep(gm, gs, 3 - humanTeam, 1, false))
        break;
      ++made;
      if ((mult > 1 || g_spawnDef) && g_onsLastNpcId >= 0)
        for (EliteSpawn &el : g_elite)
          if (!el.mult) {
            el = {g_onsLastNpcId, mult, GetTickCount(), g_spawnDef};
            break;
          }
    }
    Logf("commands: spawn (onslaught) %d of %d enemy %s for team %d", made, n, hull ? ship : "assault ships", 3 - humanTeam);
    return;
  }
  wchar_t w[32];
  MultiByteToWideChar(CP_UTF8, 0, ship, -1, w, 32);
  FNameMin name = {};
  ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&name, w, 1);
  int made = 0;
  for (int k = 0; k < n; ++k) {
    int nid = DuplicateSceneEntry(scene, tpl, 1.5f * k);
    if (nid < 0)
      break;
    uint8_t *e = *(uint8_t **)(scene + OFF_SCENE_NPCS) + (size_t)nid * NPC_SPAWN_SIZE;
    memcpy(e + OFF_NPCSPAWN_DATASET_ID, &name, sizeof(FNameMin));
    if (nid < (int)sizeof g_botsSeen)
      g_botsSeen[nid] = 1; // not multiplied again by /bots
    if (mult > 1 || g_spawnDef)
      for (EliteSpawn &el : g_elite)
        if (!el.mult) {
          el = {nid, mult, GetTickCount(), g_spawnDef};
          break;
        }
    ++made;
    LONG j = InterlockedIncrement(&g_hvcEnemyCount) - 1;
    if (j < 256)
      g_hvcEnemyIds[j] = nid;
    else
      InterlockedDecrement(&g_hvcEnemyCount);
  }
  uint8_t *gs = IsReadable(gm + OFF_GM_GAMESTATE, 8) ? *(uint8_t **)(gm + OFF_GM_GAMESTATE) : nullptr;
  if (gs && made)
    *(int32_t *)(gs + OFF_HVC_GS_MAX_WAVE_ENEMIES) += made;
  Logf("commands: spawn %s x%d (elite x%d) from scene entry %d -> %d made (scene npcs %d)", ship, n, mult, tpl, made,
       *(int32_t *)(scene + OFF_SCENE_NPCS_COUNT));
}

// /boss <id> [mult] (mmogbrain admin_bosses.go): fetch the custom boss and
// spawn it as /spawn <its base ShipId> 1 <mult>, named, with its coatings,
// modules and feats applied by ApplyElite.
static void SplitWide(const char *src, char sep, wchar_t *out, int stride, int maxN, int *n) {
  *n = 0;
  for (const char *p = src; p && *p && *n < maxN;) {
    const char *e = strchr(p, sep);
    int len = e ? (int)(e - p) : (int)strlen(p);
    if (len > 0 && len < stride && len < 256) {
      char tmp[256];
      memcpy(tmp, p, len);
      tmp[len] = 0;
      MultiByteToWideChar(CP_UTF8, 0, tmp, -1, out + (size_t)*n * stride, stride);
      ++*n;
    }
    p = e ? e + 1 : nullptr;
  }
}
// ParseBossDef fills the next ring slot from one key=value block (the
// /battle/boss body, or one "--" block of /battle/hunters).
static BossDef &ParseBossDef(const char *body, int id) {
  char v[2048];
  BossDef &d = g_bossDefs[g_bossDefNext++ % 32];
  memset(&d, 0, sizeof d);
  d.id = id;
  FieldValue(body, "ship", d.ship, sizeof d.ship);
  FieldValue(body, "name", v, sizeof v);
  MultiByteToWideChar(CP_UTF8, 0, v, -1, d.name, 32);
  d.name[31] = 0;
  d.paint = atoi(FieldValue(body, "paint", v, sizeof v) ? v : "0");
  d.pattern = atoi(FieldValue(body, "pattern", v, sizeof v) ? v : "0");
  d.decal = atoi(FieldValue(body, "decal", v, sizeof v) ? v : "0");
  d.emblem = atoi(FieldValue(body, "emblem", v, sizeof v) ? v : "0");
  if (FieldValue(body, "abilities", v, sizeof v))
    SplitWide(v, '|', &d.abil[0][0], 192, 8, &d.nAbil);
  wchar_t feats[12][96];
  int nf = 0;
  if (FieldValue(body, "feats", v, sizeof v))
    SplitWide(v, '|', &feats[0][0], 96, 12, &nf);
  for (int i = 0; i < nf; ++i) {
    wchar_t *star = wcschr(feats[i], L'*');
    if (!star)
      continue;
    *star = 0;
    wcsncpy(d.feat[d.nFeat], feats[i], 63);
    d.stacks[d.nFeat++] = std::min(40, std::max(0, _wtoi(star + 1)));
  }
  ParseInts(FieldValue(body, "parts", v, sizeof v), d.parts, 4);
  if (FieldValue(body, "officers", v, sizeof v))
    SplitWide(v, '|', &d.officer[0][0], 32, 4, &d.nOfficer);
  d.marker = FieldValue(body, "marker", v, sizeof v) && v[0] ? atoi(v) : MARKER_DEFAULT;
  return d;
}
static void CommandBossDef(uint8_t *gm, int id, int mult) {
  static char body[8192];
  char q[64], v[64];
  snprintf(q, sizeof q, "/battle/boss?id=%d", id);
  if (!HttpGetLoopback(q, body, sizeof body) || !FieldValue(body, "ok", v, sizeof v))
    return (void)Logf("commands: boss %d -- no definition from mmogbrain", id);
  BossDef &d = ParseBossDef(body, id);
  if (!d.ship[0])
    return (void)Logf("commands: boss %d -- definition has no ship", id);
  Logf("commands: boss %d \"%ls\" = %s x%d, paint %d pattern %d decal %d emblem %d, %d modules, %d feats", id, d.name,
       d.ship, mult, d.paint, d.pattern, d.decal, d.emblem, d.nAbil, d.nFeat);
  g_spawnLabel = d.name;
  g_spawnDef = &d;
  CommandSpawn(gm, d.ship, 1, mult);
  g_spawnLabel = nullptr;
  g_spawnDef = nullptr;
}

// /meteor [targets] [range] (Onslaught): one herald, a real
// DreadnoughtHeavyNukerT5 from the /spawn path (so FixNpcFireAbilities makes
// its nuke module fire, the way /boss nuked the player with visuals, live
// 2026-10-18), teleported next to each target in turn
// and left to fire on its own. Despawned after the last target. The old
// two-herald /meteor (CommandSkyMeteor) spawned the torpedoes itself: damage
// but no visuals, and its team-1 and team-2 heralds fought each other.
// GUESS: HERALD_RANGE inside the nuke's lock range, HERALD_DWELL long enough
// for the AI to pick the target and launch (cooldowns unknown).
// Live 2026-10-18 (team 255, 6000 uu): it hurt the player but the client drew
// it as a squad mate, it used its guns too, and it fired Tartarus (slot 0,
// AB_DN_Pri_WarpM_Weaken) instead of the plain nuke (AB_DN_Pri_WarpM_Dmg). So
// now: the target's enemy team, every fire slot pointed at WarpM_Dmg, and
// /meteor N R sets the range (GUESS: guns reach less far than the nuke).
// 80000 uu by default (operator 10-19: it fired from there once LOS was cleared).
#define HERALD_RANGE 80000.f
#define HERALD_DWELL_MS 15000
// Up to HERALD_MAX at once ("can't do multiple meteors while the thing is
// still there", operator 2026-10-18). The herald code works on g_herald, the
// one HeraldTick / CommandHeraldMeteor currently points g_heraldP at.
struct HeraldState {
  void *pawn, *ctrl;
  void *tgt[8];
  int nt, cur;
  ULONGLONG nextAt;
  void *shot; // its nuke in flight; on impact it moves on (or leaves after the last target)
  int shotIdx;
  ULONGLONG shotAt, watchAt, sweepAt;
  float range;
  int team;    // /meteor N R TEAM; -1: the target's enemy team
  float angle; // bearing offset (rad) so heralds on one target come from different sides
  int salvos, salvosWant; // /meteor N: N salvos from one herald, then it leaves
  ULONGLONG doneAt;       // when salvos reached salvosWant
  bool abilMissTold;
  void *seen[64];         // missiles already counted
  int nSeen;
  ULONGLONG lastMissileAt;
  const char *abil; // class stem of the module every fire slot is pointed at (null: salvo, else nuke)
};
// Operator 2026-10-21, after the old rain/droplets could not be made visible: warp
// abilities are drawn at their target, so every herald now carries one
// module, installed as its only ability: /meteor the Warp Missile nuke,
// /fighters one dreadnought jet module each (Fighters, Bombers, Interceptor).
struct HeraldModule { const char *stem; const wchar_t *path; const wchar_t *label; };
static const HeraldModule kHeraldNuke = {
    "_WarpM_Dmg_", L"/Game/Generic/Abilities/Dreadnought/Pri_WarpM_Dmg/T5/AB_DN_Pri_WarpM_Dmg_Ability_T5_BP.AB_DN_Pri_WarpM_Dmg_Ability_T5_BP_C",
    L"Comet"};
static const HeraldModule kHeraldJets[3] = {
    {"_Jet_Fighter_", L"/Game/Generic/Abilities/Dreadnought/Pri_Jet_Fighter/T5/AB_DN_Pri_Jet_Fighter_Ability_T5_BP.AB_DN_Pri_Jet_Fighter_Ability_T5_BP_C",
     L"Fighters"},
    {"_Jet_Bomber_", L"/Game/Generic/Abilities/Dreadnought/Pri_Jet_Bomber/T5/AB_DN_Pri_Jet_Bomber_Ability_T5_BP.AB_DN_Pri_Jet_Bomber_Ability_T5_BP_C",
     L"Bombers"},
    {"_Jet_Interceptor_",
     L"/Game/Generic/Abilities/Dreadnought/Pri_Jet_Interceptor/T5/AB_DN_Pri_Jet_Interceptor_Ability_T5_BP.AB_DN_Pri_Jet_Interceptor_Ability_T5_BP_C",
     L"Interceptors"},
};
static const HeraldModule *g_heraldModNext = nullptr; // CommandHeraldMeteorOne: the next herald's module
#define HERALD_MAX 8
static HeraldState g_heralds[HERALD_MAX];
static HeraldState *g_heraldP = &g_heralds[0];
#define g_herald (*g_heraldP)
#define g_heraldRange g_herald.range
#define g_heraldTeam g_herald.team
static void FillTeleport(uint8_t *parms, const char *, const char *pn, uint32_t off, void *ctx) {
  float *v = (float *)ctx; // loc[3], rot[3]
  if (!strcmp(pn, "DestLocation"))
    memcpy(parms + off, v, 12);
  else if (!strcmp(pn, "DestRotation"))
    memcpy(parms + off, v + 3, 12);
}
static void HeraldEnd(const char *why) {
  Logf("commands: meteor -- herald %p done (%s), %d of %d targets visited", g_herald.pawn, why, g_herald.cur,
       g_herald.nt);
  void *pawn = g_herald.pawn, *ctrl = g_herald.ctrl;
  g_herald = {};
  if (pawn && ObjectLive(pawn))
    CallFilled(pawn, L"K2_DestroyActor", nullptr, nullptr);
  if (ctrl && ObjectLive(ctrl))
    CallFilled(ctrl, L"K2_DestroyActor", nullptr, nullptr);
}
// Every fire slot of the herald's controller -> its WarpM_Dmg ability, each
// tick (the game resets slots, see NpcFireWatch).
// Offset of a field (searched two struct levels deep) inside a struct; -1 if absent.
static int StructFieldOffset(UObjectMin *st, const char *name, int depth = 0) {
  for (UObjectMin *s2 = st; s2 && IsReadable(s2, 0x40); s2 = *(UObjectMin **)((uint8_t *)s2 + OFF_USTRUCT_SUPER))
    for (UObjectMin *f = *(UObjectMin **)((uint8_t *)s2 + 0x38); f && IsReadable(f, 0x78); f = *(UObjectMin **)((uint8_t *)f + 0x28)) {
      int off = (int)*(uint32_t *)((uint8_t *)f + 0x50);
      const char *fn = NameText(f->Name);
      if (fn && !strcmp(fn, name))
        return off;
      UObjectMin *sub = depth < 2 && !strcmp(ClassNameOf(f), "StructProperty") ? PropRef(f, "ScriptStruct") : nullptr;
      int r = sub ? StructFieldOffset(sub, name, depth + 1) : -1;
      if (r >= 0)
        return off + r;
    }
  return -1;
}
// m_fireAbilities slot field offset by name, resolved from the reflected struct.
static int FireSlotFieldOffset(UObjectMin *ctrl, const char *name) {
  UObjectMin *f = FindPropObj(ctrl, "m_fireAbilities"), *p = f;
  if (f && !strcmp(ClassNameOf(f), "ArrayProperty"))
    p = PropRef(f, "*Property");
  UObjectMin *st = p && !strcmp(ClassNameOf(p), "StructProperty") ? PropRef(p, "ScriptStruct") : nullptr;
  return st ? StructFieldOffset(st, name) : -1;
}
// The herald fires only within ~9 km (~45000 uu) and flies closer from
// farther (live 2026-10-18), though its slot m_maxFireDistance is 100000.
// GUESS: the cap is the AI's perception / engage distances (reflected names in
// the client exe: m_maxPerceptDistance, m_engageInDistance,
// m_engageOutDistance). Each is looked up on the controller, the pawn and the
// controller's object properties one level down; its stock value is logged
// and raised past the teleport distance.
// The herald still sat passive at 60000 uu (Onslaught 16:24 2026-10-18) and
// none of the three guessed names exist. Once per herald: every float on the
// controller, the pawn and two levels of object / object-array properties
// whose name says range or whose value is 3000..300000 uu, to find the limit
// behind "fires at ~9 km, only approaches from farther".
// Found 16:4x: controller m_maxFireDistance 30000 (the slot's own is 100000)
// and the nuke abilities' m_maxDistance 50000; nothing near the ~45000 seen.
// Both are raised to the herald range x1.5 on every pass (ability objects
// can be rebuilt by the slot re-apply). GUESS: the controller value is the
// engage gate.
static void HeraldRangeSweep() {
  static bool s_logged; // once per process: 200 lines per herald was a flood
  bool log = !s_logged;
  s_logged = true;
  float want = g_heraldRange * 1.5f;
  UObjectMin *objs[256];
  int depth[256], no = 0, told = 0;
  auto add = [&](UObjectMin *v, int d) {
    if (no >= 256 || !IsReadable(v, sizeof(UObjectMin)) || !IsReadable(v->Class, sizeof(UObjectMin)))
      return;
    for (int i = 0; i < no; ++i)
      if (objs[i] == v)
        return;
    objs[no] = v, depth[no++] = d;
  };
  add((UObjectMin *)g_herald.ctrl, 0);
  add((UObjectMin *)g_herald.pawn, 0);
  for (int i = 0; i < no; ++i) {
    UObjectMin *o = objs[i];
    for (UObjectMin *cls = o->Class; cls && IsReadable(cls, 0x40); cls = *(UObjectMin **)((uint8_t *)cls + OFF_USTRUCT_SUPER))
      for (UObjectMin *f = *(UObjectMin **)((uint8_t *)cls + 0x38); f && IsReadable(f, 0x88); f = *(UObjectMin **)((uint8_t *)f + 0x28)) {
        const char *fc = ClassNameOf(f);
        uint8_t *at = (uint8_t *)o + *(uint32_t *)((uint8_t *)f + 0x50);
        if (!IsReadable(at, 16))
          continue;
        if (!strcmp(fc, "FloatProperty")) {
          float v = *(float *)at;
          const char *nm = NameText(f->Name);
          char low[96] = {};
          for (int k = 0; nm && nm[k] && k < 95; ++k)
            low[k] = (char)tolower((unsigned char)nm[k]);
          bool named = strstr(low, "dist") || strstr(low, "range") || strstr(low, "radius") || strstr(low, "proxim");
          bool raise = nm && v < want && ((depth[i] == 0 && o == (UObjectMin *)g_herald.ctrl && !strcmp(nm, "m_maxFireDistance")) ||
                                          (!strcmp(nm, "m_maxDistance") && !strncmp(ClassNameOf(o), "AB_", 3)));
          if (raise)
            *(float *)at = want;
          if (log && (named || (v >= 3000 && v <= 300000)) && told < 200)
            ++told, Logf("herald sweep: d%d %s.%s = %.1f%s", depth[i], ClassNameOf(o), nm ? nm : "?", v, raise ? " (raised)" : "");
        } else if (depth[i] < 2 && !strcmp(fc, "ObjectProperty")) {
          add(*(UObjectMin **)at, depth[i] + 1);
        } else if (depth[i] < 2 && !strcmp(fc, "ArrayProperty")) {
          UObjectMin *inner = nullptr;
          for (int k = 0x70; k <= 0x80 && !inner; k += 8) {
            UObjectMin *c = *(UObjectMin **)((uint8_t *)f + k);
            if (IsReadable(c, sizeof(UObjectMin)) && IsReadable(c->Class, sizeof(UObjectMin)) && strstr(ClassNameOf(c), "Property"))
              inner = c;
          }
          if (!inner || strcmp(ClassNameOf(inner), "ObjectProperty"))
            continue;
          UObjectMin **data = *(UObjectMin ***)at;
          int n = *(int32_t *)(at + 8);
          for (int k = 0; k < n && k < 32 && IsReadable(data + k, 8); ++k)
            add(data[k], depth[i] + 1);
        }
      }
  }
  if (log)
    Logf("herald sweep: %d objects, %d floats logged", no, told);
}
// m_maxPerceptDistance is in the exe but on none of the herald's objects
// (16:4x 2026-10-18), and the herald fired at 60000 uu but not 80000
// (operator: it cannot see that far). Once per process: every class that
// declares one of these floats, and their live instances (values logged);
// m_maxPerceptDistance instances are raised to the herald range x1.5 on each
// sweep. GUESS: that one is the AI's sight limit; the visibility ones (spotting
// / minimap) are only logged.
static struct { int idx; void *obj; uint32_t off; } g_perceptHits[32];
static int g_nPerceptHits;
static void PerceptSearch(float want) {
  static bool s_done;
  FUObjectArrayMin *arr = GObjects();
  if (!arr || !arr->Objects)
    return;
  if (!s_done) {
    s_done = true;
    static const char *names[] = {"m_maxPerceptDistance", "m_maxVisibleDistance", "m_VisibilityDistanceOverride"};
    struct { UObjectMin *cls; int which; uint32_t off; } owners[24];
    int nown = 0;
    for (int i = 0; i < arr->NumElements && nown < 24; ++i) {
      UObjectMin *o = arr->Objects[i].Object;
      if (!o || !IsReadable(o, 0x40) || !IsReadable(o->Class, sizeof(UObjectMin)))
        continue;
      const char *cc = NameText(o->Class->Name);
      if (!cc || (strcmp(cc, "Class") && strcmp(cc, "BlueprintGeneratedClass")))
        continue;
      for (UObjectMin *f = *(UObjectMin **)((uint8_t *)o + 0x38); f && IsReadable(f, 0x58); f = *(UObjectMin **)((uint8_t *)f + 0x28)) {
        const char *fn = NameText(f->Name);
        for (int w = 0; fn && w < 3; ++w)
          if (!strcmp(fn, names[w]) && nown < 24) {
            owners[nown++] = {o, w, *(uint32_t *)((uint8_t *)f + 0x50)};
            Logf("percept search: %s declares %s (%s) at +0x%X", NameText(o->Name), fn, ClassNameOf(f),
                 *(uint32_t *)((uint8_t *)f + 0x50));
          }
      }
    }
    int told = 0;
    for (int i = 0; i < arr->NumElements; ++i) {
      UObjectMin *o = arr->Objects[i].Object;
      if (!o || (arr->Objects[i].Flags & OBJ_PENDING_KILL) || !IsReadable(o, 0x40))
        continue;
      for (int k = 0; k < nown; ++k) {
        bool is = false;
        for (UObjectMin *c = o->Class; c && IsReadable(c, 0x40) && !is; c = *(UObjectMin **)((uint8_t *)c + OFF_USTRUCT_SUPER))
          is = c == owners[k].cls;
        if (!is || !IsReadable((uint8_t *)o + owners[k].off, 4))
          continue;
        float v = *(float *)((uint8_t *)o + owners[k].off);
        if (told < 40)
          ++told, Logf("percept search: %s (%s).%s = %.1f", NameText(o->Name), ClassNameOf(o), names[owners[k].which], v);
        if (owners[k].which == 0 && g_nPerceptHits < 32)
          g_perceptHits[g_nPerceptHits++] = {i, o, owners[k].off};
      }
    }
    Logf("percept search: %d declaring classes, %d m_maxPerceptDistance instances", nown, g_nPerceptHits);
  }
  for (int k = 0; k < g_nPerceptHits; ++k) {
    auto &h = g_perceptHits[k];
    if (h.idx >= arr->NumElements || arr->Objects[h.idx].Object != h.obj)
      continue;
    float *p = (float *)((uint8_t *)h.obj + h.off);
    if (*p < want)
      *p = want;
  }
}
static void HeraldStretchRange() {
  HeraldRangeSweep();
  __try {
    PerceptSearch(g_heraldRange * 1.5f);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("percept search: faulted");
  }
  static const char *names[] = {"m_maxPerceptDistance", "m_engageInDistance", "m_engageOutDistance"};
  UObjectMin *roots[2] = {(UObjectMin *)g_herald.ctrl, (UObjectMin *)g_herald.pawn};
  float want = g_heraldRange * 1.5f;
  for (const char *nm : names) {
    bool found = false;
    for (UObjectMin *root : roots) {
      if (!root || !ObjectLive(root))
        continue;
      UObjectMin *objs[48];
      int no = 0;
      objs[no++] = root;
      for (UObjectMin *cls = root->Class; cls && IsReadable(cls, 0x40) && no < 48; cls = *(UObjectMin **)((uint8_t *)cls + OFF_USTRUCT_SUPER))
        for (UObjectMin *f = *(UObjectMin **)((uint8_t *)cls + 0x38); f && IsReadable(f, 0x58) && no < 48; f = *(UObjectMin **)((uint8_t *)f + 0x28))
          if (!strcmp(ClassNameOf(f), "ObjectProperty")) {
            uint8_t *at = (uint8_t *)root + *(uint32_t *)((uint8_t *)f + 0x50);
            UObjectMin *v = IsReadable(at, 8) ? *(UObjectMin **)at : nullptr;
            if (IsReadable(v, sizeof(UObjectMin)) && IsReadable(v->Class, sizeof(UObjectMin)))
              objs[no++] = v;
          }
      for (int i = 0; i < no; ++i) {
        UObjectMin *f = FindPropObj(objs[i], nm);
        if (!f || strcmp(ClassNameOf(f), "FloatProperty"))
          continue;
        float *p = (float *)((uint8_t *)objs[i] + *(uint32_t *)((uint8_t *)f + 0x50));
        if (!IsReadable(p, 4))
          continue;
        found = true;
        static int s_told = 0;
        if (s_told < 12)
          ++s_told, Logf("commands: meteor -- %s on %s = %.0f%s", nm, ClassNameOf(objs[i]), *p, *p < want ? " (raised)" : "");
        if (*p < want)
          *p = want;
      }
    }
    if (!found) {
      static int s_missTold = 0;
      if (s_missTold < 3)
        ++s_missTold, Logf("commands: meteor -- %s not found on the herald", nm);
    }
  }
}
// Every tick: the fired ability's and the controller's cooldown floats to 0.
// GUESS: which of these names gate refire (all are reflected names in the
// exe); the stock values are logged once per class.
static void HeraldNoCooldown(UObjectMin *ab, UObjectMin *ctrl) {
  static const char *abNames[] = {"m_coolDownTime", "m_coolDown", "m_currentCoolDown", "m_currentCoolDownTime",
                                  "m_refireCoolDown", "m_weaponCooldownTime"};
  static const char *ctrlNames[] = {"m_globalCooldown", "m_globalCoolDown", "m_globalCooldownTime"};
  // 8: /fighters + /meteor + /rain together use 5 classes; 4 thrashed (onsjets).
  static struct { UObjectMin *cls; int off[6]; } s_ab[8];
  static int s_ctrlOff[3] = {-2, -2, -2};
  int k = 0;
  for (; k < 8 && s_ab[k].cls && s_ab[k].cls != ab->Class; ++k)
    ;
  if (k == 8)
    k = 7;
  if (s_ab[k].cls != ab->Class) {
    s_ab[k].cls = ab->Class;
    for (int i = 0; i < 6; ++i) {
      UObjectMin *f = FindPropObj(ab, abNames[i]);
      s_ab[k].off[i] = f && !strcmp(ClassNameOf(f), "FloatProperty") ? (int)*(uint32_t *)((uint8_t *)f + 0x50) : -1;
      if (s_ab[k].off[i] >= 0)
        Logf("commands: meteor -- %s.%s = %.2f -> 0", ClassNameOf(ab), abNames[i], *(float *)((uint8_t *)ab + s_ab[k].off[i]));
    }
  }
  for (int i = 0; i < 6; ++i)
    if (s_ab[k].off[i] >= 0)
      *(float *)((uint8_t *)ab + s_ab[k].off[i]) = 0.f;
  for (int i = 0; i < 3; ++i) {
    if (s_ctrlOff[i] == -2) {
      UObjectMin *f = FindPropObj(ctrl, ctrlNames[i]);
      s_ctrlOff[i] = f && !strcmp(ClassNameOf(f), "FloatProperty") ? (int)*(uint32_t *)((uint8_t *)f + 0x50) : -1;
      if (s_ctrlOff[i] >= 0)
        Logf("commands: meteor -- controller %s = %.2f -> 0", ctrlNames[i], *(float *)((uint8_t *)ctrl + s_ctrlOff[i]));
    }
    if (s_ctrlOff[i] >= 0)
      *(float *)((uint8_t *)ctrl + s_ctrlOff[i]) = 0.f;
  }
}
static void HeraldNukeOnly() {
  uint8_t *ctrl = (uint8_t *)g_herald.ctrl, *pawn = (uint8_t *)g_herald.pawn;
  if (!ctrl || !ObjectLive(ctrl) || !IsReadable(ctrl + 0x860, 16) || !IsReadable(pawn + OFF_PAWN_ABILITY_MANAGER, 8))
    return;
  uint8_t *mgr = *(uint8_t **)(pawn + OFF_PAWN_ABILITY_MANAGER);
  if (!IsReadable(mgr + 0xF8, 16))
    return;
  uint8_t *fa = *(uint8_t **)(ctrl + 0x860);
  int nfa = *(int32_t *)(ctrl + 0x868), na = *(int32_t *)(mgr + 0x100), nuke = -1;
  UObjectMin **abl = *(UObjectMin ***)(mgr + 0xF8);
  if (!fa || !abl || na <= 0 || !IsReadable(abl, na * 8))
    return;
  for (int j = 0; j < na && j < 32 && nuke < 0 && g_herald.abil; ++j)
    if (IsReadable(abl[j], sizeof(UObjectMin)) && strstr(ClassNameOf(abl[j]), g_herald.abil))
      nuke = j;
  // A module herald fires only its module; onsjets showed a jet herald
  // falling through to WarpM_Swarm below.
  if (nuke < 0 && g_herald.abil) {
    if (!g_herald.abilMissTold)
      g_herald.abilMissTold = true, Logf("commands: meteor -- herald %p has no %s ability now (%d abilities)", pawn, g_herald.abil, na);
    return;
  }
  // The salvo (WarpM_Swarm) instead of the single nuke (operator
  // 2026-10-18), with its cooldowns zeroed; the nuke if it has no salvo.
  for (int j = 0; j < na && j < 32 && nuke < 0; ++j)
    if (IsReadable(abl[j], sizeof(UObjectMin)) && strstr(ClassNameOf(abl[j]), "_WarpM_Swarm_"))
      nuke = j;
  for (int j = 0; j < na && j < 32 && nuke < 0; ++j)
    if (IsReadable(abl[j], sizeof(UObjectMin)) && strstr(ClassNameOf(abl[j]), "_WarpM_Dmg_"))
      nuke = j;
  if (nuke < 0) {
    static bool s_told = false;
    if (!s_told)
      s_told = true, Logf("commands: meteor -- herald has no WarpM_Swarm / WarpM_Dmg ability");
    return;
  }
  HeraldNoCooldown(abl[nuke], (UObjectMin *)ctrl);
  // The slot's own fire range caps how far the AI shoots, whatever the
  // ability reaches; stretch it past the teleport distance. The stock value is
  // logged once (12 km / 60000 uu got no shots, 2026-10-18).
  static int s_maxOff = -2;
  if (s_maxOff == -2) {
    s_maxOff = FireSlotFieldOffset((UObjectMin *)ctrl, "m_maxFireDistance");
    float was = s_maxOff >= 0 && s_maxOff + 4 <= 0xD0 && nfa > 0 ? *(float *)(fa + s_maxOff) : -1.f;
    Logf("commands: meteor -- slot m_maxFireDistance at +0x%x, stock %.0f", s_maxOff, was);
    if (s_maxOff + 4 > 0xD0)
      s_maxOff = -1;
  }
  // Every weapon row of DN_AIAbilityActivation_DT (missile, alt weapon, beam,
  // broadside) has m_requiresLineOfSight true: the herald holds fire on a
  // target terrain hides (operator 2026-10-18: fired at 60000 uu, not at
  // 80000 behind cover). Cleared on every slot. Written as a byte only when
  // its neighbour m_requiresAbilityTargeted sits at another offset, so it is a
  // plain bool, not a bitfield sharing the byte.
  static int s_losOff = -2;
  if (s_losOff == -2) {
    s_losOff = FireSlotFieldOffset((UObjectMin *)ctrl, "m_requiresLineOfSight");
    int nb = FireSlotFieldOffset((UObjectMin *)ctrl, "m_requiresAbilityTargeted");
    Logf("commands: meteor -- slot m_requiresLineOfSight at +0x%x (neighbour +0x%x), stock %d", s_losOff, nb,
         s_losOff >= 0 && s_losOff < 0xD0 && nfa > 0 ? fa[s_losOff] : -1);
    if (s_losOff < 0 || s_losOff >= 0xD0 || nb == s_losOff)
      s_losOff = -1;
  }
  for (int i = 0; i < nfa && i < 16 && IsReadable(fa + i * 0xD0, 0xD0); ++i) {
    uint8_t *e = fa + i * 0xD0;
    *(int32_t *)(e + 8) = nuke;
    *(UObjectMin **)(e + 0x10) = abl[nuke]->Class;
    if (s_maxOff >= 0 && *(float *)(e + s_maxOff) < g_heraldRange * 1.5f)
      *(float *)(e + s_maxOff) = g_heraldRange * 1.5f;
    if (s_losOff >= 0)
      e[s_losOff] = 0;
  }
}
// Operator 2026-10-18: the herald disappears after firing (after its last
// target; before that it moves on when the nuke lands). Twice a second
// GObjects is walked for AB_DN_Pri_WarpM_Dmg_Projectile_T5_BP_C (class
// pointer compare after the first lookup); the first one the herald owns
// (Owner chain, two levels) or that appears within 8000 uu of it is its
// shot. GUESS: Owner is the pawn or its weapon -- the proximity test covers
// it if not. When that projectile is gone (impact) or after 30 s, it leaves.
static bool ProjectileClass(UObjectMin *o);
static bool HeraldShotWatch(ULONGLONG now) {
  if (now < g_herald.watchAt)
    return false;
  g_herald.watchAt = now + 500;
  FUObjectArrayMin *arr = GObjects();
  float me[3];
  if (!arr || !arr->Objects || !ActorLocation(g_herald.pawn, me))
    return false;
  // N salvos counted, the last one 10 s gone quiet: leave. 60 s with no shot
  // at all after the last target visit also ends it.
  // With no cooldown the Warp Missile herald fired every ~7 s and never went
  // quiet (onsherald: salvo 10/1), so it also leaves 12 s after the Nth.
  if (g_herald.salvos && g_herald.salvos >= g_herald.salvosWant && !g_herald.doneAt)
    g_herald.doneAt = now;
  if (g_herald.doneAt && (now - g_herald.lastMissileAt > 10000 || now - g_herald.doneAt > 12000)) {
    HeraldEnd("salvos fired");
    return true;
  }
  if (!g_herald.salvos && g_herald.cur >= g_herald.nt && g_herald.shotAt && now - g_herald.shotAt > 60000) {
    HeraldEnd("never fired");
    return true;
  }
  for (int i = 0; i < arr->NumElements; ++i) {
    FUObjectItemMin &it = arr->Objects[i];
    UObjectMin *o = it.Object;
    if (!o || (it.Flags & OBJ_PENDING_KILL) || !o->Class || !ProjectileClass(o))
      continue;
    const char *nm = NameText(o->Name);
    if (!nm || !strncmp(nm, "Default__", 9))
      continue;
    bool known = false;
    for (int k = 0; k < g_herald.nSeen && !known; ++k)
      known = g_herald.seen[k] == o;
    for (HeraldState &h : g_heralds)
      for (int k = 0; &h != g_heraldP && k < h.nSeen && !known; ++k)
        known = h.seen[k] == o;
    if (known)
      continue;
    uint8_t *own = IsReadable((uint8_t *)o + OFF_ACTOR_OWNER, 8) ? *(uint8_t **)((uint8_t *)o + OFF_ACTOR_OWNER) : nullptr;
    bool mine = own == g_herald.pawn || own == g_herald.ctrl;
    float at[3];
    float d = ActorLocation(o, at) ? sqrtf((at[0] - me[0]) * (at[0] - me[0]) + (at[1] - me[1]) * (at[1] - me[1]) +
                                           (at[2] - me[2]) * (at[2] - me[2]))
                                   : 1e9f;
    if (!mine && d > 8000) // GUESS: a fresh salvo is still within 8000 uu of the herald
      continue;
    g_herald.seen[g_herald.nSeen++ % 64] = o;
    if (g_herald.nSeen > 64)
      g_herald.nSeen = 64;
    if (now - g_herald.lastMissileAt > 1500) { // GUESS: one salvo's missiles come within 1.5 s
      ++g_herald.salvos;
      Logf("commands: meteor -- herald %p salvo %d/%d: %s (%s, %.0f uu away)", g_herald.pawn, g_herald.salvos,
           g_herald.salvosWant, ClassNameOf(o), mine ? "owner match" : "proximity", d);
    }
    g_herald.lastMissileAt = now;
  }
  return false;
}
// Spot fallback / bearing offset: see HeraldClearSpot. The nuke flies a real path: with the line-of-sight check off it fired from
// behind a building and hit it (operator 2026-10-18). So the spot is chosen
// with a clear WorldStatic line to the target: bearings from the herald's
// side outwards in 30 degree steps, each at 1000 / 6000 / 12000 uu above the
// target, first clear one wins. Volume and pawn hits are stepped through as
// in the terrain pass. Returns the try number (1-based), 0 if none was clear
// (v keeps the first spot), -1 if tracing is unavailable.
static bool TraceResolve();
static bool TraceOnce(void *world, const float s[3], const float e[3], float *hz, void **actor);
static bool StepThrough(void *a);
static bool HeraldLineClear(void *world, const float s[3], const float e[3]) {
  float a[3] = {s[0], s[1], s[2]};
  for (int step = 0; step < 8; ++step) {
    float hz;
    void *hit = nullptr;
    if (!TraceOnce(world, a, e, &hz, &hit))
      return true;
    if (!StepThrough(hit))
      return false;
    // Only the impact z is returned; step along the segment to just past it.
    float dz = e[2] - a[2], t = fabsf(dz) > 1.f ? (hz - a[2]) / dz : 0.5f;
    t = std::min(1.f, std::max(0.f, t)) + 0.02f;
    if (t >= 1.f)
      return true;
    for (int k = 0; k < 3; ++k)
      a[k] += (e[k] - a[k]) * t;
  }
  return false;
}
static int HeraldClearSpot(const float at[3], float ux, float uy, float v[6]) {
  float c0 = cosf(g_herald.angle), s0 = sinf(g_herald.angle), rx = ux * c0 - uy * s0;
  uy = ux * s0 + uy * c0, ux = rx;
  // Fallback when nothing is clear: high on the first bearing, not the low
  // default (two heralds sat at z -2317 behind terrain, unseen, 18:10).
  v[0] = at[0] + ux * g_heraldRange, v[1] = at[1] + uy * g_heraldRange, v[2] = at[2] + 20000.f;
  void *world = g_herald.pawn ? ((tGetWorld)(*(void ***)g_herald.pawn)[VT_ACTOR_GET_WORLD / 8])(g_herald.pawn) : nullptr;
  bool ready = false;
  __try {
    ready = world && TraceResolve();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  if (!ready)
    return -1;
  static const float heights[] = {1000.f, 6000.f, 12000.f};
  float tgt[3] = {at[0], at[1], at[2]};
  int n = 0;
  for (int a = 0; a <= 6; ++a)
    for (int sgn = a && a < 6 ? -1 : 1; sgn <= 1; sgn += 2)
      for (float h : heights) {
        ++n;
        float ang = sgn * a * 0.5235988f, c = cosf(ang), sn = sinf(ang);
        float p[3] = {at[0] + (ux * c - uy * sn) * g_heraldRange, at[1] + (ux * sn + uy * c) * g_heraldRange, at[2] + h};
        if (HeraldLineClear(world, p, tgt)) {
          memcpy(v, p, 12);
          return n;
        }
      }
  return 0;
}
static void HeraldTickOne();
static void HeraldTick() {
  for (HeraldState &h : g_heralds)
    if (h.pawn) {
      g_heraldP = &h;
      HeraldTickOne();
    }
}
static void HeraldTickOne() {
  if (!g_herald.pawn)
    return;
  if (!ObjectLive(g_herald.pawn))
    return g_herald.pawn = nullptr, HeraldEnd("herald died");
  HeraldNukeOnly();
  ULONGLONG now = GetTickCount64();
  if (now >= g_herald.sweepAt) // the slot re-apply can rebuild the ability objects
    g_herald.sweepAt = now + 5000, HeraldRangeSweep();
  if (HeraldShotWatch(now))
    return;
  if (now < g_herald.nextAt)
    return;
  float at[3], me[3];
  while (g_herald.cur < g_herald.nt && (!ObjectLive(g_herald.tgt[g_herald.cur]) ||
                                        !ActorLocation(g_herald.tgt[g_herald.cur], at)))
    ++g_herald.cur; // target gone meanwhile
  // After the last target it stays and fights until killed (it was removed
  // 15 s after the visit, too soon to lock on: operator 2026-10-18).
  if (g_herald.cur >= g_herald.nt || !ActorLocation(g_herald.pawn, me)) {
    g_herald.nextAt = ~0ULL;
    g_herald.shotAt = now;
    return (void)Logf("commands: meteor -- herald %p visited %d of %d targets, stays until killed", g_herald.pawn,
                      g_herald.cur, g_herald.nt);
  }
  // From the side the herald is on, HERALD_RANGE out, a little above, facing it.
  float dx = me[0] - at[0], dy = me[1] - at[1], d = sqrtf(dx * dx + dy * dy);
  if (d < 1)
    dx = 1, dy = 0, d = 1;
  float v[6] = {at[0] + dx / d * g_heraldRange, at[1] + dy / d * g_heraldRange, at[2] + 1000.f, 0, 0, 0};
  int tries = HeraldClearSpot(at, dx / d, dy / d, v);
  dx = v[0] - at[0], dy = v[1] - at[1];
  // On the target's enemy team, so the client draws it as an enemy.
  // Client colours seen live 2026-10-18: 0 gray (neutral, hurts and is hurt
  // by both sides), 1 blue, 2 red, 3 purple, 4 light orange, 5 a different
  // green; 255 read as a squad mate.
  uint8_t *tp = (uint8_t *)g_herald.tgt[g_herald.cur],
          team = g_heraldTeam >= 0 ? (uint8_t)g_heraldTeam : tp[OFF_SHIP_TEAM] == 1 ? 2 : 1;
  __try {
    ((tPawnSetTeam)(g_base + RVA_PAWN_SET_TEAM))(g_herald.pawn, &team);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Logf("commands: meteor -- herald team set faulted");
  }
  v[4] = atan2f(-dy, -dx) * 57.29578f; // FRotator Pitch, Yaw, Roll
  alignas(16) uint8_t parms[256]; // CallNamed clears 256 bytes: at 128 it zeroed v, every teleport went to 0 0 0
  bool ok = false;
  __try {
    ok = CallNamed(g_herald.pawn, L"K2_TeleportTo", FillTeleport, v, parms);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  Logf("commands: meteor -- herald %p (team %d) to target %d/%d %s (%p), %.0f uu off, at %.0f %.0f %.0f (%s): teleport %s",
       g_herald.pawn, team, g_herald.cur + 1, g_herald.nt, ClassNameOf(g_herald.tgt[g_herald.cur]),
       g_herald.tgt[g_herald.cur], g_heraldRange, v[0], v[1], v[2],
       tries > 0 ? "clear line" : tries == 0 ? "no clear line found" : "not traced", ok ? "ok" : "FAILED");
  ++g_herald.cur;
  g_herald.nextAt = now + HERALD_DWELL_MS;
  HeraldStretchRange();
}
// /meteor N: one herald on the first ship (humans first), N salvos.
static bool CommandHeraldMeteorOne(uint8_t *gm, void *tgt, float angle, float range, int team);
struct HeraldModule;
static int HeraldsPerHuman(uint8_t *gm, void **pawns, int n, const HeraldModule *mod, int salvos, float range, int team);
static bool CommandHeraldMeteor(uint8_t *gm, int count, float range, int team) {
  void *pawns[64];
  float pts[64][3];
  int n = SkyTargets(pawns, pts, 64); // humans first
  if (!n)
    return Logf("commands: meteor -- no target ships"), false;
  // One herald per human ship (operator 2026-10-21: "one comet per (real)
  // ship ... no need for the NPCs to get targetted"), N shots each, no
  // cooldown; NPC ships only when no human is in.
  HeraldsPerHuman(gm, pawns, n, &kHeraldNuke, std::max(1, std::min(count, 40)), range, team);
  return true;
}
static int HeraldsPerHuman(uint8_t *gm, void **pawns, int n, const HeraldModule *mod, int salvos, float range, int team) {
  int humans = 0, made = 0;
  for (int i = 0; i < n; ++i)
    humans += !PawnIsNpc(pawns[i]);
  g_heraldModNext = mod;
  for (int i = 0; i < n; ++i) {
    if (humans ? PawnIsNpc(pawns[i]) : i > 0)
      continue;
    if (!CommandHeraldMeteorOne(gm, pawns[i], 0.f, range, team))
      break;
    g_herald.salvosWant = salvos;
    ++made;
  }
  g_heraldModNext = nullptr;
  Logf("commands: meteor -- %d %ls heralds (%d human ships), %d salvos each", made, mod->label, humans, salvos);
  return made;
}
// /fighters (Onslaught): one herald per jet module, on the human ships in
// turn. GUESS: that the AI launches jets from a fire slot like a missile.
// Jets are not projectiles, so no salvo is counted and each herald leaves
// 60 s after its visit ("never fired").
static void CommandFighters(uint8_t *gm, int team) {
  void *pawns[64], *hum[64];
  float pts[64][3];
  int n = SkyTargets(pawns, pts, 64), nh = 0;
  for (int i = 0; i < n; ++i)
    if (!PawnIsNpc(pawns[i]))
      hum[nh++] = pawns[i];
  if (!nh && n)
    hum[nh++] = pawns[0];
  if (!nh)
    return (void)Logf("commands: fighters -- no target ships");
  for (int k = 0; k < 3; ++k) {
    g_heraldModNext = &kHeraldJets[k];
    if (CommandHeraldMeteorOne(gm, hum[k % nh], 0.f, 0.f, team))
      g_herald.salvosWant = 1;
  }
  g_heraldModNext = nullptr;
  Logf("commands: fighters -- 3 jet heralds on %d ships", nh);
}
static bool CommandHeraldMeteorOne(uint8_t *gm, void *tgt, float angle, float range, int team) {
  HeraldState *slot = nullptr;
  for (HeraldState &h : g_heralds)
    if (!h.pawn || !ObjectLive(h.pawn))
      if (!slot)
        slot = &h;
  if (!slot)
    return Logf("commands: meteor -- %d heralds already out", HERALD_MAX), false;
  g_spawnLabel = g_heraldModNext ? g_heraldModNext->label : L"Comet";
  g_lastSpawnPawn = nullptr;
  g_spawnNoGameBot = true;
  CommandSpawn(gm, "DreadnoughtHeavyNukerT5", 1, 1);
  g_spawnNoGameBot = false;
  g_spawnLabel = nullptr;
  void *pawn = g_lastSpawnPawn;
  if (!pawn || !ObjectLive(pawn))
    return Logf("commands: meteor -- herald spawn failed"), false;
  HeraldAlwaysRelevant(pawn);
  if (g_heraldModNext)
    InstallAbilityClasses(pawn, "DreadnoughtHeavyNukerT5", &g_heraldModNext->path, 1);
  g_heraldP = slot;
  g_herald = {};
  g_herald.abil = g_heraldModNext ? g_heraldModNext->stem : nullptr;
  g_heraldRange = range > 0 ? range : HERALD_RANGE;
  g_heraldTeam = team;
  g_herald.pawn = pawn;
  g_herald.ctrl = *(void **)((uint8_t *)pawn + OFF_PAWN_CONTROLLER);
  g_herald.angle = angle;
  g_herald.tgt[g_herald.nt++] = tgt;
  Logf("commands: meteor -- herald %p for %d targets (slot %d)", pawn, g_herald.nt, (int)(slot - g_heralds));
  HeraldTickOne();
  return true;
}

// /swarm [n] [team] (Onslaught fighters, VH_ONS_AITargetL) and /squad [n]
// [team] (assault ships, VH_ONS_AITargetM) en masse (operator 2026-10-21).
// Team 3 by default, like /boss: purple, fights both sides. In Onslaught the
// game mode's own classes and spawn points (any team's) are used; elsewhere
// the classes are loaded by path and spawned at player starts. Each ship is
// then teleported 6000-12000 uu from a random human ship: AI ships left at a
// far player start never moved or fired (spawn-fight 2026-10-20).
// GUESS: n defaults, the distances, and that a team-3 creep finds targets.
static void *SwarmClassByPath(int size) {
  static void *s_cls[2];
  if (s_cls[size] && ObjectLive(s_cls[size]))
    return s_cls[size];
  const wchar_t *path = size == 0 ? L"/Game/Generic/AI/Onslaught/Targets/VH_ONS_AITargetL_Pawn_BP.VH_ONS_AITargetL_Pawn_BP_C"
                                  : L"/Game/Generic/AI/Onslaught/Targets/VH_ONS_AITargetM_Pawn_BP.VH_ONS_AITargetM_Pawn_BP_C";
  UObjectMin *uc = ResolveUClassClass();
  UObjectMin *k = nullptr;
  __try {
    if (uc)
      k = (UObjectMin *)((tStaticLoadClass)(g_base + RVA_STATIC_LOAD_CLASS))(uc, nullptr, path, nullptr, 0, nullptr, false);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    k = nullptr;
  }
  if (!IsReadable(k, sizeof(UObjectMin)))
    return Logf("commands: swarm -- %ls did not load", path), nullptr;
  PinToRootSet(k);
  return s_cls[size] = k;
}
static bool TeleportActor(void *actor, const float loc[3], float yawDeg) {
  float v[6] = {loc[0], loc[1], loc[2], 0, yawDeg, 0};
  alignas(16) uint8_t parms[256];
  __try {
    return CallNamed(actor, L"K2_TeleportTo", FillTeleport, v, parms);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
static void *SwarmOne(uint8_t *gm, uint8_t *gs, int size, int team, void **starts, int ns) {
  void *at = nullptr, *cls = nullptr;
  if (g_onsCreeps.gm == gm && g_onsCreeps.nPoints) {
    int cand[256], nc = 0;
    for (int i = 0; i < g_onsCreeps.nPoints && nc < 256; ++i)
      if (g_onsCreeps.points[i].size == size)
        cand[nc++] = i;
    if (nc) {
      at = g_onsCreeps.points[cand[rand() % nc]].actor;
      cls = OnslaughtCreepClass(gm, gs, size, *(void **)((uint8_t *)at + OFF_SPAWN_CLASS));
    }
  }
  bool atStart = false;
  if (!at && ns) {
    at = starts[rand() % ns];
    cls = SwarmClassByPath(size);
    atStart = true;
  }
  if (!at || !cls)
    return nullptr;
  uint8_t t = (uint8_t)team;
  void *pawn = nullptr;
  AllowCreepSpawnWhenColliding(cls);
  __try {
    if (size == 0) {
      void *scene = ((tGetSceneManager)(g_base + RVA_COMBAT_SCENE_MANAGER))();
      if (scene)
        pawn = ((tSpawnCreep)(g_base + RVA_SPAWN_CREEP))(scene, cls, at, &t);
    } else {
      g_spawnAtStart = atStart;
      pawn = SpawnOnslaughtAIShip(gm, cls, at, team, false);
      g_spawnAtStart = false;
      if (pawn)
        ((tPawnSetTeam)(g_base + RVA_PAWN_SET_TEAM))(pawn, &t);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_spawnAtStart = false;
    pawn = nullptr;
  }
  return pawn;
}
// A UEnum value by its name's suffix ("YCSB_ENGAGE"), -1 if not found. Same
// layout GUESS as MarkerFind (Names TArray at +0x40, 16-byte entries, value
// low byte at +8).
static int EnumValue(const wchar_t *enumName, const char *suffix) {
  FNameMin en;
  ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&en, enumName, 1);
  FUObjectArrayMin *arr = GObjects();
  if (!arr || !arr->Objects)
    return -1;
  for (int i = 0; i < arr->NumElements; ++i) {
    UObjectMin *o = arr->Objects[i].Object;
    if (!o || !o->Class || o->Name.ComparisonIndex != en.ComparisonIndex || strncmp(ClassNameOf(o), "Enum", 4))
      continue;
    uint8_t *names = IsReadable((uint8_t *)o + 0x48, 12) ? *(uint8_t **)((uint8_t *)o + 0x40) : nullptr;
    int n = names ? *(int32_t *)((uint8_t *)o + 0x48) : 0;
    for (int k = 0; k < n && k < 64 && IsReadable(names + k * 16, 16); ++k) {
      const char *t = NameText(*(FNameMin *)(names + k * 16));
      size_t tl = t ? strlen(t) : 0, sl = strlen(suffix);
      if (tl >= sl && !strcmp(t + tl - sl, suffix))
        return *(uint8_t *)(names + k * 16 + 8);
    }
  }
  return -1;
}

// Squad AI (operator 2026-10-21: the assault ships "move around", little
// effect): their Blueprint only ever asks the scene manager for ROUTE along
// the lane they were spawned on, so teleported next to a player they head
// back to the lane. This replaces it with ENGAGE on the player's ship.
// GUESS: ENGAGE's m_location is the actor to fight and m_targetActor (an NPC
// id for ROUTE) -1 for "no NPC"; whether the Blueprint's TickBehavior puts
// ROUTE back is what the 10 s follow-up log shows.
static int g_ycsbEngage = -2;
static struct { int id[64]; void *target[64], *pawn[64], *sent[64]; DWORD sentAt[64]; int variant[64]; int n, logged; bool ours; uint8_t *gm; } g_squadIds = {};
// Variant probe (onsrain: ENGAGE with target -1 was back to ROUTE within
// 10 s): k%4 = 0 ENGAGE target -1, 1 ENGAGE target its own NPC id (as ROUTE
// passes), 2 SKIRMISH target -1, 3 GOTO the player, own id.
static int g_ycsbSkirmish = -2, g_ycsbGoto = -2;
static bool SquadEngage(void *pawn, void *target, int variant = 0) {
  uint8_t *ctrl = *(uint8_t **)((uint8_t *)pawn + OFF_PAWN_CONTROLLER);
  uint8_t *scene = (uint8_t *)((tGetSceneManager)(g_base + RVA_COMBAT_SCENE_MANAGER))();
  if (!ctrl || !scene || !target || !ClassIsA(((UObjectMin *)ctrl)->Class, *(void **)(g_base + OFF_CLASSPTR_NPC_AI_CONTROLLER)))
    return false;
  if (g_ycsbEngage == -2) {
    g_ycsbEngage = EnumValue(L"EYCombatSceneBehavior", "YCSB_ENGAGE");
    g_ycsbSkirmish = EnumValue(L"EYCombatSceneBehavior", "YCSB_SKIRMISH");
    g_ycsbGoto = EnumValue(L"EYCombatSceneBehavior", "YCSB_GOTO");
    Logf("commands: squad -- SKIRMISH %d GOTO %d", g_ycsbSkirmish, g_ycsbGoto);
    Logf("commands: squad -- YCSB_ENGAGE = %d (EnumValue ROUTE %d, NONE %d; defines %d %d)", g_ycsbEngage,
         EnumValue(L"EYCombatSceneBehavior", "YCSB_ROUTE"), EnumValue(L"EYCombatSceneBehavior", "YCSB_NONE"), YCSB_ROUTE, YCSB_NONE);
  }
  int id = *(int32_t *)(ctrl + OFF_NPC_SCENE_INDEX);
  if (g_ycsbEngage < 0 || id < 0 || id >= *(int32_t *)(scene + OFF_SCENE_NPCS_COUNT))
    return false;
  uint8_t *b = (uint8_t *)((tFMemoryMalloc)(g_base + RVA_FMEMORY_MALLOC))(BEHAVIOUR_DATA_SIZE, 0);
  if (!b)
    return false;
  memset(b, 0, BEHAVIOUR_DATA_SIZE);
  *(float *)(b + 0x00) = -1.0f;
  int v = variant & 3;
  b[0x08] = (uint8_t)(v == 2 && g_ycsbSkirmish >= 0 ? g_ycsbSkirmish : v == 3 && g_ycsbGoto >= 0 ? g_ycsbGoto : g_ycsbEngage);
  *(void **)(b + 0x18) = target;
  *(int32_t *)(b + 0x20) = v == 1 || v == 3 ? id : -1;
  TArrayRaw arr = {b, 1, 1};
  g_squadIds.ours = true;
  ((tSceneReplaceBehaviours)(g_base + RVA_SCENE_REPLACE_NPC_BEHAVIOURS))(scene, id, &arr, 0);
  g_squadIds.ours = false;
  if (!variant)
    Logf("commands: squad -- npc %d state right after the replace: %d", id, *(int32_t *)(ctrl + 0xA38));
  int slot = -1;
  for (int i = 0; i < g_squadIds.n; ++i)
    if (g_squadIds.id[i] == id)
      slot = i;
  if (slot < 0 && g_squadIds.n < 64)
    slot = g_squadIds.n++;
  if (slot >= 0)
    g_squadIds.id[slot] = id, g_squadIds.target[slot] = target, g_squadIds.pawn[slot] = pawn, g_squadIds.variant[slot] = variant;
  if (slot >= 0)
    g_squadIds.sent[slot] = target, g_squadIds.sentAt[slot] = GetTickCount();
  if (!variant)
    g_squadIds.logged = 0;
  return true;
}
// The behaviour list the squad ships' Blueprint keeps putting back
// (onsrain2: all four variants read ROUTE again 5 s later). Every
// ReplaceNPCBehaviours for a squad ship's NPC id that is not ours is logged
// (first 12, with the caller) and its ROUTE entries become ENGAGE on the
// squad's player.
static void __fastcall HookSceneReplace(void *scene, int id, void *behaviours, uint8_t keepTree) {
  int k = -1;
  for (int i = 0; !g_squadIds.ours && i < g_squadIds.n; ++i)
    if (g_squadIds.id[i] == id)
      k = i;
  TArrayRaw *arr = (TArrayRaw *)behaviours;
  if (k >= 0 && arr && IsReadable(arr, 16) && arr->num > 0 && arr->num < 16 && IsReadable(arr->data, arr->num * BEHAVIOUR_DATA_SIZE)) {
    void *t = g_squadIds.target[k], *me = g_squadIds.pawn[k];
    float ml[3];
    if (!me || !ObjectLive(me) || !ActorLocation(me, ml)) {
      // The ship is gone; its scene id may go to another NPC.
      g_squadIds.id[k] = g_squadIds.id[--g_squadIds.n];
      g_squadIds.target[k] = g_squadIds.target[g_squadIds.n], g_squadIds.pawn[k] = g_squadIds.pawn[g_squadIds.n];
      g_squadIds.variant[k] = g_squadIds.variant[g_squadIds.n];
      g_squadIds.sent[k] = g_squadIds.sent[g_squadIds.n], g_squadIds.sentAt[k] = g_squadIds.sentAt[g_squadIds.n];
      g_origSceneReplace(scene, id, behaviours, keepTree);
      return;
    }
    // Nearest live player ship, so a squad follows respawns and spreads out.
    uint8_t *pcs[64];
    int np = g_squadIds.gm ? CollectHumanPCs(g_squadIds.gm, pcs, 64) : 0;
    float best = 1e30f;
    for (int i = 0; i < np; ++i) {
      void *hp = IsReadable(pcs[i] + OFF_CONTROLLER_PAWN, 8) ? *(void **)(pcs[i] + OFF_CONTROLLER_PAWN) : nullptr;
      float hl[3];
      if (hp && ActorLocation(hp, hl)) {
        float d = (hl[0] - ml[0]) * (hl[0] - ml[0]) + (hl[1] - ml[1]) * (hl[1] - ml[1]) + (hl[2] - ml[2]) * (hl[2] - ml[2]);
        if (d < best)
          best = d, t = hp;
      }
    }
    g_squadIds.target[k] = t;
    bool live = t && ObjectLive(t);
    // Live 2026-10-21: re-sending GOTO every second made the boats stutter
    // (GUESS: each new GOTO restarts the move). Same target within 5 s: the
    // Blueprint's ROUTE is dropped and the running GOTO kept. The dropped
    // array's 0x28-byte buffer leaks (no FMemory::Free RVA known).
    if (live && t == g_squadIds.sent[k] && (int)(GetTickCount() - g_squadIds.sentAt[k]) < 5000)
      return;
    if (live)
      g_squadIds.sent[k] = t, g_squadIds.sentAt[k] = GetTickCount();
    int changed = 0;
    char buf[96];
    int len = 0;
    for (int i = 0; i < arr->num; ++i) {
      uint8_t *b = (uint8_t *)arr->data + i * BEHAVIOUR_DATA_SIZE;
      if (len < 80)
        len += snprintf(buf + len, sizeof(buf) - len, " %d", b[0x08]);
      if (live && b[0x08] == YCSB_ROUTE && g_ycsbEngage >= 0) {
        int v = g_squadIds.variant[k] & 3;
        b[0x08] = (uint8_t)(v == 2 && g_ycsbSkirmish >= 0 ? g_ycsbSkirmish : v == 3 && g_ycsbGoto >= 0 ? g_ycsbGoto : g_ycsbEngage);
        *(void **)(b + 0x18) = t, ++changed;
        *(int32_t *)(b + 0x20) = v == 1 || v == 3 ? id : -1;
      }
    }
    if (g_squadIds.logged < 12) {
      ++g_squadIds.logged;
      Logf("commands: squad -- npc %d behaviours replaced by caller +0x%llx:%s, %d rewritten", id,
           (unsigned long long)((uintptr_t)__builtin_return_address(0) - g_base), buf, changed);
    }
  }
  g_origSceneReplace(scene, id, behaviours, keepTree);
}

static struct { void *pawn[20], *target; int n, round; DWORD at; } g_squadWatch = {};
static void SquadWatchTick() {
  if (!g_squadWatch.n || (int)(GetTickCount() - g_squadWatch.at) < 0)
    return;
  char buf[400];
  int len = 0;
  float tl[3] = {0, 0, 0};
  bool ht = g_squadWatch.target && ObjectLive(g_squadWatch.target) && ActorLocation(g_squadWatch.target, tl);
  for (int i = 0; i < g_squadWatch.n && len < 360; ++i) {
    uint8_t *p = (uint8_t *)g_squadWatch.pawn[i];
    uint8_t *c = p && ObjectLive(p) ? *(uint8_t **)(p + OFF_PAWN_CONTROLLER) : nullptr;
    float pl[3];
    int dist = ht && c && ActorLocation(p, pl) ? (int)sqrtf((pl[0] - tl[0]) * (pl[0] - tl[0]) + (pl[1] - tl[1]) * (pl[1] - tl[1]) + (pl[2] - tl[2]) * (pl[2] - tl[2])) : -1;
    len += snprintf(buf + len, sizeof buf - len, " v%d:%d@%d", i & 3, c && IsReadable(c + OFF_NPC_BEHAVIOUR_STATE, 1) ? c[OFF_NPC_BEHAVIOUR_STATE] : -1, dist);
  }
  ++g_squadWatch.round;
  float thp = ht ? CallFloatGetter(g_squadWatch.target, "GetCurrentHealth") : -1.f;
  Logf("commands: squad -- behaviour/distance %d s after engage:%s; target health %.0f", g_squadWatch.round * 5, buf, thp);
  g_squadWatch.at = GetTickCount() + 5000;
  if (g_squadWatch.round >= 12)
    g_squadWatch.n = 0;
}

// Owners and offsets of the creep weapon fields (operator 2026-10-21:
// fighters "don't attack"; the exe has m_fakeWeaponDamageCreep and
// m_maxFireDistanceCreeps). Logged once with the value read off the first
// object of that class found.
static void CreepFieldProbe() {
  static bool done = false;
  if (done)
    return;
  done = true;
  static const wchar_t *names[] = {L"m_fakeWeaponDamageCreep", L"m_maxFireDistanceCreeps", L"m_proximityAgainstCreeps",
                                   L"m_creepAIController", L"m_damagesCreepsAffectedByPulses", L"m_targetCreeps"};
  FNameMin fn[6];
  for (int i = 0; i < 6; ++i)
    ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&fn[i], names[i], 1);
  FUObjectArrayMin *arr = GObjects();
  if (!arr || !arr->Objects)
    return;
  for (int i = 0; i < arr->NumElements; ++i) {
    UObjectMin *o = arr->Objects[i].Object;
    if (!o || !o->Class || !IsReadable(o, 0x58))
      continue;
    for (int f = 0; f < 6; ++f)
      if (o->Name.ComparisonIndex == fn[f].ComparisonIndex && strstr(ClassNameOf(o), "Property")) {
        UObjectMin *owner = o->Outer;
        const char *on = owner ? NameText(owner->Name) : nullptr;
        Logf("commands: swarm -- field %S %s owned by %s +0x%x", names[f], ClassNameOf(o), on ? on : "?",
             *(uint32_t *)((uint8_t *)o + 0x50));
      }
  }
}

static void CommandSwarm(uint8_t *gm, int size, int n, int team) {
  if (size == 0)
    CreepFieldProbe();
  const char *what = size == 0 ? "swarm" : "squad";
  uint8_t *gs = IsReadable(gm + OFF_GM_GAMESTATE, 8) ? *(uint8_t **)(gm + OFF_GM_GAMESTATE) : nullptr;
  if (!gs)
    return;
  if (InvasionGameState(gm) && (g_onsCreeps.gm != gm || !g_onsCreeps.nPoints)) {
    if (g_onsCreeps.gm != gm) {
      memset(&g_onsCreeps, 0, sizeof(g_onsCreeps));
      g_onsCreeps.gm = gm;
      g_onsCreeps.firstSeen = GetTickCount();
    }
    FindOnslaughtSpawnPoints();
  }
  void *starts[64];
  int ns = g_onsCreeps.gm == gm && g_onsCreeps.nPoints ? 0 : FindPlayerStarts(starts, 64);
  uint8_t *pcs[64];
  int np = CollectHumanPCs(gm, pcs, 64), made = 0, moved = 0;
  float hum[64][3];
  void *hpawn[64];
  int nh = 0, engaged = 0;
  for (int i = 0; i < np; ++i) {
    void *hp = IsReadable(pcs[i] + OFF_CONTROLLER_PAWN, 8) ? *(void **)(pcs[i] + OFF_CONTROLLER_PAWN) : nullptr;
    if (hp && ActorLocation(hp, hum[nh]))
      hpawn[nh++] = hp;
  }
  int hi = nh ? rand() % nh : -1;
  // One cluster per command, so a swarm arrives together.
  float c[3] = {0, 0, 0};
  if (nh) {
    float *h = hum[hi], a = (float)(rand() % 6283) / 1000.f, d = 6000.f + (float)(rand() % 6001);
    c[0] = h[0] + cosf(a) * d, c[1] = h[1] + sinf(a) * d, c[2] = h[2] + (float)(rand() % 2001 - 500);
  }
  // /wave sends /boss first: gather around that boss (operator 2026-10-21:
  // the boss came in far off, the squads next to the player).
  float bl[3];
  if (g_lastBoss.pawn && (int)(GetTickCount() - g_lastBoss.at) < 20000 && ObjectLive(g_lastBoss.pawn) &&
      ActorLocation(g_lastBoss.pawn, bl)) {
    float a = (float)(rand() % 6283) / 1000.f, d = 3000.f + (float)(rand() % 2001);
    c[0] = bl[0] + cosf(a) * d, c[1] = bl[1] + sinf(a) * d, c[2] = bl[2];
    Logf("commands: %s -- gathering at the boss %.0f %.0f %.0f", what, bl[0], bl[1], bl[2]);
  }
  for (int k = 0; k < n; ++k) {
    void *pawn = SwarmOne(gm, gs, size, team, starts, ns);
    if (!pawn)
      break;
    ++made;
    if (nh) {
      float r = size == 0 ? 2500.f : 4000.f;
      float at[3] = {c[0] + r * (2.f * rand() / RAND_MAX - 1.f), c[1] + r * (2.f * rand() / RAND_MAX - 1.f),
                     c[2] + 0.5f * r * (2.f * rand() / RAND_MAX - 1.f)};
      moved += TeleportActor(pawn, at, (float)(rand() % 360));
      // onssquad4: of ENGAGE (-1 / own id), SKIRMISH and GOTO (variant 3)
      // re-asserted through the hook, only GOTO moved: 9000 -> 2500 uu from
      // the player and held; ENGAGE and SKIRMISH sat still for 60 s.
      if (g_squadIds.gm != gm)
        g_squadIds.n = 0, g_squadIds.gm = gm;
      if (!k)
        g_squadIds.logged = 0;
      if (size == 1 && SquadEngage(pawn, hpawn[hi], 3)) {
        ++engaged;
        if (!k)
          g_squadWatch.n = g_squadWatch.round = 0, g_squadWatch.target = hpawn[hi];
        if (g_squadWatch.n < 20)
          g_squadWatch.pawn[g_squadWatch.n++] = pawn, g_squadWatch.at = GetTickCount() + 5000;
      }
    }
    if (size == 0) {
      // Fighters "don't attack" (operator 2026-10-21). Offsets from the
      // CreepFieldProbe log (onsrain2): YCreepPawn m_fakeWeaponDamageCreep
      // +0xc7c, YCreepAIController m_maxFireDistanceCreeps +0x7c4.
      // GUESS: the fire distance gates their shooting; tripled (at least
      // 20000 uu) to see whether they open fire.
      uint8_t *cc = *(uint8_t **)((uint8_t *)pawn + OFF_PAWN_CONTROLLER);
      bool isCreepCtrl = cc && (strstr(ClassNameOf((UObjectMin *)cc), "Creep") || strstr(ClassNameOf((UObjectMin *)cc), "AITargetL"));
      float *fd = isCreepCtrl && IsReadable(cc + 0x7c4, 4) ? (float *)(cc + 0x7c4) : nullptr;
      if (k == 0)
        Logf("commands: swarm -- controller %s, fake damage %.1f, max fire distance %.0f", cc ? ClassNameOf((UObjectMin *)cc) : "none",
             *(float *)((uint8_t *)pawn + 0xc7c), fd ? *fd : -1.f);
      if (fd && *fd >= 0.f && *fd < 1e6f)
        *fd = *fd * 3.f < 20000.f ? 20000.f : *fd * 3.f;
      if (k == 0 && fd)
        Logf("commands: swarm -- max fire distance now %.0f", *fd);
    }
    if (k == 0)
      Logf("commands: %s -- first one %s, ship team byte %d", what, ClassNameOf((UObjectMin *)pawn), ((uint8_t *)pawn)[OFF_SHIP_TEAM]);
  }
  Logf("commands: %s -- %d of %d %s for team %d (%s), %d moved near a player, %d told to engage", what, made, n,
       size == 0 ? "fighters" : "assault ships", team, ns ? "player starts" : "Onslaught points", moved, engaged);
}

// /hunters [n] [team] [all] (operator 2026-10-21: "make better NPC enemies";
// 10-22: "pulled from a better curated pool"): n hunters per human ship, or n
// in all spread over the players with "all" (/wave), drawn by weight from the
// admin Hunters pool (GET /battle/hunters?n=K, admin_hunters.go; "hunterdef
// <id>" asks for that one). Each is a BossDef flagged hunter: its base ShipId
// spawned like /spawn, then its modules, coatings and feats applied as an
// elite without the boss treatment. Packs are teleported 6000-9000 uu from
// their player and sent there with the /squad GOTO (re-asserted by
// HookSceneReplace on the nearest player). Enemy team by default. With no
// pool (mmogbrain down or every weight 0) a random class x weight T5 hull,
// as before. GUESS: max 12 in all, the distances, and that a T5 AI ship
// within 2500 uu opens fire on its own.
static void CommandHunters(uint8_t *gm, int per, int team, bool all, int id) {
  static const char *cls[] = {"Assault", "Dreadnought", "Scout", "Sniper", "Support"}, *wts[] = {"Heavy", "Medium", "Light"};
  uint8_t *pcs[64];
  int np = CollectHumanPCs(gm, pcs, 64), made = 0, engaged = 0, nh = 0;
  void *hpawn[64];
  float hum[64][3];
  for (int i = 0; i < np; ++i) {
    void *hp = IsReadable(pcs[i] + OFF_CONTROLLER_PAWN, 8) ? *(void **)(pcs[i] + OFF_CONTROLLER_PAWN) : nullptr;
    if (hp && ActorLocation(hp, hum[nh]))
      hpawn[nh++] = hp;
  }
  if (!nh)
    return (void)Logf("commands: hunters -- no live player ship");
  int total = std::min(12, id > 0 ? 1 : all ? per : per * nh);
  // The pool's picks, one "--" block each.
  static char body[16384];
  char q[64];
  const BossDef *defs[12];
  int mults[12], nd = 0;
  if (id > 0)
    snprintf(q, sizeof q, "/battle/hunters?id=%d", id);
  else
    snprintf(q, sizeof q, "/battle/hunters?n=%d", total);
  if (HttpGetLoopback(q, body, sizeof body))
    for (char *blk = strstr(body, "\n--\n"); blk && nd < 12;) {
      blk += 4;
      char *next = strstr(blk, "\n--\n");
      if (next)
        next[1] = 0; // keep the block's last newline, cut the separator
      BossDef &d = ParseBossDef(blk, 0);
      char v[16];
      d.hunter = true;
      if (d.ship[0])
        mults[nd] = std::min(3, std::max(1, atoi(FieldValue(blk, "mult", v, sizeof v) ? v : "1"))), defs[nd++] = &d;
      blk = next;
    }
  if (id > 0 && !nd)
    return (void)Logf("commands: hunters -- no hunter %d in the pool", id);
  if (g_squadIds.gm != gm)
    g_squadIds.n = 0, g_squadIds.gm = gm;
  g_squadIds.logged = 0;
  g_squadWatch.n = g_squadWatch.round = 0, g_squadWatch.target = hpawn[0];
  char names[240];
  int len = 0;
  names[0] = 0;
  float c[64][3];
  for (int h = 0; h < nh; ++h) {
    float a = (float)(rand() % 6283) / 1000.f, d = 6000.f + (float)(rand() % 3001);
    c[h][0] = hum[h][0] + cosf(a) * d, c[h][1] = hum[h][1] + sinf(a) * d, c[h][2] = hum[h][2] + (float)(rand() % 1501 - 500);
  }
  for (int k = 0; k < total; ++k) {
    int h = all || id > 0 ? k % nh : k / std::max(1, per);
    if (h >= nh)
      break;
    const BossDef *def = nd ? defs[k % nd] : nullptr;
    char ship[48];
    if (def)
      snprintf(ship, sizeof ship, "%s", def->ship);
    else
      snprintf(ship, sizeof ship, "%s%sT5", cls[rand() % 5], wts[rand() % 3]);
    g_lastSpawnPawn = nullptr;
    g_spawnLabel = def ? def->name : L"Hunter";
    g_spawnDef = def;
    g_spawnNoGameBot = true;
    CommandSpawn(gm, ship, 1, def ? mults[k % nd] : 1);
    g_spawnNoGameBot = false;
    g_spawnDef = nullptr;
    g_spawnLabel = nullptr;
    void *pawn = g_lastSpawnPawn;
    if (!pawn || !ObjectLive(pawn))
      continue;
    ++made;
    if (len < 200)
      len += def ? snprintf(names + len, sizeof names - len, " %ls(%s)", def->name, ship)
                 : snprintf(names + len, sizeof names - len, " %s", ship);
    if (team) {
      uint8_t t = (uint8_t)team;
      __try {
        ((tPawnSetTeam)(g_base + RVA_PAWN_SET_TEAM))(pawn, &t);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("commands: hunters -- team %d faulted", team);
      }
    }
    float at[3] = {c[h][0] + 2500.f * (2.f * rand() / RAND_MAX - 1.f), c[h][1] + 2500.f * (2.f * rand() / RAND_MAX - 1.f),
                   c[h][2] + 1000.f * (2.f * rand() / RAND_MAX - 1.f)};
    TeleportActor(pawn, at, (float)(rand() % 360));
    if (SquadEngage(pawn, hpawn[h], 3)) {
      ++engaged;
      if (g_squadWatch.n < 20 && hpawn[h] == g_squadWatch.target)
        g_squadWatch.pawn[g_squadWatch.n++] = pawn, g_squadWatch.at = GetTickCount() + 5000;
    }
  }
  Logf("commands: hunters -- %d of %d ships (%s %d, %d players, %d from the pool), %d sent in, team %s:%s", made, total,
       all || id > 0 ? "in all" : "per player", per, nh, nd, engaged, team ? "set" : "enemy", names);
}


static void CommandDifficulty(uint8_t *gm, const char *arg) {
  int want = !strcmp(arg, "recruit") ? 1 : !strcmp(arg, "veteran") ? 2 : !strcmp(arg, "legendary") ? 3 : 0;
  uint8_t *gs = IsReadable(gm + OFF_GM_GAMESTATE, 8) ? *(uint8_t **)(gm + OFF_GM_GAMESTATE) : nullptr;
  int off = gs && ClassChainContains((UObjectMin *)gs, "YGameState_PVE") ? PropOffset((UObjectMin *)gs, "m_pveDifficulty") : -1;
  if (!want || off <= 0) {
    Logf("commands: difficulty %s -- not applied (gs %p, offset %d)", arg, gs, off);
    return;
  }
  int was = gs[off];
  gs[off] = (uint8_t)want;
  Logf("commands: difficulty %d -> %d", was, want);
}

// Actor rotation (pitch, yaw, roll degrees) through K2_GetActorRotation, the
// FRotator twin of K2_GetActorLocation.
static bool ActorRotation(void *actor, float out[3]) {
  alignas(16) uint8_t parms[256];
  struct Ctx { uint32_t off; } c = {0xFFFFFFFF};
  auto fill = [](uint8_t *, const char *, const char *pn, uint32_t off, void *ctx) {
    if (!strcmp(pn, "ReturnValue"))
      ((Ctx *)ctx)->off = off;
  };
  __try {
    if (!CallNamed(actor, L"K2_GetActorRotation", fill, &c, parms) || c.off == 0xFFFFFFFF)
      return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  memcpy(out, parms + c.off, 12);
  return true;
}

// Live projectiles for the viewer: every GObjects entry whose class chain
// names a projectile actor, verdict cached per UClass. Which classes are
// projectiles in flight is not known yet: the client's strings name
// YProjectile, YProjectileTracking, YVimInstanceWeaponProjectile, and
// YProjectileWeapon (GUESS: the gun, not the shot, so excluded) plus
// non-actor YProjectileManager / ...ContentReferenceTracker / ...Ability.
// The first scans log every class that passed with its count, to settle it.
// Open-addressed by UClass pointer: the scan touches every object, so the
// per-object cost must be a pointer hash, not IsReadable (a VirtualQuery).
struct ProjClass {
  void *cls;
  bool proj;
  int seen;
};
enum { PROJ_SLOTS = 8192 };
static ProjClass g_projCls[PROJ_SLOTS];

static void DescribeProps(const char *tag, UObjectMin *obj, const char *sub);
// Beam weapons are not projectiles: the viewer needs their source/end while
// firing. Layout unknown -- log, for the first two instances, the reflected
// properties and functions on the class chain that look beam/fire related.
static void BeamProbe(UObjectMin *o) {
  static int s_done = 0;
  if (s_done >= 2)
    return;
  ++s_done;
  Logf("beam probe: %s (%s)", NameText(o->Name), ClassNameOf(o));
  DescribeProps("beam probe:", o, "eam");
  DescribeProps("beam probe:", o, "Fir");
  DescribeProps("beam probe:", o, "arget");
  for (UObjectMin *cls = o->Class; cls && IsReadable(cls, 0x40); cls = *(UObjectMin **)((uint8_t *)cls + OFF_USTRUCT_SUPER)) {
    const char *cn = NameText(cls->Name);
    if (!cn || !strcmp(cn, "Actor"))
      break;
    for (UObjectMin *f = *(UObjectMin **)((uint8_t *)cls + 0x38); f && IsReadable(f, 0x58); f = *(UObjectMin **)((uint8_t *)f + 0x28))
      if (!strcmp(ClassNameOf(f), "Function"))
        Logf("beam probe:   fn %s.%s", cn, NameText(f->Name));
  }
}

static bool ProjectileClass(UObjectMin *o) {
  void *cls = o->Class;
  if (!cls)
    return false;
  uint32_t h = (uint32_t)(((uintptr_t)cls >> 4) * 2654435761u) % PROJ_SLOTS;
  for (uint32_t k = 0; k < PROJ_SLOTS; ++k, h = (h + 1) % PROJ_SLOTS) {
    ProjClass &e = g_projCls[h];
    if (e.cls == cls)
      return ++e.seen, e.proj;
    if (e.cls)
      continue;
    const char *cn = IsReadable(cls, sizeof(UObjectMin)) ? ClassNameOf(o) : nullptr;
    // "Projectile" alone also matches ProjectileMovementComponent and
    // YProjectileReplicator; the exact Actor test is what excludes them.
    bool p = cn && ClassChainContains(o, "Projectile") && ClassChainIs(o, "Actor") && !strstr(cn, "Manager") &&
             !strstr(cn, "Tracker") && !strstr(cn, "Ability") &&
             !(ClassChainContains(o, "ProjectileWeapon") && !ClassChainContains(o, "WeaponProjectile"));
    e = {cls, p, 1};
    return p;
  }
  return false;
}

// The scan used to walk all of GObjects inside every snapshot: 80.55 ms over
// 98177 objects on the game thread every 2 s (first scan of a host, 13:09:07,
// DeepSeek round 12) -- a hitch players feel. Now ProjScanStep walks it a slice
// at a time from FrameTick (PROJ_SCAN_MS per frame gap) into a candidate list,
// and the snapshot reads the last completed list, re-checking each entry is
// still the same live object. The stored pointer is only compared with the
// GObjects slot, never dereferenced until that check passes, so a freed
// candidate is skipped, not used. A pass takes a fraction of a second; shells newer
// than the last pass are missed until the next.
enum { VIEWER_MAX_PROJ = 256, PROJ_LIST = 1024 };
static const double PROJ_SCAN_MS = 1.0;
static int g_projIdx[2][PROJ_LIST];
static void *g_projObj[2][PROJ_LIST];
static int g_projN[2], g_projCur = -1; // g_projCur: last completed list, -1 none yet
static volatile bool g_viewerOn = false, g_projScanOff = false;

static void ProjScanStep(double budgetMs) {
  static int s_at = 0, s_n = 0, s_arch = 0, s_over = 0, s_passes = 0;
  static double s_ms = 0, s_wall0 = 0;
  FUObjectArrayMin *arr = GObjects();
  if (!arr || !arr->Objects)
    return;
  int b = g_projCur == 0 ? 1 : 0;
  double t0 = NowMs(), now = t0;
  if (s_at == 0)
    s_wall0 = t0;
  for (; s_at < arr->NumElements && now - t0 < budgetMs; ++s_at) {
    if ((s_at & 31) == 31) // cold classes cost a VirtualQuery + chain walk each
      now = NowMs();
    FUObjectItemMin &it = arr->Objects[s_at];
    UObjectMin *o = it.Object;
    if (!o || (it.Flags & OBJ_PENDING_KILL))
      continue;
    if (!ProjectileClass(o)) { // caller's __try covers the rest
      static int s_beams = 0;
      if (s_beams < 2 && s_passes < 3 && o->Class && ClassChainIs(o, "Actor") && ClassChainContains(o, "BeamWeapon")) {
        const char *bn = NameText(o->Name);
        if (bn && strncmp(bn, "Default__", 9))
          ++s_beams, BeamProbe(o);
      }
      continue;
    }
    const char *nm = NameText(o->Name);
    if (!nm || !strncmp(nm, "Default__", 9)) {
      ++s_arch;
      continue;
    }

    if (s_n < PROJ_LIST)
      g_projIdx[b][s_n] = s_at, g_projObj[b][s_n] = o, ++s_n;
    else
      ++s_over;
  }
  s_ms += NowMs() - t0;
  if (s_at < arr->NumElements)
    return;
  g_projN[b] = s_n;
  g_projCur = b;
  if (++s_passes <= 2 || s_passes % 100 == 0) {
    Logf("viewer: projectile scan pass %d: %d candidates (%d over the cap, %d archetypes skipped) over %d "
         "objects, %.2f ms scanning, %.0f ms wall",
         s_passes, s_n, s_over, s_arch, arr->NumElements, s_ms, NowMs() - s_wall0);
    if (s_passes <= 2)
      for (int i = 0; i < PROJ_SLOTS; ++i)
        if (g_projCls[i].proj)
          Logf("viewer:   projectile class %s seen %d", NameText(((UObjectMin *)g_projCls[i].cls)->Name),
               g_projCls[i].seen);
  }
  s_at = s_n = s_arch = s_over = 0;
  s_ms = 0;
}

// Appends "p x y z kind id" lines, at most VIEWER_MAX_PROJ, to body, from the last
// completed scan list.
static int ViewerProjectiles(char *body, int len, int cap) {
  FUObjectArrayMin *arr = GObjects();
  int cur = g_projCur;
  if (!arr || !arr->Objects || cur < 0)
    return len;
  int n = 0, gone = 0, noloc = 0;
  for (int k = 0; k < g_projN[cur] && n < VIEWER_MAX_PROJ && len < cap - 64; ++k) {
    int i = g_projIdx[cur][k];
    if (i >= arr->NumElements || arr->Objects[i].Object != g_projObj[cur][k] ||
        (arr->Objects[i].Flags & OBJ_PENDING_KILL)) {
      ++gone;
      continue;
    }
    float at[3];
    if (!ActorLocation((UObjectMin *)g_projObj[cur][k], at)) {
      ++noloc;
      continue;
    }
    // Pooled shells park at ~1e7 uu (live snapshot 2026-10-17): not in flight.
    if (fabsf(at[0]) > 5e6f || fabsf(at[1]) > 5e6f || fabsf(at[2]) > 5e6f)
      continue;
    // Kind for the viewer: m missile (trail), b ballistic shell, p other.
    const char *cn = ClassNameOf((UObjectMin *)g_projObj[cur][k]);
    char kind = cn && strstr(cn, "Missile") ? 'm' : cn && strstr(cn, "Ballistic") ? 'b' : 'p';
    // id: the GObjects index, so the viewer can follow one shot instead of
    // pairing nearest neighbours. Shells are pooled, so one index can be a
    // new shot later; the viewer also checks the motion.
    int w = _snprintf_s(body + len, cap - len, _TRUNCATE, "p %.0f %.0f %.0f %c %d\n", at[0], at[1], at[2], kind, i);
    if (w > 0)
      len += w, ++n;
  }
  static int s_calls = 0;
  if (++s_calls <= 3 || s_calls % 150 == 0) // why candidates drop: gone since the pass, or no location
    Logf("viewer: snapshot %d: %d projectiles of %d candidates (%d gone since the scan, %d without a location)",
         s_calls, n, g_projN[cur], gone, noloc);
  return len;
}

// Battle viewer (admin dashboard): every 2 s, one line per live ship to
// POST /battle/snapshot?match=<id>: "x y z team hp% npc pid class yaw vid name
// cloaked", then
// "p x y z" per live projectile (ViewerProjectiles). Ships come
// from the feats-component registry (every ship pawn with a UYFeatsComponent;
// GUESS: fighters/drones without one are not shown). Off with
// dn_host_no_viewer.txt / DN_HOST_NO_VIEWER.
// The snapshot POST runs on its own thread: HttpGetLoopbackRaw is synchronous
// with 2 s socket timeouts, so a slow or restarting mmogbrain froze the game
// thread for 2 s per snapshot (DeepSeek round 12). One slot, newest wins: a
// snapshot not yet sent when the next arrives is stale anyway.
static CRITICAL_SECTION g_snapCs;
static HANDLE g_snapEv = nullptr;
static char g_snapBody[40960], g_snapPath[160];
static int g_snapLen = -1; // -1 empty
static DWORD WINAPI SnapshotPostThread(LPVOID) {
  static char body[40960], path[160], buf[512];
  for (;;) {
    WaitForSingleObject(g_snapEv, INFINITE);
    EnterCriticalSection(&g_snapCs);
    int len = g_snapLen;
    if (len >= 0)
      memcpy(body, g_snapBody, len), memcpy(path, g_snapPath, sizeof path);
    g_snapLen = -1;
    LeaveCriticalSection(&g_snapCs);
    if (len < 0)
      continue;
    const char *raw;
    int n;
    HttpGetLoopbackRaw(path, buf, (int)sizeof buf, &raw, &n, body, len);
  }
}
static void SnapshotPost(const char *match, const char *body, int len) {
  static int s_mode = 0; // 0 untried, 1 thread, 2 inline
  if (!s_mode) {
    InitializeCriticalSection(&g_snapCs);
    g_snapEv = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    s_mode = g_snapEv && CreateThread(nullptr, 0, SnapshotPostThread, nullptr, 0, nullptr) ? 1 : 2;
    if (s_mode == 2)
      Logf("viewer: no snapshot post thread -- posting inline");
  }
  if (s_mode == 2) {
    {
      char path[160], buf[512];
      _snprintf_s(path, sizeof path, _TRUNCATE, "/battle/snapshot?match=%s", match);
      const char *raw;
      int n;
      HttpGetLoopbackRaw(path, buf, (int)sizeof buf, &raw, &n, body, len);
      return;
    }
  }
  if (len > (int)sizeof g_snapBody)
    len = (int)sizeof g_snapBody;
  EnterCriticalSection(&g_snapCs);
  memcpy(g_snapBody, body, len);
  g_snapLen = len;
  _snprintf_s(g_snapPath, sizeof g_snapPath, _TRUNCATE, "/battle/snapshot?match=%s", match);
  LeaveCriticalSection(&g_snapCs);
  SetEvent(g_snapEv);
}

// The name an NPC carries in game (its nameplate): APawn::PlayerState ->
// APlayerState::PlayerName, both by reflection. The game's wave spawner names
// it from the spawn entry (+0x60 FName, see GiveNpcPlayerState); our own
// creeps get the name we pass there. Percent-encoded (spaces and anything
// outside printable ASCII) so it stays one field; "-" when there is none.
// APlayerState::PlayerName by reflection, percent-encoded as above; out is
// left alone when there is none.
static void PlayerStateName(void *psv, char *out, int cap) {
  __try {
    // Cached per class: a YNPCReplicationInfo (creeps, game bots) has no
    // PlayerName, and one offset cached from either kind is wrong for the other.
    static UObjectMin *s_cls[4];
    static int s_off[4], s_next;
    UObjectMin *ps = (UObjectMin *)psv;
    if (!ps || !ObjectLive(ps))
      return;
    int s_nameOff = -1, k = 0;
    while (k < 4 && s_cls[k] != ps->Class)
      ++k;
    if (k < 4)
      s_nameOff = s_off[k];
    else
      s_nameOff = PropOffset(ps, "PlayerName"), k = s_next++ & 3, s_off[k] = s_nameOff, s_cls[k] = ps->Class;
    if (s_nameOff < 0)
      return;
    FStringMin *fs = (FStringMin *)((uint8_t *)ps + s_nameOff);
    if (!fs->data || fs->num <= 1 || fs->num > 64 || !IsReadable(fs->data, fs->num * 2))
      return;
    int n = 0;
    for (int i = 0; i < fs->num - 1 && n < cap - 4; ++i) {
      wchar_t c = fs->data[i];
      if (c > 32 && c < 127 && c != '%')
        out[n++] = (char)c;
      else if (c < 256)
        n += _snprintf_s(out + n, cap - n, _TRUNCATE, "%%%02X", (unsigned)c);
    }
    out[n] = 0;
    if (!n)
      strcpy_s(out, cap, "-");
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    strcpy_s(out, cap, "-");
  }
}

static void NpcDisplayName(void *pawn, char *out, int cap) {
  __try {
    static int s_psOff = -1, s_told;
    if (s_psOff < 0) // not cached while a miss: the first pawn may lack it
      s_psOff = PropOffset((UObjectMin *)pawn, "PlayerState");
    if (s_psOff < 0) {
      if (s_told++ < 4)
        Logf("viewer: NPC %p (%s) has no reflected PlayerState", pawn, ClassNameOf((UObjectMin *)pawn));
      return;
    }
    void *ps = *(void **)((uint8_t *)pawn + s_psOff);
    PlayerStateName(ps, out, cap);
    // Game bots: the YNPCReplicationInfo is at pawn +0x658, not in the
    // reflected PlayerState (gamebot-name6: label "-" with m_NPCName set).
    void *info = IsReadable((uint8_t *)pawn + OFF_PAWN_NPC_PLAYERSTATE, 8)
                     ? *(void **)((uint8_t *)pawn + OFF_PAWN_NPC_PLAYERSTATE) : nullptr;
    if (!strcmp(out, "-") && !NpcInfoName(ps, out, cap) && !(info && NpcInfoName(info, out, cap))) {
      if (s_told < 8 && ps && ObjectLive(ps)) {
        int off = NpcNameOffset((UObjectMin *)ps);
        ++s_told, Logf("viewer: NPC %p (%s) PlayerState %p (%s) / +0x658 %p has no name; m_NPCName +0x%X data %p", pawn,
                       ClassNameOf((UObjectMin *)pawn), ps, ClassNameOf((UObjectMin *)ps), info, off,
                       off >= 0 ? *(void **)((uint8_t *)ps + off) : nullptr);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    strcpy_s(out, cap, "-");
  }
}

// Scoreboard and kill feed lines for the viewer, appended to a snapshot:
//   "t kills1 kills2 points1 points2" (points -1 outside Onslaught),
//   "s team kills deaths assists score bot pid name" per player state in
//   GameState PlayerArray that is a YPlayerReplicationInfo (creeps carry a
//   YNPCReplicationInfo and are left out; bot = its owner is no
//   PlayerController), and "k seq kteam vteam kclass vclass kpid vpid kname
//   vname" for the recent kills (ViewerRecordKill).
static int ViewerScoreLines(uint8_t *gm, char *body, int len, int cap) {
  uint8_t *gs = gm && IsReadable(gm + OFF_GM_GAMESTATE, 8) ? *(uint8_t **)(gm + OFF_GM_GAMESTATE) : nullptr;
  if (gs && IsReadable(gs + OFF_GS_KILLS_T2, 4)) {
    int k1 = *(int32_t *)(gs + OFF_GS_KILLS_T1), k2 = *(int32_t *)(gs + OFF_GS_KILLS_T2), p1 = -1, p2 = -1;
    if (g_ons.gm == gm && InvasionGameState(gm) == gs)
      p1 = OnslaughtPoints(gs, 1), p2 = OnslaughtPoints(gs, 2);
    int w = _snprintf_s(body + len, cap - len, _TRUNCATE, "t %d %d %d %d\n", k1, k2, p1, p2);
    if (w > 0)
      len += w;
  }
  if (gs && IsReadable(gs + OFF_GS_PLAYER_ARRAY, 16)) {
    uint8_t **arr = *(uint8_t ***)(gs + OFF_GS_PLAYER_ARRAY);
    int count = *(int32_t *)(gs + OFF_GS_PLAYER_ARRAY + 8);
    if (count > 0 && count <= 128 && IsReadable(arr, (size_t)count * 8))
      for (int i = 0; i < count && len < cap - 256; ++i) {
        uint8_t *pri = arr[i];
        if (!IsReadable(pri, OFF_PRI_TEAM + 1) || !ObjectLive(pri) ||
            !ClassChainContains((UObjectMin *)pri, "YPlayerReplicationInfo"))
          continue;
        uint8_t *owner = IsReadable(pri + OFF_ACTOR_OWNER, 8) ? *(uint8_t **)(pri + OFF_ACTOR_OWNER) : nullptr;
        bool human = owner && ObjectLive(owner) && ClassChainIs((UObjectMin *)owner, "PlayerController");
        int team = pri[OFF_PRI_TEAM];
        if ((team < 1 || team > 2) && human && IsReadable(owner + OFF_PC_TEAM, 1))
          team = owner[OFF_PC_TEAM];
        char pid[80] = "-", nm[96] = "-";
        if (human && (!RememberedControllerPID(owner, pid, sizeof pid) || !pid[0]))
          strcpy_s(pid, sizeof pid, "-");
        PlayerStateName(pri, nm, sizeof nm);
        int w = _snprintf_s(body + len, cap - len, _TRUNCATE, "s %d %d %d %d %d %d %s %s\n", team,
                            *(int32_t *)(pri + OFF_PRI_KILLS), *(int32_t *)(pri + OFF_PRI_DEATHS),
                            *(int32_t *)(pri + OFF_PRI_ASSISTS), *(int32_t *)(pri + OFF_PRI_SCORE), human ? 0 : 1, pid, nm);
        if (w > 0)
          len += w;
      }
  }
  for (uint32_t q = g_vKillSeq > VIEWER_KILLS ? g_vKillSeq - VIEWER_KILLS + 1 : 1; q <= g_vKillSeq && len < cap - 400; ++q) {
    ViewerKill &k = g_vKills[q % VIEWER_KILLS];
    if (k.seq != q)
      continue;
    int w = _snprintf_s(body + len, cap - len, _TRUNCATE, "k %u %d %d %s %s %s %s %s %s\n", k.seq, k.kt, k.vt, k.kcls,
                        k.vcls, k.kpid, k.vpid, k.kname, k.vname);
    if (w > 0)
      len += w;
  }
  return len;
}

// Cloaked, as the client gates enemy markers (DeepSeek 10-17,
// OnSetEnemySpotted 0x5BD5E0): the weak pointer at pawn+0x814 names the
// UYCloakingComponent, whose byte +0x108 is nonzero while cloaked.
static bool PawnCloaked(void *pawn) {
  __try {
    uint8_t *p = (uint8_t *)pawn;
    if (!IsReadable(p + 0x814, 8) || !*(uint64_t *)(p + 0x814))
      return false;
    uint8_t *cc = (uint8_t *)((tWeakGet)(g_base + RVA_WEAK_PTR_GET))(p + 0x814);
    return cc && IsReadable(cc + 0x108, 1) && cc[0x108];
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static void ViewerSnapshot(const char *match) {
  static int s_on = -1;
  if (s_on < 0)
    s_on = SwitchOn("DN_HOST_NO_VIEWER", "dn_host_no_viewer.txt") ? 0 : 1;
  if (!s_on)
    return;
  g_viewerOn = g_frameWanted = true;
  static char body[40960];
  int len = 0;
  for (FeatsByPawn &e : g_featsByPawn) {
    void *pawn = e.pawn;
    // Slots go stale when a ship dies, and its memory is reused: onsfun12 had
    // a WeaponMesh SkeletalMeshComponent at a dead pawn's address, the freed
    // feats component still naming that address as its owner, and
    // K2_GetActorLocation on it was fatal ("Failed to find function ... in
    // SkeletalMeshComponent", ScriptCore.cpp:1268). So the component must be
    // live and the object must still be a pawn.
    if (!pawn || len > (int)sizeof body - 256 || !ObjectLive(pawn) || !ObjectLive(e.comp) ||
        FeatsCompOf(pawn) != e.comp || !ClassChainIs((UObjectMin *)pawn, "Pawn"))
      continue;
    float at[3];
    if (!ActorLocation(pawn, at))
      continue;
    float hp = CallFloatGetter(pawn, "GetCurrentHealth");
    if (hp != hp)
      hp = CallFloatGetter(pawn, "GetHealth");
    float mx = CallFloatGetter(pawn, "GetMaxHealth");
    int pct = (hp == hp && mx == mx && mx > 0.f) ? (int)(100.f * hp / mx + 0.5f) : -1;
    if (pct == 0)
      continue; // wreck still drifting
    bool npc = PawnIsNpc(pawn) || pawn == g_sky.pawn[0] || pawn == g_sky.pawn[1]; // heralds have no controller
    char pid[80] = "-";
    if (!npc && IsReadable((uint8_t *)pawn + OFF_PAWN_CONTROLLER, 8)) {
      void *pc = *(void **)((uint8_t *)pawn + OFF_PAWN_CONTROLLER);
      if (!pc || !RememberedControllerPID(pc, pid, sizeof pid) || !pid[0])
        strcpy_s(pid, sizeof pid, "-");
    }
    float rot[3];
    char yaw[16] = "-";
    if (ActorRotation(pawn, rot))
      _snprintf_s(yaw, sizeof yaw, _TRUNCATE, "%.0f", rot[1]);
    char nm[96] = "-";
    if (npc) {
      NpcDisplayName(pawn, nm, sizeof nm);
    }
    int w = _snprintf_s(body + len, sizeof body - len, _TRUNCATE, "%.0f %.0f %.0f %d %d %d %s %s %s %d %s %d\n", at[0], at[1],
                        at[2], ShipTeam(pawn), pct, npc ? 1 : 0, pid, ClassNameOf(pawn), yaw, e.vid, nm,
                        PawnCloaked(pawn) ? 1 : 0);
    if (w > 0)
      len += w;
  }
  // Scores change slowly: once a second is plenty (the kill lines ride along).
  static ULONGLONG s_scoreAt = 0;
  if (GetTickCount64() - s_scoreAt >= 1000)
    __try {
      s_scoreAt = GetTickCount64();
      len = ViewerScoreLines(g_terrainGm, body, len, (int)sizeof body - 4096);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  __try {
    len = ViewerProjectiles(body, len, (int)sizeof body);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  SnapshotPost(match, body, len);
}

// Battle viewer terrain: the map's collision as a heightfield, sampled once
// per match with downward KismetSystemLibrary::LineTraceSingleForObjects
// (WorldStatic) called through ProcessEvent on its CDO; parameter and
// FHitResult member offsets come from reflection, not a hardcoded 4.13
// layout. A coarse pass over +-1e6 uu finds where geometry is; the fine pass
// covers those bounds for TERRAIN_TICK_MS per CommandsTick, each column traced
// both down and up (TraceColumn) so solids come out as intervals, not a
// heightfield. Hits on volumes and pawns are stepped through (start again just
// past the hit) so blocking volumes and ships do not show as terrain. Result
// goes to POST /battle/terrain?match=: "grid2 nx ny x0 y0 dx dy" then one row
// per line, each cell "n" or its surfaces (see the fine phase of TerrainTick).
// Off with DN_HOST_NO_VIEWER.
//
// Playable area: the map's AYLevelBoundaries{Warp,Warning,Height}Volume
// actors (DreadGame/Private/YLevelBoundariesWarpVolume.cpp in the exe's
// strings) bound where ships may fly; the coarse pass alone also finds the
// low-detail backdrop, ~+-875000 uu on every map tried. When boundary volumes
// exist the fine pass covers their union instead, and each one goes to the
// viewer as a "box kind x0 y0 z0 x1 y1 z1" line after the grid rows (from
// AActor::GetActorBounds, origin +- extent). The fine pass also tallies which
// actors the surface hits land on (terrain: hit actors ...), to tell whether a
// map is one landscape or many static meshes.
enum { TERRAIN_COARSE = 33, TERRAIN_FINE = 256, TERRAIN_STEPS = 32, TERRAIN_MAX_BOXES = 32 };
// Fine-pass time per CommandsTick call. It runs on the game thread: the old
// fixed 800 traces (~15-30 ms) hitched the whole match each tick until the
// pass ended, one source of ships snapping in the viewer.
static const double TERRAIN_TICK_MS = 3.0;
// The fine phase also runs per frame (TerrainFrameTick, from the ProcessEvent
// hook): CommandsTick rides the game-mode timer, which fires once a second and
// acts every other call, so 3 ms per 2 s took Skybridge's 285 ms of tracing
// three minutes and never finished Highlands or Space02 inside a 7-minute run
// (terrain-sweep2-1017-1303). TERRAIN_FRAME_MS every TERRAIN_FRAME_GAP_MS of
// wall clock keeps any one frame's hitch small.
static const double TERRAIN_FRAME_MS = 2.0, TERRAIN_FRAME_GAP_MS = 16.0;
static const float TERRAIN_R = 1.0e6f, TERRAIN_TOP = 5.0e5f, TERRAIN_BOTTOM = -5.0e5f;
static const float TERRAIN_MISS = 3.0e38f;

struct TraceFn {
  void *cdo, *fn;
  int world, start, end, objTypes, complex, ignore, debug, hit, self, ret;
  int hitPoint, hitActor; // inside FHitResult
  bool ok, tried;
};
static TraceFn g_trace;

static bool TraceResolve() {
  if (g_trace.tried)
    return g_trace.ok;
  g_trace.tried = true;
  TraceFn &t = g_trace;
  t.world = t.start = t.end = t.objTypes = t.complex = t.ignore = t.debug = t.hit = t.self = t.ret = -1;
  t.hitPoint = t.hitActor = -1;
  t.cdo = FindObjectNamedOfClass("Default__KismetSystemLibrary", "KismetSystemLibrary");
  if (!t.cdo) {
    Logf("terrain: KismetSystemLibrary CDO not found");
    return false;
  }
  FNameMin n = {};
  ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&n, L"LineTraceSingleForObjects", 1);
  t.fn = FindFunctionSafe(t.cdo, *(uint64_t *)&n);
  if (!t.fn || !IsReadable((uint8_t *)t.fn + 0x40, 8)) {
    Logf("terrain: LineTraceSingleForObjects not found");
    return false;
  }
  UObjectMin *pp = *(UObjectMin **)((uint8_t *)t.fn + 0x38);
  for (int j = 0; pp && j < 24 && IsReadable(pp, 0x90); ++j, pp = *(UObjectMin **)((uint8_t *)pp + 0x28)) {
    int off = (int)*(uint32_t *)((uint8_t *)pp + 0x50);
    const char *pn = NameText(pp->Name);
    if (!pn)
      continue;
    int *slot = !strcmp(pn, "WorldContextObject") ? &t.world : !strcmp(pn, "Start") ? &t.start
              : !strcmp(pn, "End") ? &t.end : !strcmp(pn, "ObjectTypes") ? &t.objTypes
              : !strcmp(pn, "bTraceComplex") ? &t.complex : !strcmp(pn, "ActorsToIgnore") ? &t.ignore
              : !strcmp(pn, "DrawDebugType") ? &t.debug : !strcmp(pn, "OutHit") ? &t.hit
              : !strcmp(pn, "bIgnoreSelf") ? &t.self : !strcmp(pn, "ReturnValue") ? &t.ret : nullptr;
    if (slot)
      *slot = off;
    if (strcmp(pn, "OutHit"))
      continue;
    // The UScriptStruct sits among the UStructProperty's trailing qwords
    // (DescribeProps).
    for (int k = 0x58; k <= 0x88; k += 8) {
      UObjectMin *st = *(UObjectMin **)((uint8_t *)pp + k);
      if (!IsReadable(st, 0x40) || !IsReadable(st->Class, sizeof(UObjectMin)) || strcmp(ClassNameOf(st), "ScriptStruct"))
        continue;
      for (UObjectMin *m = *(UObjectMin **)((uint8_t *)st + 0x38); m && IsReadable(m, 0x58); m = *(UObjectMin **)((uint8_t *)m + 0x28)) {
        const char *mn = NameText(m->Name);
        if (mn && !strcmp(mn, "ImpactPoint"))
          t.hitPoint = (int)*(uint32_t *)((uint8_t *)m + 0x50);
        else if (mn && !strcmp(mn, "Actor"))
          t.hitActor = (int)*(uint32_t *)((uint8_t *)m + 0x50);
      }
      break;
    }
  }
  t.ok = t.world >= 0 && t.start >= 0 && t.end >= 0 && t.objTypes >= 0 && t.hit >= 0 && t.ret >= 0 &&
         t.hitPoint >= 0 && t.ret < 1000 && t.hit + t.hitPoint < 1000;
  Logf("terrain: LineTraceSingleForObjects %s -- world %d start %d end %d objTypes %d complex %d ignore %d "
       "debug %d hit %d (ImpactPoint +%d, Actor +%d) self %d ret %d",
       t.ok ? "resolved" : "NOT usable", t.world, t.start, t.end, t.objTypes, t.complex, t.ignore, t.debug, t.hit,
       t.hitPoint, t.hitActor, t.self, t.ret);
  return t.ok;
}

// Fine-pass tally of the actors surface hits land on: open-addressed by
// actor pointer, logged as the top entries when the pass ends.
struct HitSlot {
  void *actor;
  int hits;
  char kind; // KindOf cache, 0x80 = filled
};
static HitSlot *g_hitTally;
enum { HIT_SLOTS = 4096 };
static void HitTallyLog() {
  int distinct = 0;
  for (int i = 0; i < HIT_SLOTS; ++i)
    distinct += g_hitTally[i].actor != nullptr;
  Logf("terrain: hit actors: %d distinct", distinct);
  for (int top = 0; top < 12; ++top) {
    int best = -1;
    for (int i = 0; i < HIT_SLOTS; ++i)
      if (g_hitTally[i].hits > 0 && (best < 0 || g_hitTally[i].hits > g_hitTally[best].hits))
        best = i;
    if (best < 0)
      break;
    UObjectMin *a = (UObjectMin *)g_hitTally[best].actor;
    const char *n = IsReadable(a, sizeof(UObjectMin)) ? NameText(a->Name) : nullptr;
    Logf("terrain:   %5d hits  %c %s (%s)", g_hitTally[best].hits, g_hitTally[best].kind & 0x7F, n ? n : "?", n ? ClassNameOf(a) : "?");
    g_hitTally[best].hits = -g_hitTally[best].hits;
  }
}

// One WorldStatic trace s -> e: impact z and hit actor of the first blocking hit.
static bool TraceOnce(void *world, const float s[3], const float e[3], float *hz, void **actor) {
  static uint8_t objTypes[1] = {0}; // EObjectTypeQuery::ObjectTypeQuery1 = WorldStatic
  struct { void *data; int32_t num, max; } arr = {objTypes, 1, 1};
  alignas(16) uint8_t parms[1024];
  memset(parms, 0, sizeof parms);
  *(void **)(parms + g_trace.world) = world;
  memcpy(parms + g_trace.start, s, 12);
  memcpy(parms + g_trace.end, e, 12);
  memcpy(parms + g_trace.objTypes, &arr, sizeof arr);
  __try {
    void **vt = *(void ***)g_trace.cdo;
    ((tProcessEventVirt)vt[VT_PROCESS_EVENT / 8])(g_trace.cdo, g_trace.fn, parms);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  if (!parms[g_trace.ret])
    return false;
  *hz = ((float *)(parms + g_trace.hit + g_trace.hitPoint))[2];
  *actor = g_trace.hitActor >= 0 ? ((tWeakGet)(g_base + RVA_WEAK_PTR_GET))(parms + g_trace.hit + g_trace.hitActor) : nullptr;
  return true;
}

static bool StepThrough(void *a) {
  return a && IsReadable(a, sizeof(UObjectMin)) &&
         (ClassChainContains((UObjectMin *)a, "Volume") || ClassChainContains((UObjectMin *)a, "Pawn"));
}

// Highest WorldStatic surface below (x, y), or TERRAIN_MISS (coarse pass).
// Up to TERRAIN_STEPS volume/pawn hits are stepped through: a map stacks 3-4
// boundary volumes plus trigger volumes, and the old limit of 4 ran out over
// whole rectangles of the map, the gaps in the viewer (2026-10-17).
static float TraceDown(void *world, float x, float y) {
  float z = TERRAIN_TOP;
  for (int step = 0; step < TERRAIN_STEPS && z > TERRAIN_BOTTOM; ++step) {
    float s[3] = {x, y, z}, e[3] = {x, y, TERRAIN_BOTTOM}, hz;
    void *a = nullptr;
    if (!TraceOnce(world, s, e, &hz, &a))
      return TERRAIN_MISS;
    if (StepThrough(a)) {
      z = hz - 50.f;
      continue;
    }
    return hz;
  }
  return TERRAIN_MISS;
}

static bool HasCI(const char *s, const char *const *needles) {
  if (!s)
    return false;
  for (; *needles; ++needles)
    for (const char *p = s; *p; ++p)
      if (!_strnicmp(p, *needles, strlen(*needles)))
        return true;
  return false;
}

// Viewer surface kind of a hit actor: 0 = step through (pawns, volumes that
// are not water or a boundary), 'w' water, 't' terrain, 'o' object, 'r'/'x'
// warning/warp boundary volume (recorded, then stepped through). The split is by name and
// class only, for colouring -- a viewer heuristic, not game data. Water: the
// sweep's hit tallies show it as ordinary static meshes ("oceanplane" on
// Amirani, "Arena_Lake" on Highlands); a water PhysicsVolume would otherwise
// be stepped through, so volumes named for water count too. Terrain: Landscape
// plus the big backdrop/cliff static meshes the tallies show carrying the
// ground (GorgeCanyonBG*, Cliff*, HighlandPile*).
static char SurfaceKind(void *a) {
  static const char *const water[] = {"water", "ocean", "lake", "river", nullptr};
  static const char *const ground[] = {"landscape", "terrain", "cliff", "canyon", "rock", "mountain", "pile", "ground", "BG", nullptr};
  if (!a || !IsReadable(a, sizeof(UObjectMin)))
    return 'o';
  UObjectMin *o = (UObjectMin *)a;
  const char *n = NameText(o->Name), *cn = ClassNameOf(o);
  bool wet = HasCI(n, water) || HasCI(cn, water);
  if (ClassChainContains(o, "Pawn"))
    return 0;
  if (ClassChainContains(o, "Volume")) {
    // The playable limit as the game draws it: the brush of the warning
    // ("return to the battlefield") and warp volumes, convex shapes rather
    // than their bounding boxes. A downward ray hits a brush only inside its
    // footprint, so the columns carrying r/x trace that shape.
    if (cn && strstr(cn, "LevelBoundariesWarning"))
      return 'r';
    if (cn && strstr(cn, "LevelBoundariesWarp"))
      return 'x';
    return wet ? 'w' : 0;
  }
  if (wet)
    return 'w';
  if (ClassChainContains(o, "Landscape") || HasCI(n, ground))
    return 't';
  return 'o';
}

// Kind per actor, cached in the fine-pass tally (which also counts top hits).
static char KindOf(void *a, bool count) {
  if (!a || !g_hitTally)
    return SurfaceKind(a);
  for (uint32_t h = (uint32_t)(((uintptr_t)a >> 4) * 2654435761u) % HIT_SLOTS, k = 0; k < HIT_SLOTS; ++k, h = (h + 1) % HIT_SLOTS) {
    HitSlot &sl = g_hitTally[h];
    if (sl.actor == a || !sl.actor) {
      if (!sl.actor)
        sl.actor = a, sl.kind = SurfaceKind(a) | 0x80;
      if (count && sl.kind & 0x7F)
        ++sl.hits;
      return (char)(sl.kind & 0x7F);
    }
  }
  return SurfaceKind(a);
}

// A column of the fine pass: every surface a downward ray meets (tops of
// solids, entering each from above) and every one an upward ray meets
// (undersides). Starting a ray just inside a solid passes through it, so the
// two lists pair into solid intervals in the viewer: overhangs, asteroids and
// stacked platforms instead of a single heightfield needle.
enum { TERRAIN_LAYERS = 8 };
struct TerrainColumn {
  float top[TERRAIN_LAYERS], bot[TERRAIN_LAYERS];
  char kind[TERRAIN_LAYERS];
  uint8_t ntop, nbot;
};

static void TraceColumn(void *world, float x, float y, TerrainColumn *c) {
  c->ntop = c->nbot = 0;
  bool wet = false, warn = false, warp = false;
  float z = TERRAIN_TOP;
  for (int step = 0; step < TERRAIN_STEPS && z > TERRAIN_BOTTOM && c->ntop < TERRAIN_LAYERS; ++step) {
    float s[3] = {x, y, z}, e[3] = {x, y, TERRAIN_BOTTOM}, hz;
    void *a = nullptr;
    if (!TraceOnce(world, s, e, &hz, &a))
      break;
    char k = KindOf(a, true);
    bool &once = k == 'w' ? wet : k == 'r' ? warn : warp;
    if (k && !((k == 'w' || k == 'r' || k == 'x') && once)) { // first water/boundary surface only
      if (k == 'w' || k == 'r' || k == 'x')
        once = true;
      c->top[c->ntop] = hz, c->kind[c->ntop++] = k;
    }
    z = hz - 50.f;
  }
  z = TERRAIN_BOTTOM;
  for (int step = 0; step < TERRAIN_STEPS && z < TERRAIN_TOP && c->nbot < TERRAIN_LAYERS; ++step) {
    float s[3] = {x, y, z}, e[3] = {x, y, TERRAIN_TOP}, hz;
    void *a = nullptr;
    if (!TraceOnce(world, s, e, &hz, &a))
      break;
    char k = KindOf(a, false);
    if (k && k != 'w' && k != 'r' && k != 'x')
      c->bot[c->nbot++] = hz;
    z = hz + 50.f;
  }
}

struct BoundBox {
  char kind[16];
  float lo[3], hi[3];
};
static BoundBox g_boxes[TERRAIN_MAX_BOXES];
static int g_nboxes;

static void FillBounds(uint8_t *parms, const char *, const char *pn, uint32_t off, void *ctx) {
  uint32_t *o = (uint32_t *)ctx;
  if (!strcmp(pn, "bOnlyCollidingComponents"))
    parms[off] = 0;
  else if (!strcmp(pn, "Origin"))
    o[0] = off;
  else if (!strcmp(pn, "BoxExtent"))
    o[1] = off;
}

// Boundary volumes in the loaded world (see the TERRAIN_ comment); also logs
// which class owns GetBoundaryMin/Max, unexplored so far.
static void BoundsProbe() {
  g_nboxes = 0;
  FUObjectArrayMin *arr = GObjects();
  for (int i = 0; arr && arr->Objects && i < arr->NumElements; ++i) {
    UObjectMin *o = arr->Objects[i].Object;
    const char *n = o && IsReadable(o, sizeof(UObjectMin)) ? NameText(o->Name) : nullptr;
    if (!n)
      continue;
    const char *cn = ClassNameOf(o);
    if (cn && !strcmp(cn, "Function") && (!strcmp(n, "GetBoundaryMin") || !strcmp(n, "GetBoundaryMax"))) {
      const char *on = o->Outer && IsReadable(o->Outer, sizeof(UObjectMin)) ? NameText(o->Outer->Name) : nullptr;
      Logf("terrain: UFunction %s on %s", n, on ? on : "?");
      continue;
    }
    if (!cn || !strncmp(n, "Default__", 9) || !strstr(cn, "LevelBoundaries"))
      continue;
    alignas(16) uint8_t parms[256];
    uint32_t off[2] = {0xFFFFFFFF, 0xFFFFFFFF};
    bool ok = false;
    __try {
      ok = CallNamed(o, L"GetActorBounds", FillBounds, off, parms) && off[0] != 0xFFFFFFFF && off[1] != 0xFFFFFFFF;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ok = false;
    }
    if (!ok) {
      Logf("terrain: boundary %s (%s): no bounds", n, cn);
      continue;
    }
    float *c = (float *)(parms + off[0]), *e = (float *)(parms + off[1]);
    Logf("terrain: boundary %s (%s) centre %.0f %.0f %.0f extent %.0f %.0f %.0f", n, cn, c[0], c[1], c[2], e[0], e[1], e[2]);
    if (g_nboxes >= TERRAIN_MAX_BOXES)
      continue;
    BoundBox &b = g_boxes[g_nboxes++];
    const char *k = strstr(cn, "Warp") ? "warp" : strstr(cn, "Warning") ? "warning" : strstr(cn, "Height") ? "height" : "other";
    strcpy_s(b.kind, sizeof b.kind, k);
    for (int a = 0; a < 3; ++a)
      b.lo[a] = c[a] - e[a], b.hi[a] = c[a] + e[a];
  }
  Logf("terrain: %d boundary volume(s)", g_nboxes);
}

// Centre of the largest "other" LevelBoundaries volume BoundsProbe read (the
// play space; warp/warning/height volumes only if nothing else), for the
// /meteor heralds. False when the probe found none.
static bool MapCentre(float out[3]) {
  int best = -1;
  float area = -1;
  for (int pass = 0; pass < 2 && best < 0; ++pass)
    for (int b = 0; b < g_nboxes; ++b) {
      if (pass == 0 && strcmp(g_boxes[b].kind, "other"))
        continue;
      float a = (g_boxes[b].hi[0] - g_boxes[b].lo[0]) * (g_boxes[b].hi[1] - g_boxes[b].lo[1]);
      if (a > area)
        area = a, best = b;
    }
  if (best < 0)
    return false;
  for (int a = 0; a < 3; ++a)
    out[a] = 0.5f * (g_boxes[best].lo[a] + g_boxes[best].hi[a]);
  return true;
}

// Object boxes for the viewer: every distinct actor the fine pass classified
// 'o' (structures, rocks, props), as its colliding-components bounds, one
// "obj <hits> <lo xyz> <hi xyz>" line each. The sampled 'o' tops are one per
// ~1 km cell with no identity, so the viewer could only smear them; a box per
// actor gives exact extents and hard edges. Axis-aligned, so a rotated long
// rock reads larger than it is.
// Each box is followed by its shape: "objh nx ny z,z,..." -- a top-down
// heightfield of THAT actor only over its xy bounds (rays that meet another
// actor first step through it), "n" where the ray misses it. Cells ~1/14 of
// the larger extent, at least OBJ_SHAPE_MIN_UU (GUESS: enough to read a
// structure's outline, ~100k traces a match, once, during orbit).
enum { OBJ_BOX_MAX = 1024, OBJ_SHAPE_MAX = 14 };
static const float OBJ_SHAPE_MIN_UU = 150.f;
static void FillColliding(uint8_t *parms, const char *, const char *pn, uint32_t off, void *ctx) {
  uint32_t *o = (uint32_t *)ctx;
  if (!strcmp(pn, "bOnlyCollidingComponents"))
    parms[off] = 1;
  else if (!strcmp(pn, "Origin"))
    o[0] = off;
  else if (!strcmp(pn, "BoxExtent"))
    o[1] = off;
}
static int ObjectBoxes(void *world, char *body, int cap, int *len) {
  int n = 0, failed = 0;
  double t0 = NowMs();
  long traces = 0;
  for (int i = 0; i < HIT_SLOTS && n < OBJ_BOX_MAX && *len < cap - 128; ++i) {
    HitSlot &sl = g_hitTally[i];
    if (!sl.actor || (sl.kind & 0x7F) != 'o' || !ObjectLive(sl.actor))
      continue;
    alignas(16) uint8_t parms[256];
    uint32_t off[2] = {0xFFFFFFFF, 0xFFFFFFFF};
    bool ok = false;
    __try {
      ok = CallNamed((UObjectMin *)sl.actor, L"GetActorBounds", FillColliding, off, parms) && off[0] != 0xFFFFFFFF &&
           off[1] != 0xFFFFFFFF;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ok = false;
    }
    if (!ok) {
      ++failed;
      continue;
    }
    float *c = (float *)(parms + off[0]), *e = (float *)(parms + off[1]);
    if (!(e[0] > 0 && e[1] > 0 && e[2] >= 0) || e[0] > 1e6f || e[1] > 1e6f)
      continue;
    int w = _snprintf_s(body + *len, cap - *len, _TRUNCATE, "obj %d %.0f %.0f %.0f %.0f %.0f %.0f\n", abs(sl.hits),
                        c[0] - e[0], c[1] - e[1], c[2] - e[2], c[0] + e[0], c[1] + e[1], c[2] + e[2]);
    if (w <= 0)
      continue;
    *len += w, ++n;
    float lo[3] = {c[0] - e[0], c[1] - e[1], c[2] - e[2]}, hi[3] = {c[0] + e[0], c[1] + e[1], c[2] + e[2]};
    float big = e[0] > e[1] ? e[0] : e[1], cell = 2 * big / OBJ_SHAPE_MAX;
    if (cell < OBJ_SHAPE_MIN_UU)
      cell = OBJ_SHAPE_MIN_UU;
    int nx = (int)ceilf(2 * e[0] / cell), ny = (int)ceilf(2 * e[1] / cell);
    nx = nx < 1 ? 1 : nx > OBJ_SHAPE_MAX ? OBJ_SHAPE_MAX : nx;
    ny = ny < 1 ? 1 : ny > OBJ_SHAPE_MAX ? OBJ_SHAPE_MAX : ny;
    if (*len > cap - (16 + nx * ny * 7))
      continue;
    *len += _snprintf_s(body + *len, cap - *len, _TRUNCATE, "objh %d %d ", nx, ny);
    for (int j = 0; j < ny; ++j)
      for (int i = 0; i < nx; ++i) {
        // cell centres
        float x = lo[0] + (i + 0.5f) * 2 * e[0] / nx, y = lo[1] + (j + 0.5f) * 2 * e[1] / ny, z = hi[2] + 100.f;
        float top = TERRAIN_MISS;
        for (int step = 0; step < 4 && z > lo[2] - 100.f; ++step) {
          float st[3] = {x, y, z}, en[3] = {x, y, lo[2] - 100.f}, hz;
          void *a = nullptr;
          ++traces;
          if (!TraceOnce(world, st, en, &hz, &a))
            break;
          if (a == sl.actor) {
            top = hz;
            break;
          }
          z = hz - 50.f;
        }
        int ww = top == TERRAIN_MISS ? _snprintf_s(body + *len, cap - *len, _TRUNCATE, "%sn", i || j ? "," : "")
                                     : _snprintf_s(body + *len, cap - *len, _TRUNCATE, "%s%.0f", i || j ? "," : "", top);
        if (ww > 0)
          *len += ww;
      }
    body[(*len)++] = '\n';
    // "objb": the same cells traced bottom-up, the actor's underside, so the
    // viewer can leave room under overhangs and lying hulls instead of
    // walling every column down to the box bottom.
    if (*len > cap - (16 + nx * ny * 7))
      continue;
    *len += _snprintf_s(body + *len, cap - *len, _TRUNCATE, "objb %d %d ", nx, ny);
    for (int j = 0; j < ny; ++j)
      for (int i = 0; i < nx; ++i) {
        float x = lo[0] + (i + 0.5f) * 2 * e[0] / nx, y = lo[1] + (j + 0.5f) * 2 * e[1] / ny, z = lo[2] - 100.f;
        float bot = TERRAIN_MISS;
        for (int step = 0; step < 4 && z < hi[2] + 100.f; ++step) {
          float st[3] = {x, y, z}, en[3] = {x, y, hi[2] + 100.f}, hz;
          void *a = nullptr;
          ++traces;
          if (!TraceOnce(world, st, en, &hz, &a))
            break;
          if (a == sl.actor) {
            bot = hz;
            break;
          }
          z = hz + 50.f;
        }
        int ww = bot == TERRAIN_MISS ? _snprintf_s(body + *len, cap - *len, _TRUNCATE, "%sn", i || j ? "," : "")
                                     : _snprintf_s(body + *len, cap - *len, _TRUNCATE, "%s%.0f", i || j ? "," : "", bot);
        if (ww > 0)
          *len += ww;
      }
    body[(*len)++] = '\n';
  }
  Logf("terrain: %d object boxes with shapes, %ld traces, %.0f ms%s", n, traces, NowMs() - t0,
       failed ? " (some actors gave no bounds)" : "");
  return n;
}

// The coarse pass hit over half of its +-1e6 cells: backdrop meshes reach the
// horizon (Glacier: 1056 of 1089, Arena_FarOceanplane and icebergs), so ground
// on the fine grid's edge says nothing about the arena and the widen step must
// not run -- it took Glacier from +-165k to +-657k, 5 km cells, the floes
// reduced to a few blocks (2026-10-17). GUESS: the half threshold; HVC_Map2,
// the map the widen exists for, hit 0.
static bool g_coarseEverywhere = false;

static void TerrainTick(uint8_t *gm, const char *match, bool frame = false) {
  static int s_phase = 0; // 0 start, 1 fine, 2 done
  static float s_x0, s_y0, s_dx, s_dy;
  static TerrainColumn *s_col = nullptr;
  static int s_next = 0;
  static double s_ms = 0;
  // One retrace of the final grid RESWEEP_MS after the first upload: level
  // streaming can still be bringing parts of the map in while the first pass
  // runs (HVC_Map2 2026-10-17: the map ended in a dead-straight line through
  // the city on the rows traced first -- GUESS: those were traced before the
  // sublevel carrying them was in; the coarse pass also hit nothing). The
  // second upload replaces the first; the viewer refetches on terrain_at.
  static DWORD s_resweepAt = 0;
  static bool s_retracing = false;
  enum { RESWEEP_MS = 30000 };
  if (s_phase == 2) {
    if (!s_resweepAt || frame || (int)(GetTickCount() - s_resweepAt) < 0)
      return;
    s_resweepAt = 0;
    s_retracing = true;
    g_hitTally = (HitSlot *)calloc(HIT_SLOTS, sizeof(HitSlot));
    s_col = (TerrainColumn *)malloc(sizeof(TerrainColumn) * TERRAIN_FINE * TERRAIN_FINE);
    if (!s_col)
      return;
    s_next = 0;
    s_ms = 0;
    s_phase = 1;
    g_terrainFine = true;
    g_frameWanted = true;
    Logf("terrain: retracing the final grid (streamed-in sublevels)");
    return;
  }
  // Frame ticks only advance the fine trace; start and upload stay on the timer.
  if (frame && (s_phase != 1 || s_next >= TERRAIN_FINE * TERRAIN_FINE))
    return;
  static int s_on = -1;
  if (s_on < 0)
    s_on = SwitchOn("DN_HOST_NO_VIEWER", "dn_host_no_viewer.txt") ? 0 : 1;
  if (!s_on || !TraceResolve()) {
    s_phase = 2;
    return;
  }
  DWORD t0 = GetTickCount();
  if (s_phase == 0) {
    float step = 2 * TERRAIN_R / (TERRAIN_COARSE - 1), lo[2] = {TERRAIN_MISS, TERRAIN_MISS}, hi[2] = {-TERRAIN_MISS, -TERRAIN_MISS};
    int hits = 0;
    for (int j = 0; j < TERRAIN_COARSE; ++j)
      for (int i = 0; i < TERRAIN_COARSE; ++i) {
        float x = -TERRAIN_R + i * step, y = -TERRAIN_R + j * step;
        if (TraceDown(gm, x, y) == TERRAIN_MISS)
          continue;
        ++hits;
        lo[0] = x < lo[0] ? x : lo[0], hi[0] = x > hi[0] ? x : hi[0];
        lo[1] = y < lo[1] ? y : lo[1], hi[1] = y > hi[1] ? y : hi[1];
      }
    g_coarseEverywhere = hits > TERRAIN_COARSE * TERRAIN_COARSE / 2;
    if (hits)
      Logf("terrain: coarse %dx%d -> %d hits, x %.0f..%.0f y %.0f..%.0f (%lu ms)", TERRAIN_COARSE, TERRAIN_COARSE, hits,
           lo[0], hi[0], lo[1], hi[1], GetTickCount() - t0);
    else
      // lo/hi are still the TERRAIN_MISS sentinel here (+3.0e38 / -3.0e38), so
      // printing them said "the coarse pass covered 3000000005497755757778
      // 03994281145270272..-3000... in x" — an empty grid read as an
      // astronomical one (terrain sweep summary 2026-10-17 10:57:59). The
      // fine pass below is driven by the boundary boxes in that case, and by
      // nothing at all when there are none (the early return), so there is no
      // extent to report.
      Logf("terrain: coarse %dx%d -> 0 hits, x/y n/a (%lu ms)", TERRAIN_COARSE, TERRAIN_COARSE, GetTickCount() - t0);
    BoundsProbe();
    if (!hits && !g_nboxes) {
      s_phase = 2;
      return;
    }
    s_x0 = lo[0] - step, s_y0 = lo[1] - step;
    s_dx = (hi[0] - lo[0] + 2 * step) / (TERRAIN_FINE - 1);
    s_dy = (hi[1] - lo[1] + 2 * step) / (TERRAIN_FINE - 1);
    if (g_nboxes) {
      float blo[2] = {TERRAIN_MISS, TERRAIN_MISS}, bhi[2] = {-TERRAIN_MISS, -TERRAIN_MISS};
      for (int b = 0; b < g_nboxes; ++b)
        for (int a = 0; a < 2; ++a)
          blo[a] = g_boxes[b].lo[a] < blo[a] ? g_boxes[b].lo[a] : blo[a],
          bhi[a] = g_boxes[b].hi[a] > bhi[a] ? g_boxes[b].hi[a] : bhi[a];
      s_x0 = blo[0], s_y0 = blo[1];
      s_dx = (bhi[0] - blo[0]) / (TERRAIN_FINE - 1);
      s_dy = (bhi[1] - blo[1]) / (TERRAIN_FINE - 1);
      Logf("terrain: fine pass over boundary union x %.0f..%.0f y %.0f..%.0f", blo[0], bhi[0], blo[1], bhi[1]);
    }
    g_hitTally = (HitSlot *)calloc(HIT_SLOTS, sizeof(HitSlot));
    s_col = (TerrainColumn *)malloc(sizeof(TerrainColumn) * TERRAIN_FINE * TERRAIN_FINE);
    s_next = 0;
    s_ms = 0;
    s_phase = s_col ? 1 : 2;
    g_terrainFine = s_phase == 1;
    g_frameWanted = g_frameWanted || g_terrainFine;
    return;
  }
  double t1 = NowMs(), now = t1;
  for (; s_next < TERRAIN_FINE * TERRAIN_FINE && now - t1 < (frame ? TERRAIN_FRAME_MS : TERRAIN_TICK_MS); ++s_next) {
    TraceColumn(gm, s_x0 + (s_next % TERRAIN_FINE) * s_dx, s_y0 + (s_next / TERRAIN_FINE) * s_dy, &s_col[s_next]);
    if (s_next % 8 == 7)
      now = NowMs();
  }
  s_ms += NowMs() - t1;
  if (s_next < TERRAIN_FINE * TERRAIN_FINE)
    return;
  // Refine once: the first pass spans the boundary union, which on Highlands
  // is the warp volume -- the ground was a 94x91 patch of the 256x256 grid
  // (5,660 of 65,536 columns hit, ~1.3 km cells). If the hit columns fill
  // less than ~60% of the grid either way (GUESS), trace it again over their
  // bounding box plus a cell, for ~2.5x finer cells where the map is.
  // Before that, widen once: the boundary union can be smaller than the ground
  // the match is fought over (HVC_Map2: warp box +-65536, terrain hit on three
  // of the grid's four edges, so the viewer showed a map sliced off along
  // straight lines -- operator report 2026-10-17). Every side whose edge row
  // or column carries ground grows by half the span, and the pass reruns. The
  // coarse pass cannot say how far to go: it hits everywhere on most maps
  // (backdrop meshes out to +-1e6) and nothing on HVC_Map2.
  // The retrace (below) may widen once more: a side that was still empty on
  // the first pass (not streamed in yet) can carry ground by then.
  static bool s_checkedFirst = false, s_checkedRetrace = false, s_refined = false;
  bool &checked = s_retracing ? s_checkedRetrace : s_checkedFirst;
  if (!checked && !g_coarseEverywhere) {
    checked = true;
    bool e[4] = {false, false, false, false}; // -x +x -y +y
    for (int k = 0; k < TERRAIN_FINE; ++k) {
      e[0] = e[0] || s_col[k * TERRAIN_FINE].ntop;
      e[1] = e[1] || s_col[k * TERRAIN_FINE + TERRAIN_FINE - 1].ntop;
      e[2] = e[2] || s_col[k].ntop;
      e[3] = e[3] || s_col[(TERRAIN_FINE - 1) * TERRAIN_FINE + k].ntop;
    }
    if (e[0] || e[1] || e[2] || e[3]) {
      // GUESS: half the span per side is enough for the maps seen; more
      // widenings would cost resolution everywhere for a sliver of backdrop.
      float w = s_dx * (TERRAIN_FINE - 1), h = s_dy * (TERRAIN_FINE - 1);
      float gx = w / 2, gy = h / 2;
      s_x0 -= e[0] ? gx : 0, s_y0 -= e[2] ? gy : 0;
      s_dx = (w + (e[0] + e[1]) * gx) / (TERRAIN_FINE - 1);
      s_dy = (h + (e[2] + e[3]) * gy) / (TERRAIN_FINE - 1);
      s_refined = true; // the widened grid is what the ground needs; no shrink after
      Logf("terrain: ground on grid edges%s%s%s%s -> widened to x0 %.0f y0 %.0f dx %.0f dy %.0f",
           e[0] ? " -x" : "", e[1] ? " +x" : "", e[2] ? " -y" : "", e[3] ? " +y" : "", s_x0, s_y0, s_dx, s_dy);
      s_next = 0;
      return;
    }
  }
  if (!s_refined) {
    s_refined = true;
    int i0 = TERRAIN_FINE, i1 = -1, j0 = TERRAIN_FINE, j1 = -1;
    for (int k = 0; k < TERRAIN_FINE * TERRAIN_FINE; ++k)
      if (s_col[k].ntop) {
        int i = k % TERRAIN_FINE, j = k / TERRAIN_FINE;
        i0 = i < i0 ? i : i0, i1 = i > i1 ? i : i1, j0 = j < j0 ? j : j0, j1 = j > j1 ? j : j1;
      }
    if (i1 >= 0 && (i1 - i0 < TERRAIN_FINE * 6 / 10 || j1 - j0 < TERRAIN_FINE * 6 / 10)) {
      i0 = i0 > 0 ? i0 - 1 : 0, j0 = j0 > 0 ? j0 - 1 : 0;
      i1 = i1 < TERRAIN_FINE - 1 ? i1 + 1 : i1, j1 = j1 < TERRAIN_FINE - 1 ? j1 + 1 : j1;
      float x0 = s_x0 + i0 * s_dx, y0 = s_y0 + j0 * s_dy;
      s_dx = (i1 - i0) * s_dx / (TERRAIN_FINE - 1), s_dy = (j1 - j0) * s_dy / (TERRAIN_FINE - 1);
      s_x0 = x0, s_y0 = y0;
      Logf("terrain: refining over hit cells %d..%d x %d..%d -> x0 %.0f y0 %.0f dx %.0f dy %.0f (first pass %.0f ms)",
           i0, i1, j0, j1, s_x0, s_y0, s_dx, s_dy, s_ms);
      s_next = 0;
      return;
    }
  }
  g_terrainFine = false;
  if (frame)
    return; // the next timer tick uploads
  s_phase = 2;
  static bool s_resweptOnce = false;
  if (!s_resweptOnce)
    s_resweptOnce = true, s_resweepAt = GetTickCount() + RESWEEP_MS;
  // "grid2 nx ny x0 y0 dx dy", then one line per row: per cell "n" (nothing)
  // or comma-separated surfaces, kind letter + z: t/o/w tops, b undersides.
  int cap = TERRAIN_FINE * TERRAIN_FINE * (2 * TERRAIN_LAYERS * 9 + 2) + TERRAIN_MAX_BOXES * 160 + OBJ_BOX_MAX * (96 + OBJ_SHAPE_MAX * OBJ_SHAPE_MAX * 7) + 128;
  char *body = (char *)malloc(cap);
  if (!body) {
    free(s_col);
    s_col = nullptr;
    return;
  }
  int len = _snprintf_s(body, cap, _TRUNCATE, "grid2 %d %d %.0f %.0f %.1f %.1f\n", TERRAIN_FINE, TERRAIN_FINE,
                        s_x0, s_y0, s_dx, s_dy);
  int hits = 0, surfaces = 0;
  for (int k = 0; k < TERRAIN_FINE * TERRAIN_FINE && len > 0 && len < cap - 256; ++k) {
    const TerrainColumn &c = s_col[k];
    hits += c.ntop > 0;
    surfaces += c.ntop + c.nbot;
    if (!c.ntop && !c.nbot)
      body[len++] = 'n';
    for (int i = 0; i < c.ntop + c.nbot; ++i) {
      int w = i < c.ntop ? _snprintf_s(body + len, cap - len, _TRUNCATE, "%s%c%.0f", i ? "," : "", c.kind[i], c.top[i])
                         : _snprintf_s(body + len, cap - len, _TRUNCATE, "%sb%.0f", i ? "," : "", c.bot[i - c.ntop]);
      if (w > 0)
        len += w;
    }
    body[len++] = (k + 1) % TERRAIN_FINE ? ' ' : '\n';
  }
  for (int b = 0; b < g_nboxes && len > 0 && len < cap - 160; ++b) {
    const BoundBox &x = g_boxes[b];
    int w = _snprintf_s(body + len, cap - len, _TRUNCATE, "box %s %.0f %.0f %.0f %.0f %.0f %.0f\n", x.kind,
                        x.lo[0], x.lo[1], x.lo[2], x.hi[0], x.hi[1], x.hi[2]);
    if (w > 0)
      len += w;
  }
  int objs = g_hitTally ? ObjectBoxes(gm, body, cap, &len) : 0;
  free(s_col);
  s_col = nullptr;
  if (g_hitTally) {
    HitTallyLog();
    free(g_hitTally);
    g_hitTally = nullptr;
  }
  char path[160], buf[512];
  _snprintf_s(path, sizeof path, _TRUNCATE, "/battle/terrain?match=%s", match);
  const char *raw;
  int n;
  bool sent = HttpGetLoopbackRaw(path, buf, (int)sizeof buf, &raw, &n, body, len);
  free(body);
  Logf("terrain: fine %dx%d, %d columns hit, %d surfaces, %d object boxes, %.0f ms tracing, %d bytes -> %s",
       TERRAIN_FINE, TERRAIN_FINE, hits, surfaces, objs, s_ms, len, sent ? "sent" : "NOT sent");
}

// Any outermost ProcessEvent on the game thread is a safe point to trace from
// (the timer hook is one too); `busy` covers the traces' own ProcessEvent calls.
// Runs the terrain fine pass and the viewer's projectile scan in slices.
// Jump probe: are ships teleporting on the SERVER (players and the viewer
// both report snapping)? Each frame tick, every live pawn's location (humans
// and bots, from g_featsByPawn like the viewer snapshot) against the last one.
// Two shapes are logged with the frame gap:
//  - jump: a step faster than JUMP_SPEED in one tick;
//  - stall: a moving pawn whose location did not change for STALL_MS while
//    the host kept ticking, then moved again -- the server is not advancing
//    that pawn (for a human/headless pawn: no ServerMove arriving), which is
//    the "gap in the sim" a viewer sees as a hang then a snap.
// GUESS: thresholds. 20000 uu/s is well over any ship's cruise speed; a
// 60 Hz client sends moves every 1-3 frames, so 120 ms without one is a gap.
static const double JUMP_SPEED = 20000.0, STALL_MS = 120.0, STALL_MOVING = 300.0;
static bool g_jumpProbeOff = false;
static long g_jumps = 0, g_stalls = 0;
static double g_stallWorstMs = 0;
static void JumpProbe(uint8_t *gm, double now) {
  struct Seen { void *pawn; float at[3]; double t, movedAt, speed; };
  static Seen s_seen[FEATS_SLOTS];
  static int s_logged = 0;
  uint8_t *pcs[16];
  void *human[16];
  int nh = CollectHumanPCs(gm, pcs, 16);
  for (int i = 0; i < nh; ++i)
    human[i] = IsReadable(pcs[i] + OFF_CONTROLLER_PAWN, 8) ? *(void **)(pcs[i] + OFF_CONTROLLER_PAWN) : nullptr;
  for (int slot = 0; slot < FEATS_SLOTS; ++slot) {
    FeatsByPawn &e = g_featsByPawn[slot];
    void *pawn = e.pawn;
    if (!pawn || !ObjectLive(pawn) || !ObjectLive(e.comp) || FeatsCompOf(pawn) != e.comp ||
        !ClassChainIs((UObjectMin *)pawn, "Pawn"))
      continue;
    float at[3];
    if (!ActorLocation(pawn, at))
      continue;
    int who = -1;
    for (int i = 0; i < nh; ++i)
      if (human[i] == pawn)
        who = i;
    Seen &o = s_seen[slot];
    if (o.pawn == pawn && now > o.t) {
      double dx = at[0] - o.at[0], dy = at[1] - o.at[1], dz = at[2] - o.at[2];
      double d = sqrt(dx * dx + dy * dy + dz * dz), dt = now - o.t;
      if (d > 0.5) {
        double still = now - o.movedAt;
        if (o.speed > STALL_MOVING && still > STALL_MS && d > 50.0) {
          ++g_stalls;
          if (still > g_stallWorstMs)
            g_stallWorstMs = still;
          if (s_logged < 400 && ++s_logged)
            Logf("jump probe: %s %d pawn %p stalled %.0f ms then moved %.0f uu (was %.0f uu/s)",
                 who >= 0 ? "player" : "bot", who >= 0 ? who : slot, pawn, still, d, o.speed);
        }
        if (d > 500.0 && d / (dt / 1000.0) > JUMP_SPEED) {
          ++g_jumps;
          if (s_logged < 400 && ++s_logged)
            Logf("jump probe: %s %d pawn %p moved %.0f uu in %.0f ms (%.0f %.0f %.0f -> %.0f %.0f %.0f)",
                 who >= 0 ? "player" : "bot", who >= 0 ? who : slot, pawn, d, dt, o.at[0], o.at[1], o.at[2],
                 at[0], at[1], at[2]);
        }
        // A spawn is a teleport from the pool far below the map (~430,000 uu,
        // live 2026-10-17); it must not count as speed, or the parked ship
        // after it logs as a multi-second "stall".
        double span = now - o.movedAt;
        o.speed = d > 50000.0 ? 0 : span > 0 ? d / (span / 1000.0) : 0;
        o.movedAt = now;
      }
    } else {
      o.movedAt = now;
      o.speed = 0;
    }
    o.pawn = pawn;
    memcpy(o.at, at, sizeof at);
    o.t = now;
  }
}

// Viewer snapshots at VIEWER_SNAP_MS from the frame tick (the 1 s CommandsTick
// cadence made the viewer's motion visibly stepped); CommandsTick still posts
// one when the frame path has not for a while.
static const double VIEWER_SNAP_MS = 100.0;
static char g_viewerMatch[96];
static double g_viewerSnapAt = 0;
static bool g_viewerSnapOff = false;

// One "stutter:" line per frame gap over 100 ms (at most one per 500 ms, the
// rest counted into the next): how much of the gap our zones took and the
// biggest of them, the last chat command and its age, live counts. suspect=
// is our zone when ours is at least half the gap, "after:<cmd>" when a
// command ran in the last 3 s, else empty (the engine: GC, streaming, net).
static void StutterReport(double from, double to) {
  static double s_lastLog = 0;
  static int s_skipped = 0;
  if (to - s_lastLog < 500.0) {
    ++s_skipped;
    return;
  }
  s_lastLog = to;
  double ours = 0, bigMs = 0;
  const char *big = "";
  for (ZoneRec &z : g_zoneRing) {
    double a = z.start > from ? z.start : from, b = z.end < to ? z.end : to;
    if (z.name[0] && b > a) {
      ours += b - a;
      if (b - a > bigMs)
        bigMs = b - a, big = z.name;
    }
  }
  int npcs = -1, proj = g_projCur >= 0 ? g_projN[g_projCur] : -1;
  __try {
    uint8_t *scene = (uint8_t *)((tGetSceneManager)(g_base + RVA_COMBAT_SCENE_MANAGER))();
    if (scene && IsReadable(scene + OFF_SCENE_NPCS_COUNT, 4))
      npcs = *(int32_t *)(scene + OFF_SCENE_NPCS_COUNT);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  double cmdAge = g_lastCmdAt ? (to - g_lastCmdAt) / 1000.0 : -1;
  char suspect[80] = "";
  if (ours * 2 >= to - from && big[0])
    snprintf(suspect, sizeof suspect, "%s", big);
  else if (cmdAge >= 0 && cmdAge < 3.0)
    snprintf(suspect, sizeof suspect, "after:%s", g_lastCmd);
  for (char *c = suspect; *c; ++c)
    if (*c == ' ' || *c == '"')
      *c = '_';
  Logf("stutter: %s %.0f ms gap suspect=%s ours=%.0f biggest=\"%s\" biggest_ms=%.0f last_cmd=\"%s\" cmd_age_s=%.1f "
       "npcs=%d projectiles=%d phase=%ld skipped=%d",
       g_viewerMatch[0] ? g_viewerMatch : "none", to - from, suspect[0] ? suspect : "engine", ours, big, bigMs, g_lastCmd,
       cmdAge, npcs, proj, (long)g_framePhase, s_skipped);
  s_skipped = 0;
}

static void PhaseMark(long ph) {
  double t = NowMs();
  g_vqMs[g_vqWhere & 7] += t - g_vqT;
  g_vqT = t;
  g_vqWhere = g_framePhase = ph;
}

static void FrameTick() {
  static double s_last = 0;
  if (g_frameBusySp || !(g_terrainFine || (g_viewerOn && !g_projScanOff)) || GetCurrentThreadId() != g_gameThreadId)
    return;
  double now = NowMs();
  // Server hitch meter: the gap between outermost ProcessEvents on the game
  // thread is (at least) the frame time. A host stall is what makes clients
  // snap back, so the heartbeat reports the worst gap and how many passed
  // 100 ms (GUESS: threshold; a 60 Hz host frame is ~16 ms).
  static double s_seen = 0;
  if (s_seen > 0) {
    double gap = now - s_seen;
    if (gap > g_frameWorstMs)
      g_frameWorstMs = gap;
    if (gap > 100.0) {
      ++g_frameHitches;
      StutterReport(s_seen, now);
    }
  }
  s_seen = now;
  if (now - s_last < TERRAIN_FRAME_GAP_MS)
    return;
  g_frameBusySp = __builtin_frame_address(0);
  s_last = now;
  ++g_frameTicks;
  g_vqT = now;
  VqCacheOn(true);
  PhaseMark(1);
  if (g_terrainFine && g_terrainGm)
    __try {
      TerrainTick(g_terrainGm, nullptr, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_terrainFine = false;
      Logf("terrain: fault in a frame tick -- fine pass continues on the timer only");
    }
  if (g_viewerOn && !g_projScanOff)
    __try {
      PhaseMark(2);
      ProjScanStep(PROJ_SCAN_MS);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_projScanOff = true;
      Logf("viewer: fault in the projectile scan -- projectiles off for this host");
    }
  if (g_viewerOn && !g_viewerSnapOff && g_viewerMatch[0] && now - g_viewerSnapAt >= VIEWER_SNAP_MS)
    __try {
      PhaseMark(4);
      g_viewerSnapAt = now;
      ViewerSnapshot(g_viewerMatch);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_viewerSnapOff = true;
      Logf("viewer: fault in a frame-tick snapshot -- back to the 1 s cadence");
    }
  if (g_terrainGm && !g_jumpProbeOff)
    __try {
      PhaseMark(3);
      JumpProbe(g_terrainGm, now);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_jumpProbeOff = true;
      Logf("jump probe: fault -- off for this host");
    }
  PhaseMark(0);
  VqCacheOn(false);
  g_frameBusySp = nullptr;
}

// YNPCAIController::m_fireAbilities (+0x860, stride 0xD0): +0x10 is the
// ability class a slot wants, +8 the pawn ability index the trigger
// (0x2AAAA4) fires, -1 = "fail ability index=N". The Horde controller BPs name
// T0 classes (AB_DN_Sec_MslH_Dmg_Ability_T0_BP_C) while /spawn pawns carry T5
// ones, so the exact-class match leaves -1 (boss13), and an index written in
// directly is reset to -1 again (boss14: errors unchanged). So point +0x10 at
// the pawn's own class too. Matching is by class name with the tier stripped.
// GUESS: a slot with no class means "the pawn ability at the slot's index" --
// on DreadnoughtHeavyNukerT5 the classless slots 0/3/4/5 are exactly the pawn
// abilities not named by a slot (WarpM Weaken, ArInc buff, WarpM Dmg, Swarm).
// Returns how many slots it had to (re)write.
static int FixNpcFireAbilities(void *ctrl, void *pawn, bool log) {
  if (!IsReadable((uint8_t *)ctrl + 0x860, 16) || !IsReadable((uint8_t *)pawn + OFF_PAWN_ABILITY_MANAGER, 8))
    return 0;
  uint8_t *mgr = *(uint8_t **)((uint8_t *)pawn + OFF_PAWN_ABILITY_MANAGER);
  if (!IsReadable(mgr + 0xF8, 16))
    return 0;
  auto stem = [](const char *n, char *out, size_t cap) {
    size_t k = 0;
    for (; n && n[k] && k + 1 < cap; ++k) {
      if (n[k] == '_' && n[k + 1] == 'T' && n[k + 2] >= '0' && n[k + 2] <= '9')
        break;
      out[k] = n[k];
    }
    out[k] = 0;
  };
  uint8_t *fa = *(uint8_t **)((uint8_t *)ctrl + 0x860);
  int nfa = *(int32_t *)((uint8_t *)ctrl + 0x868);
  int na = *(int32_t *)(mgr + 0x100);
  UObjectMin **abl = *(UObjectMin ***)(mgr + 0xF8);
  if (!fa || !abl || na <= 0 || !IsReadable(abl, na * 8))
    return 0;
  int fixed = 0;
  for (int i = 0; i < nfa && i < 16 && IsReadable(fa + i * 0xD0, 0xD0); ++i) {
    uint8_t *e = fa + i * 0xD0;
    int32_t *idx = (int32_t *)(e + 8);
    UObjectMin **want = (UObjectMin **)(e + 0x10);
    if (*idx >= 0 && *idx < na)
      continue;
    const char *wn = IsReadable(*want, sizeof(UObjectMin)) ? NameText((*want)->Name) : nullptr;
    int hit = -1;
    if (wn) {
      char ws[128], as[128];
      stem(wn, ws, sizeof ws);
      for (int j = 0; j < na && j < 32 && hit < 0; ++j)
        if (IsReadable(abl[j], sizeof(UObjectMin))) {
          stem(ClassNameOf(abl[j]), as, sizeof as);
          if (!strcmp(ws, as))
            hit = j;
        }
    } else if (i < na) {
      hit = i; // GUESS: see above
    }
    if (hit < 0 || !IsReadable(abl[hit], sizeof(UObjectMin)))
      continue;
    *want = abl[hit]->Class;
    *idx = hit;
    ++fixed;
    if (log)
      Logf("commands: spawn abilities -- slot %d wants %s -> pawn ability %d (%s)", i, wn ? wn : "(none)", hit,
           ClassNameOf(abl[hit]));
  }
  return fixed;
}

// Re-check the /spawn controllers for as long as they hold their pawn: the
// game resets the slot indices to -1 again mid-fight (boss17: no errors while
// this ran, 2 before it and ~600 after a 180 s limit ran out), so re-apply.
struct NpcFireWatch {
  void *ctrl, *pawn;
  double until;
  int resets;
};
static NpcFireWatch g_npcFireWatch[16];
static void NoteNpcFireAbilities(void *ctrl, void *pawn) {
  for (NpcFireWatch &w : g_npcFireWatch)
    if (!w.ctrl || NowMs() > w.until) {
      w = {ctrl, pawn, NowMs() + 1e12, 0};
      return;
    }
}
static void NpcFireAbilitiesTick() {
  for (NpcFireWatch &w : g_npcFireWatch) {
    if (!w.ctrl || NowMs() > w.until)
      continue;
    // The controller must still hold this pawn (AController::Pawn).
    if (!IsReadable((uint8_t *)w.ctrl + OFF_CONTROLLER_PAWN, 8) || *(void **)((uint8_t *)w.ctrl + OFF_CONTROLLER_PAWN) != w.pawn) {
      Logf("commands: spawn abilities -- %p no longer holds pawn %p, watch dropped", w.ctrl, w.pawn);
      w.ctrl = nullptr;
      continue;
    }
    int n = FixNpcFireAbilities(w.ctrl, w.pawn, w.resets == 0);
    if (n && (++w.resets <= 5 || w.resets % 100 == 0))
      Logf("commands: spawn abilities -- %p: %d slot(s) were reset, re-applied (#%d)", w.ctrl, n, w.resets);
  }
}

static void CommandsTick(uint8_t *gm) {
  ZONE("NpcFireAbilitiesTick", NpcFireAbilitiesTick());
  ZONE("NpcTierTick", NpcTierTick());
  ZONE("BotsTick", BotsTick());
  ZONE("EliteTick", EliteTick(gm));
  ZONE("GameBotEliteTick", GameBotEliteTick(gm));
  ZONE("SkyMeteorTick", SkyMeteorTick(gm));
  ZONE("SquadWatchTick", SquadWatchTick());
  ZONE("HeraldTick", HeraldTick());
  static int s_tick = 0;
  if (++s_tick % 2)
    return;
  static char s_match[96] = "";
  static bool s_haveMatch = false;
  if (!s_haveMatch)
    s_haveMatch = MatchIDFromCommandLine(s_match, sizeof s_match) || (strcpy(s_match, "none"), true);
  // Heartbeat while a frame-path job is wanted (~20 s): "0 ticks" is how the
  // missing ProcessEvent hook on non-Havoc hosts showed up. Match id because
  // concurrent hosts share this log.
  static int s_hb = 0;
  if (g_frameWanted && ++s_hb % 10 == 1) {
    Logf("frame: %s: %ld ticks, phase %ld, stale PE resets %ld, busy resets %ld, worst frame %.0f ms, %ld hitches >100 ms, %ld jumps, %ld stalls (worst %.0f ms), terrain fine %d, projectile list %d", s_match, g_frameTicks, g_framePhase, g_peStaleResets, g_frameBusyResets, g_frameWorstMs, g_frameHitches, g_jumps, g_stalls, g_stallWorstMs,
         (int)g_terrainFine, g_projCur);
    Logf("frame: VirtualQuery calls (ms) elsewhere %ld, terrain %ld (%.0f), projectiles %ld (%.0f), jump %ld (%.0f), snapshot %ld (%.0f), timer %ld; frame cache hits %ld",
         g_vqN[0], g_vqN[1], g_vqMs[1], g_vqN[2], g_vqMs[2], g_vqN[3], g_vqMs[3], g_vqN[4], g_vqMs[4], g_vqN[5], g_vqHits);
    g_vqHits = 0;
    for (int i = 0; i < 8; ++i)
      g_vqN[i] = 0, g_vqMs[i] = 0;
    g_frameWorstMs = 0;
    g_frameHitches = 0;
    g_stallWorstMs = 0;
  }
  strcpy_s(g_viewerMatch, sizeof g_viewerMatch, s_match);
  if (NowMs() - g_viewerSnapAt > 1500.0)
    ViewerSnapshot(s_match);
  g_gameThreadId = GetCurrentThreadId();
  g_terrainGm = gm;
  TerrainTick(gm, s_match);
  static long long s_since = 0;
  char path[192], buf[4096];
  _snprintf_s(path, sizeof path, _TRUNCATE, "/battle/commands?match=%s&since=%lld", s_match, s_since);
  const char *raw = nullptr;
  int n = 0;
  // 300 ms rather than the 2 s default: this poll runs on the game thread every
  // other CommandsTick (2 s), and mmogbrain is on loopback, so 300 ms is
  // generous. A poll that misses is harmless -- `since` retries the batch next
  // tick -- while the 2 s default would hold every player's frame for two
  // seconds whenever mmogbrain is slow (round 12/13).
  static const int COMMANDS_TIMEOUT_MS = 300;
  if (!HttpGetLoopbackRaw(path, buf, (int)sizeof buf, &raw, &n, nullptr, 0, COMMANDS_TIMEOUT_MS) || !raw || n <= 0)
    return;
  char body[4096];
  memcpy(body, raw, n < (int)sizeof body ? n : (int)sizeof body - 1);
  body[n < (int)sizeof body ? n : (int)sizeof body - 1] = 0;
  for (char *line = strtok(body, "\r\n"); line; line = strtok(nullptr, "\r\n")) {
    long long seq = 0;
    char cmd[32] = "", arg[64] = "", arg2[64] = "", arg3[32] = "";
    if (sscanf(line, "%lld %31s %63s %63s %31s", &seq, cmd, arg, arg2, arg3) < 2 || seq <= s_since)
      continue;
    s_since = seq;
    Logf("commands: #%lld %s %s", seq, cmd, arg);
    ZoneBegin("cmd", cmd); // ended by the next command's, or after the loop
    snprintf(g_lastCmd, sizeof g_lastCmd, "%s %s %s", cmd, arg, arg2);
    g_lastCmdAt = NowMs();
    if (!strcmp(cmd, "turbo"))
      CommandTurbo(!strcmp(arg, "on"));
    else if (!strcmp(cmd, "npc"))
      g_npcTier = !strcmp(arg, "brutal") ? 2 : !strcmp(arg, "hard") ? 1 : 0;
    else if (!strcmp(cmd, "wave") && !strcmp(arg, "skip"))
      CommandWaveSkip();
    else if (!strcmp(cmd, "wave") && atoi(arg) >= 1)
      g_hvcWaveJump = atoi(arg) > 50 ? 50 : atoi(arg);
    else if (!strcmp(cmd, "bots"))
      g_botsMult = atoi(arg) < 1 ? 1 : atoi(arg) > 4 ? 4 : atoi(arg);
    else if (!strcmp(cmd, "bottier")) {
      g_botTier = std::min(5, std::max(0, atoi(arg)));
      Logf("commands: bot tier %d", g_botTier);
    } else if (!strcmp(cmd, "botship") && atoi(arg) >= 0 && atoi(arg) <= 2) {
      FNameMin n = {};
      if (_stricmp(arg2, "off") && ShipIdPawnClass(arg2, false)) {
        wchar_t w[64];
        swprintf(w, 64, L"%hs", arg2);
        ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&n, w, 1 /* FNAME_Add */);
      }
      g_botShip[atoi(arg)] = n;
      snprintf(g_botShipStr[atoi(arg)], 64, "%s", *(uint64_t *)&n ? arg2 : "");
      g_botShipGm = nullptr;
      Logf("commands: bot ship team %d -> %s", atoi(arg), *(uint64_t *)&n ? arg2 : "the game's pick");
    } else if (!strcmp(cmd, "team") && (atoi(arg2) == 1 || atoi(arg2) == 2)) {
      TeamOverride *slot = nullptr;
      for (TeamOverride &o : g_teamOverride)
        if (!_stricmp(o.pid, arg) || (!slot && !o.team))
          slot = &o;
      if (slot) {
        strncpy(slot->pid, arg, sizeof slot->pid - 1);
        slot->team = atoi(arg2);
      }
      Logf("commands: team %s -> %s", arg, arg2);
    } else if (!strcmp(cmd, "shake"))
      CommandShake(gm, (float)std::min(10, std::max(1, atoi(arg))));
    else if (!strcmp(cmd, "flash"))
      CommandFlash(gm, arg);
    else if (!strcmp(cmd, "slowmo"))
      CommandSlowmo(gm, std::min(1.f, std::max(0.2f, (float)atof(arg))));
    else if (!strcmp(cmd, "difficulty"))
      CommandDifficulty(gm, arg);
    else if (!strcmp(cmd, "nuke"))
      CommandNuke(gm);
    else if (!strcmp(cmd, "meteor")) {
      // /meteor [targets]: the herald in Onslaught; elsewhere the old shower.
      // Braced: unbraced, the else-ifs below bound to this inner if, and
      // props/kill/ffa/spawn/enemy silently did nothing (2026-10-18).
      if (HostIsHavoc() || HostIsHorde() || !InvasionGameState(gm) || !CommandHeraldMeteor(gm, atoi(arg) < 1 ? 1 : atoi(arg), (float)atof(arg2), arg3[0] ? std::min(255, atoi(arg3)) : 0))
        CommandSkyMeteor(gm, atoi(arg) < 1 ? 12 : std::min(40, atoi(arg)), (float)atof(arg2));
    } else if (!strcmp(cmd, "fighters")) {
      CommandFighters(gm, arg[0] ? std::min(255, std::max(0, atoi(arg))) : 0);
    } else if (!strcmp(cmd, "hunters")) {
      CommandHunters(gm, atoi(arg) > 0 ? std::min(12, atoi(arg)) : 2, arg2[0] ? std::min(255, std::max(0, atoi(arg2))) : 0,
                     !strcmp(arg3, "all"), 0);
    } else if (!strcmp(cmd, "swarm") || !strcmp(cmd, "squad")) {
      bool sw = cmd[1] == 'w';
      int n = atoi(arg) > 0 ? std::min(sw ? 60 : 20, atoi(arg)) : sw ? 24 : 8;
      CommandSwarm(gm, sw ? 0 : 1, n, arg2[0] ? std::min(255, std::max(0, atoi(arg2))) : 3);
    } else if (!strcmp(cmd, "probefn"))
      ProbeFn(gm, arg, arg2);
    else if (!strcmp(cmd, "props"))
      ProbeProps(gm, arg, arg2);
    else if (!strcmp(cmd, "kill"))
      CommandKill(gm, arg);
    else if (!strcmp(cmd, "ffa")) {
      g_ffaGm = strcmp(arg, "off") ? gm : nullptr;
      g_ffaN = 0;
      if (g_ffaLimitGm == gm && g_ffaLimitSaved > 0 && IsReadable(gm + OFF_GM_KILLS_FOR_WINNING, 4))
        *(int32_t *)(gm + OFF_GM_KILLS_FOR_WINNING) = g_ffaLimitSaved; // back to the team rule (on: re-read)
      g_ffaLimitGm = nullptr;
      g_ffaLimitArg = g_ffaGm ? atoi(arg2) : 0;
      Logf("ffa: %s for game mode %p", g_ffaGm ? "on" : "off", gm);
    } else if (!strcmp(cmd, "hunterdef") && atoi(arg) > 0)
      CommandHunters(gm, 1, 0, true, atoi(arg));
    else if (!strcmp(cmd, "bossdef") && atoi(arg) > 0)
      CommandBossDef(gm, atoi(arg), std::min(10, std::max(1, atoi(arg2))));
    else if (!strcmp(cmd, "spawn"))
      CommandSpawn(gm, arg, atoi(arg2) < 1 ? 1 : atoi(arg2) > 8 ? 8 : atoi(arg2), std::min(10, std::max(1, atoi(arg3))));
    else if (!strcmp(cmd, "enemy")) {
      g_hvcEnemyOverrideOn = strcmp(arg, "off") != 0;
      if (g_hvcEnemyOverrideOn) {
        wchar_t w[32];
        MultiByteToWideChar(CP_UTF8, 0, arg, -1, w, 32);
        ((tFNameCtor)(g_base + RVA_FNAME_CTOR_WIDE))(&g_hvcEnemyOverride, w, 1);
      }
      Logf("commands: enemy override %s", g_hvcEnemyOverrideOn ? arg : "off");
    }
  }
  ZoneEnd();
}
