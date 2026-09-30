// Interception of the game's cohtml resource handler (coui://base/...).
//
// The handler object's OnResourceRequest lives in one vtable slot in .rdata. Like CRModMenu, MapFusion and
// RadarPlus we swap that slot for our function and call whatever was there before, so any number of mods
// chain. The UI bundle's response is wrapped in an object implementing the engine's IAsyncResourceResponse
// vtable (layout recovered from the game build 25472515):
//   [0] scalar deleting dtor   [1] void* GetSpace(u64)     [2] (arg)   [3] (arg, optimized script)
//   [4] () streaming           [5] SetStatus(u16)          [6] (arg)   [7] SetHeader(name, value)
//   [8] Finish(status 0=ok 1=failure)
#include "common.h"
#include <atomic>

namespace cp {

static const char* kSigResourceHandler = "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 40 FE FF FF";
static const char* kBundleUrl = "coui://base/uiresources/game/ui/globals_island.bundle.js";
static const char* kEndpointPrefix = "coui://base/__cameraplus";
static const char* kMarker = "/* CAMERAPLUS_RUNTIME_V1 */";

using OnRequestFn = void (*)(void* self, void* request, void* response);
static OnRequestFn g_next = nullptr;
static std::atomic<uint32_t> g_requestCount{0};

static inline void** VT(void* obj) { return *(void***)obj; }
static const char* RequestUrl(void* req) { return ((const char* (*)(void*))VT(req)[2])(req); }
static void* RespGetSpace(void* r, uint64_t n) { return ((void* (*)(void*, uint64_t))VT(r)[1])(r, n); }
static void RespSetStatus(void* r, int status) { ((void (*)(void*, int))VT(r)[5])(r, status); }
static void RespSetHeader(void* r, const char* k, const char* v) {
    ((void (*)(void*, const char*, const char*))VT(r)[7])(r, k, v);
}
static void RespFinish(void* r, int status) { ((void (*)(void*, int))VT(r)[8])(r, status); }

// ---------------------------------------------------------------------------------------------
// Response wrapper for the UI bundle
struct Wrapper {
    void** vtbl;  // must stay first
    void* real;   // must stay at +8 (the forwarding thunks read it)
    bool bypass;  // data already handed to the real response untouched
    bool failed;
    std::vector<uint8_t> buf;  // complete original body
};

static void* g_wrapperVtbl[9];

// Hands buffered data to the real response and stops transforming (unknown delivery path).
static void Flush(Wrapper* w) {
    if (w->bypass) return;
    bool failed = false;
    if (!w->buf.empty()) {
        void* dst = RespGetSpace(w->real, w->buf.size());
        if (dst) memcpy(dst, w->buf.data(), w->buf.size());
        else failed = true;
    }
    w->failed = failed;
    w->bypass = true;
}

static void* W_Dtor(Wrapper* w, unsigned flags) {
    w->buf.clear();
    w->buf.shrink_to_fit();
    if (flags & 1) delete w;
    return w;
}

static void* W_GetSpace(Wrapper* w, uint64_t size) {
    if (w->bypass) return RespGetSpace(w->real, size);
    if (size > (32ull << 20)) {  // absurd for the bundle; stop transforming
        Flush(w);
        return RespGetSpace(w->real, size);
    }
    try {
        w->buf.resize((size_t)size);
    } catch (...) {
        Flush(w);
        return RespGetSpace(w->real, size);
    }
    return w->buf.data();
}

static uintptr_t Forward(Wrapper* w, int slot, uintptr_t a) {
    return ((uintptr_t(*)(void*, uintptr_t))VT(w->real)[slot])(w->real, a);
}

static uintptr_t W_Slot2(Wrapper* w, uintptr_t a) {
    Flush(w);
    return Forward(w, 2, a);
}

static uintptr_t W_Slot3(Wrapper* w, uintptr_t a) {
    Flush(w);
    uintptr_t r = Forward(w, 3, a);
    Log("Optimized script response: original cache kept, CameraPlus not injected this time");
    return r;
}

static uintptr_t W_Slot4(Wrapper* w) {
    Flush(w);
    uintptr_t r = 0;
    if (!w->failed) r = ((uintptr_t(*)(void*))VT(w->real)[4])(w->real);
    Log("Streaming script response: CameraPlus not injected this time");
    return r;
}

bool BuildInjectedScript(const std::vector<uint8_t>& original, std::string& out) {
    std::string src((const char*)original.data(), original.size());
    while (!src.empty() && src.back() == '\0') src.pop_back();
    if (src.find(kMarker) != std::string::npos) return false;
    std::string js;
    if (!ReadWholeFile(g_modDir + L"CameraPlus.js", js, 8u << 20) || js.empty()) {
        Log("CameraPlus.js missing or empty; nothing injected");
        return false;
    }
    if (js.size() >= 3 && (uint8_t)js[0] == 0xEF && (uint8_t)js[1] == 0xBB && (uint8_t)js[2] == 0xBF) js.erase(0, 3);
    // The script travels as a string and is compiled by new Function inside try/catch, so a mistake
    // in CameraPlus.js can never break the game's own bundle.
    std::string lit;
    lit.reserve(js.size() + js.size() / 8 + 16);
    for (size_t i = 0; i < js.size(); ++i) {
        unsigned char c = (unsigned char)js[i];
        switch (c) {
            case '\\': lit += "\\\\"; break;
            case '"': lit += "\\\""; break;
            case '\n': lit += "\\n"; break;
            case '\r': break;
            case '\t': lit += "\\t"; break;
            case '<': lit += "\\x3c"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    sprintf_s(b, "\\x%02x", c);
                    lit += b;
                } else if (c == 0xE2 && i + 2 < js.size() && (unsigned char)js[i + 1] == 0x80 &&
                           ((unsigned char)js[i + 2] == 0xA8 || (unsigned char)js[i + 2] == 0xA9)) {
                    lit += ((unsigned char)js[i + 2] == 0xA8) ? "\\u2028" : "\\u2029";
                    i += 2;
                } else {
                    lit.push_back((char)c);
                }
        }
    }
    out.reserve(src.size() + lit.size() + 4096);
    out = src;
    out += "\n;";
    out += kMarker;
    out += "\n";
    out += BuildConfigJs();
    out += "\n;(function(){try{(new Function(\"";
    out += lit;
    out += "\"))();}catch(e){try{var x=new XMLHttpRequest();x.open('GET','coui://base/__cameraplus_log__.json?m='+"
           "encodeURIComponent('script error: '+(e&&e.stack||e)),true);x.send();}catch(_){}}})();\n";
    return true;
}

static void W_Finish(Wrapper* w, int status) {
    void* real = w->real;
    int outStatus = status;
    if (status == 0 && !w->bypass) {
        bool ok = true;
        if (!w->buf.empty()) {
            std::string out;
            bool changed = false;
            try {
                changed = BuildInjectedScript(w->buf, out);
            } catch (...) {
                changed = false;
            }
            void* dst = nullptr;
            if (changed) {
                dst = RespGetSpace(real, out.size());
                if (dst) {
                    memcpy(dst, out.data(), out.size());
                    Log("CameraPlus script appended to %s (%zu -> %zu bytes)", kBundleUrl, w->buf.size(), out.size());
                } else {
                    Log("Transformation failed: restoring original source response");
                }
            }
            if (!dst) {
                dst = RespGetSpace(real, w->buf.size());
                if (dst) memcpy(dst, w->buf.data(), w->buf.size());
                else ok = false;
            }
        }
        if (!ok) outStatus = 1;
    }
    if (w->failed) outStatus = 1;
    delete w;
    RespFinish(real, outStatus);
}

// Pure forwarders for slots 5..7: mov rcx,[rcx+8]; mov rax,[rcx]; jmp [rax+slot*8]
static void* MakeForwarders() {
    uint8_t* mem = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) return nullptr;
    for (int slot = 5; slot <= 7; ++slot) {
        uint8_t* p = mem + (slot - 5) * 16;
        const uint8_t code[] = {0x48, 0x8B, 0x49, 0x08, 0x48, 0x8B, 0x01, 0xFF, 0x60, (uint8_t)(slot * 8)};
        memcpy(p, code, sizeof(code));
        g_wrapperVtbl[slot] = p;
    }
    DWORD old;
    VirtualProtect(mem, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, 4096);
    return mem;
}

static Wrapper* NewWrapper(void* real) {
    Wrapper* w = new (std::nothrow) Wrapper();
    if (!w) return nullptr;
    w->vtbl = g_wrapperVtbl;
    w->real = real;
    w->bypass = false;
    w->failed = false;
    return w;
}

// ---------------------------------------------------------------------------------------------
// Endpoints
static void RespondJson(void* resp, const std::string& body) {
    RespSetHeader(resp, "Content-Type", "application/json");
    RespSetHeader(resp, "Cache-Control", "no-store");
    RespSetStatus(resp, 200);
    void* dst = RespGetSpace(resp, body.size());
    if (dst) memcpy(dst, body.data(), body.size());
    RespFinish(resp, dst ? 0 : 1);
}

static std::atomic<uint32_t> g_jsLogLines{0};

static void HandleEndpoint(const char* url, size_t pathLen, void* resp) {
    std::string name(url + strlen(kEndpointPrefix), pathLen - strlen(kEndpointPrefix));
    const char* q = strchr(url, '?');
    const char* query = q ? q + 1 : nullptr;
    std::string body;
    if (name == "__.json") {
        std::string a;
        if (!QueryParam(query, "a", a)) a = "status";
        body = PanelAction(a, query);
    } else if (name == "_log__.json") {
        std::string m;
        if (QueryParam(query, "m", m) && ++g_jsLogLines <= 3000) {
            if (m.size() > 1500) m.resize(1500);
            for (char& c : m)
                if (c == '\r' || c == '\n') c = ' ';
            Log("JS %s", m.c_str());
        }
        body = "{\"ok\":true}";
    } else {
        body = "{\"ok\":false,\"error\":\"unknown endpoint\"}";
    }
    RespondJson(resp, body);
}

static const char* SafeRequestUrl(void* request) {
    __try {
        return RequestUrl(request);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static void HookedRequest(void* self, void* request, void* response) {
    const char* url = SafeRequestUrl(request);
    if (url) {
        size_t pathLen = strcspn(url, "?#");
        uint32_t n = ++g_requestCount;
        if (g_cfg.diagnostics && n <= 60) Log("UI request %s", url);
        if (pathLen > strlen(kEndpointPrefix) && strncmp(url, kEndpointPrefix, strlen(kEndpointPrefix)) == 0) {
            try {
                HandleEndpoint(url, pathLen, response);
            } catch (...) {
                RespFinish(response, 1);
            }
            return;
        }
        if (pathLen == strlen(kBundleUrl) && _strnicmp(url, kBundleUrl, pathLen) == 0) {
            Log("Intercepting %s", url);
            Wrapper* w = NewWrapper(response);
            if (w) {
                g_next(self, request, w);
                return;
            }
        }
    }
    g_next(self, request, response);
}

bool FindResourceSlot(const Image& img, uint32_t& slotRva, std::string& err) {
    uint32_t fn = FindUnique(img, "resource handler", kSigResourceHandler, err);
    if (!fn) return false;
    const uint64_t want = img.prefBase + fn;
    std::vector<uint32_t> slots;
    for (const Section& s : img.secs) {
        if (s.exec || s.write) continue;
        for (uint32_t off = (s.rva + 7) & ~7u; off + 8 <= s.rva + s.size; off += 8)
            if (img.U64(off) == want) slots.push_back(off);
    }
    if (slots.size() != 1) {
        err = slots.empty() ? "resource handler: no vtable slot points to it"
                            : "resource handler: more than one vtable slot";
        return false;
    }
    slotRva = slots[0];
    return true;
}

bool InstallResourceHook(const Image& img, std::string& err) {
    uint32_t slotRva = 0;
    if (!FindResourceSlot(img, slotRva, err)) return false;
    g_wrapperVtbl[0] = (void*)&W_Dtor;
    g_wrapperVtbl[1] = (void*)&W_GetSpace;
    g_wrapperVtbl[2] = (void*)&W_Slot2;
    g_wrapperVtbl[3] = (void*)&W_Slot3;
    g_wrapperVtbl[4] = (void*)&W_Slot4;
    g_wrapperVtbl[8] = (void*)&W_Finish;
    if (!MakeForwarders()) {
        err = "could not allocate forwarders";
        return false;
    }
    void** slot = (void**)(g_gameBase + slotRva);

    // Same mutex CRModMenu, MapFusion and RadarPlus use, so concurrent installs never lose a link.
    wchar_t mutexName[64];
    swprintf_s(mutexName, L"Local\\ControlMods.ResourceHook.%lu", GetCurrentProcessId());
    HANDLE mtx = CreateMutexW(nullptr, FALSE, mutexName);
    if (!mtx) {
        err = "resource hook mutex failed";
        return false;
    }
    DWORD wait = WaitForSingleObject(mtx, 5000);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mtx);
        err = "resource hook mutex timeout";
        return false;
    }
    bool ok = false;
    for (int attempt = 0; attempt < 4 && !ok; ++attempt) {
        void* cur = *(void* volatile*)slot;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(cur, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) ||
            !(mbi.Protect & 0xF0))
            break;
        g_next = (OnRequestFn)cur;
        DWORD old = 0, tmp = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) break;
        void* prev = InterlockedCompareExchangePointer((void* volatile*)slot, (void*)&HookedRequest, cur);
        VirtualProtect(slot, sizeof(void*), old, &tmp);
        ok = (prev == cur);
    }
    ReleaseMutex(mtx);
    CloseHandle(mtx);
    if (!ok) {
        err = "could not chain resource handler";
        return false;
    }
    Log("Resource handler active (slot +0x%x, chained to %p)", slotRva, (void*)g_next);
    return true;
}

}  // namespace cp
