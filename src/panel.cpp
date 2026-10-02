// The tuning panel. The panel key (F1) or the panel button (D-pad Left) opens it on the left of the screen; both are
// set on the MODS page. While it is open the keyboard or the controller drives it (up/down picks a row, left/right
// changes it) and the game doesn't see those presses. The mouse and the sticks still reach the game, so the camera
// can be turned (and Jesse walked) to look at a change as it happens.
//
// The panel edits one camera at a time (exploration, indoor, combat, zoom, idle) and shows that one live while open. Its
// rows depend on the camera (idle has an on/off and a time instead of a style). It also sets the zoom key and the
// zoom button: Enter (A) on their row, then the key, mouse button or controller button.
//
// The game reads the keyboard and mouse as raw input (WM_INPUT, GetRawInputData) in its window procedure. We
// subclass that window: while the panel is open we keep key presses from it (releases always go through, so a key
// held when the panel opens is released in the game as usual), and in play we watch the zoom key and the wheel.
// The wheel zooms (the live camera's distance), so in play the game's own GetRawInputData calls (through its import
// table) get the mouse without it: its default binding switches the controller's ability layer. The controller
// side is pad.cpp. CameraPlus.js draws the panel from PanelAction's status, reports the HUD's combat flag and passes
// on the MODS page's sliders.
//
// The keyboard (the window's thread), the controller (pad.cpp's thread) and the script (the UI thread) all reach
// the panel, so its state changes under one lock.
#include "common.h"
#include <cmath>
#include <stdio.h>

namespace cp {

static SRWLOCK g_lock = SRWLOCK_INIT;
static volatile bool g_open = false;
static volatile bool g_capture = false;  // waiting for the new zoom key / zoom button
enum { kCaptureKey, kCapturePad };
static int g_captureWhat = kCaptureKey;
static bool g_lastPad = false;           // the last press that drove the panel came from a controller (for the hints)
static int g_sel = 2;
static int g_edit = kExploration;
static volatile LONG g_serial = 0;  // bumps on every change, so the script redraws only then
static HWND g_hwnd = nullptr;
static WNDPROC g_prevProc = nullptr;
static bool g_sawRawKeyboard = false;
static bool g_panelKeyDown = false;

struct Locked {
    Locked() { AcquireSRWLockExclusive(&g_lock); }
    ~Locked() { ReleaseSRWLockExclusive(&g_lock); }
};

// ---- rows ----
enum Row { kOnOff, kEditing, kStyle, kDistance, kHeight, kSide, kPosY, kPosZ, kFov, kZoomKeyRow, kZoomModeRow,
           kResetView, kDump, kIdleOn, kIdleAfter, kZoomPadRow, kJump, kWheelRow, kTouchRow, kCombatDelay, kRowKinds };

// The rows shown for the camera being edited; g_sel is a place in this list.
static int Rows(int* out) {
    int n = 0;
    out[n++] = kOnOff;
    out[n++] = kEditing;
    if (g_edit == kIdle) {
        for (int r : {kIdleOn, kIdleAfter, kDistance, kHeight, kFov}) out[n++] = r;
    } else {
        for (int r : {kStyle, kDistance, kHeight, kSide, kPosY, kPosZ, kFov, kJump}) out[n++] = r;
    }
    for (int r : {kZoomKeyRow, kZoomPadRow, kZoomModeRow, kWheelRow, kTouchRow, kCombatDelay, kResetView}) out[n++] = r;
    if (g_cfg.diagnostics) out[n++] = kDump;
    return n;
}
static int RowCount() {
    int r[kRowKinds];
    return Rows(r);
}
static int RowAt(int i) {
    int r[kRowKinds];
    const int n = Rows(r);
    return i >= 0 && i < n ? r[i] : kOnOff;
}
static void Bump() { InterlockedIncrement(&g_serial); }

static float Clamp(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }
static float Snap(float v, float step) { return roundf(v / step) * step; }

// The style row's choices for a camera: combat and indoor can also be the exploration camera (-2).
static int StyleChoices(int v, int* out) {
    int n = 0;
    if (v == kCombat || v == kIndoor) out[n++] = -2;
    for (int s = 0; s < StyleCount(); ++s) out[n++] = s;
    return n;
}

// The order the Editing row goes through the cameras.
static const int kEditOrder[kViewCount] = {kExploration, kIndoor, kCombat, kZoom, kIdle};
static int NextEdit(int v, int dir) {
    int at = 0;
    for (int i = 0; i < kViewCount; ++i)
        if (kEditOrder[i] == v) at = i;
    return kEditOrder[(at + dir + kViewCount) % kViewCount];
}

// dir -1/+1 changes the row by a step (fine with Shift / LB); dir 0 puts it back to the game's value.
static void Change(int row, int dir, bool fine) {
    switch (row) {
        case kOnOff: SetCameraOn(dir == 0 ? true : !CameraOn()); return;
        case kEditing:
            g_edit = NextEdit(g_edit, dir);
            SetPreview(g_edit);
            return;
        case kStyle: {
            int choices[16];
            const int n = StyleChoices(g_edit, choices);
            const int cur = ViewStyle(g_edit);
            int at = -1;
            for (int i = 0; i < n; ++i)
                if (choices[i] == cur) at = i;
            const int next = dir == 0 ? (choices[0] == -2 ? 1 : 0)  // Delete: the game's camera
                                      : at < 0 ? (dir > 0 ? 0 : n - 1) : (at + dir + n) % n;
            const int s = choices[next];
            if (s == -2 && g_edit == kCombat) SetCombatFollows(true);
            else if (s == -2) SetIndoorFollows(true);
            else SetView(g_edit, StyleTuning(s));
            return;
        }
        case kZoomKeyRow:
        case kZoomPadRow:
            if (dir == 0) {
                if (row == kZoomKeyRow) SetZoomKey(VK_XBUTTON1);
                else SetZoomButton(0);
            } else {
                g_capture = true;
                g_captureWhat = row == kZoomKeyRow ? kCaptureKey : kCapturePad;
            }
            return;
        case kZoomModeRow: SetZoomToggle(dir == 0 ? false : !ZoomToggle()); return;
        case kWheelRow: SetWheelZoom(dir == 0 ? true : !WheelZoom()); return;
        case kTouchRow: SetTouchZoom(dir == 0 ? true : !TouchZoom()); return;
        case kIdleOn: SetIdleEnabled(dir == 0 ? true : !IdleEnabled()); return;
        case kIdleAfter: {
            const float step = fine ? 0.5f : 1.f;
            SetIdleAfter(dir ? Snap(IdleAfter() + dir * step, step) : 8.f);
            return;
        }
        case kCombatDelay: {
            const float step = fine ? 0.1f : 0.5f;
            SetCombatEndDelay(dir ? Snap(CombatEndDelay() + dir * step, step) : 3.f);
            return;
        }
        case kResetView:
            if (g_edit == kIndoor) SetIndoorFollows(true);  // its fresh-install camera: the exploration one
            else SetView(g_edit, DefaultView(g_edit));
            if (g_edit == kIdle) SetIdleAfter(8.f), SetIdleEnabled(true);
            if (g_edit == kCombat) SetCombatEndDelay(3.f);
            return;
        case kDump: RequestDump(); return;
    }
    Tuning t = GetView(g_edit);
    switch (row) {
        case kDistance: {  // shown in percent
            const float step = fine ? 1.f : 5.f;
            const float pct = dir ? Snap(t.distMul * 100 + dir * step, step) : 100.f;
            t.distMul = Clamp(pct, 20, 300) / 100;
            break;
        }
        case kHeight:
        case kSide:
        case kPosY:
        case kPosZ: {
            float& v = row == kHeight ? t.targetAdd[1] : t.posAdd[row == kSide ? 0 : row == kPosY ? 1 : 2];
            const float step = fine ? 0.01f : 0.05f;
            v = dir ? Clamp(Snap(v + dir * step, step), -2, 2) : 0.f;
            if (fabsf(v) < 1e-4f) v = 0;
            break;
        }
        case kFov: {
            const float step = fine ? 0.5f : 2.f;
            t.fovAdd = dir ? Clamp(Snap(t.fovAdd + dir * step, step), -40, 40) : 0.f;
            break;
        }
        case kJump: {  // shown in percent of the game's jump camera
            const float step = fine ? 5.f : 10.f;
            t.jump = dir ? Clamp(Snap(t.jump * 100 + dir * step, step), 0, 100) / 100 : 1.f;
            break;
        }
    }
    SetView(g_edit, t);
}

static std::string Value(int row) {
    char b[96];
    const Tuning t = GetView(g_edit);
    switch (row) {
        case kOnOff: return CameraOn() ? "On" : "Off (game camera)";
        case kEditing: return std::string(ViewName(g_edit)) + " camera";
        case kStyle: {
            const int s = ViewStyle(g_edit);
            return s == -2 ? "Same as exploration" : StyleName(s);
        }
        case kDistance: sprintf_s(b, "%.0f%%", t.distMul * 100); return b;
        case kHeight: sprintf_s(b, "%+.2f m", t.targetAdd[1]); return b;
        case kSide: sprintf_s(b, "%+.2f m", t.posAdd[0]); return b;
        case kPosY: sprintf_s(b, "%+.2f m", t.posAdd[1]); return b;
        case kPosZ: sprintf_s(b, "%+.2f m", t.posAdd[2]); return b;
        case kFov: sprintf_s(b, "%+.1f\xC2\xB0", t.fovAdd); return b;
        case kJump: sprintf_s(b, "%.0f%%", t.jump * 100); return b;
        case kZoomKeyRow:
            return g_capture && g_captureWhat == kCaptureKey ? "Press a key or mouse button" : KeyName(ZoomKey());
        case kZoomPadRow:
            if (g_capture && g_captureWhat == kCapturePad) return "Press a controller button";
            return ZoomButton() ? PadButtonName(ZoomButton()) : "None";
        case kZoomModeRow: return ZoomToggle() ? "Toggle" : "Hold";
        case kWheelRow: return !WheelBlockInstalled() ? "Not available" : WheelZoom() ? "On" : "Off";
        case kTouchRow: return TouchZoom() ? "On" : "Off";
        case kIdleOn: return IdleEnabled() ? "On" : "Off";
        case kIdleAfter: sprintf_s(b, "%.1f s", IdleAfter()); return b;
        case kCombatDelay: sprintf_s(b, "%.1f s", CombatEndDelay()); return b;
        case kResetView: return "Enter";
        case kDump: return "Enter";
    }
    return "";
}

static std::string Label(int row) {
    static const char* names[] = {"CameraPlus", "Editing", "Style", "Distance", "Look-at height", "Side offset",
                                  "Position Y", "Position Z", "Field of view", "Zoom key", "Zoom", "",
                                  "Write values to log", "Idle zoom", "Idle after", "Zoom button", "Jump camera",
                                  "Wheel zoom", "Touchpad zoom", "Combat end delay"};
    if (row == kResetView) return std::string("Reset ") + ViewName(g_edit) + " camera";
    if (row == kDistance && g_edit == kIdle) return "Distance (of exploration)";
    return row >= 0 && row < kRowKinds ? names[row] : "";
}

// ---- presses (under the lock) ----
bool PanelOpen() { return g_open; }
bool PanelCapturing() { return g_capture; }

static void SetOpen(bool open) {
    if (g_open == open) return;
    g_open = open;
    g_capture = false;
    SetPreview(open ? g_edit : -1);
    Bump();
    if (!open) SaveViews();  // keep what was tuned for the next start
    Log("panel %s", open ? "opened" : "closed");
}

// A press while the panel is open (keyboard keys, or the controller's as the keys they stand for).
static void Press(int vk, bool fine) {
    const int n = RowCount();
    switch (vk) {
        case VK_ESCAPE: SetOpen(false); break;
        case VK_UP: case 'W': g_sel = (g_sel + n - 1) % n; break;
        case VK_DOWN: case 'S': g_sel = (g_sel + 1) % n; break;
        case VK_LEFT: case 'A': Change(RowAt(g_sel), -1, fine); break;
        case VK_RIGHT: case 'D': Change(RowAt(g_sel), +1, fine); break;
        case VK_RETURN: case VK_SPACE: {
            const int r = RowAt(g_sel);
            if (r == kOnOff || r == kZoomKeyRow || r == kZoomPadRow || r == kZoomModeRow || r == kWheelRow ||
                r == kTouchRow || r == kResetView || r == kDump || r == kIdleOn)
                Change(r, +1, fine);
            break;
        }
        case VK_DELETE: case VK_BACK: Change(RowAt(g_sel), 0, fine); break;
        case 'I': SetKeyListShown(!KeyListShown()); break;  // the list of keys on the right
        default: break;  // any other key: kept from the game too while the panel is open
    }
    if (g_sel >= RowCount()) g_sel = RowCount() - 1;  // the idle camera has fewer rows
    Bump();
}

static void CaptureKey(int vk) {
    if (!g_capture) return;
    if (g_captureWhat == kCapturePad) {  // waiting for a controller button: Esc cancels, other keys don't count
        if (vk == VK_ESCAPE) g_capture = false;
        Bump();
        return;
    }
    g_capture = false;
    if (vk != VK_ESCAPE && vk != g_cfg.panelKey && vk != VK_LBUTTON) SetZoomKey(vk);
    Bump();
}

void PanelCapture(int vk) {
    Locked l;
    CaptureKey(vk);
}

void PanelCapturePad(int button, bool cancel) {
    Locked l;
    if (!g_capture) return;
    g_lastPad = true;
    if (cancel) {
        g_capture = false;
    } else if (g_captureWhat == kCapturePad) {
        g_capture = false;
        if (button != PanelButton()) SetZoomButton(button);  // the panel button can't also zoom
    }
    Bump();
}

bool PanelKey(int vk, bool shift) {
    Locked l;
    if (g_capture) {
        CaptureKey(vk);
        return true;
    }
    if (g_cfg.panelKey && vk == g_cfg.panelKey) {
        g_lastPad = false;
        SetOpen(!g_open);
        return true;
    }
    if (!g_open) return false;
    g_lastPad = false;
    Press(vk, shift);
    return true;
}

void PanelToggleFromPad() {
    Locked l;
    g_lastPad = true;
    SetOpen(!g_open);
}

void PanelPadKey(int vk, bool fine) {
    Locked l;
    if (!g_open) return;
    g_lastPad = true;
    Press(vk, fine);
}

void PanelZoomStep(int step) {
    Locked l;
    if (StepDistance(step)) Bump();  // the panel's Distance row, if it's open
}

// ---- the wheel ----
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static GetRawInputDataFn volatile g_gameGetRawInputData = nullptr;  // what the game's import slot held
static bool g_wheelBlock = false;
static int g_wheel = 0;  // turned, but not a whole notch yet (smooth wheels send less at a time); window thread
static const int kWheelStep = 10;  // distance percentage points a notch (5, the panel's step, felt too slow)

bool WheelBlockInstalled() { return g_wheelBlock; }
bool WheelZooming() { return g_wheelBlock && WheelZoom() && CameraOn() && !GamePaused(); }

void PanelWheel(int delta) {
    if ((g_wheel > 0 && delta < 0) || (g_wheel < 0 && delta > 0)) g_wheel = 0;  // turned back: from here
    g_wheel += delta;
    for (; g_wheel >= WHEEL_DELTA; g_wheel -= WHEEL_DELTA) PanelZoomStep(kWheelStep);
    for (; g_wheel <= -WHEEL_DELTA; g_wheel += WHEEL_DELTA) PanelZoomStep(-kWheelStep);
}

// The game's GetRawInputData: the same packets, but a mouse packet's wheel turn is taken out while it zooms (the
// rest of the packet, movement and buttons, is the game's as usual).
static UINT WINAPI GameGetRawInputData(HRAWINPUT h, UINT command, LPVOID data, PUINT size, UINT headerSize) {
    const UINT got = g_gameGetRawInputData(h, command, data, size, headerSize);
    if (data && command == RID_INPUT && got != (UINT)-1 && got >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)) {
        RAWINPUT* ri = (RAWINPUT*)data;
        if (ri->header.dwType == RIM_TYPEMOUSE && (ri->data.mouse.usButtonFlags & RI_MOUSE_WHEEL) && WheelZooming()) {
            ri->data.mouse.usButtonFlags &= ~RI_MOUSE_WHEEL;
            ri->data.mouse.usButtonData = 0;  // the turn (only the wheels use it)
        }
    }
    return got;
}

// The slot's function is called from here on (user32's, or another mod's hook on it).
bool InstallWheelBlockAt(void** slot, std::string& err) {
    void* was = *slot;
    if (!was) {
        err = "the import slot is empty";
        return false;
    }
    g_gameGetRawInputData = (GetRawInputDataFn)was;
    MemoryBarrier();
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        err = "could not write the import slot";
        return false;
    }
    InterlockedExchangePointer(slot, (void*)&GameGetRawInputData);
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), old, &tmp);
    g_wheelBlock = true;
    return true;
}

bool InstallWheelBlock(const Image& img, std::string& err) {
    const uint32_t slot = FindImportSlot(img, "user32.dll", "GetRawInputData");
    if (!slot) {
        err = "the game doesn't import GetRawInputData";
        return false;
    }
    void** live = (void**)(g_gameBase + slot);
    const void* user32 = (const void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetRawInputData");
    Log("GetRawInputData import slot at +0x%x%s", slot, *live == user32 ? "" : " (already changed by another module)");
    return InstallWheelBlockAt(live, err);
}

// ---- the window ----
static bool ShiftDown() { return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0; }

// A raw mouse event's change of button vk: +1 down, -1 up, 0 none.
static int MouseButton(const RAWMOUSE& m, int vk) {
    const USHORT f = m.usButtonFlags;
    switch (vk) {
        case VK_RBUTTON: return f & RI_MOUSE_RIGHT_BUTTON_DOWN ? 1 : f & RI_MOUSE_RIGHT_BUTTON_UP ? -1 : 0;
        case VK_MBUTTON: return f & RI_MOUSE_MIDDLE_BUTTON_DOWN ? 1 : f & RI_MOUSE_MIDDLE_BUTTON_UP ? -1 : 0;
        case VK_XBUTTON1: return f & RI_MOUSE_BUTTON_4_DOWN ? 1 : f & RI_MOUSE_BUTTON_4_UP ? -1 : 0;
        case VK_XBUTTON2: return f & RI_MOUSE_BUTTON_5_DOWN ? 1 : f & RI_MOUSE_BUTTON_5_UP ? -1 : 0;
    }
    return 0;
}

static LRESULT CALLBACK PanelWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INPUT: {
            RAWINPUT ri;
            UINT size = sizeof(ri);
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) break;
            if (ri.header.dwType == RIM_TYPEMOUSE) {
                if (g_capture) {  // the new zoom key can be a mouse button (not the left one)
                    for (int vk : {VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2})
                        if (MouseButton(ri.data.mouse, vk) > 0) {
                            PanelCapture(vk);
                            return DefWindowProcW(h, msg, wp, lp);
                        }
                    break;
                }
                const int pk = g_cfg.panelKey;  // the panel key can be a mouse button too (the MODS slider offers some)
                if (pk && MouseButton(ri.data.mouse, pk) > 0) {
                    PanelKey(pk, false);
                    return DefWindowProcW(h, msg, wp, lp);
                }
                const int zk = ZoomKey();
                if (!g_open && zk) {
                    const int b = MouseButton(ri.data.mouse, zk);
                    if (b) ZoomKeyEvent(b > 0);
                }
                if ((ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) && WheelZooming())
                    PanelWheel((SHORT)ri.data.mouse.usButtonData);
                break;  // the mouse always reaches the game (without the wheel while it zooms)
            }
            if (ri.header.dwType != RIM_TYPEKEYBOARD) break;
            g_sawRawKeyboard = true;
            const int vk = ri.data.keyboard.VKey;
            const bool up = (ri.data.keyboard.Flags & RI_KEY_BREAK) != 0;
            if (vk == g_cfg.panelKey && g_cfg.panelKey && !g_capture) {  // toggles once per press, never repeats
                const bool first = !up && !g_panelKeyDown;
                g_panelKeyDown = !up;
                if (first) PanelKey(vk, false);
                if (!up) return DefWindowProcW(h, msg, wp, lp);  // the game never sees it pressed
                break;
            }
            if (!g_open) {
                if (vk && vk == ZoomKey()) ZoomKeyEvent(!up);  // the zoom key still reaches the game
                break;
            }
            if (up) break;  // releases always reach the game
            PanelKey(vk, ShiftDown());
            return DefWindowProcW(h, msg, wp, lp);  // lets Windows clean up the input; the game doesn't read it
        }
        case WM_KEYDOWN:
        case WM_KEYUP: {
            // The same keys as legacy messages: handled here only if the game gets no raw keyboard input.
            const int vk = (int)wp;
            const bool down = msg == WM_KEYDOWN, repeat = (lp & (1 << 30)) != 0;
            if (!g_sawRawKeyboard) {
                if (!down) {
                    if (!g_open && vk == ZoomKey()) ZoomKeyEvent(false);
                    break;
                }
                if (vk == g_cfg.panelKey && g_cfg.panelKey) {
                    if (!repeat) PanelKey(vk, false);
                    return 0;
                }
                if (g_open) {
                    PanelKey(vk, ShiftDown());
                    return 0;
                }
                if (vk == ZoomKey() && !repeat) ZoomKeyEvent(true);
                break;
            }
            if (down && (g_open || (vk == g_cfg.panelKey && g_cfg.panelKey))) return 0;
            break;
        }
        case WM_CHAR:
            if (g_open) return 0;
            break;
        case WM_ACTIVATEAPP:
            if (!wp) ZoomRelease();
            break;
        case WM_KILLFOCUS:
            ZoomRelease();
            break;
        case WM_CLOSE:
        case WM_DESTROY:
            SaveIfDue(~0ull);  // a zoom step not saved yet (the game is quitting)
            SaveUsualCamera();
            break;
    }
    return CallWindowProcW(g_prevProc, h, msg, wp, lp);
}

struct FindCtx { DWORD pid; HWND best; LONG area; };
static BOOL CALLBACK FindMain(HWND h, LPARAM lp) {
    FindCtx* c = (FindCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r;
    if (!GetClientRect(h, &r)) return TRUE;
    const LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > c->area) c->best = h, c->area = area;
    return TRUE;
}

// Hooks the game's main window as soon as it exists, and again if the game makes a new one.
static DWORD WINAPI WatchWindow(void*) {
    for (;;) {
        FindCtx c{GetCurrentProcessId(), nullptr, 0};
        EnumWindows(FindMain, (LPARAM)&c);
        if (c.best && c.best != g_hwnd) {
            WNDPROC prev = (WNDPROC)GetWindowLongPtrW(c.best, GWLP_WNDPROC);
            if (prev && prev != PanelWndProc) {
                g_prevProc = prev;
                MemoryBarrier();
                if (SetWindowLongPtrW(c.best, GWLP_WNDPROC, (LONG_PTR)PanelWndProc)) {
                    wchar_t cls[128] = {};
                    GetClassNameW(c.best, cls, 128);
                    Log("Panel ready on window %p (%s): %s or %s opens it", c.best, Utf8(cls).c_str(),
                        Utf8(g_cfg.panelKeyName).c_str(), PanelButton() ? PadButtonName(PanelButton()).c_str() : "no button");
                    g_hwnd = c.best;
                }
            }
        }
        Sleep(g_hwnd ? 3000 : 500);
    }
}

void StartPanel() {
    HANDLE h = CreateThread(nullptr, 0, WatchWindow, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

// ---- the endpoint ----
// Every status read may carry the HUD's combat flag (c) and the MODS page's sliders (k: the panel key's place in
// KeyList(), b: the panel button's place in the controller list).
std::string PanelAction(const std::string& action, const char* query) {
    std::string c, k, b;
    if (QueryParam(query, "c", c)) ReportCombat(c == "1");
    if (QueryParam(query, "k", k) && !k.empty()) SetPanelKeyIndex(atoi(k.c_str()));
    if (QueryParam(query, "b", b) && !b.empty()) {
        const int i = atoi(b.c_str());
        if (i >= 0 && i < PadButtonCount()) SetPanelButton(i);
    }
    Locked l;
    std::string s = "{\"ok\":true,\"version\":\"" CP_VERSION "\",\"serial\":";
    s += std::to_string(g_serial);
    s += ",\"open\":";
    s += g_open ? "true" : "false";
    s += ",\"panelKey\":" + std::to_string(g_cfg.panelKeyIndex) + ",\"panelButton\":" + std::to_string(PanelButton());
    if (action != "status" || !g_open) return s + "}";
    s += ",\"sel\":" + std::to_string(g_sel) + ",\"key\":\"" + JsonEscape(Utf8(g_cfg.panelKeyName)) + "\"";
    s += ",\"button\":\"" + JsonEscape(PadButtonName(PanelButton())) + "\",\"pad\":" + (g_lastPad ? "true" : "false");
    s += ",\"edit\":\"" + JsonEscape(ViewName(g_edit)) + "\",\"capture\":" + (g_capture ? "true" : "false");
    // For the list of keys: what's bound, and what's on.
    s += ",\"keyList\":" + std::string(KeyListShown() ? "true" : "false");
    s += ",\"zoomKey\":\"" + JsonEscape(ZoomKey() ? KeyName(ZoomKey()) : "") + "\",\"zoomButton\":\"" +
         JsonEscape(PadButtonName(ZoomButton())) + "\",\"zoomToggle\":" + (ZoomToggle() ? "true" : "false");
    s += ",\"wheel\":" + std::string(WheelZoom() && WheelBlockInstalled() ? "true" : "false") +
         ",\"touch\":" + (TouchZoom() ? "true" : "false");
    s += ",\"rows\":[";
    int rows[kRowKinds];
    const int n = Rows(rows);
    for (int i = 0; i < n; ++i) {
        if (i) s += ",";
        s += "[\"" + JsonEscape(Label(rows[i])) + "\",\"" + JsonEscape(Value(rows[i])) + "\"]";
    }
    s += "]}";
    return s;
}

// The script's first values: its version, and the MODS sliders' names (keys and buttons by place) and positions.
std::string BuildConfigJs() {
    std::string js = "window.__CameraPlusConfig={version:\"" CP_VERSION "\",diagnostics:";
    js += g_cfg.diagnostics ? "1" : "0";
    js += ",hook:";
    js += CameraHookInstalled() ? "1" : "0";
    js += ",panelKey:" + std::to_string(g_cfg.panelKeyIndex) + ",panelButton:" + std::to_string(PanelButton());
    js += ",keys:[";
    for (size_t i = 0; i < KeyList().size(); ++i) js += (i ? ",\"" : "\"") + JsonEscape(KeyList()[i].name) + "\"";
    js += "],pad:[";
    for (int i = 0; i < PadButtonCount(); ++i) js += (i ? ",\"" : "\"") + JsonEscape(PadButtonName(i)) + "\"";
    js += "]};";
    return js;
}

}  // namespace cp
