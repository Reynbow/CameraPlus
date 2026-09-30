// The keys the MODS page's "Panel key" slider offers, with their names on the player's keyboard (the same list as
// FastTravelPlus's hotkey slider). The slider saves a position in this list, so it only ever grows at the end.
#include "common.h"

namespace cp {

static std::vector<KeyChoice> g_keys;

// The characters a key types on the current layout, unshifted and shifted: "` ~" on a US keyboard.
static std::wstring LayoutName(int vk) {
    HKL layout = GetKeyboardLayout(0);
    UINT sc = MapVirtualKeyExW((UINT)vk, MAPVK_VK_TO_VSC, layout);
    BYTE state[256] = {};
    wchar_t plain[8] = {}, shifted[8] = {};
    // Flag 4: leave the keyboard's dead-key state alone. A dead key returns -1 with its spacing character.
    int n1 = ToUnicodeEx((UINT)vk, sc, state, plain, 8, 4, layout);
    state[VK_SHIFT] = 0x80;
    int n2 = ToUnicodeEx((UINT)vk, sc, state, shifted, 8, 4, layout);
    auto usable = [](int n, const wchar_t* s) { return (n == 1 || n == -1) && s[0] > L' ' && s[0] != 0x7f; };
    std::wstring name;
    if (usable(n1, plain)) name = std::wstring(1, plain[0]);
    if (usable(n2, shifted) && shifted[0] != plain[0]) name += (name.empty() ? L"" : L" ") + std::wstring(1, shifted[0]);
    if (!name.empty()) return name;
    wchar_t text[64] = {};
    if (sc && GetKeyNameTextW((LONG)(sc << 16), text, 64) > 0) return text;
    wchar_t hex[16];
    swprintf_s(hex, L"Key 0x%02X", vk);
    return hex;
}

void BuildKeyList() {
    std::vector<KeyChoice> k;
    auto add = [&](int vk, const std::wstring& name) { k.push_back({vk, Utf8(name)}); };
    add(0, L"Off");
    add(VK_OEM_3, LayoutName(VK_OEM_3));
    for (int i = 1; i <= 12; ++i) add(VK_F1 + i - 1, L"F" + std::to_wstring(i));  // F1 is kDefaultPanelKey
    for (wchar_t c : std::wstring(L"1234567890")) add((int)c, std::wstring(1, c));
    for (wchar_t c = L'A'; c <= L'Z'; ++c) add((int)c, std::wstring(1, c));
    for (int vk : {VK_OEM_MINUS, VK_OEM_PLUS, VK_OEM_4, VK_OEM_6, VK_OEM_5, VK_OEM_1, VK_OEM_7, VK_OEM_COMMA,
                   VK_OEM_PERIOD, VK_OEM_2, VK_OEM_102})
        add(vk, LayoutName(vk));
    add(VK_TAB, L"Tab");
    add(VK_CAPITAL, L"Caps Lock");
    add(VK_BACK, L"Backspace");
    add(VK_INSERT, L"Insert");
    add(VK_DELETE, L"Delete");
    add(VK_HOME, L"Home");
    add(VK_END, L"End");
    add(VK_PRIOR, L"Page Up");
    add(VK_NEXT, L"Page Down");
    add(VK_UP, L"Up");
    add(VK_DOWN, L"Down");
    add(VK_LEFT, L"Left");
    add(VK_RIGHT, L"Right");
    for (int i = 0; i <= 9; ++i) add(VK_NUMPAD0 + i, L"Numpad " + std::to_wstring(i));
    add(VK_DIVIDE, L"Numpad /");
    add(VK_MULTIPLY, L"Numpad *");
    add(VK_SUBTRACT, L"Numpad -");
    add(VK_ADD, L"Numpad +");
    add(VK_DECIMAL, L"Numpad .");
    add(VK_SCROLL, L"Scroll Lock");
    add(VK_PAUSE, L"Pause");
    add(VK_MBUTTON, L"Middle mouse");
    add(VK_XBUTTON1, L"Mouse 4");
    add(VK_XBUTTON2, L"Mouse 5");
    for (int i = 13; i <= 24; ++i) add(VK_F1 + i - 1, L"F" + std::to_wstring(i));
    g_keys.swap(k);
}

const std::vector<KeyChoice>& KeyList() { return g_keys; }

int KeyListIndex(int vk) {
    for (size_t i = 0; i < g_keys.size(); ++i)
        if (g_keys[i].vk == vk) return (int)i;
    return -1;
}

}  // namespace cp
