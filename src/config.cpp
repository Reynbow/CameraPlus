#include "common.h"
#include <wchar.h>

namespace cp {

Config g_cfg;

static std::wstring IniPath() { return g_modDir + L"CameraPlus.ini"; }

static std::wstring ReadString(const wchar_t* section, const wchar_t* key) {
    wchar_t buf[256];
    GetPrivateProfileStringW(section, key, L"", buf, 256, IniPath().c_str());
    std::wstring s(buf);
    size_t semi = s.find(L';');
    if (semi != std::wstring::npos) s.resize(semi);
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t start = 0;
    while (start < s.size() && iswspace(s[start])) ++start;
    return s.substr(start);
}

static bool ReadBool(const wchar_t* key, bool def) {
    std::wstring s = ReadString(L"CameraPlus", key);
    if (s.empty()) return def;
    if (_wcsicmp(s.c_str(), L"true") == 0 || _wcsicmp(s.c_str(), L"on") == 0 || _wcsicmp(s.c_str(), L"yes") == 0)
        return true;
    if (_wcsicmp(s.c_str(), L"false") == 0 || _wcsicmp(s.c_str(), L"off") == 0 || _wcsicmp(s.c_str(), L"no") == 0)
        return false;
    return _wtoi(s.c_str()) != 0;
}

struct NamedKey { const wchar_t* name; int vk; };
static const NamedKey kKeys[] = {
    {L"Insert", VK_INSERT}, {L"Delete", VK_DELETE}, {L"Home", VK_HOME},       {L"End", VK_END},
    {L"PageUp", VK_PRIOR},  {L"PageDown", VK_NEXT}, {L"Pause", VK_PAUSE},     {L"ScrollLock", VK_SCROLL},
    {L"Tilde", VK_OEM_3},   {L"Backslash", VK_OEM_5}, {L"LeftBracket", VK_OEM_4}, {L"RightBracket", VK_OEM_6},
    {L"Semicolon", VK_OEM_1}, {L"Quote", VK_OEM_7}, {L"Comma", VK_OEM_COMMA}, {L"Period", VK_OEM_PERIOD},
    {L"Slash", VK_OEM_2},   {L"Mouse4", VK_XBUTTON1}, {L"Mouse5", VK_XBUTTON2}, {L"MouseMiddle", VK_MBUTTON},
    {L"MouseRight", VK_RBUTTON},
};

int ParseKeyName(const std::wstring& raw) {
    std::wstring n;
    for (wchar_t c : raw)
        if (!iswspace(c)) n.push_back(c);
    if (n.empty() || _wcsicmp(n.c_str(), L"none") == 0 || _wcsicmp(n.c_str(), L"off") == 0) return 0;
    if (n.size() > 2 && n[0] == L'0' && (n[1] == L'x' || n[1] == L'X')) {
        const int v = (int)wcstol(n.c_str() + 2, nullptr, 16);
        return v > 0 && v < 256 ? v : 0;
    }
    if (n.size() == 1) {
        const wchar_t c = towupper(n[0]);
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) return (int)c;
    }
    if ((n[0] == L'F' || n[0] == L'f') && n.size() <= 3 && iswdigit(n[1])) {
        const int f = _wtoi(n.c_str() + 1);
        if (f >= 1 && f <= 24) return VK_F1 + f - 1;
    }
    for (const NamedKey& k : kKeys)
        if (_wcsicmp(n.c_str(), k.name) == 0) return k.vk;
    return 0;
}

bool Tuning::Identity() const {
    return fovAdd == 0 && distMul == 1 && posAdd[0] == 0 && posAdd[1] == 0 && posAdd[2] == 0 && targetAdd[0] == 0 &&
           targetAdd[1] == 0 && targetAdd[2] == 0 && jump == 1;
}

// ---- the MODS page (Mod Settings Menu 1.7.1's key options) ----
// The player picks the panel key and button by pressing them; Mod Settings Menu shows the key's name or the button's
// icon and saves a code in ModMenuConfig\cameraplus.ini: a key's virtual-key code (3-254, mouse buttons included), a
// controller button as 256 + its place in A B X Y LB RB LT RT View Menu LS RS D-pad Up Down Left Right (our pad list
// without its "none"), 0 for none. Before 1.4.5 the options were sliders saving a place in KeyList() and in the pad
// list (panel_key, panel_button); a choice saved that way is moved over once, under the new names.
static std::wstring MenuIniPath() { return g_modDir + L"ModMenuConfig\\cameraplus.ini"; }

// A value as Mod Settings Menu saved it, -1 if not saved.
static int ReadMenuValue(const wchar_t* key) {
    wchar_t buf[64];
    GetPrivateProfileStringW(L"Settings", key, L"", buf, 64, MenuIniPath().c_str());
    if (!buf[0]) return -1;
    wchar_t* end = nullptr;
    const double v = wcstod(buf, &end);
    return end == buf || v < 0 || v > 100000 ? -1 : (int)(v + 0.5);
}

static void WriteMenuValue(const wchar_t* key, int v) {
    wchar_t b[16];
    swprintf_s(b, L"%d", v);
    WritePrivateProfileStringW(L"Settings", key, b, MenuIniPath().c_str());
}

bool ValidKeyCode(int vk) { return vk == 0 || (vk >= 3 && vk <= 254); }
bool ValidPadCode(int code) { return code == 0 || (code >= 256 && code < 256 + PadButtonCount() - 1); }
int PadCodeOf(int index) { return index > 0 && index < PadButtonCount() ? 256 + index - 1 : 0; }
int PadIndexOf(int code) { return code >= 256 && code < 256 + PadButtonCount() - 1 ? code - 256 + 1 : 0; }

static void UseKey(Config& c, int vk) {
    c.panelKey = vk;
    c.panelKeyName = vk ? Wide(KeyName(vk)) : L"None";
}

// The panel key and button: the MODS page's key options first (or their old sliders, moved over), then
// CameraPlus.ini (PanelKey= a key name, PanelButton= a place in the controller list), then F1 and D-pad Left.
void LoadConfig() {
    BuildKeyList();
    Config c;
    c.enabled = ReadBool(L"Enabled", true);
    c.diagnostics = ReadBool(L"Diagnostics", false);
    int vk = ReadMenuValue(L"panel_hotkey");
    if (vk < 0) {
        const int old = ReadMenuValue(L"panel_key");
        if (old >= 0 && old < (int)KeyList().size()) WriteMenuValue(L"panel_hotkey", vk = KeyList()[old].vk);
    }
    if (vk >= 0 && ValidKeyCode(vk)) {
        UseKey(c, vk);
    } else {
        std::wstring k = ReadString(L"CameraPlus", L"PanelKey");
        if (!k.empty()) {
            c.panelKey = ParseKeyName(k);
            c.panelKeyName = c.panelKey ? k : L"None";
        }
    }
    int code = ReadMenuValue(L"panel_pad");
    if (code < 0) {
        const int old = ReadMenuValue(L"panel_button");
        if (old >= 0 && old < PadButtonCount()) WriteMenuValue(L"panel_pad", code = PadCodeOf(old));
    }
    int button;
    if (code >= 0 && ValidPadCode(code)) {
        button = PadIndexOf(code);
    } else {
        std::wstring b = ReadString(L"CameraPlus", L"PanelButton");
        button = b.empty() ? kDefaultPanelButton : _wtoi(b.c_str());
        if (button < 0 || button >= PadButtonCount()) button = kDefaultPanelButton;
    }
    SetPanelButton(button);
    g_cfg = c;
    LoadViews();
}

bool SetPanelKey(int vk) {
    if (!ValidKeyCode(vk)) return false;
    if (vk == g_cfg.panelKey) return true;
    UseKey(g_cfg, vk);
    Log("Panel key: %s", Utf8(g_cfg.panelKeyName).c_str());
    return true;
}

}  // namespace cp
