// The camera hook. heron::cameraset::update_blending runs once per tick for the player: it blends the camera set
// the game picked (select_set) into OutputData+0x50, which the tail camera then reads (distance, positions,
// field of view, auto-rotation...). Everything after it (collision, the pivot, jumps, gravity, lock-on) stays the
// game's own work, on our values.
//
// We change the set the blend goes TO (+0x410), never the blend's result or where it starts. Every copy the game
// makes from there then carries our values: the blend itself (+0x50), where the next blend starts (select_set copies
// the output to +0x230 at a switch) and the tail camera's history (update_stick_blending copies +0x230 into its
// HistoryCache at a switch; its rig, 0x14204a6f0, evaluates each history state, folds them by their weights, then
// blends toward the output by t). An earlier version changed the output and put the game's values back into +0x230,
// so every switch (sprint, dash, the game's idle set) started the rig from the game's own camera: a jump out, then
// a slide back in.
//
// Between switches the target holds what we wrote; select_set's conversion writes the game's values there when it
// picks a new set (a new id at +0xc). Each tick, before the blend, a field that no longer holds what we wrote (or
// every field, after a new set id) is the game's new value: we keep it and write ours over it. Our changes ease
// (settings.cpp), so the target is rewritten every tick.
//
// The camera systems run on the game's fixed tick (FixedTime, about 30 a second: one tick after one or two drawn
// frames), so the easing goes by that tick's time too.
#include "common.h"
#include <cmath>

namespace cp {

// update_blending(Entity*, const FixedTime&, const Ptr<GameState>&). The entity holds component array bases and
// the row: OutputData = q[1] + q[3] * 0x610 (the signature pins that multiply). The prologue is three movs to the
// home area (15 bytes).
static const char* kBlendSig =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 56 41 57 48 8B EC 48 83 EC 50 4D 8B F8 48 8B FA 48 8B D9 "
    "E8 ?? ?? ?? ?? 80 78 38 00 0F 85 ?? ?? ?? ?? C5 FC 10 0B C5 FC 11 4D D8 C4 E3 7D 19 C8 01 C4 E3 F9 16 C6 01 "
    "4C 8B 75 D8 48 69 DE 10 06 00 00";

bool FindCameraTargets(const Image& img, CameraTargets& t, std::string& err) {
    t.blend = FindUnique(img, "cameraset update_blending", kBlendSig, err);
    return t.blend != 0;
}

using BlendFn = void (*)(void*, void*, void*);
static BlendFn volatile g_origBlend = nullptr;
static bool g_installed = false;
bool CameraHookInstalled() { return g_installed; }

// The params fields we change (float offsets inside the block).
static const size_t kFields[] = {od::pFov,         od::pDistMul,      od::pDefault,      od::pDefault + 4,
                                 od::pDefault + 8, od::pSafe,         od::pSafe + 4,     od::pSafe + 8,
                                 od::pFallback,    od::pFallback + 4, od::pFallback + 8, od::pTarget,
                                 od::pTarget + 4,  od::pTarget + 8};
static const int kN = (int)(sizeof(kFields) / sizeof(kFields[0]));

// One state for the player's camera. Not keyed by address: the ECS moves the player's components to another
// chunk whenever its archetype changes (at a load, and in play), copying our values along; the check "still holds
// what we wrote" works wherever the row now is.
struct Slot {
    uint8_t* od = nullptr;  // where the row was last tick
    bool valid = false;     // raw/mod hold the target's game values and what we wrote over them
    float raw[kN], mod[kN];
    int32_t rawSet = -1;    // the set id raw was taken from
    float ground[kN];       // the game's camera before the last jump (raw, while not jumping)
    bool groundValid = false;
    int32_t set = -2;       // diagnostics: the set id last logged
    int32_t move = -1;      // diagnostics: the movement mode last logged
    uint8_t mode = 0xff, state = 0xff;
    int moves = 0;
    int trace = 0;  // diagnostics: ticks left to log after a set switch
};
static SRWLOCK g_lock = SRWLOCK_INIT;
static Slot g_slot;
static Tuning g_cur;         // eased toward the live camera (settings.cpp), or the identity while off
static const int kEased = 9; // g_cur's values (Eased)
static float g_vel[kEased];  // how fast each is moving
static bool g_ease = true;   // off in the tests: changes land at once
static bool g_testOverride = false;  // tests: g_testTarget / g_testOn instead of the cameras
static Tuning g_testTarget;
static bool g_testOn = true;
static LARGE_INTEGER g_lastEase = {};
static bool g_seen[64];
static uint32_t g_seenIds[64];
static ULONGLONG g_lastSnap = 0;

static inline float F(const uint8_t* p, size_t off) { float v; memcpy(&v, p + off, 4); return v; }
static inline void SetF(uint8_t* p, size_t off, float v) { memcpy(p + off, &v, 4); }
static inline bool Same(float a, float b) { return memcmp(&a, &b, 4) == 0; }

static uint8_t* OutputOf(void* entity) {
    const uint64_t* q = (const uint64_t*)entity;
    if (!q || !q[1] || q[3] > 0x100000) return nullptr;
    return (uint8_t*)(q[1] + q[3] * od::kSize);
}

static Slot* SlotFor(uint8_t* p) {
    Slot& s = g_slot;
    if (s.od != p) {
        if (s.od && g_cfg.diagnostics && ++s.moves <= 50)
            Log("row moved %p -> %p (set %08x)", s.od, p, *(const uint32_t*)(p + od::kSetId));
        s.od = p;
    }
    return &s;
}

// ---- easing ----
// Each value moves toward the live camera's like a critically damped spring: it sets off and arrives gently and never
// overshoots, so a change that starts at once (the zoom key) doesn't jerk, and a target that keeps moving (the wheel)
// is followed smoothly. smooth is about the time it takes.
static float Spring(float cur, float want, float& vel, float smooth, float dt) {
    const float omega = 2.f / std::max(smooth, 1e-3f);
    const float x = omega * dt;
    const float e = 1.f / (1.f + x + 0.48f * x * x + 0.235f * x * x * x);
    const float change = cur - want;
    const float temp = (vel + omega * change) * dt;
    vel = (vel - omega * temp) * e;
    float out = want + (change + temp) * e;
    if ((change < 0) == (out > want)) {  // there (or past it)
        out = want;
        vel = 0;
    }
    return out;
}

static float* Eased(Tuning& t, int i) {
    switch (i) {
        case 0: return &t.fovAdd;
        case 1: return &t.distMul;
        case 2: return &t.jump;
    }
    return i < 6 ? &t.posAdd[i - 3] : &t.targetAdd[i - 6];
}

// tick: the game's time step (FixedTime). Timing the easing by the wall clock instead (21 ms, then 42 ms between
// ticks, as the ticks fall on every other drawn frame) moved the camera unevenly from tick to tick: a stutter in
// quick changes like the zoom.
static void Ease(float tick) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = g_lastEase.QuadPart ? (float)(now.QuadPart - g_lastEase.QuadPart) / (float)freq.QuadPart : 0.f;
    g_lastEase = now;
    if (tick > 0.f && tick < 0.5f) dt = tick;
    dt = std::min(dt, 0.5f);
    Tuning want;
    float smooth = 0.2f;
    if (g_testOverride) {
        if (g_testOn) want = g_testTarget;
    } else {
        LiveTarget(want, smooth);
    }
    if (!g_ease) {
        g_cur = want;
        memset(g_vel, 0, sizeof(g_vel));
        return;
    }
    bool there = true;
    for (int i = 0; i < kEased; ++i) {
        float* c = Eased(g_cur, i);
        const float w = *Eased(want, i);
        *c = Spring(*c, w, g_vel[i], smooth, dt);
        there = there && fabsf(*c - w) < (i == 0 ? 1e-3f : 1e-4f) && fabsf(g_vel[i]) < 1e-3f;
    }
    if (there) {  // exactly the live camera ("off" is exactly the game's camera)
        g_cur = want;
        memset(g_vel, 0, sizeof(g_vel));
    }
}

// The game's values -> ours, field by field (kFields order).
static void Apply(const float* raw, float* mod) {
    const Tuning& t = g_cur;
    // The set's field of view is in radians when it's small; the setting is in degrees.
    const float fovScale = raw[0] < 3.5f ? 3.14159265f / 180.f : 1.f;
    mod[0] = raw[0] + t.fovAdd * fovScale;
    mod[1] = raw[1] * t.distMul;
    for (int v = 0; v < 3; ++v)  // default, safe, fallback
        for (int a = 0; a < 3; ++a) mod[2 + v * 3 + a] = raw[2 + v * 3 + a] + t.posAdd[a];
    for (int a = 0; a < 3; ++a) mod[11 + a] = raw[11 + a] + t.targetAdd[a];
    if (t.Identity())
        for (int i = 0; i < kN; ++i) mod[i] = raw[i];  // bit-exact
}

// ---- the panel's side (panel.cpp calls these on the window's thread) ----
static void DumpParams(const uint8_t* p, const char* what) {
    for (size_t row = 0; row < 0x180; row += 0x20) {
        char line[256];
        int n = sprintf_s(line, "%s +%03zx:", what, row);
        for (size_t i = 0; i < 0x20 && n > 0; i += 4) n += sprintf_s(line + n, sizeof(line) - n, " %9.4f", F(p, row + i));
        Log("%s", line);
    }
}

static bool g_dump = false;  // the panel asked for a dump; the next tick writes it

void RequestDump() { g_dump = true; }

static void DumpNow(const Slot& s) {
    g_dump = false;
    if (!s.od) return;
    Log("dump: set %08x mode %u state %u t=%.3f", *(const uint32_t*)(s.od + od::kSetId), s.od[od::kMode],
        s.od[od::kState], F(s.od, od::kT));
    DumpParams(s.od + od::kOut, "out");
}

// ---- diagnostics ----
static void LogSetOnce(const uint8_t* p, uint32_t id, const float* raw) {
    for (int i = 0; i < 64; ++i) {
        if (g_seen[i] && g_seenIds[i] == id) return;
        if (!g_seen[i]) {
            g_seen[i] = true;
            g_seenIds[i] = id;
            break;
        }
    }
    const uint8_t* to = p + od::kTo;
    Log("  set %08x: fov %.4f aspect %.3f dist x%.3f hide %.2f vertical(%.3f %.3f) grounded(%.3f %.3f %.3f) "
        "airborne(%.3f %.3f %.3f)", id, raw[0], F(to, od::pAspect), raw[1], F(to, od::pHide), F(to, od::pVertical),
        F(to, od::pVertical + 4), F(to, od::pGroundedSmooth), F(to, od::pGroundedSmooth + 4),
        F(to, od::pGroundedSmooth + 8), F(to, od::pAirborneSmooth), F(to, od::pAirborneSmooth + 4),
        F(to, od::pAirborneSmooth + 8));
    Log("  set %08x: default(%.3f %.3f %.3f) safe(%.3f %.3f %.3f) fallback(%.3f %.3f %.3f) target(%.3f %.3f %.3f)", id,
        raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10], raw[11], raw[12], raw[13]);
}

static void Diagnose(Slot& s) {
    const uint8_t* p = s.od;
    const int32_t set = *(const int32_t*)(p + od::kSetId);
    const int32_t move = *(const int32_t*)(p + od::kMoveMode);
    if (set != s.set || p[od::kMode] != s.mode || p[od::kState] != s.state || move != s.move) {
        Log("set %08x -> %08x  mode %u -> %u  state %u -> %u  move %d -> %d  switched %u", (uint32_t)s.set,
            (uint32_t)set, s.mode, p[od::kMode], s.state, p[od::kState], s.move, move, p[od::kSwitched]);
        s.move = move;
        if (set != -1 && s.valid) LogSetOnce(p, (uint32_t)set, s.raw);
        if (set != -1 && s.set >= 0 && set != s.set) {
            // What the new set changes (in our values): every params float that differs from where the blend starts.
            char line[1024];
            int n = sprintf_s(line, "  diff:");
            for (size_t f = 4; f < 0x180 && n > 0 && n < 900; f += 4) {
                const float a = F(p + od::kFrom, f), b = F(p + od::kTo, f);
                if (fabsf(a - b) > 1e-4f) n += sprintf_s(line + n, sizeof(line) - n, " +%zx %.3f>%.3f", f, a, b);
            }
            Log("%s", line);
            s.trace = 20;  // then the blend tick by tick
        }
        s.set = set, s.mode = p[od::kMode], s.state = p[od::kState];
    }
    if (s.trace > 0) {
        // The game's target, ours, where the blend started (the tail camera's history takes it) and the blend.
        --s.trace;
        const uint8_t* o = p + od::kOut;
        const uint8_t* f = p + od::kFrom;
        Log("  tick t=%.3f dist game %.3f to %.3f from %.3f out %.3f | camera y game %.3f to %.3f out %.3f | look game "
            "%.3f to %.3f out %.3f", F(p, od::kT), s.raw[1], s.mod[1], F(f, od::pDistMul), F(o, od::pDistMul), s.raw[3],
            s.mod[3], F(o, od::pDefault + 4), s.raw[12], s.mod[12], F(o, od::pTarget + 4));
    }
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastSnap >= 5000) {
        g_lastSnap = now;
        const uint8_t* o = p + od::kOut;
        Log("snap set %08x t=%.2f out: fov %.4f dist x%.3f default(%.3f %.3f %.3f) target(%.3f %.3f %.3f) ease dist "
            "x%.3f jump %.0f%%", (uint32_t)set, F(p, od::kT), F(o, od::pFov), F(o, od::pDistMul), F(o, od::pDefault),
            F(o, od::pDefault + 4), F(o, od::pDefault + 8), F(o, od::pTarget), F(o, od::pTarget + 4),
            F(o, od::pTarget + 8), g_cur.distMul, g_cur.jump * 100);
    }
}

// ---- the hook ----
// The idle zoom is ours (settings.cpp): the game's idle timer is read and kept at zero, so its idle set never
// comes. With CameraPlus off the game's own idle camera works as usual. Before Ease, so this tick's idle counts.
static void TakeIdleTimer(Slot& s) {
    if (!CameraOn() || g_testOverride || *(const int32_t*)(s.od + od::kSetId) == -1) return;
    IdleTick(F(s.od, od::kIdleTime));
    SetF(s.od, od::kIdleTime, 0.f);
}

// Our values into the blend's target (see the top of the file).
static void ApplyTarget(Slot& s) {
    uint8_t* p = s.od;
    const int32_t set = *(const int32_t*)(p + od::kSetId);
    if (set == -1) {  // no set yet: the game blends nothing
        s.valid = false;
        return;
    }
    uint8_t* to = p + od::kTo;
    const bool newSet = !s.valid || set != s.rawSet;
    for (int i = 0; i < kN; ++i) {
        const float v = F(to, kFields[i]);
        if (newSet || !Same(v, s.mod[i])) s.raw[i] = v;
    }
    s.rawSet = set;
    // The jump camera: in the air (select_set's movement mode) the game's jump set drops the camera and aims lower
    // (the walking set's camera +0.25 m, look-at 1.4 m; the jump set's -1.30 m, 1.0 m), which puts the player high on
    // the screen, the more so the closer the camera. Its field of view, distance and positions are taken only part of
    // the way from the camera on the ground (the last set before the jump); the rest (auto-pitch, smooth times...)
    // stays the jump set's.
    float base[kN];
    memcpy(base, s.raw, sizeof(base));
    if (*(const int32_t*)(p + od::kMoveMode) != od::kMoveJump) {
        memcpy(s.ground, s.raw, sizeof(s.ground));
        s.groundValid = true;
    } else if (s.groundValid && g_cur.jump != 1.f) {
        for (int i = 0; i < kN; ++i) base[i] = s.ground[i] + (s.raw[i] - s.ground[i]) * g_cur.jump;
    }
    Apply(base, s.mod);
    for (int i = 0; i < kN; ++i) SetF(to, kFields[i], s.mod[i]);
    s.valid = true;
}

// ---- the end of a fight ----
// The game's fight camera (camera state 2: wide sets, distance x2) comes from a camera state zone that asks for it
// while a fight is on, and drops its request as the last enemy dies, a moment before the HUD says the fight is over:
// the camera zoomed in at once, whatever the combat end delay. So while a fight is on CameraPlus keeps a request of
// its own for that state beside the zone's (same priority, after it), which keeps the state when the zone's goes,
// until the combat end delay is over. Then it drops it and the game blends to its usual camera itself, as the
// combat camera switches back to the exploration camera. The game's own add and drop are used (its allocator grows
// the vector); the zone system and update_blending both write OutputData, so they never run at the same time.
struct StateRequest {
    uint64_t entity;
    uint32_t priority;
    uint32_t state;  // the low byte
};
using AddRequestFn = void (*)(uint8_t* od, const StateRequest* r);
using DropRequestFn = void (*)(uint8_t* od, uint64_t entity);
static AddRequestFn g_addRequest = nullptr;
static DropRequestFn g_dropRequest = nullptr;
static const uint64_t kOurRequest = 0x2153554C504D4143ull;  // "CAMPLUS!", not an entity
static ULONGLONG g_zoneGone = 0;  // when the zone's request went while the HUD still said fighting (0: it didn't)

static const char* kAddRequestSig = "40 53 48 83 EC 20 48 8B 41 40 48 8D 59 40 8B 49 48 4C 8B DA 48 C1 E1 04 48 03 C8";
static const char* kDropRequestSig = "48 89 54 24 10 53 48 83 EC 20 48 8B D9 33 C9 44 8B 4B 48 45 8B D9 4D 03 DB";

bool FindStateRequests(const Image& img, uint32_t& add, uint32_t& drop, std::string& err) {
    add = FindUnique(img, "state request add", kAddRequestSig, err);
    drop = add ? FindUnique(img, "state request drop", kDropRequestSig, err) : 0;
    return add && drop;
}

bool InstallStateRequests(const Image& img, std::string& err) {
    uint32_t add = 0, drop = 0;
    if (!FindStateRequests(img, add, drop, err)) return false;
    Log("state requests: add +0x%x, drop +0x%x", add, drop);
    UseStateRequestsForTest((void*)(g_gameBase + add), (void*)(g_gameBase + drop));
    return true;
}

void UseStateRequestsForTest(void* add, void* drop) {
    AcquireSRWLockExclusive(&g_lock);
    g_addRequest = (AddRequestFn)add;
    g_dropRequest = (DropRequestFn)drop;
    ReleaseSRWLockExclusive(&g_lock);
}

bool StateRequestsReady() { return g_addRequest && g_dropRequest; }

static void KeepFightState(Slot& s) {
    if (!g_addRequest || !g_dropRequest) return;
    uint8_t* p = s.od;
    const StateRequest* list = *(const StateRequest* const*)(p + od::kRequests);
    const uint32_t n = *(const uint32_t*)(p + od::kRequestCount);
    if (n && !list) return;
    bool ours = false, zone = false;
    uint32_t priority = 0;
    for (uint32_t i = 0; i < n && i < 64; ++i) {
        if (list[i].entity == kOurRequest) ours = true;
        else if ((uint8_t)list[i].state == od::kStateFight && !zone) zone = true, priority = list[i].priority;
    }
    const bool fighting = InCombat(), settling = CombatSettling();
    bool keep = false;
    if (CameraOn() && !g_testOverride && CombatEndDelay() > 0.f) {
        if (zone) {
            g_zoneGone = 0;
            keep = fighting || settling;
        } else if (ours && fighting) {  // the zone's went: the HUD should say the fight is over in a moment
            const ULONGLONG now = GetTickCount64();
            if (!g_zoneGone) g_zoneGone = now;
            keep = now - g_zoneGone < 1500;  // still fighting after that: a zone left mid-fight, let it go
        } else {
            keep = ours && settling;
        }
    }
    if (keep && !ours && zone) {
        const StateRequest r{kOurRequest, priority, (uint32_t)od::kStateFight};
        g_addRequest(p, &r);
        if (g_cfg.diagnostics) Log("fight camera: kept beside the zone's (priority %u)", priority);
    } else if (!keep && ours) {
        g_dropRequest(p, kOurRequest);
        g_zoneGone = 0;
        if (g_cfg.diagnostics) Log("fight camera: let go");
    }
}

static Slot* Enter(void* entity) {
    uint8_t* p = OutputOf(entity);
    return p ? SlotFor(p) : nullptr;
}

static void HookBlend(void* entity, void* time, void* state) {
    AcquireSRWLockExclusive(&g_lock);
    Slot* s = nullptr;
    __try {
        s = Enter(entity);
        if (s) {
            TakeIdleTimer(*s);
            ReportCameraState(s->od[od::kState]);  // the indoor camera
            Ease(time ? *(const float*)time : 0.f);  // FixedTime starts with the tick's time step
            ApplyTarget(*s);
            KeepFightState(*s);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (s) s->valid = false;
        s = nullptr;
    }
    g_origBlend(entity, time, state);
    __try {
        if (s) {
            if (g_dump) DumpNow(*s);
            if (g_cfg.diagnostics) Diagnose(*s);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool InstallCameraHookAt(uint8_t* blend, const uint8_t* prologue, std::string& err) {
    g_cur = Tuning();
    if (!InstallJmpHook(blend, prologue, (void*)&HookBlend, (void* volatile*)&g_origBlend, "update_blending", err))
        return false;
    g_installed = true;
    return true;
}

bool InstallCameraHook(const Image& img, std::string& err) {
    CameraTargets t;
    if (!FindCameraTargets(img, t, err)) return false;
    Log("update_blending at +0x%x", t.blend);
    return InstallCameraHookAt((uint8_t*)(g_gameBase + t.blend), &img.mem[t.blend], err);
}

void SetTuningForTest(const Tuning& t, bool on) {
    AcquireSRWLockExclusive(&g_lock);
    g_testOverride = true;
    g_testTarget = t;
    g_testOn = on;
    g_ease = false;
    ReleaseSRWLockExclusive(&g_lock);
}

void EndTuningTest() {
    AcquireSRWLockExclusive(&g_lock);
    g_testOverride = false;
    g_ease = false;
    ReleaseSRWLockExclusive(&g_lock);
}

void SetEaseForTest(bool on) {
    AcquireSRWLockExclusive(&g_lock);
    g_ease = on;
    memset(g_vel, 0, sizeof(g_vel));
    g_lastEase.QuadPart = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace cp
