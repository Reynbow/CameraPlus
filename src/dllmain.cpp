// Entry point. crloader (winmm.dll) loads every DLL in crmods\ at start-up; we do our setup on a worker
// thread so the loader lock is never held while we read and scan the game image.
#include "common.h"

namespace cp {

HMODULE g_self = nullptr;
std::wstring g_modDir;
uintptr_t g_gameBase = 0;
bool g_knownBuild = false;

static const char* kKnownBuildId = "6ab107a0-06301000-05eedcbd";  // build 25472515

static void Setup() {
    LoadConfig();
    LogInit();
    Log("CameraPlus " CP_VERSION " folder=%s", Utf8(g_modDir).c_str());
    if (!g_cfg.enabled) {
        Log("Disabled by INI (Enabled=0)");
        return;
    }
    wchar_t name[64];
    swprintf_s(name, L"Local\\CameraPlus.Instance.%lu", GetCurrentProcessId());
    HANDLE instance = CreateMutexW(nullptr, TRUE, name);
    if (!instance || GetLastError() == ERROR_ALREADY_EXISTS) {
        Log("Another CameraPlus copy is already active in this process; this copy stays idle");
        return;
    }
    // Intentionally never closed: marks this process as served for its lifetime.

    g_gameBase = (uintptr_t)GetModuleHandleW(nullptr);
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
    if (!n || n >= sizeof(exe) / sizeof(exe[0])) {
        Log("Cannot resolve the game executable path; not installing");
        return;
    }
    Image img;
    std::string build;
    if (!LoadPristineImage(exe, img, build)) {
        Log("Cannot read the game image; not installing");
        return;
    }
    g_knownBuild = build == kKnownBuildId;
    Log("Game build %s (%s)", build.c_str(), g_knownBuild ? "known build 25472515" : "other build; running on signatures");
    for (int v = 0; v < kViewCount; ++v) {
        const Tuning t = GetView(v);
        const int st = ViewStyle(v);
        Log("%s camera: %s, dist x%.3f side %+.2f look-at %+.2f fov %+.1f", ViewName(v),
            st == -2 ? "same as exploration" : StyleName(st), t.distMul, t.posAdd[0], t.targetAdd[1], t.fovAdd);
    }
    Log("Settings: diagnostics=%d, panel key %s, zoom key %s (%s), wheel zoom %s, touchpad zoom %s", g_cfg.diagnostics,
        Utf8(g_cfg.panelKeyName).c_str(), KeyName(ZoomKey()).c_str(), ZoomToggle() ? "toggle" : "hold",
        WheelZoom() ? "on" : "off", TouchZoom() ? "on" : "off");

    std::string err;
    if (!InstallCameraHook(img, err)) {
        Log("Camera hook off: %s. CameraPlus stays off on this game version.", err.c_str());
        return;
    }
    // Controllers: the game's pause flag (the panel and zoom buttons are the game's in menus) and the hook that keeps
    // controller presses from the game while the panel is open. Both optional.
    CameraTargets targets;
    uint32_t pausedSlot = 0;
    err.clear();
    if (FindCameraTargets(img, targets, err) && FindPausedFlag(img, targets.blend, pausedSlot)) {
        UsePausedFlag(pausedSlot);
        Log("pause flag at +0x%x (+0x38)", pausedSlot);
    } else {
        Log("Pause flag not found: the panel and zoom buttons stay CameraPlus's in menus too");
    }
    err.clear();
    if (!InstallPadBlock(img, err))
        Log("Controller block off: %s. Controller presses also reach the game while the panel is open.", err.c_str());

    // The panel: the script that draws it (appended to the game's UI bundle), the key handling and the controller.
    err.clear();
    if (!InstallResourceHook(img, err)) {
        Log("Resource interception failed (%s); no tuning panel, the saved tuning still applies", err.c_str());
    } else {
        StartPanel();
        StartPad();
        // Wheel zoom (read in the panel's window hook): in play the game's own raw input reads get the mouse without
        // the wheel. It needs the pause flag, so menus (the map, the pause menu) keep their wheel.
        err.clear();
        if (!PauseKnown()) Log("Wheel zoom off: it needs the pause flag");
        else if (!InstallWheelBlock(img, err)) Log("Wheel zoom off: %s", err.c_str());
    }
    Log("Setup done: camera hook installed");
}

static DWORD WINAPI SetupThread(void*) {
    try {
        Setup();
    } catch (...) {
        Log("Initialization exception; no further setup attempted");
    }
    return 0;
}

}  // namespace cp

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        cp::g_self = module;
        wchar_t path[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(module, path, (DWORD)(sizeof(path) / sizeof(path[0])));
        std::wstring p(path, n);
        size_t slash = p.find_last_of(L"\\/");
        cp::g_modDir = slash == std::wstring::npos ? L".\\" : p.substr(0, slash + 1);
        HANDLE h = CreateThread(nullptr, 0, cp::SetupThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
