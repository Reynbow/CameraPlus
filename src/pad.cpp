// Controllers. Two jobs:
//
// Reading: Xbox-style pads through XInput and DualSense, DualSense Edge and DualShock 4 pads from their HID input
// reports (FastTravelPlus's reader, so they work without Steam Input). Both are read-only and shared with the
// game. This drives the panel button (or two pressed together), the panel's own controls and the zoom button, and on
// PlayStation pads the touchpad zoom (a one-finger swipe up or down; the game only uses the touchpad's press).
//
// Keeping presses from the game: the game asks about a pad button through two small dispatchers, whatever the pad
// (0x142975160 "down", 0x1429751a0 "went down this frame"; each forwards to the XInput backend at +0x40 or the
// second backend at +0x48 of the pad device). We replace both. While the panel is open, and after it closes until
// every button is let go, they answer "not down". During play (the game not paused) the panel button and the zoom
// button always answer "not down": they're CameraPlus's; in the pause menu and the map they're the game's. Two panel
// buttons are the game's one at a time; while either is down the other answers "not down" (it opens the panel). The
// analog queries (sticks, triggers as axes) are left alone, so the sticks still move and look while the panel is
// open.
//
// Button positions are what the MODS page slider and the INI save, so the list only ever grows at the end.
#include "common.h"
#include <atomic>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

namespace cp {

static const char* kPadButtons[] = {"",           "A (Cross)",       "B (Circle)",       "X (Square)",
                                    "Y (Triangle)", "LB (L1)",         "RB (R1)",          "LT (L2)",
                                    "RT (R2)",     "View (Create)",   "Menu (Options)",   "LS (L3)",
                                    "RS (R3)",     "D-pad Up",        "D-pad Down",       "D-pad Left",
                                    "D-pad Right"};
// The game's code for each (its pad record's byte is code - 0xcb; LT and RT are the triggers past a threshold).
static const uint16_t kPadCodes[] = {0,    0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xdc, 0xda, 0xdd,
                                     0xe3, 0xe6, 0xdb, 0xde, 0xd1, 0xd2, 0xd3, 0xd4};
enum { kA = 1, kB, kX, kY, kLB, kRB, kLT, kRT, kView, kMenu, kLS, kRS, kUp, kDown, kLeft, kRight, kPadCount };
static inline uint32_t Bit(int button) { return 1u << button; }

int PadButtonCount() { return kPadCount; }
std::string PadButtonName(int index) { return index > 0 && index < kPadCount ? kPadButtons[index] : ""; }
uint16_t PadButtonCode(int index) { return index > 0 && index < kPadCount ? kPadCodes[index] : 0; }

static std::atomic<int> g_openButton{kDefaultPanelButton};
static std::atomic<int> g_openButton2{0};        // pressed together with it (0: the panel button alone)
static std::atomic<uint32_t> g_hidButtons{0};
static std::atomic<uint32_t> g_held{0};          // the buttons down at the last poll
static std::atomic<bool> g_guard{false};         // the panel closed with buttons down: keep them from the game

int PanelButton() { return g_openButton.load(); }
void SetPanelButton(int index) {
    if (index < 0 || index >= kPadCount) return;
    if (g_openButton.exchange(index) != index) Log("Panel button: %s", index ? kPadButtons[index] : "off");
}
int PanelButton2() { return g_openButton2.load(); }
void SetPanelButton2(int index) {
    if (index < 0 || index >= kPadCount) return;
    if (g_openButton2.exchange(index) != index) Log("Panel button 2: %s", index ? kPadButtons[index] : "none");
}

// Either one set alone is a single panel button; the same button twice is one.
int PanelButtonOne() {
    const int a = g_openButton.load();
    return a ? a : g_openButton2.load();
}
int PanelButtonTwo() {
    const int a = g_openButton.load(), b = g_openButton2.load();
    return a && b != a ? b : 0;
}
static uint32_t PanelMask() {  // the panel buttons as bits (0: none)
    const int one = PanelButtonOne(), two = PanelButtonTwo();
    return (one ? Bit(one) : 0) | (two ? Bit(two) : 0);
}
std::string PanelButtonsName() {
    const int one = PanelButtonOne(), two = PanelButtonTwo();
    if (!one) return "off";
    return two ? std::string(kPadButtons[one]) + " + " + kPadButtons[two] : kPadButtons[one];
}

// ---- the game's pause flag ----
// update_blending skips while byte +0x38 of a global game-state object is set (the pause menu, the map...): the
// call right after its prologue is that object's getter, "mov rax, [rip+x]; ret".
static uint8_t* const volatile* g_pausedSlot = nullptr;
static const uint8_t* g_testPaused = nullptr;

bool GamePaused() {
    if (g_testPaused) return *g_testPaused != 0;
    if (!g_pausedSlot) return false;
    const uint8_t* g = *g_pausedSlot;
    return g && g[0x38] != 0;
}

bool PauseKnown() { return g_testPaused || g_pausedSlot; }

bool FindPausedFlag(const Image& img, uint32_t blend, uint32_t& slotRva) {
    slotRva = 0;
    if (!blend || !img.Contains(blend + 0x24, 5) || img.mem[blend + 0x24] != 0xE8) return false;
    const uint32_t getter = blend + 0x29 + (uint32_t)img.I32(blend + 0x25);
    Pattern p;
    if (!p.Parse("48 8B 05 ?? ?? ?? ?? C3") || !MatchAt(img, getter, p)) return false;
    slotRva = getter + 7 + (uint32_t)img.I32(getter + 3);
    return img.Contains(slotRva, 8);
}

void UsePausedFlag(uint32_t slotRva) { g_pausedSlot = (uint8_t* const volatile*)(g_gameBase + slotRva); }
void SetPausedForTest(const uint8_t* flag) { g_testPaused = flag; }

// ---- keeping presses from the game ----
// Four dispatchers in a row (0x20 apart): "down", a stub, "went down", and the analog value.
static const char* kDispatchSig =
    "48 83 7A 40 00 0F 85 ?? ?? ?? ?? 48 83 7A 48 00 0F 85 ?? ?? ?? ?? 32 C0 C3 CC CC CC CC CC CC CC "
    "48 83 7A 40 00 0F 85 ?? ?? ?? ?? 48 83 7A 48 00 0F 85 ?? ?? ?? ?? 32 C0 C3 CC CC CC CC CC CC CC "
    "48 83 7A 40 00 0F 85 ?? ?? ?? ?? 48 83 7A 48 00 0F 85 ?? ?? ?? ?? 32 C0 C3 CC CC CC CC CC CC CC "
    "48 83 7A 40 00 0F 85 ?? ?? ?? ?? 48 83 7A 48 00 0F 85 ?? ?? ?? ?? C5 F8 57 C0 C3";

using PadQuery = bool (*)(void* sys, uint8_t* dev, uint64_t code, uint64_t flags);
static PadQuery g_downXInput = nullptr, g_downOther = nullptr, g_edgeXInput = nullptr, g_edgeOther = nullptr;
static bool g_blockInstalled = false;
bool PadBlockInstalled() { return g_blockInstalled; }

static bool Blocked(uint16_t code) {
    if (PanelOpen() || g_guard.load()) return true;
    if (GamePaused()) return false;
    const uint16_t zoom = PadButtonCode(ZoomButton());
    if (zoom && code == zoom) return true;
    const int one = PanelButtonOne(), two = PanelButtonTwo();
    if (!two) return one && code == PadButtonCode(one);
    const uint32_t held = g_held.load();
    if (code == PadButtonCode(one)) return (held & Bit(two)) != 0;
    if (code == PadButtonCode(two)) return (held & Bit(one)) != 0;
    return false;
}

static bool HookDown(void* sys, uint8_t* dev, uint64_t code, uint64_t flags) {
    if (Blocked((uint16_t)code)) return false;
    if (*(void* const*)(dev + 0x40)) return g_downXInput(sys, dev, code, flags);
    if (*(void* const*)(dev + 0x48)) return g_downOther(sys, dev, code, flags);
    return false;
}

static bool HookEdge(void* sys, uint8_t* dev, uint64_t code, uint64_t flags) {
    if (Blocked((uint16_t)code)) return false;
    if (*(void* const*)(dev + 0x40)) return g_edgeXInput(sys, dev, code, flags);
    if (*(void* const*)(dev + 0x48)) return g_edgeOther(sys, dev, code, flags);
    return false;
}

// A dispatcher's two backends, from its jne rel32 fields (at +7 and +18; the instructions end at +11 and +22).
static void Targets(const uint8_t* d, PadQuery& xinput, PadQuery& other) {
    int32_t a, b;
    memcpy(&a, d + 7, 4);
    memcpy(&b, d + 18, 4);
    xinput = (PadQuery)(d + 11 + a);
    other = (PadQuery)(d + 22 + b);
}

// Replaces a dispatcher outright (its bytes are relative jumps, so no trampoline): jmp [rip+0]; dq hook; nop.
static bool Replace(uint8_t* at, void* hook, const char* what, std::string& err) {
    uint8_t patch[15] = {0xFF, 0x25, 0, 0, 0, 0};
    const uint64_t dest = (uint64_t)hook;
    memcpy(patch + 6, &dest, 8);
    patch[14] = 0x90;
    if (!PatchCode(at, patch, sizeof(patch))) {
        err = std::string("could not patch ") + what;
        return false;
    }
    return true;
}

bool FindPadTargets(const Image& img, uint32_t& dispatch, std::string& err) {
    dispatch = FindUnique(img, "pad dispatchers", kDispatchSig, err);
    return dispatch != 0;
}

// dispatch: the "down" dispatcher; the "went down" one is 0x40 after it. Both must still be the game's bytes.
bool InstallPadBlockAt(uint8_t* dispatch, const uint8_t* original, std::string& err) {
    if (memcmp(dispatch, original, 25) != 0 || memcmp(dispatch + 0x40, original + 0x40, 25) != 0) {
        err = "pad dispatchers are already patched in memory (another mod hooks them)";
        return false;
    }
    Targets(dispatch, g_downXInput, g_downOther);
    Targets(dispatch + 0x40, g_edgeXInput, g_edgeOther);
    MemoryBarrier();
    if (!Replace(dispatch, (void*)&HookDown, "pad down", err) || !Replace(dispatch + 0x40, (void*)&HookEdge, "pad edge", err))
        return false;
    g_blockInstalled = true;
    return true;
}

bool InstallPadBlock(const Image& img, std::string& err) {
    uint32_t d = 0;
    if (!FindPadTargets(img, d, err)) return false;
    Log("pad dispatchers at +0x%x", d);
    return InstallPadBlockAt((uint8_t*)(g_gameBase + d), &img.mem[d], err);
}

// ---- XInput (Xbox-style pads, and PlayStation pads through Steam Input) ----
struct XPad {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XState {
    DWORD dwPacketNumber;
    XPad Gamepad;
};
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XState*);
static XInputGetStateFn g_xinputGetState = nullptr;

static void LoadXInput() {
    const wchar_t* dlls[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (const wchar_t* d : dlls) {
        HMODULE m = GetModuleHandleW(d);
        if (!m) m = LoadLibraryW(d);
        if (m && (g_xinputGetState = (XInputGetStateFn)GetProcAddress(m, "XInputGetState"))) return;
    }
}

static uint32_t FromXInput(const XPad& p) {
    const WORD w = p.wButtons;
    uint32_t b = 0;
    if (w & 0x1000) b |= Bit(kA);
    if (w & 0x2000) b |= Bit(kB);
    if (w & 0x4000) b |= Bit(kX);
    if (w & 0x8000) b |= Bit(kY);
    if (w & 0x0100) b |= Bit(kLB);
    if (w & 0x0200) b |= Bit(kRB);
    if (p.bLeftTrigger > 64) b |= Bit(kLT);
    if (p.bRightTrigger > 64) b |= Bit(kRT);
    if (w & 0x0020) b |= Bit(kView);
    if (w & 0x0010) b |= Bit(kMenu);
    if (w & 0x0040) b |= Bit(kLS);
    if (w & 0x0080) b |= Bit(kRS);
    if (w & 0x0001) b |= Bit(kUp);
    if (w & 0x0002) b |= Bit(kDown);
    if (w & 0x0004) b |= Bit(kLeft);
    if (w & 0x0008) b |= Bit(kRight);
    return b;
}

// ---- PlayStation pads over HID ----
// Both families share the button bytes: b0 = D-pad hat (low nibble, 8 = none) + Square, Cross, Circle,
// Triangle (bits 4-7); b1 = L1, R1, L2, R2, Share/Create, Options, L3, R3 (bits 0-7).
static uint32_t FromSony(uint8_t b0, uint8_t b1) {
    uint32_t b = 0;
    if (b0 & 0x20) b |= Bit(kA);
    if (b0 & 0x40) b |= Bit(kB);
    if (b0 & 0x10) b |= Bit(kX);
    if (b0 & 0x80) b |= Bit(kY);
    if (b1 & 0x01) b |= Bit(kLB);
    if (b1 & 0x02) b |= Bit(kRB);
    if (b1 & 0x04) b |= Bit(kLT);
    if (b1 & 0x08) b |= Bit(kRT);
    if (b1 & 0x10) b |= Bit(kView);
    if (b1 & 0x20) b |= Bit(kMenu);
    if (b1 & 0x40) b |= Bit(kLS);
    if (b1 & 0x80) b |= Bit(kRS);
    switch (b0 & 0x0f) {
        case 0: b |= Bit(kUp); break;
        case 1: b |= Bit(kUp) | Bit(kRight); break;
        case 2: b |= Bit(kRight); break;
        case 3: b |= Bit(kDown) | Bit(kRight); break;
        case 4: b |= Bit(kDown); break;
        case 5: b |= Bit(kDown) | Bit(kLeft); break;
        case 6: b |= Bit(kLeft); break;
        case 7: b |= Bit(kUp) | Bit(kLeft); break;
        default: break;
    }
    return b;
}

static bool IsDualSense(uint16_t pid) { return pid == 0x0CE6 || pid == 0x0DF2; }
static bool IsSonyPad(uint16_t vid, uint16_t pid) {
    return vid == 0x054C && (IsDualSense(pid) || pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0);
}

// One input report. reportLength is the device's input report size: 64 over USB, larger over Bluetooth. The
// touchpad's first finger is 25 bytes after the buttons on a DualSense and 30 on a DualShock 4 (Bluetooth's short
// reports have none): a byte whose top bit is set while no finger is down (the rest counts the touches), then x
// and y in 12 bits each. The touchpad is 1920 x 1080 on a DualSense, 1920 x 942 on a DualShock 4; its press is bit
// 1 of the byte after the buttons.
bool ParseSonyReport(uint16_t pid, size_t reportLength, const uint8_t* d, size_t n, SonyReport& out) {
    out = SonyReport();
    if (n < 10) return false;
    size_t at = 0;     // offset of the two button bytes
    size_t touch = 0;  // offset of the first finger (0: none in this report)
    int height = 1080;
    if (IsDualSense(pid)) {
        if (d[0] == 0x01 && reportLength == 64 && n >= 11) at = 8, touch = at + 25;  // USB
        else if (d[0] == 0x31 && n >= 12) at = 9, touch = at + 25;                   // Bluetooth, full reports
        else if (d[0] == 0x01) at = 5;                                               // Bluetooth, simple reports
    } else {
        height = 942;
        if (d[0] == 0x01) at = 5, touch = reportLength == 64 ? at + 30 : 0;          // USB, and Bluetooth simple reports
        else if (d[0] == 0x11 && n >= 10) at = 7, touch = at + 30;                   // Bluetooth, full reports
    }
    if (!at) return false;
    out.buttons = FromSony(d[at], d[at + 1]);
    if (touch && n >= touch + 4) {
        const uint8_t* f = d + touch;
        out.hasTouch = true;
        out.touching = !(f[0] & 0x80);
        out.touchId = f[0] & 0x7f;
        out.touchY = (f[2] >> 4) | (f[3] << 4);
        out.touchHeight = height;
        out.touchClick = (d[at + 2] & 0x02) != 0;
    }
    return true;
}

// Every sixteenth of the touchpad's height the finger moves up is a step closer, down a step further. A touch that
// presses the touchpad in (the game's map) never zooms.
int TouchSteps(TouchTrack& t, const SonyReport& r) {
    if (!r.hasTouch || !r.touching) {
        t.down = false;
        return 0;
    }
    if (!t.down || r.touchId != t.id) {  // a new touch
        t.down = true;
        t.id = r.touchId;
        t.anchor = r.touchY;
        t.click = r.touchClick;
        return 0;
    }
    t.click = t.click || r.touchClick;
    if (t.click) return 0;
    const int step = std::max(1, r.touchHeight / 16);
    int steps = 0;
    for (; t.anchor - r.touchY >= step; t.anchor -= step) ++steps;
    for (; r.touchY - t.anchor >= step; t.anchor += step) --steps;
    return steps;
}

static bool GameHasFocus();

// The touchpad zooms in play, like the wheel (the touch is still followed in menus, so a swipe that goes on after
// one closes doesn't jump). A whole swipe up or down the touchpad is 80% of the distance (half that felt too slow).
static const int kTouchStep = 5;  // distance percentage points a step
static void TouchZoomSteps(int steps) {
    if (!steps || !TouchZoom() || !CameraOn() || GamePaused() || !GameHasFocus()) return;
    for (; steps > 0; --steps) PanelZoomStep(kTouchStep);
    for (; steps < 0; ++steps) PanelZoomStep(-kTouchStep);
}

struct HidPad {
    HANDLE h = INVALID_HANDLE_VALUE;
    HANDLE ev = nullptr;
    OVERLAPPED ov{};
    std::vector<uint8_t> buf;
    uint16_t pid = 0;
    bool pending = false;
    uint32_t buttons = 0;
    TouchTrack touch;
    std::wstring path;
};

static void ClosePad(HidPad* p) {
    if (p->pending) {
        CancelIoEx(p->h, &p->ov);
        DWORD got = 0;
        GetOverlappedResult(p->h, &p->ov, &got, TRUE);
    }
    if (p->h != INVALID_HANDLE_VALUE) CloseHandle(p->h);
    if (p->ev) CloseHandle(p->ev);
    delete p;
}

// Opens PlayStation pads that aren't open yet. Every device is opened shared (the game and Steam keep theirs).
static void ScanPads(std::vector<HidPad*>& pads) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA itf{sizeof(itf)};
    for (DWORD i = 0; pads.size() < 4 && SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &itf); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &itf, nullptr, 0, &need, nullptr);
        if (!need || need > 4096) continue;
        std::vector<uint8_t> raw(need);
        auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)raw.data();
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &itf, detail, need, nullptr, nullptr)) continue;
        std::wstring path = detail->DevicePath;
        bool open = false;
        for (HidPad* p : pads) open = open || _wcsicmp(p->path.c_str(), path.c_str()) == 0;
        if (open) continue;
        // Attributes first with no access rights, which works for any HID device.
        HANDLE probe = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (probe == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr{sizeof(attr)};
        const bool sony = HidD_GetAttributes(probe, &attr) && IsSonyPad(attr.VendorID, attr.ProductID);
        CloseHandle(probe);
        if (!sony) continue;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        PHIDP_PREPARSED_DATA pre = nullptr;
        HIDP_CAPS caps{};
        const bool ok = HidD_GetPreparsedData(h, &pre) && HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS &&
                        caps.InputReportByteLength >= 10 && caps.InputReportByteLength <= 1024;
        if (pre) HidD_FreePreparsedData(pre);
        if (!ok) {
            CloseHandle(h);
            continue;
        }
        HidD_SetNumInputBuffers(h, 4);  // we read the latest few reports, not a long backlog
        HidPad* p = new HidPad();
        p->h = h;
        p->ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        p->buf.assign(caps.InputReportByteLength, 0);
        p->pid = attr.ProductID;
        p->path = path;
        pads.push_back(p);
        Log("Controller: PlayStation pad 054C:%04X (%u-byte reports)", attr.ProductID, caps.InputReportByteLength);
    }
    SetupDiDestroyDeviceInfoList(set);
}

static DWORD WINAPI HidThread(void*) {
    Sleep(15000);  // let the game open its pads first
    std::vector<HidPad*> pads;
    ULONGLONG nextScan = 0;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= nextScan) {
            ScanPads(pads);
            nextScan = now + (pads.empty() ? 3000 : 10000);
        }
        if (pads.empty()) {
            g_hidButtons = 0;
            Sleep(200);
            continue;
        }
        HANDLE events[4];
        DWORD count = 0;
        for (size_t i = 0; i < pads.size(); ++i) {
            HidPad* p = pads[i];
            if (!p->pending) {
                ResetEvent(p->ev);
                p->ov = OVERLAPPED{};
                p->ov.hEvent = p->ev;
                if (ReadFile(p->h, p->buf.data(), (DWORD)p->buf.size(), nullptr, &p->ov) ||
                    GetLastError() == ERROR_IO_PENDING)
                    p->pending = true;
                else
                    p->buttons = 0xFFFFFFFF;  // marks it for closing below
            }
            events[count++] = p->ev;
        }
        WaitForMultipleObjects(count, events, FALSE, 100);
        for (size_t i = 0; i < pads.size();) {
            HidPad* p = pads[i];
            DWORD got = 0;
            bool gone = p->buttons == 0xFFFFFFFF;
            if (!gone && p->pending && GetOverlappedResult(p->h, &p->ov, &got, FALSE)) {
                p->pending = false;
                SonyReport r;
                if (ParseSonyReport(p->pid, p->buf.size(), p->buf.data(), got, r)) {
                    p->buttons = r.buttons;
                    TouchZoomSteps(TouchSteps(p->touch, r));
                }
            } else if (!gone && p->pending && GetLastError() != ERROR_IO_INCOMPLETE) {
                p->pending = false;
                gone = true;  // unplugged or switched off
            }
            if (gone) {
                Log("Controller: PlayStation pad 054C:%04X gone", p->pid);
                ClosePad(p);
                pads.erase(pads.begin() + i);
                continue;
            }
            ++i;
        }
        uint32_t all = 0;
        for (HidPad* p : pads) all |= p->buttons;
        g_hidButtons = all;
        Sleep(4);
    }
}

static bool GameHasFocus() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// ---- the panel's controller ----
// D-pad up/down picks a row and left/right changes it (held: repeats; with LB held: finer steps), A selects, X puts
// the row back to the game's value, Y hides or shows the list of buttons, B closes. The panel button opens it during play; pressed again it closes the
// panel, unless it's one of those controls. Two panel buttons do it pressed together, in either order (the one
// pressed second does it). While the panel waits for a zoom button, the next button is it (B cancels).
struct Repeat { ULONGLONG since = 0, last = 0; };
static const uint32_t kPanelControls = Bit(kUp) | Bit(kDown) | Bit(kLeft) | Bit(kRight) | Bit(kA) | Bit(kX) | Bit(kY) |
                                       Bit(kB) | Bit(kLB);

// The buttons held as the panel opened: ignored until they're let go. The panel button is D-pad Left by default, which
// also changes the selected row's value; still held on the next poll, its repeat (timed from a stale start) changed the
// value at once.
static uint32_t g_swallow = 0;
static bool g_wasOpen = false;

static void PanelTick(uint32_t buttons, uint32_t prev, ULONGLONG now, Repeat* rep) {
    uint32_t edge = buttons & ~prev;
    const uint32_t open = PanelMask();
    const bool paused = GamePaused();
    // The panel button(s): all down, one of them just pressed.
    auto pressed = [open](uint32_t down, uint32_t went) { return open && (down & open) == open && (went & open); };
    if (!PanelOpen()) {
        g_wasOpen = false;
        if (pressed(buttons, edge) && !paused) PanelToggleFromPad();
        if (!PanelOpen()) return;
    }
    if (!g_wasOpen) {  // just opened (by this button, or the keyboard): what's held now waits for a new press
        g_wasOpen = true;
        g_swallow = buttons;
        for (int i = 0; i < 4; ++i) rep[i] = Repeat();
        return;
    }
    const uint32_t down = buttons;  // with what's still held from the opening (a held panel button counts to close)
    g_swallow &= buttons;  // let go: counts again
    buttons &= ~g_swallow;
    edge &= ~g_swallow;
    if (PanelCapturing()) {  // a zoom button: the first new press (this poll's edges came after the capture began)
        for (int b = 1; b < kPadCount; ++b)
            if (edge & Bit(b)) {
                PanelCapturePad(b == kB ? 0 : b, b == kB);
                return;
            }
        return;
    }
    const bool fine = (buttons & Bit(kLB)) != 0;
    const int nav[4] = {kUp, kDown, kLeft, kRight};
    const int vk[4] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT};
    for (int i = 0; i < 4; ++i) {
        const uint32_t bit = Bit(nav[i]);
        if (!(buttons & bit)) {
            rep[i] = Repeat();
            continue;
        }
        bool fire = false;
        if (edge & bit) {
            rep[i].since = rep[i].last = now;
            fire = true;
        } else if (now - rep[i].since >= 350 && now - rep[i].last >= 90) {
            rep[i].last = now;
            fire = true;
        }
        if (fire) PanelPadKey(vk[i], fine);
    }
    if (edge & Bit(kA)) PanelPadKey(VK_RETURN, fine);
    if (edge & Bit(kX)) PanelPadKey(VK_DELETE, fine);
    if (edge & Bit(kY)) PanelPadKey('I', fine);  // the list of buttons on the right
    if (edge & Bit(kB)) PanelPadKey(VK_ESCAPE, fine);
    if (!(open & kPanelControls) && pressed(down, edge) && PanelOpen()) PanelToggleFromPad();
}

static DWORD WINAPI PadThread(void*) {
    bool connected[4] = {};
    ULONGLONG nextProbe[4] = {};
    uint32_t prev = 0;
    bool wasPaused = false, wasZoom = false;
    Repeat rep[4];
    for (;;) {
        Sleep(8);
        const ULONGLONG now = GetTickCount64();
        uint32_t buttons = g_hidButtons.load();
        for (DWORD i = 0; g_xinputGetState && i < 4; ++i) {
            // Asking an empty slot is slow; do that every 2 s only.
            if (!connected[i] && now < nextProbe[i]) continue;
            XState st{};
            connected[i] = g_xinputGetState(i, &st) == ERROR_SUCCESS;
            if (!connected[i]) {
                nextProbe[i] = now + 2000;
                continue;
            }
            buttons |= FromXInput(st.Gamepad);
        }
        if (!GameHasFocus()) buttons = 0;
        PadPoll(buttons, prev, now, rep, wasZoom);
        prev = buttons;
        const bool paused = GamePaused();
        if (paused != wasPaused && g_cfg.diagnostics) Log("game %s", paused ? "paused" : "running");
        wasPaused = paused;
        // The wheel and touchpad zoom change a camera; this thread saves it once the steps stop (the window and
        // the game shouldn't wait on the file).
        SaveIfDue(now);
    }
}

// One poll's work (the tests call it with made-up buttons).
void PadPoll(uint32_t buttons, uint32_t prev, ULONGLONG now, void* repeat, bool& wasZoom) {
    g_held = buttons;
    PanelTick(buttons, prev, now, (Repeat*)repeat);
    // After the panel closes, the buttons still down stay the game's-blind until they're all let go.
    if (PanelOpen()) g_guard = true;
    else if (g_guard.load() && buttons == 0) g_guard = false;
    const int zoom = ZoomButton();
    const bool zoomDown = zoom && !PanelOpen() && !GamePaused() && (buttons & Bit(zoom));
    if (zoomDown != wasZoom) ZoomPadEvent(zoomDown);
    wasZoom = zoomDown;
}

size_t PadRepeatStateSize() { return sizeof(Repeat) * 4; }

void StartPad() {
    LoadXInput();
    HANDLE h = CreateThread(nullptr, 0, PadThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    h = CreateThread(nullptr, 0, HidThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    Log("Controller: panel button %s, zoom button %s (XInput %s)", PanelButtonsName().c_str(),
        ZoomButton() ? kPadButtons[ZoomButton()] : "none", g_xinputGetState ? "ready" : "missing");
}

}  // namespace cp
