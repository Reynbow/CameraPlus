// The cameras (exploration, indoor, combat, zoom, idle), their styles, the zoom key and which camera is live.
//
// Live camera: the one being edited while the panel is open (so it can be seen); otherwise zoom while the zoom
// key holds (or toggles) it, combat while the HUD says we're fighting (and for a short grace after), indoor while
// the game's camera state is its indoor one (camera zones in the levels set it; the hook reports it every tick),
// else exploration. The script reports the HUD's combat flag with every status read.
//
// Idle: the game zooms in after you stand still a while by switching to an idle camera set, which fights our
// changes (some of a set's values switch at once, the rest blend). We keep the game's idle timer at zero and do
// the zoom ourselves: after the same time the idle camera (a change on top of the exploration or indoor camera,
// like the game's own idle set's change) eases in slowly.
//
// Wheel and touchpad zoom: each notch or swipe step changes the live camera's Distance (the camera whose values it
// uses: exploration's while combat or indoor follow it), saved once the steps stop.
#include "common.h"
#include <cmath>
#include <stdio.h>

namespace cp {

// Styles: changes to the game's own camera (distance is a multiple of the game's, the rest are added). The jump
// camera keeps part of the game's jump set (it drops the camera and aims lower while in the air): each style's keeps
// the game's jump framing at its distance (in the close camera a jump would otherwise climb twice as far up the
// screen).
struct Style {
    const char* name;
    float distMul, side, height, fov, jump;  // side = position X, height = look-at height
};
static const Style kStyles[] = {
    {"Game default", 1.00f, 0.00f, 0.00f, 0.f, 1.0f},
    {"Over the shoulder", 0.70f, 0.45f, -0.05f, 0.f, 0.7f},
    {"Close shoulder", 0.50f, 0.55f, -0.10f, -4.f, 0.5f},
    {"Cinematic", 1.15f, 0.25f, -0.15f, -8.f, 1.0f},
    {"Zoom", 0.40f, 0.50f, -0.05f, -15.f, 0.4f},
};
static const int kStyleCount = (int)(sizeof(kStyles) / sizeof(kStyles[0]));
static const char* kViewNames[kViewCount] = {"Exploration", "Combat", "Zoom", "Idle", "Indoor"};
static const wchar_t* kSections[kViewCount] = {L"Exploration", L"Combat", L"Zoom", L"Idle", L"Indoor"};
// A fresh install: exploration close over the shoulder, combat the game's, zoom; idle has no style; indoor is the
// exploration camera (IndoorFollows), Close shoulder when it gets its own.
static const int kDefaultStyle[kViewCount] = {2, 0, 4, -1, 2};

static SRWLOCK g_lock = SRWLOCK_INIT;
static Tuning g_views[kViewCount];
static bool g_combatFollows = false;  // combat camera = the exploration camera
static bool g_indoorFollows = true;   // indoor camera = the exploration camera
static bool g_indoor = false;         // the game's camera state is its indoor one
static bool g_on = true;
static int g_zoomKey = VK_XBUTTON1;
static bool g_zoomToggle = false;
static bool g_zoomHeld = false, g_zoomLatched = false;
static int g_zoomPad = 0;          // the controller's zoom button (a place in the pad list)
static bool g_zoomPadHeld = false;
static bool g_combat = false;
static ULONGLONG g_combatSeen = 0, g_combatReport = 0;
static int g_preview = -1;  // the panel shows this camera
static int g_lastLive = -1;
static const ULONGLONG kCombatGraceMs = 2000;
static bool g_idleOn = true;
static float g_idleAfter = 8.f;  // seconds standing still, like the game's "Idle CameraSet Time"
static float g_idleSecs = 0.f;
static bool g_idle = false;
static bool g_wheelZoom = true, g_touchZoom = true;
static volatile LONG64 g_saveAt = 0;  // when SaveIfDue saves the zoom steps' camera (0: nothing to save)
static volatile LONG g_zoomed = -1;   // the camera the last zoom step changed
static const ULONGLONG kZoomSaveMs = 1500;

const char* ViewName(int v) { return v >= 0 && v < kViewCount ? kViewNames[v] : ""; }
int StyleCount() { return kStyleCount; }
const char* StyleName(int s) { return s >= 0 && s < kStyleCount ? kStyles[s].name : "Custom"; }

Tuning StyleTuning(int s) {
    Tuning t;
    if (s < 0 || s >= kStyleCount) return t;
    t.distMul = kStyles[s].distMul;
    t.posAdd[0] = kStyles[s].side;
    t.targetAdd[1] = kStyles[s].height;
    t.fovAdd = kStyles[s].fov;
    t.jump = kStyles[s].jump;
    return t;
}

float JumpForDistance(float distMul) {
    if (!(distMul > 0)) return 1.f;
    return std::min(1.f, std::max(0.f, roundf(distMul * 10.f) / 10.f));
}

// The idle camera's starting change: the game's own idle set against its walking set (distance 1.62 -> 1.30,
// look-at 1.2 -> 0.9 m, field of view 80 -> 73 degrees).
Tuning DefaultView(int v) {
    if (v != kIdle) return StyleTuning(kDefaultStyle[v < 0 || v >= kViewCount ? 0 : v]);
    Tuning t;
    t.distMul = 0.80f;
    t.targetAdd[1] = -0.30f;
    t.fovAdd = -7.f;
    return t;
}

// b on top of a: distances and the jump camera multiply, the rest add.
static Tuning Combine(const Tuning& a, const Tuning& b) {
    Tuning t;
    t.distMul = a.distMul * b.distMul;
    t.jump = a.jump * b.jump;
    t.fovAdd = a.fovAdd + b.fovAdd;
    for (int i = 0; i < 3; ++i) {
        t.posAdd[i] = a.posAdd[i] + b.posAdd[i];
        t.targetAdd[i] = a.targetAdd[i] + b.targetAdd[i];
    }
    return t;
}

static bool Near(float a, float b) { return fabsf(a - b) < 1e-3f; }
static bool SameTuning(const Tuning& a, const Tuning& b) {
    if (!Near(a.fovAdd, b.fovAdd) || !Near(a.distMul, b.distMul) || !Near(a.jump, b.jump)) return false;
    for (int i = 0; i < 3; ++i)
        if (!Near(a.posAdd[i], b.posAdd[i]) || !Near(a.targetAdd[i], b.targetAdd[i])) return false;
    return true;
}

// The camera whose values v uses: combat and indoor can be the exploration camera.
static int Source(int v) {
    if ((v == kCombat && g_combatFollows) || (v == kIndoor && g_indoorFollows)) return kExploration;
    return v;
}

int ViewStyle(int v) {  // -1 = custom, -2 = the exploration camera (combat, indoor)
    if (Source(v) != v) return -2;
    if (v == kIdle) return -1;
    const Tuning t = GetView(v);
    for (int s = 0; s < kStyleCount; ++s)
        if (SameTuning(t, StyleTuning(s))) return s;
    return -1;
}

Tuning GetView(int v) {
    if (v < 0 || v >= kViewCount) v = 0;
    AcquireSRWLockShared(&g_lock);
    const Tuning t = g_views[Source(v)];
    ReleaseSRWLockShared(&g_lock);
    return t;
}

void SetView(int v, const Tuning& t) {
    if (v < 0 || v >= kViewCount) return;
    AcquireSRWLockExclusive(&g_lock);
    g_views[v] = t;
    if (v == kCombat) g_combatFollows = false;
    if (v == kIndoor) g_indoorFollows = false;
    const Tuning& n = g_views[v];
    Log("%s camera: dist x%.3f side %+.2f posY %+.2f posZ %+.2f look-at %+.2f fov %+.1f jump %.0f%%", kViewNames[v],
        n.distMul, n.posAdd[0], n.posAdd[1], n.posAdd[2], n.targetAdd[1], n.fovAdd, n.jump * 100);
    ReleaseSRWLockExclusive(&g_lock);
}

bool CombatFollows() { return g_combatFollows; }
void SetCombatFollows(bool on) {
    AcquireSRWLockExclusive(&g_lock);
    g_combatFollows = on;
    if (on) g_views[kCombat] = g_views[kExploration];
    ReleaseSRWLockExclusive(&g_lock);
    Log("Combat camera: %s", on ? "same as exploration" : "its own");
}

bool IndoorFollows() { return g_indoorFollows; }
void SetIndoorFollows(bool on) {
    AcquireSRWLockExclusive(&g_lock);
    g_indoorFollows = on;
    if (on) g_views[kIndoor] = g_views[kExploration];
    ReleaseSRWLockExclusive(&g_lock);
    Log("Indoor camera: %s", on ? "same as exploration" : "its own");
}

void ReportCameraState(int state) {
    const bool indoor = state == kStateIndoor;
    if (indoor != g_indoor) Log("%s (camera state %d)", indoor ? "indoors" : "indoors over", state);
    g_indoor = indoor;
}

bool CameraOn() { return g_on; }
void SetCameraOn(bool on) {
    g_on = on;
    Log("CameraPlus %s", on ? "on" : "OFF (the game's camera)");
}

int ZoomKey() { return g_zoomKey; }
void SetZoomKey(int vk) {
    g_zoomKey = vk;
    g_zoomHeld = g_zoomLatched = false;
    Log("Zoom key: %s", KeyName(vk).c_str());
}
bool ZoomToggle() { return g_zoomToggle; }
void SetZoomToggle(bool on) {
    g_zoomToggle = on;
    g_zoomLatched = false;
}

void ZoomKeyEvent(bool down) {
    if (g_zoomToggle) {
        if (down && !g_zoomHeld) g_zoomLatched = !g_zoomLatched;
        g_zoomHeld = down;
    } else {
        g_zoomHeld = down;
    }
}
void ZoomRelease() { g_zoomHeld = false; }  // the game lost focus: keys released unseen

int ZoomButton() { return g_zoomPad; }
void SetZoomButton(int index) {
    if (index < 0 || index >= PadButtonCount()) return;
    g_zoomPad = index;
    g_zoomPadHeld = false;
    Log("Zoom button: %s", index ? PadButtonName(index).c_str() : "none");
}
void ZoomPadEvent(bool down) {
    if (g_zoomToggle && down && !g_zoomPadHeld) g_zoomLatched = !g_zoomLatched;
    g_zoomPadHeld = down;
}

bool WheelZoom() { return g_wheelZoom; }
void SetWheelZoom(bool on) {
    g_wheelZoom = on;
    Log("Wheel zoom %s", on ? "on" : "off");
}
bool TouchZoom() { return g_touchZoom; }
void SetTouchZoom(bool on) {
    g_touchZoom = on;
    Log("Touchpad zoom %s", on ? "on" : "off");
}

bool StepDistance(int step) {
    const int v = Source(LiveView());
    AcquireSRWLockExclusive(&g_lock);
    float& d = g_views[v].distMul;
    const float pct = std::min(300.f, std::max(20.f, roundf((d * 100 - step) / 5) * 5));  // on the panel's 5% grid
    const bool changed = fabsf(pct / 100 - d) > 1e-4f;
    if (changed) d = pct / 100;
    ReleaseSRWLockExclusive(&g_lock);
    if (!changed) return false;
    g_zoomed = v;
    InterlockedExchange64(&g_saveAt, (LONG64)(GetTickCount64() + kZoomSaveMs));
    if (g_cfg.diagnostics) Log("zoom step: %s camera distance %.0f%%", kViewNames[v], pct);
    return true;
}

void SaveIfDue(ULONGLONG now) {
    const LONG64 at = g_saveAt;
    if (!at || now < (ULONGLONG)at || InterlockedCompareExchange64(&g_saveAt, 0, at) != at) return;
    SaveViews();
    const int v = g_zoomed;
    if (v >= 0 && v < kViewCount) Log("%s camera distance %.0f%% (zoomed), saved", kViewNames[v], GetView(v).distMul * 100);
}

void ReportCombat(bool combat) {
    const ULONGLONG now = GetTickCount64();
    g_combatReport = now;
    if (combat) g_combatSeen = now;
    const bool was = g_combat;
    g_combat = combat || (g_combat && now - g_combatSeen < kCombatGraceMs);
    if (g_combat != was) Log("combat %s", g_combat ? "started" : "over");
}

void SetPreview(int v) { g_preview = v; }

bool IdleEnabled() { return g_idleOn; }
void SetIdleEnabled(bool on) {
    g_idleOn = on;
    Log("Idle zoom %s", on ? "on" : "off");
}
float IdleAfter() { return g_idleAfter; }
void SetIdleAfter(float s) { g_idleAfter = std::min(120.f, std::max(1.f, s)); }

// The game's idle timer grew by add this tick (0: something moved, it restarted).
void IdleTick(float add) {
    if (add > 0 && add < 1) g_idleSecs += add;
    else g_idleSecs = 0;
}

int LiveView() {
    if (g_preview >= 0) return g_preview;
    if (g_zoomToggle ? g_zoomLatched : (g_zoomHeld || g_zoomPadHeld)) return kZoom;
    // No report for a while (no HUD, a loading screen): not fighting.
    const ULONGLONG now = GetTickCount64();
    if (g_combat && now - g_combatReport > 3000 && now - g_combatSeen > kCombatGraceMs) g_combat = false;
    return g_combat ? kCombat : g_indoor ? kIndoor : kExploration;
}

bool LiveTarget(Tuning& out, float& tau) {
    const int v = LiveView();
    const bool idle = g_idleOn && (v == kExploration || v == kIndoor) && g_preview < 0 && g_idleSecs >= g_idleAfter;
    if (idle != g_idle) {
        if (g_cfg.diagnostics) Log("idle zoom %s", idle ? "in" : "out");
        g_idle = idle;
    }
    tau = v == kZoom || g_lastLive == kZoom ? 0.12f : 0.35f;  // zoom snaps, combat eases
    if (idle) tau = 2.5f;                                     // the idle zoom creeps in, like the game's
    if (g_preview >= 0) tau = 0.2f;
    if (v != g_lastLive) {
        if (g_cfg.diagnostics) Log("live camera: %s", kViewNames[v]);
        g_lastLive = v;
    }
    if (!g_on) {
        out = Tuning();
        return false;
    }
    AcquireSRWLockShared(&g_lock);
    if (v == kIdle) out = Combine(g_views[kExploration], g_views[kIdle]);
    else if (idle) out = Combine(g_views[Source(v)], g_views[kIdle]);
    else out = g_views[Source(v)];
    ReleaseSRWLockShared(&g_lock);
    return true;
}

// ---- key names ----
std::string KeyName(int vk) {
    switch (vk) {
        case 0: return "None";
        case VK_LBUTTON: return "Left mouse";
        case VK_RBUTTON: return "Right mouse";
        case VK_MBUTTON: return "Middle mouse";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
    }
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    switch (vk) {  // keys in the extended block need the extended bit to be named right
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_RCONTROL: case VK_RMENU:
            sc |= 0x100;
    }
    wchar_t buf[64] = {};
    if (sc && GetKeyNameTextW((LONG)(sc << 16), buf, 64) > 0) return Utf8(buf);
    char b[16];
    sprintf_s(b, "Key 0x%02X", vk);
    return b;
}

// ---- saving ----
// What the panel sets is saved in CameraPlus.state.ini, which isn't in the download, so extracting an update over
// the mod keeps it. Reading falls back to CameraPlus.ini (hand-made settings, and builds before 1.0.0 saved there).
static std::wstring StatePath() { return g_modDir + L"CameraPlus.state.ini"; }
static std::wstring IniPath() { return g_modDir + L"CameraPlus.ini"; }

static void ReadStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def, wchar_t* buf, DWORD n) {
    GetPrivateProfileStringW(sec, key, L"", buf, n, StatePath().c_str());
    if (!buf[0]) GetPrivateProfileStringW(sec, key, def, buf, n, IniPath().c_str());
}

static int ReadInt(const wchar_t* sec, const wchar_t* key, int def) {
    wchar_t buf[32];
    ReadStr(sec, key, L"", buf, 32);
    return buf[0] ? _wtoi(buf) : def;
}

static float ReadF(const wchar_t* sec, const wchar_t* key, float def, float lo, float hi) {
    wchar_t buf[64];
    ReadStr(sec, key, L"", buf, 64);
    if (!buf[0]) return def;
    wchar_t* end = nullptr;
    const float v = wcstof(buf, &end);
    if (end == buf || !(v == v)) return def;
    return std::min(hi, std::max(lo, v));
}

void LoadViews() {
    for (int v = 0; v < kViewCount; ++v) {
        const Tuning d = DefaultView(v);
        Tuning& t = g_views[v];
        const wchar_t* s = kSections[v];
        t.distMul = ReadF(s, L"DistanceMultiplier", d.distMul, 0.05f, 5);
        t.fovAdd = ReadF(s, L"FovAdd", d.fovAdd, -60, 60);
        t.posAdd[0] = ReadF(s, L"PositionX", d.posAdd[0], -5, 5);
        t.posAdd[1] = ReadF(s, L"PositionY", d.posAdd[1], -5, 5);
        t.posAdd[2] = ReadF(s, L"PositionZ", d.posAdd[2], -5, 5);
        t.targetAdd[0] = ReadF(s, L"TargetX", d.targetAdd[0], -5, 5);
        t.targetAdd[1] = ReadF(s, L"TargetY", d.targetAdd[1], -5, 5);
        t.targetAdd[2] = ReadF(s, L"TargetZ", d.targetAdd[2], -5, 5);
        // Cameras saved before 1.1.0 have no jump camera: the one that keeps the game's jump framing at their distance
        // (a saved style then still matches). Idle's is a change on top of exploration's.
        t.jump = ReadF(s, L"JumpCamera", v == kIdle ? d.jump : JumpForDistance(t.distMul), 0, 1);
    }
    g_combatFollows = ReadInt(L"Combat", L"SameAsExploration", 0) != 0;
    if (g_combatFollows) g_views[kCombat] = g_views[kExploration];
    g_indoorFollows = ReadInt(L"Indoor", L"SameAsExploration", 1) != 0;  // before 1.2.0 indoors was exploration
    if (g_indoorFollows) g_views[kIndoor] = g_views[kExploration];
    wchar_t buf[64];
    ReadStr(L"CameraPlus", L"ZoomKey", L"Mouse4", buf, 64);
    g_zoomKey = ParseKeyName(buf);
    ReadStr(L"CameraPlus", L"ZoomMode", L"Hold", buf, 64);
    g_zoomToggle = _wcsicmp(buf, L"Toggle") == 0;
    const int zb = ReadInt(L"CameraPlus", L"ZoomButton", 0);
    g_zoomPad = zb >= 0 && zb < PadButtonCount() ? zb : 0;
    g_wheelZoom = ReadInt(L"CameraPlus", L"WheelZoom", 1) != 0;
    g_touchZoom = ReadInt(L"CameraPlus", L"TouchpadZoom", 1) != 0;
    g_idleOn = ReadInt(L"Idle", L"Enabled", 1) != 0;
    SetIdleAfter(ReadF(L"Idle", L"After", 8.f, 1, 120));
}

void SaveViews() {
    static SRWLOCK saving = SRWLOCK_INIT;  // the panel (closing) and the pad thread (after a zoom) both save
    AcquireSRWLockExclusive(&saving);
    const std::wstring ini = StatePath();
    const wchar_t* const* sections = kSections;
    AcquireSRWLockShared(&g_lock);
    Tuning views[kViewCount];
    for (int v = 0; v < kViewCount; ++v) views[v] = g_views[v];
    const bool follows = g_combatFollows, indoorFollows = g_indoorFollows;
    ReleaseSRWLockShared(&g_lock);
    for (int v = 0; v < kViewCount; ++v) {
        const Tuning& t = views[v];
        auto put = [&](const wchar_t* key, float x) {
            wchar_t b[32];
            swprintf_s(b, L"%.3f", x);
            WritePrivateProfileStringW(sections[v], key, b, ini.c_str());
        };
        const int style = ViewStyle(v);
        if (v != kIdle)
            WritePrivateProfileStringW(sections[v], L"Style",
                                       style == -2 ? L"Same as exploration" : Wide(StyleName(style)).c_str(), ini.c_str());
        put(L"DistanceMultiplier", t.distMul);
        put(L"FovAdd", t.fovAdd);
        put(L"PositionX", t.posAdd[0]);
        put(L"PositionY", t.posAdd[1]);
        put(L"PositionZ", t.posAdd[2]);
        put(L"TargetX", t.targetAdd[0]);
        put(L"TargetY", t.targetAdd[1]);
        put(L"TargetZ", t.targetAdd[2]);
        put(L"JumpCamera", t.jump);
    }
    WritePrivateProfileStringW(L"Combat", L"SameAsExploration", follows ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Indoor", L"SameAsExploration", indoorFollows ? L"1" : L"0", ini.c_str());
    wchar_t b[16];
    swprintf_s(b, L"0x%02X", g_zoomKey);
    const wchar_t* name = g_zoomKey == VK_XBUTTON1 ? L"Mouse4" : g_zoomKey == VK_XBUTTON2 ? L"Mouse5"
                        : g_zoomKey == VK_MBUTTON ? L"MouseMiddle" : g_zoomKey == VK_RBUTTON ? L"MouseRight"
                        : g_zoomKey == 0 ? L"None" : b;
    WritePrivateProfileStringW(L"CameraPlus", L"ZoomKey", name, ini.c_str());
    WritePrivateProfileStringW(L"CameraPlus", L"ZoomMode", g_zoomToggle ? L"Toggle" : L"Hold", ini.c_str());
    swprintf_s(b, L"%d", g_zoomPad);
    WritePrivateProfileStringW(L"CameraPlus", L"ZoomButton", b, ini.c_str());
    WritePrivateProfileStringW(L"CameraPlus", L"WheelZoom", g_wheelZoom ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"CameraPlus", L"TouchpadZoom", g_touchZoom ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Idle", L"Enabled", g_idleOn ? L"1" : L"0", ini.c_str());
    swprintf_s(b, L"%.1f", g_idleAfter);
    WritePrivateProfileStringW(L"Idle", L"After", b, ini.c_str());
    ReleaseSRWLockExclusive(&saving);
}

void ResetViewsForTest() {
    for (int v = 0; v < kViewCount; ++v) g_views[v] = DefaultView(v);
    g_idleOn = true;
    g_idleAfter = 8.f;
    g_idleSecs = 0;
    g_idle = false;
    g_combatFollows = false;
    g_indoorFollows = true;
    g_indoor = false;
    g_on = true;
    g_zoomKey = VK_XBUTTON1;
    g_zoomToggle = g_zoomHeld = g_zoomLatched = false;
    g_zoomPad = 0;
    g_zoomPadHeld = false;
    g_wheelZoom = g_touchZoom = true;
    g_saveAt = 0;
    g_zoomed = -1;
    g_combat = false;
    g_preview = -1;
}

int DefaultStyle(int v) { return v >= 0 && v < kViewCount ? kDefaultStyle[v] : 0; }

}  // namespace cp
