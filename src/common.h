// CameraPlus - a closer, tunable third-person camera for CONTROL Resonant.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <algorithm>
#include <string>
#include <vector>
#include "version.h"

namespace cp {

struct Image;

// ---- globals (dllmain.cpp) ----
extern HMODULE g_self;
extern std::wstring g_modDir;   // folder holding cameraplus.dll, trailing backslash
extern uintptr_t g_gameBase;    // live base of CONTROLResonant.exe
extern bool g_knownBuild;       // the build the offsets were checked on (25600401)

// ---- logging (util.cpp) ----
void LogInit();
void Log(const char* fmt, ...);

// ---- small helpers (util.cpp) ----
std::string Utf8(const std::wstring& w);
std::wstring Wide(const std::string& s);
bool ReadWholeFile(const std::wstring& path, std::string& out, size_t maxBytes);
std::string UrlDecode(const char* s, size_t n);
// Value of key in a query string ("a=1&b=2"), URL-decoded. Returns false when absent.
bool QueryParam(const char* query, const char* key, std::string& out);
std::string JsonEscape(const std::string& s);

// ---- configuration (config.cpp) ----
// One camera's changes to the game's own settings. Positions are in the camera set's space (metres).
struct Tuning {
    float fovAdd = 0;        // degrees added to the field of view
    float distMul = 1;       // multiplies the set's distance curve (cameraOffsetDistanceCurveMultiplier)
    float posAdd[3] = {};    // added to the default, safe and fallback camera positions
    float targetAdd[3] = {}; // added to the target offset
    float jump = 1;          // how much of the game's jump camera to keep (1 = all, 0 = the camera from the ground)
    bool Identity() const;
};
struct Config {
    bool enabled = true;
    bool diagnostics = false;  // extra log lines (set switches, periodic snapshots)
    int panelKey = VK_F1;      // opens and closes the tuning panel (0 = none)
    std::wstring panelKeyName = L"F1";
    int panelKeyIndex = 2;     // its place in KeyList() (the MODS slider), -1 if the INI named another key
};
extern Config g_cfg;
void LoadConfig();  // also loads the cameras (LoadViews)
// The panel key as the MODS page's slider picks it (a place in KeyList()); false if out of range.
bool SetPanelKeyIndex(int index);

// ---- the keys the MODS slider offers (keys.cpp) ----
struct KeyChoice {
    int vk;
    std::string name;  // as the player's keyboard names it
};
const int kDefaultPanelKey = 2;  // F1
void BuildKeyList();
const std::vector<KeyChoice>& KeyList();
int KeyListIndex(int vk);  // -1 if not in the list

// ---- controllers (pad.cpp) ----
// Buttons by place in the list (0 = none; the MODS slider and the INI save places): A, B, X, Y, LB, RB, LT, RT,
// View, Menu, LS, RS, D-pad Up, Down, Left, Right.
const int kDefaultPanelButton = 15;  // D-pad Left
int PadButtonCount();
std::string PadButtonName(int index);  // "" for 0
uint16_t PadButtonCode(int index);     // the game's code for it (0 for none)
int PanelButton();
void SetPanelButton(int index);
bool GamePaused();                     // the game's pause flag (the pause menu, the map...)
bool PauseKnown();                     // the pause flag was found (else GamePaused() is always false)
bool FindPausedFlag(const Image& img, uint32_t blend, uint32_t& slotRva);
void UsePausedFlag(uint32_t slotRva);
void SetPausedForTest(const uint8_t* flag);
bool FindPadTargets(const Image& img, uint32_t& dispatch, std::string& err);
bool InstallPadBlock(const Image& img, std::string& err);
bool InstallPadBlockAt(uint8_t* dispatch, const uint8_t* original, std::string& err);  // on stand-ins (tests)
bool PadBlockInstalled();
void PadPoll(uint32_t buttons, uint32_t prev, ULONGLONG now, void* repeat, bool& wasZoom);  // one poll (tests)
size_t PadRepeatStateSize();
void StartPad();
// A PlayStation pad's HID input report: its buttons and the touchpad's first finger.
struct SonyReport {
    uint32_t buttons = 0;
    bool hasTouch = false;  // the report has the touchpad (Bluetooth's short reports don't)
    bool touching = false;  // a finger is on it
    uint8_t touchId = 0;    // counts up with every new touch
    int touchY = 0;         // 0 at the top edge
    int touchHeight = 1080; // the touchpad's height in the same units
    bool touchClick = false;  // the touchpad is pressed in (the game's map and quest log button)
};
bool ParseSonyReport(uint16_t pid, size_t reportLength, const uint8_t* d, size_t n, SonyReport& out);
// A one-finger swipe up or down the touchpad, as zoom steps (+1 closer, -1 further).
struct TouchTrack {
    bool down = false, click = false;
    uint8_t id = 0;
    int anchor = 0;  // where the finger was at the last step
};
int TouchSteps(TouchTrack& t, const SonyReport& r);
int ParseKeyName(const std::wstring& name);  // "F1", "Insert", "K", "0x70"...; 0 = none/unknown

// ---- the cameras (settings.cpp) ----
// Idle: a change on top of exploration (or indoor). Indoor: the game's indoor camera zones (camera state 1).
enum View { kExploration, kCombat, kZoom, kIdle, kIndoor, kViewCount };
const char* ViewName(int v);
int StyleCount();
const char* StyleName(int s);        // out of range: "Custom"
Tuning StyleTuning(int s);
int DefaultStyle(int v);             // a fresh install's style for each camera (-1: idle, no style)
Tuning DefaultView(int v);           // a fresh install's values for each camera
int ViewStyle(int v);                // the style a camera's values match; -1 custom, -2 combat = exploration
Tuning GetView(int v);
void SetView(int v, const Tuning& t);
bool CombatFollows();
void SetCombatFollows(bool on);
bool IndoorFollows();                // the indoor camera is the exploration camera (the default)
void SetIndoorFollows(bool on);
// The game's camera state (OutputData+1), which camera zones in the levels set; 1 is the indoor one (a closer,
// narrower set), 2 a wide one (big rooms, fights), 0 the rest.
const int kStateIndoor = 1;
// The player mode select_set matched (OutputData+8): flags of the player's status (player_status::Status, set by
// update_player_mode): 1 Adventure (exploring), 2 Combat (and the camera tables' fallback), 4 Story, 8 Flashback,
// 0x10 GameplayConversation... Since the game's 1 October update its tighter indoor camera mostly comes from a mode
// other than exploring or combat (the mode's own camera sets), not from the camera state.
inline bool SpecialPlayerMode(int m) { return m != 0 && (m & 3) == 0; }
void ReportCameraState(int state, int playerMode);  // indoors: the indoor state, or a special player mode
bool CameraOn();
void SetCameraOn(bool on);
int ZoomKey();
void SetZoomKey(int vk);
bool ZoomToggle();
void SetZoomToggle(bool on);
void ZoomKeyEvent(bool down);        // the zoom key went down / up in play
int ZoomButton();                    // the controller's zoom button (a place in the pad list, 0 = none)
void SetZoomButton(int index);
void ZoomPadEvent(bool down);        // the zoom button went down / up in play
void ZoomRelease();
bool WheelZoom();                    // the mouse wheel zooms in play
void SetWheelZoom(bool on);
bool TouchZoom();                    // a swipe on a PlayStation pad's touchpad zooms in play
void SetTouchZoom(bool on);
bool KeyListShown();                 // the list of keys on the right while the panel is open (I / Y hides it)
void SetKeyListShown(bool on);
// A wheel notch or touchpad step: the live camera's distance (the camera the panel shows while it's open) step
// percentage points closer (+) or further (-), on the panel's 5% grid. False if it's already at the end. Saved a
// moment later (SaveIfDue).
bool StepDistance(int step);
void SaveIfDue(ULONGLONG now);       // saves the cameras once the zoom steps have stopped for a moment
void ReportCombat(bool combat);      // the HUD's combat flag, from the script
float CombatEndDelay();              // seconds the combat camera stays after a fight (0: it switches at once)
void SetCombatEndDelay(float s);
bool InCombat();                     // the HUD says we're fighting
bool CombatSettling();               // the fight is over, the combat end delay isn't
void SetPreview(int v);              // the panel shows camera v live (-1: none)
bool IdleEnabled();
void SetIdleEnabled(bool on);
float IdleAfter();
void SetIdleAfter(float s);
void IdleTick(float add);            // the game's idle timer grew by add this tick (0: restarted)
int LiveView();
bool LiveTarget(Tuning& out, float& tau);  // the live camera's tuning and how fast to ease to it (s)
// The jump camera that keeps the game's jump framing at a distance multiplier: its drop and lower aim are distances
// in the world, so a camera twice as close keeps half of them (distMul, in 10% steps, at most all of it).
float JumpForDistance(float distMul);
std::string KeyName(int vk);
void LoadViews();
void SaveViews();
void SaveUsualCamera();  // the walking set's framing for the next start, if it changed (also done by SaveViews)
void ResetViewsForTest();

// ---- pristine game image (image.cpp) ----
struct Section {
    char name[9];
    uint32_t rva, size;
    bool exec, write;
};
struct Image {
    std::vector<uint8_t> mem;  // sections copied to their RVAs
    uint64_t prefBase = 0;
    uint32_t sizeOfImage = 0;
    uint32_t importRva = 0;    // the import directory
    std::vector<Section> secs;
    const Section* SectionOf(uint32_t rva) const;
    bool Contains(uint32_t rva, uint32_t n) const { return (uint64_t)rva + n <= mem.size(); }
    uint32_t U32(uint32_t rva) const { uint32_t v; memcpy(&v, &mem[rva], 4); return v; }
    int32_t I32(uint32_t rva) const { int32_t v; memcpy(&v, &mem[rva], 4); return v; }
    uint64_t U64(uint32_t rva) const { uint64_t v; memcpy(&v, &mem[rva], 8); return v; }
    float F32(uint32_t rva) const { float v; memcpy(&v, &mem[rva], 4); return v; }
};
bool LoadPristineImage(const std::wstring& exePath, Image& img, std::string& buildId);
struct Pattern {
    std::vector<int> bytes;  // -1 = wildcard
    bool Parse(const char* text);
};
// All matches of pattern in executable sections (up to max).
std::vector<uint32_t> FindPattern(const Image& img, const Pattern& p, size_t max = 16);
bool MatchAt(const Image& img, uint32_t rva, const Pattern& p);
// Unique match or 0 with an error message.
uint32_t FindUnique(const Image& img, const char* what, const char* pattern, std::string& err);
// The import table slot the game calls func of dll through (an RVA), or 0.
uint32_t FindImportSlot(const Image& img, const char* dll, const char* func);

// ---- inline hooks (hook.cpp) ----
bool PatchCode(uint8_t* target, const uint8_t* patch, size_t n);
// Replaces the first 15 bytes of target with a jump to hook; *original gets a trampoline that runs the first n
// (position independent, whole instructions) bytes and continues at target+n.
bool InstallJmpHook(uint8_t* target, const uint8_t* prologue, void* hook, void* volatile* original, const char* what,
                    std::string& err, size_t n = 15);

// ---- the camera (camera.cpp) ----
struct CameraTargets {
    uint32_t blend = 0;  // heron::cameraset::update_blending's body (hooked)
};
bool FindCameraTargets(const Image& img, CameraTargets& t, std::string& err);
bool InstallCameraHook(const Image& img, std::string& err);
bool InstallCameraHookAt(uint8_t* blend, const uint8_t* prologue, std::string& err);  // on a stand-in (tests)
bool CameraHookInstalled();
void SetTuningForTest(const Tuning& t, bool on);  // overrides the cameras and settles at once (no easing)
void EndTuningTest();                              // back to the cameras (still no easing)
void SetEaseForTest(bool on);                      // the cameras ease as in the game (tests of the easing)
void RequestDump();  // the next tick writes the blended params to the log
// The game's add and drop of a camera state request (the end of a fight keeps the game's fight camera a while).
bool FindStateRequests(const Image& img, uint32_t& add, uint32_t& drop, std::string& err);
bool InstallStateRequests(const Image& img, std::string& err);
void UseStateRequestsForTest(void* add, void* drop);
bool StateRequestsReady();
// The game's camera evaluator (a set's camera offset at a pitch): indoors the usual camera's framing replaces the
// indoor set's, their distances compared with it.
bool FindCurveEval(const Image& img, uint32_t& eval, std::string& err);
void UseCurveEval(void* eval);
// The walking set's framing (field of view, distance multiplier, default/safe/fallback/target positions) and its
// measured distance: what indoors is framed like, kept for the next start.
struct UsualFrame {
    uint32_t set = 0;
    float values[14] = {};
    float dist = 0;
};
bool GetUsualFrame(UsualFrame& f);  // false until a usual set has been seen (or restored)
void SetUsualFrame(const UsualFrame& f);

// ---- the tuning panel (panel.cpp) ----
// A panel on the left of the screen (CameraPlus.js draws it from PanelAction's status). While it is open the
// keyboard drives it and the game doesn't see the keys; the mouse still turns the camera.
void StartPanel();                        // finds the game window and hooks its keyboard input
bool PanelKey(int vk, bool shift);        // a key press while open (or the panel key); true = the panel used it
bool PanelOpen();
bool PanelCapturing();                    // waiting for the new zoom key or zoom button
void PanelCapture(int vk);                // the key (or mouse button) pressed while capturing
void PanelCapturePad(int button, bool cancel);  // a controller button pressed while capturing (B cancels)
void PanelToggleFromPad();                // the panel button
void PanelPadKey(int vk, bool fine);      // a controller press while open, as the key it stands for
std::string PanelAction(const std::string& action, const char* query);  // the __cameraplus__.json endpoint
// Wheel and touchpad zoom. The game reads the mouse through GetRawInputData (its import table) and binds the wheel
// in play too (the controller's ability layer); while wheel zoom works, its own calls get the mouse without the wheel.
bool InstallWheelBlock(const Image& img, std::string& err);
bool InstallWheelBlockAt(void** slot, std::string& err);  // on a stand-in slot (tests)
bool WheelBlockInstalled();
bool WheelZooming();                      // the wheel is CameraPlus's now: wheel zoom on, CameraPlus on, in play
void PanelWheel(int delta);               // the wheel turned (120 a notch, + away from you) while WheelZooming()
void PanelZoomStep(int step);             // a zoom step (+ closer): StepDistance, and the panel shows it

// ---- UI resource interception (resources.cpp) ----
bool FindResourceSlot(const Image& img, uint32_t& slotRva, std::string& err);
bool InstallResourceHook(const Image& img, std::string& err);
bool BuildInjectedScript(const std::vector<uint8_t>& original, std::string& out);
std::string BuildConfigJs();

// heron::cameraset::component::OutputData, one per player (the blended camera set the tail camera reads).
// The set block ("params") is laid out the same at +0x50 (the blend, what the camera uses), +0x230 (where the
// blend started) and +0x410 (the set it goes to); offsets inside it are from the conversion at +0x27c3950.
namespace od {
const size_t kSize = 0x610;
// Bytes select_set picks the set by: a mode and the camera state (camera_set_state_zone's zones in the levels ask for
// one: 0x1427c0740 adds a zone's request when the player enters it, 0x1427c07f0 drops it when they leave).
const size_t kMode = 0x0, kState = 0x1;
const size_t kPlayerMode = 0x8;          // byte: the player mode select_set matched (SpecialPlayerMode)
// The state requests: a vector of 16-byte {entity, priority (u32), state (low byte of a u32)} sorted by priority,
// highest first (a new one goes after equal ones), and its count. select_set takes the first one's state.
const size_t kRequests = 0x40, kRequestCount = 0x48;
// The state the game's fight camera uses (wide sets, distance x2): a zone asks for it while a fight is on and drops
// the request as the last enemy dies (seen 0.06-0.2 s before the HUD's combat flag clears).
const int kStateFight = 2;
// int: the movement the set is for, as select_set picks it each frame (0x1427c3570): the number of the first bit of
// {1 idle, 2 dodge, 3 dash, 4 jump, 5 sprint, 6 execution/reach, 7 carry, 8..10 lock-on} the state has a set for,
// else 0. Jump: the Jump component's +0x15 is 2 and +0x14 isn't 4 (in the air after a jump).
const size_t kMoveMode = 0x4;
const int kMoveJump = 4;
const size_t kSetId = 0xc;               // the set blended to (-1 = none yet)
const size_t kOut = 0x50, kFrom = 0x230, kTo = 0x410;
const size_t kT = 0x5f0;                 // blend weight of kTo, 0..1
const size_t kSwitched = 0x5fc;          // select_set switched sets this frame
const size_t kIdleTime = 0x600;         // update_idle_time: seconds standing still (the idle set at the tweakable)
// Inside the params block:
const size_t pSetId = 0x0, pAspect = 0x4, pFov = 0x8, pDistMul = 0x14, pHide = 0x18;
const size_t pGroundedSmooth = 0xa0, pAirborneSmooth = 0xb0;                   // vec3 (+pad), seconds
const size_t pDefault = 0xc0, pSafe = 0xd0, pFallback = 0xe0, pTarget = 0xf0;  // vec3 (+pad)
const size_t pVertical = 0x100;          // verticalAngleRange (vec2)
const size_t pXCurve = 0x188, pDistCurve = 0x190;  // cameraOffsetXCurve, cameraOffsetDistanceCurve (resources)
const size_t kParamsSize = 0x1e0;        // the whole block, curves included
}  // namespace od

}  // namespace cp
