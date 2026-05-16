// ImGui-DX12 overlay for the unified mod. The DX12 init / fence /
// WndProc machinery follows the same pattern as dpsmeter-dll's
// overlay.cpp — see those comments for the deep rationale. The
// farever-mod-specific bits are at the bottom (render_imgui_window).

#include "overlay.h"
#include "log.h"
#include "aggregator.h"
#include "damage.h"
#include "hero_state.h"
#include "textures.h"
#include "pois.h"
#include "skill_resolve.h"

#include <unordered_map>

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace farever {
namespace {

constexpr UINT kMaxBackBuffers = 8;

struct FrameContext {
    ID3D12CommandAllocator*     allocator    = nullptr;
    ID3D12GraphicsCommandList*  command_list = nullptr;
    ID3D12Resource*             back_buffer  = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle{};
    UINT64                      fence_value  = 0;
};

struct Overlay {
    bool initialized = false;
    bool init_failed = false;

    IDXGISwapChain3*            owned_swap_chain = nullptr;
    ID3D12Device*               device           = nullptr;
    ID3D12DescriptorHeap*       rtv_heap         = nullptr;
    ID3D12DescriptorHeap*       srv_heap         = nullptr;
    UINT                        rtv_descriptor_size = 0;
    UINT                        back_buffer_count   = 0;
    std::vector<FrameContext>   frames;

    ID3D12Fence*                fence            = nullptr;
    UINT64                      next_fence_value = 0;
    HANDLE                      fence_event      = nullptr;

    HWND                        hwnd         = nullptr;
    WNDPROC                     orig_wndproc = nullptr;
    DXGI_FORMAT                 rt_format    = DXGI_FORMAT_R8G8B8A8_UNORM;

    // SRV slot 0 = ImGui's font atlas; 1=mosaic, 2=POI atlas,
    // 3=player arrow. Slots 5..63 are dynamically allocated to the
    // weapon / class skill atlases as the DPS meter encounters new
    // skills (see g_atlas_cache).
    LoadedTexture               mosaic{};
    LoadedTexture               poi_atlas{};
    LoadedTexture               player_arrow{};
};

Overlay           g_overlay;
std::atomic<bool> g_in_render{false};

// Skill-atlas cache: every skill class refers to a specific weapon /
// class atlas (e.g. atlas_class_Mage_96PX.png). We lazy-load each one
// to a fresh SRV slot on first reference. Slots 0-3 are reserved
// (font, mosaic, POI atlas, player arrow); slot 4 is free; 5+ go to
// dynamic atlases.
constexpr UINT kSkillAtlasSlotBase = 5;
constexpr UINT kSkillAtlasSlotMax  = 64;  // upper bound of srv_heap
UINT g_next_atlas_slot = kSkillAtlasSlotBase;
std::unordered_map<std::string, LoadedTexture> g_atlas_cache;

// Per-window visibility toggles (user keys). The overall panic switch
// (auto-disable on GPU stalls) is kept as a separate fail-safe via
// g_overlay_enabled — if a wedged queue forces us off, the user can't
// re-enable just one window because the whole submission path is
// short-circuited.
std::atomic<bool> g_overlay_enabled{true};  // panic / auto-disable
std::atomic<bool> g_dps_visible{true};      // F10
std::atomic<bool> g_minimap_visible{true};  // F8
constexpr int kFenceTimeoutMs        = 50;
constexpr int kAutoDisableSlowFrames = 30;
int g_consecutive_slow_frames = 0;

// WoW-bezel palette.
constexpr ImU32 kColBezel       = IM_COL32(212, 175,  55, 240);
constexpr ImU32 kColBezelShadow = IM_COL32(  0,   0,   0, 160);
constexpr ImU32 kColBtnFill     = IM_COL32( 32,  24,  12, 235);
constexpr ImU32 kColBtnHover    = IM_COL32( 70,  52,  24, 245);
constexpr ImU32 kColBtnActive   = IM_COL32( 18,  12,   6, 255);
constexpr ImU32 kColIcon        = IM_COL32(255, 220, 130, 255);
constexpr ImU32 kColNorth       = IM_COL32(255,  80,  80, 230);
constexpr ImU32 kColPlayer      = IM_COL32(255, 200,  50, 255);
constexpr ImU32 kColText        = IM_COL32(255, 230, 180, 255);

// --- minimap calibration --------------------------------------------
//
// Knobs derived analytically from the mosaic geometry + the engine's
// 256 m/tile constant: px_per_meter = 1024 / 256 = 4, etc. See
// minimap-dll/overlay.cpp for the full derivation. Hot-reloadable via
// data/minimap_calibration.json so the user can adjust without
// rebuilding.
struct Calibration {
    float world_to_full_x_scale  = 4.0f;
    float world_to_full_y_scale  = 4.0f;
    float world_to_full_x_offset = 4096.0f;
    float world_to_full_y_offset = 6144.0f;
    bool  flip_y                 = true;
    float zoom                   = 12.0f;   // 1.0 = whole mosaic visible
};
Calibration g_calib;
constexpr float kFullMosaicPx = 11264.0f;
constexpr float kZoomMin  = 10.0f;
constexpr float kZoomMax  = 20.0f;
constexpr float kZoomStep = 1.0f;

constexpr float kCompassSizes[3] = { 256.0f, 384.0f, 512.0f };
int g_compass_size_idx = 2;   // start largest

struct PoiFilter {
    bool obelisks   = true;
    bool respawns   = true;
    bool merchants  = true;
    bool dungeons   = true;
    bool activities = true;
};
PoiFilter g_filter;
bool g_filter_open       = false;
bool g_keys_open         = false;
bool g_compass_collapsed = false;
int  g_selected_fight_id = 0;   // 0 = no fight detail open

float compass_size_px() { return kCompassSizes[g_compass_size_idx]; }

bool poi_passes_filter(const PoiRow& p) {
    if (std::strcmp(p.kind, "obelisk")   == 0) return g_filter.obelisks;
    if (std::strcmp(p.kind, "respawn")   == 0) return g_filter.respawns;
    if (std::strcmp(p.kind, "merchant")  == 0) return g_filter.merchants;
    if (std::strcmp(p.kind, "dungeon")   == 0) return g_filter.dungeons;
    if (std::strcmp(p.kind, "activity")  == 0) return g_filter.activities;
    return true;
}

// Resolve a path relative to this DLL (dinput8.dll) so the mod
// works regardless of where the user dropped it (release zips
// land in unpredictable folders).
std::wstring dll_dir() {
    HMODULE hmod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&dll_dir),
        &hmod);
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(hmod, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    std::wstring s(path);
    auto pos = s.find_last_of(L'\\');
    if (pos == std::wstring::npos) return L".";
    s.resize(pos);
    return s;
}

std::wstring data_path(const wchar_t* relative) {
    std::wstring s = dll_dir();
    s += L"\\data\\";
    s += relative;
    return s;
}

// Lazy atlas loader. `filename` is just the basename, e.g.
// "atlas_class_Mage_96PX.png". Looks for the file under data/atlases/
// (mirror of res.pak's UI/icons/ path). Returns nullptr if the file is
// missing or the slot allocator is full.
LoadedTexture* get_or_load_atlas(const char* filename) {
    if (!filename || !*filename) return nullptr;
    std::string key = filename;
    auto it = g_atlas_cache.find(key);
    if (it != g_atlas_cache.end()) {
        return it->second.resource ? &it->second : nullptr;
    }
    if (g_next_atlas_slot >= kSkillAtlasSlotMax) return nullptr;

    // Build the on-disk path.
    std::wstring full = dll_dir();
    full += L"\\data\\atlases\\UI\\icons\\";
    int n = MultiByteToWideChar(CP_UTF8, 0, filename, -1, nullptr, 0);
    if (n <= 0) return nullptr;
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, filename, -1, w.data(), n);
    full += w;

    LoadedTexture tex{};
    UINT slot = g_next_atlas_slot;
    if (!load_texture_from_file(g_overlay.device, g_overlay.srv_heap,
                                slot, full.c_str(), &tex)) {
        logf("overlay: atlas %s load failed (slot %u)", filename, slot);
        g_atlas_cache[key] = LoadedTexture{};  // negative cache
        return nullptr;
    }
    g_next_atlas_slot++;
    g_atlas_cache[key] = tex;
    logf("overlay: atlas %s loaded into slot %u (%ux%u)",
         filename, slot, tex.width, tex.height);
    auto& stored = g_atlas_cache[key];
    return stored.resource ? &stored : nullptr;
}

const std::wstring& kCalibPath() {
    static const std::wstring p = data_path(L"minimap_calibration.json");
    return p;
}

bool calib_extract_double(const std::string& json, const char* key,
                          double& out) {
    std::string needle = "\""; needle += key; needle += "\":";
    auto i = json.find(needle);
    if (i == std::string::npos) return false;
    i += needle.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    char* end = nullptr;
    double v = strtod(json.c_str() + i, &end);
    if (end == json.c_str() + i) return false;
    out = v;
    return true;
}

bool calib_extract_bool(const std::string& json, const char* key, bool& out) {
    std::string needle = "\""; needle += key; needle += "\":";
    auto i = json.find(needle);
    if (i == std::string::npos) return false;
    i += needle.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    if (json.compare(i, 4, "true")  == 0) { out = true;  return true; }
    if (json.compare(i, 5, "false") == 0) { out = false; return true; }
    return false;
}

void calib_maybe_reload() {
    static FILETIME last_write{};
    static bool first_check = true;
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExW(kCalibPath().c_str(), GetFileExInfoStandard, &attr))
        return;
    if (!first_check &&
        attr.ftLastWriteTime.dwLowDateTime  == last_write.dwLowDateTime &&
        attr.ftLastWriteTime.dwHighDateTime == last_write.dwHighDateTime)
        return;
    last_write  = attr.ftLastWriteTime;
    first_check = false;

    std::ifstream f(kCalibPath());
    if (!f) return;
    std::string text((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

    Calibration c = g_calib;
    double v;
    if (calib_extract_double(text, "scale_x",  v)) c.world_to_full_x_scale  = (float)v;
    if (calib_extract_double(text, "scale_y",  v)) c.world_to_full_y_scale  = (float)v;
    if (calib_extract_double(text, "offset_x", v)) c.world_to_full_x_offset = (float)v;
    if (calib_extract_double(text, "offset_y", v)) c.world_to_full_y_offset = (float)v;
    if (calib_extract_double(text, "zoom",     v)) c.zoom                   = (float)v;
    calib_extract_bool(text, "flip_y", c.flip_y);
    if (c.zoom < 1.0f)  c.zoom = 1.0f;
    if (c.zoom > 64.0f) c.zoom = 64.0f;
    g_calib = c;
    logf("overlay: calibration reloaded (zoom=%.1f)", c.zoom);
}

// --- keybinds -------------------------------------------------------
//
// Defaults are the historical F10 (DPS) / F8 (map) / F9 (reset). The
// user can override any of them via data/keybinds.json, accepting
// either a key name ("F11", "M", "INSERT", "NUMPAD0", "OEM_3") or a
// raw Virtual-Key code ("toggle_dps": 121). Hot-reloaded on file
// mtime change, same as the calibration json.
struct Keybinds {
    UINT toggle_dps     = VK_F10;
    UINT toggle_minimap = VK_F8;
    UINT reset_dps      = VK_F9;
};
Keybinds g_keybinds;

// In-game rebind: the user clicks a slot in the keys panel, we set
// this to the slot id, and the next non-modifier key press in
// overlay_wndproc gets written into that slot (ESC cancels).
enum class RebindSlot : int { None = 0, Dps = 1, Map = 2, Reset = 3 };
std::atomic<int> g_rebind_listening{0};

UINT key_from_name(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (s.rfind("VK_", 0) == 0) s = s.substr(3);
    if (s.empty()) return 0;

    if (s.size() == 1) {
        char c = s[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (UINT)c;
    }
    if (s.size() >= 2 && s[0] == 'F') {
        char* end = nullptr;
        long n = strtol(s.c_str() + 1, &end, 10);
        if (end && *end == '\0' && n >= 1 && n <= 24) {
            return VK_F1 + (UINT)(n - 1);
        }
    }
    if (s.rfind("NUMPAD", 0) == 0 && s.size() == 7) {
        char d = s[6];
        if (d >= '0' && d <= '9') return VK_NUMPAD0 + (UINT)(d - '0');
    }
    struct { const char* name; UINT vk; } table[] = {
        {"HOME", VK_HOME}, {"END", VK_END},
        {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE},
        {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT},
        {"UP", VK_UP}, {"DOWN", VK_DOWN},
        {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"SPACE", VK_SPACE},
        {"ENTER", VK_RETURN}, {"RETURN", VK_RETURN},
        {"TAB", VK_TAB},
        {"ESC", VK_ESCAPE}, {"ESCAPE", VK_ESCAPE},
        {"BACKSPACE", VK_BACK}, {"BACK", VK_BACK},
        {"CAPSLOCK", VK_CAPITAL}, {"PAUSE", VK_PAUSE},
        {"SCROLLLOCK", VK_SCROLL}, {"NUMLOCK", VK_NUMLOCK},
        {"PRINTSCREEN", VK_SNAPSHOT}, {"PRINT", VK_PRINT},
        {"OEM_PLUS", VK_OEM_PLUS}, {"OEM_MINUS", VK_OEM_MINUS},
        {"OEM_COMMA", VK_OEM_COMMA}, {"OEM_PERIOD", VK_OEM_PERIOD},
        {"OEM_1", VK_OEM_1}, {"OEM_2", VK_OEM_2}, {"OEM_3", VK_OEM_3},
        {"OEM_4", VK_OEM_4}, {"OEM_5", VK_OEM_5}, {"OEM_6", VK_OEM_6},
        {"OEM_7", VK_OEM_7}, {"OEM_8", VK_OEM_8},
        {"ADD", VK_ADD}, {"SUBTRACT", VK_SUBTRACT},
        {"MULTIPLY", VK_MULTIPLY}, {"DIVIDE", VK_DIVIDE},
        {"DECIMAL", VK_DECIMAL},
    };
    for (auto& e : table) if (s == e.name) return e.vk;
    return 0;
}

std::string key_to_name(UINT vk) {
    char buf[16];
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(buf, sizeof(buf), "F%u", (unsigned)(vk - VK_F1 + 1));
        return buf;
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
        return std::string(1, (char)vk);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        snprintf(buf, sizeof(buf), "Numpad%u", (unsigned)(vk - VK_NUMPAD0));
        return buf;
    }
    switch (vk) {
        case VK_HOME:     return "Home";
        case VK_END:      return "End";
        case VK_INSERT:   return "Insert";
        case VK_DELETE:   return "Delete";
        case VK_PRIOR:    return "PageUp";
        case VK_NEXT:     return "PageDown";
        case VK_UP:       return "Up";
        case VK_DOWN:     return "Down";
        case VK_LEFT:     return "Left";
        case VK_RIGHT:    return "Right";
        case VK_SPACE:    return "Space";
        case VK_RETURN:   return "Enter";
        case VK_TAB:      return "Tab";
        case VK_ESCAPE:   return "Esc";
        case VK_BACK:     return "Backspace";
    }
    snprintf(buf, sizeof(buf), "VK_%u", (unsigned)vk);
    return buf;
}

bool keybinds_extract_key(const std::string& json, const char* key, UINT& out) {
    std::string needle = "\""; needle += key; needle += "\":";
    auto i = json.find(needle);
    if (i == std::string::npos) return false;
    i += needle.size();
    while (i < json.size() &&
           (json[i] == ' ' || json[i] == '\t' ||
            json[i] == '\r' || json[i] == '\n')) ++i;
    if (i >= json.size()) return false;
    if (json[i] == '"') {
        ++i;
        std::string val;
        while (i < json.size() && json[i] != '"') { val += json[i]; ++i; }
        UINT vk = key_from_name(val);
        if (vk == 0) {
            logf("keybinds: unknown key name \"%s\" for %s",
                 val.c_str(), key);
            return false;
        }
        out = vk;
        return true;
    }
    char* end = nullptr;
    long v = strtol(json.c_str() + i, &end, 0);
    if (end == json.c_str() + i) return false;
    if (v <= 0 || v > 254) return false;
    out = (UINT)v;
    return true;
}

const std::wstring& kKeybindsPath() {
    static const std::wstring p = data_path(L"keybinds.json");
    return p;
}

void keybinds_maybe_reload() {
    static FILETIME last_write{};
    static bool logged_missing = false;
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExW(kKeybindsPath().c_str(),
                              GetFileExInfoStandard, &attr)) {
        if (!logged_missing) {
            logf("overlay: no keybinds.json, using defaults "
                 "(dps=F10 map=F8 reset=F9)");
            logged_missing = true;
        }
        last_write = FILETIME{};
        return;
    }
    logged_missing = false;
    if (attr.ftLastWriteTime.dwLowDateTime  == last_write.dwLowDateTime &&
        attr.ftLastWriteTime.dwHighDateTime == last_write.dwHighDateTime)
        return;
    last_write = attr.ftLastWriteTime;

    std::ifstream f(kKeybindsPath());
    if (!f) return;
    std::string text((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    Keybinds kb = g_keybinds;
    keybinds_extract_key(text, "toggle_dps",     kb.toggle_dps);
    keybinds_extract_key(text, "toggle_minimap", kb.toggle_minimap);
    keybinds_extract_key(text, "reset_dps",      kb.reset_dps);
    g_keybinds = kb;
    logf("overlay: keybinds reloaded (dps=%s map=%s reset=%s)",
         key_to_name(kb.toggle_dps).c_str(),
         key_to_name(kb.toggle_minimap).c_str(),
         key_to_name(kb.reset_dps).c_str());
}

void keybinds_save() {
    std::wstring dir_path = dll_dir() + L"\\data";
    CreateDirectoryW(dir_path.c_str(), nullptr);
    std::ofstream f(kKeybindsPath());
    if (!f) {
        logf("keybinds: cannot open keybinds.json for write");
        return;
    }
    f << "{\n"
      << "  \"_comment\": \"Rebind via the minimap key panel or edit by hand. "
         "Names: F1..F24, A..Z, 0..9, Home, End, Insert, Delete, PageUp, "
         "PageDown, Up, Down, Left, Right, Space, Enter, Tab, Esc, "
         "Backspace, Numpad0..Numpad9, OEM_1..OEM_8.\",\n"
      << "  \"toggle_dps\":     \"" << key_to_name(g_keybinds.toggle_dps)     << "\",\n"
      << "  \"toggle_minimap\": \"" << key_to_name(g_keybinds.toggle_minimap) << "\",\n"
      << "  \"reset_dps\":      \"" << key_to_name(g_keybinds.reset_dps)      << "\"\n"
      << "}\n";
}

// world (m) -> mosaic pixel (top-left origin).
ImVec2 world_to_full(double world_x, double world_y) {
    float fx = (float)world_x * g_calib.world_to_full_x_scale
               + g_calib.world_to_full_x_offset;
    float fy = (float)world_y * g_calib.world_to_full_y_scale
               + g_calib.world_to_full_y_offset;
    if (g_calib.flip_y) fy = kFullMosaicPx - fy;
    return ImVec2(fx, fy);
}

struct ViewUV { ImVec2 uv0; ImVec2 uv1; };

ViewUV compute_view_uv(double wx, double wy, bool have_player) {
    float vsize = 1.0f / g_calib.zoom;
    if (vsize >= 1.0f || !have_player) return {ImVec2(0,0), ImVec2(1,1)};
    ImVec2 full = world_to_full(wx, wy);
    float cu = full.x / kFullMosaicPx;
    float cv = full.y / kFullMosaicPx;
    float half = vsize * 0.5f;
    if (cu < half)        cu = half;
    if (cu > 1.0f - half) cu = 1.0f - half;
    if (cv < half)        cv = half;
    if (cv > 1.0f - half) cv = 1.0f - half;
    return {ImVec2(cu - half, cv - half), ImVec2(cu + half, cv + half)};
}

ImVec2 player_to_screen(double wx, double wy, const ViewUV& v, float size_px) {
    ImVec2 full = world_to_full(wx, wy);
    float u = full.x / kFullMosaicPx;
    float vv = full.y / kFullMosaicPx;
    float sx = (u  - v.uv0.x) / (v.uv1.x - v.uv0.x) * size_px;
    float sy = (vv - v.uv0.y) / (v.uv1.y - v.uv0.y) * size_px;
    return ImVec2(sx, sy);
}

// --- bezel buttons --------------------------------------------------

struct BezelButton {
    ImVec2 center;
    float  radius;
    bool   clicked;
    bool   hovered;
    bool   active;
};

BezelButton bezel_hit(const char* id, ImVec2 center, float bezel_r,
                      float angle_rad, float btn_r) {
    BezelButton b{};
    b.center = ImVec2(center.x + cosf(angle_rad) * bezel_r,
                      center.y + sinf(angle_rad) * bezel_r);
    b.radius = btn_r;
    ImGui::SetCursorScreenPos(ImVec2(b.center.x - btn_r, b.center.y - btn_r));
    ImGui::SetNextItemAllowOverlap();
    b.clicked = ImGui::InvisibleButton(id, ImVec2(btn_r * 2, btn_r * 2));
    b.hovered = ImGui::IsItemHovered();
    b.active  = ImGui::IsItemActive();
    return b;
}

void bezel_draw_base(ImDrawList* dl, const BezelButton& b) {
    ImU32 fill = b.active ? kColBtnActive : b.hovered ? kColBtnHover : kColBtnFill;
    dl->AddCircleFilled(b.center, b.radius, fill, 32);
    dl->AddCircle(b.center, b.radius, kColBezel, 32, 2.0f);
}
void bezel_draw_plus(ImDrawList* dl, const BezelButton& b) {
    bezel_draw_base(dl, b);
    float a = b.radius * 0.45f;
    dl->AddLine({b.center.x - a, b.center.y}, {b.center.x + a, b.center.y}, kColIcon, 2.5f);
    dl->AddLine({b.center.x, b.center.y - a}, {b.center.x, b.center.y + a}, kColIcon, 2.5f);
}
void bezel_draw_minus(ImDrawList* dl, const BezelButton& b) {
    bezel_draw_base(dl, b);
    float a = b.radius * 0.45f;
    dl->AddLine({b.center.x - a, b.center.y}, {b.center.x + a, b.center.y}, kColIcon, 2.5f);
}
void bezel_draw_pin(ImDrawList* dl, const BezelButton& b) {
    bezel_draw_base(dl, b);
    ImVec2 head(b.center.x - 1.5f, b.center.y - 3.5f);
    ImVec2 tip (b.center.x + 4.5f, b.center.y + 5.5f);
    dl->AddLine(head, tip, kColIcon, 2.0f);
    dl->AddCircleFilled(head, 3.0f, kColIcon, 12);
}
void bezel_draw_size(ImDrawList* dl, const BezelButton& b, int idx) {
    bezel_draw_base(dl, b);
    const float steps[3] = { 4.0f, 6.0f, 8.0f };
    float h = steps[idx] * 0.5f;
    dl->AddRect({b.center.x - h, b.center.y - h},
                {b.center.x + h, b.center.y + h}, kColIcon, 1.0f, 0, 1.8f);
}
void bezel_draw_collapse(ImDrawList* dl, const BezelButton& b) {
    bezel_draw_base(dl, b);
    float a = b.radius * 0.5f;
    dl->AddLine({b.center.x - a, b.center.y}, {b.center.x + a, b.center.y}, kColIcon, 2.5f);
}
void bezel_draw_filter(ImDrawList* dl, const BezelButton& b, bool active) {
    bezel_draw_base(dl, b);
    ImU32 c = active ? IM_COL32(255, 255, 200, 255) : kColIcon;
    float w = 5.0f, h = 6.0f;
    ImVec2 tl(b.center.x - w, b.center.y - h);
    ImVec2 tr(b.center.x + w, b.center.y - h);
    ImVec2 nl(b.center.x - 1.5f, b.center.y + 1.0f);
    ImVec2 nr(b.center.x + 1.5f, b.center.y + 1.0f);
    ImVec2 sb(b.center.x, b.center.y + h);
    dl->AddLine(tl, tr, c, 1.8f);
    dl->AddLine(tl, nl, c, 1.8f);
    dl->AddLine(tr, nr, c, 1.8f);
    dl->AddLine({nl.x, nl.y}, {nl.x + 1.5f, sb.y}, c, 1.8f);
    dl->AddLine({nr.x, nr.y}, {nr.x - 1.5f, sb.y}, c, 1.8f);
}
void bezel_draw_key(ImDrawList* dl, const BezelButton& b, bool active) {
    bezel_draw_base(dl, b);
    ImU32 c = active ? IM_COL32(255, 255, 200, 255) : kColIcon;
    // bow (round head of the key) on the left
    ImVec2 bow(b.center.x - 3.0f, b.center.y - 1.0f);
    dl->AddCircle(bow, 3.0f, c, 14, 1.6f);
    // shaft to the right
    dl->AddLine({bow.x + 3.0f, bow.y},
                {b.center.x + 5.0f, bow.y}, c, 1.8f);
    // two teeth at the tip
    dl->AddLine({b.center.x + 3.0f, bow.y},
                {b.center.x + 3.0f, bow.y + 3.0f}, c, 1.8f);
    dl->AddLine({b.center.x + 5.0f, bow.y},
                {b.center.x + 5.0f, bow.y + 3.0f}, c, 1.8f);
}

void render_compass(const HeroSnapshot& h) {
    constexpr float kPi = 3.14159265358979323846f;
    const float size = compass_size_px();
    const float r    = size * 0.5f;
    const float btn_r =
        (size <= 256.0f) ? 11.0f : (size <= 384.0f) ? 13.0f : 14.0f;

    ImVec2 p_min  = ImGui::GetCursorScreenPos();
    ImVec2 p_max(p_min.x + size, p_min.y + size);
    ImVec2 center(p_min.x + r,   p_min.y + r);

    // Body hit-area first (AllowOverlap so the bezel buttons can take
    // priority over the disc).
    ImGui::SetCursorScreenPos(p_min);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##compass_body", ImVec2(size, size));

    BezelButton pin      = bezel_hit("##pin",        center, r, -kPi * 0.32f, btn_r);
    BezelButton sizeb    = bezel_hit("##size_cycle", center, r, -kPi * 0.20f, btn_r);
    BezelButton collapse = bezel_hit("##collapse",   center, r, -kPi * 0.50f, btn_r);
    BezelButton filter   = bezel_hit("##filter",     center, r,  kPi * 1.20f, btn_r);
    BezelButton keys     = bezel_hit("##keys",       center, r,  kPi * 0.68f, btn_r);
    BezelButton plus     = bezel_hit("##zoom_plus",  center, r,  kPi * 0.20f, btn_r);
    BezelButton minus    = bezel_hit("##zoom_minus", center, r,  kPi * 0.32f, btn_r);

    auto* dl = ImGui::GetWindowDrawList();
    ViewUV view = compute_view_uv(h.x, h.y, h.locked);

    if (g_overlay.mosaic.resource) {
        dl->AddImageRounded((ImTextureID)g_overlay.mosaic.srv_gpu.ptr,
                            p_min, p_max, view.uv0, view.uv1,
                            IM_COL32_WHITE, r);
    } else {
        dl->AddCircleFilled(center, r, IM_COL32(20, 22, 30, 255), 64);
    }
    dl->AddCircle(center, r,        kColBezel,       96, 3.0f);
    dl->AddCircle(center, r - 2.0f, kColBezelShadow, 96, 1.0f);
    dl->AddLine({center.x, p_min.y}, {center.x, p_min.y + 10.0f},
                kColNorth, 2.5f);

    // POIs (clipped to the bezel disc).
    {
        const auto& pois = pois_get();
        const float clip_r  = r - 4.0f;
        const float clip_r2 = clip_r * clip_r;
        const float icon_size =
            (size <= 256.0f) ? 9.0f : (size <= 384.0f) ? 11.0f : 13.0f;
        const float shape_size = icon_size * 0.45f;
        ImTextureID atlas = g_overlay.poi_atlas.resource
            ? (ImTextureID)g_overlay.poi_atlas.srv_gpu.ptr : (ImTextureID)0;
        for (const auto& poi : pois) {
            if (!poi_passes_filter(poi)) continue;
            ImVec2 sp_local = player_to_screen(poi.x, poi.y, view, size);
            ImVec2 sp(p_min.x + sp_local.x, p_min.y + sp_local.y);
            float ddx = sp.x - center.x, ddy = sp.y - center.y;
            if (ddx * ddx + ddy * ddy > clip_r2) continue;
            ImVec2 uv0, uv1;
            if (atlas && pois_atlas_uv(poi, uv0, uv1)) {
                pois_draw_atlas(dl, atlas, sp, uv0, uv1, icon_size);
            } else {
                pois_draw_marker(dl, sp, pois_style(poi), shape_size);
            }
        }
    }

    if (!h.locked) {
        dl->AddText({center.x - 60.0f, center.y - 6.0f}, kColText,
                    "Waiting for Hero alloc...");
    } else {
        ImVec2 dot_local = player_to_screen(h.x, h.y, view, size);
        ImVec2 dot(p_min.x + dot_local.x, p_min.y + dot_local.y);
        float c = cosf((float)h.rot_z), s = sinf((float)h.rot_z);
        if (g_overlay.player_arrow.resource) {
            float arr =
                (size <= 256.0f) ? 9.0f : (size <= 384.0f) ? 11.0f : 13.0f;
            auto rot = [&](float lx, float ly) {
                return ImVec2(dot.x + lx * c - ly * s,
                              dot.y + lx * s + ly * c);
            };
            ImVec2 p0 = rot(-arr, -arr), p1 = rot(arr, -arr);
            ImVec2 p2 = rot( arr,  arr), p3 = rot(-arr, arr);
            dl->AddImageQuad((ImTextureID)g_overlay.player_arrow.srv_gpu.ptr,
                             p0, p1, p2, p3,
                             ImVec2(0, 0), ImVec2(1, 0),
                             ImVec2(1, 1), ImVec2(0, 1));
        } else {
            dl->AddLine(dot, {dot.x + c * 14.0f, dot.y + s * 14.0f},
                        kColPlayer, 2.0f);
            dl->AddCircleFilled(dot, 5.0f, kColPlayer, 16);
        }
    }

    bezel_draw_pin     (dl, pin);
    bezel_draw_size    (dl, sizeb, g_compass_size_idx);
    bezel_draw_collapse(dl, collapse);
    bezel_draw_filter  (dl, filter, g_filter_open);
    bezel_draw_key     (dl, keys,   g_keys_open);
    bezel_draw_plus    (dl, plus);
    bezel_draw_minus   (dl, minus);

    if (plus.clicked)  { g_calib.zoom += kZoomStep; if (g_calib.zoom > kZoomMax) g_calib.zoom = kZoomMax; }
    if (minus.clicked) { g_calib.zoom -= kZoomStep; if (g_calib.zoom < kZoomMin) g_calib.zoom = kZoomMin; }
    if (sizeb.clicked)    g_compass_size_idx = (g_compass_size_idx + 1) % 3;
    if (filter.clicked)   g_filter_open      = !g_filter_open;
    if (keys.clicked)     g_keys_open        = !g_keys_open;
    if (collapse.clicked) g_compass_collapsed = true;
    if (pin.active) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        if (d.x != 0.0f || d.y != 0.0f) {
            ImVec2 wp = ImGui::GetWindowPos();
            ImGui::SetWindowPos({wp.x + d.x, wp.y + d.y});
        }
    }
}

void render_minimap_window() {
    if (!g_minimap_visible.load()) return;
    HeroSnapshot h = hero_state_read();
    if (!h.locked) return;
    calib_maybe_reload();

    ImGuiWindowFlags wflags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2, 2));
    ImGui::SetNextWindowPos(ImVec2(600, 20), ImGuiCond_FirstUseEver);
    ImGui::Begin("minimap", nullptr, wflags);

    if (g_compass_collapsed) {
        const float puck = 36.0f;
        ImVec2 cmin = ImGui::GetCursorScreenPos();
        ImVec2 cc(cmin.x + puck * 0.5f, cmin.y + puck * 0.5f);
        ImGui::InvisibleButton("##puck", ImVec2(puck, puck));
        if (ImGui::IsItemActive()) {
            ImVec2 d = ImGui::GetIO().MouseDelta;
            if (d.x != 0.0f || d.y != 0.0f) {
                ImVec2 wp = ImGui::GetWindowPos();
                ImGui::SetWindowPos({wp.x + d.x, wp.y + d.y});
            }
        }
        if (ImGui::IsItemDeactivated()) {
            ImVec2 dd = ImGui::GetMouseDragDelta(0);
            if (std::fabs(dd.x) + std::fabs(dd.y) < 4.0f)
                g_compass_collapsed = false;
        }
        auto* dl = ImGui::GetWindowDrawList();
        float pr = puck * 0.5f;
        dl->AddCircleFilled(cc, pr, kColBtnFill, 32);
        dl->AddCircle(cc, pr, kColBezel, 32, 2.5f);
        float a = 5.0f;
        dl->AddQuad({cc.x, cc.y - a}, {cc.x + a, cc.y},
                    {cc.x, cc.y + a}, {cc.x - a, cc.y},
                    kColIcon, 1.8f);
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    render_compass(h);

    if (g_filter_open) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg,        kColBtnFill);
        ImGui::PushStyleColor(ImGuiCol_Border,         kColBezel);
        ImGui::PushStyleColor(ImGuiCol_FrameBg,        IM_COL32(22, 16, 8, 235));
        ImGui::PushStyleColor(ImGuiCol_CheckMark,      kColIcon);
        ImGui::PushStyleColor(ImGuiCol_Text,           kColText);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,   8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,   4.0f);
        ImGui::BeginChild("##filter_tablet", ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeX |
                              ImGuiChildFlags_AutoResizeY |
                              ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::Checkbox("Obelisks",   &g_filter.obelisks);
        ImGui::Checkbox("Respawns",   &g_filter.respawns);
        ImGui::Checkbox("Dungeons",   &g_filter.dungeons);
        ImGui::Checkbox("Merchants",  &g_filter.merchants);
        ImGui::Checkbox("Activities", &g_filter.activities);
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(5);
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

void render_fight_detail_window() {
    if (g_selected_fight_id == 0) return;
    if (!hero_state_read().locked) return;

    AggSnapshot snap = aggregator_snapshot();
    const FightLogEntry* f = nullptr;
    for (std::size_t i = 0; i < snap.history_count; ++i) {
        if (snap.history[i].id == g_selected_fight_id) {
            f = &snap.history[i];
            break;
        }
    }
    if (!f) {
        // Selected fight rolled off the history ring.
        g_selected_fight_id = 0;
        return;
    }

    // Local hh:mm:ss from the saved unix-ms timestamp.
    std::int64_t ft100ns =
        f->ended_unix_ms * 10000LL + 116444736000000000LL;
    FILETIME ft;
    ft.dwLowDateTime  = (DWORD)(ft100ns & 0xffffffff);
    ft.dwHighDateTime = (DWORD)(ft100ns >> 32);
    SYSTEMTIME utc, lt;
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &lt);

    ImGui::PushStyleColor(ImGuiCol_WindowBg,         kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_Border,           kColBezel);
    ImGui::PushStyleColor(ImGuiCol_TitleBg,          kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,    kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_Text,             kColText);
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg,    IM_COL32(48, 36, 16, 240));
    ImGui::PushStyleColor(ImGuiCol_TableRowBg,       IM_COL32( 0,  0,  0, 60));
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt,    IM_COL32( 0,  0,  0, 110));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, kColBezelShadow);
    ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, kColBezel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);

    char title[80];
    std::snprintf(title, sizeof(title),
                  "Fight #%d  %02u:%02u:%02u###fight_detail",
                  f->id, lt.wHour, lt.wMinute, lt.wSecond);

    ImGui::SetNextWindowSize(ImVec2(560, 320), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_FirstUseEver);

    bool open = true;
    if (ImGui::Begin(title, &open, ImGuiWindowFlags_NoScrollbar)) {
        ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.52f, 1.0f),
                           "duration %.1fs", f->duration_sec);
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::Text("total %.0f", f->total_damage);
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                           "DPS %.1f", f->dps);
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::TextDisabled("%d hits", f->hit_count);

        ImGui::Spacing();

        constexpr ImGuiTableFlags flags =
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_BordersOuter |
            ImGuiTableFlags_SizingStretchProp |
            ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("##fight_detail_skills", 7, flags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Skill", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Hits",  ImGuiTableColumnFlags_WidthStretch, 0.55f);
            ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Max",   ImGuiTableColumnFlags_WidthStretch, 0.85f);
            ImGui::TableSetupColumn("Crit%", ImGuiTableColumnFlags_WidthStretch, 0.65f);
            ImGui::TableSetupColumn("DPS",   ImGuiTableColumnFlags_WidthStretch, 0.85f);
            ImGui::TableSetupColumn("%",     ImGuiTableColumnFlags_WidthStretch, 0.55f);
            ImGui::TableHeadersRow();

            const float kIconPx = 22.0f;
            for (std::size_t i = 0; i < f->row_count; ++i) {
                const SkillRow& r = f->rows[i];
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                SkillGfx sgfx{};
                LoadedTexture* atlas_tex = nullptr;
                if (skill_resolve_lookup(r.skill, &sgfx)) {
                    atlas_tex = get_or_load_atlas(sgfx.atlas_filename);
                }
                if (atlas_tex && atlas_tex->resource && sgfx.size > 0 &&
                    atlas_tex->width > 0 && atlas_tex->height > 0) {
                    float aw  = (float)atlas_tex->width;
                    float ah  = (float)atlas_tex->height;
                    float px0 = (float)(sgfx.x * sgfx.size);
                    float py0 = (float)(sgfx.y * sgfx.size);
                    float pw  = (float)(sgfx.width  * sgfx.size);
                    float ph  = (float)(sgfx.height * sgfx.size);
                    ImVec2 uv0(px0 / aw, py0 / ah);
                    ImVec2 uv1((px0 + pw) / aw, (py0 + ph) / ah);
                    ImGui::Image((ImTextureID)atlas_tex->srv_gpu.ptr,
                                 ImVec2(kIconPx, kIconPx), uv0, uv1);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", r.skill);
                } else {
                    ImGui::TextUnformatted(r.skill);
                }

                ImGui::TableSetColumnIndex(1); ImGui::Text("%d", r.hit_count);
                ImGui::TableSetColumnIndex(2); ImGui::Text("%.0f", r.total);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%.0f", r.max_hit);

                ImGui::TableSetColumnIndex(4);
                float crit_pct = r.hit_count > 0
                    ? 100.0f * (float)r.crit_count / (float)r.hit_count : 0.0f;
                if (crit_pct > 0.0f) {
                    ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                                       "%.0f%%", crit_pct);
                } else {
                    ImGui::TextUnformatted("-");
                }

                ImGui::TableSetColumnIndex(5);
                double row_dps = r.total / f->duration_sec;
                ImGui::Text("%.1f", row_dps);

                ImGui::TableSetColumnIndex(6);
                float pct = f->total_damage > 0.001
                    ? 100.0f * (float)(r.total / f->total_damage) : 0.0f;
                ImGui::Text("%.0f%%", pct);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
    if (!open) g_selected_fight_id = 0;

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(10);
}

void render_keys_window() {
    if (!g_keys_open) return;
    if (!hero_state_read().locked) return;

    ImGui::PushStyleColor(ImGuiCol_WindowBg,      kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_Border,        kColBezel);
    ImGui::PushStyleColor(ImGuiCol_TitleBg,       kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_Text,          kColText);
    ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(60, 44, 20, 240));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(96, 70, 32, 245));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(40, 28, 12, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,    4.0f);

    ImGui::SetNextWindowPos(ImVec2(620, 540), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Hotkeys", &g_keys_open,
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoScrollbar)) {
        auto row = [](const char* label, RebindSlot slot, UINT vk) {
            int active  = g_rebind_listening.load();
            bool listen = (active == (int)slot);
            ImGui::PushID((int)slot);
            ImGui::TextUnformatted(label);
            ImGui::SameLine(140);
            const char* btn = listen ? "press a key..."
                                     : key_to_name(vk).c_str();
            if (listen) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      IM_COL32(96, 64, 24, 245));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      IM_COL32(120, 84, 32, 245));
            }
            if (ImGui::Button(btn, ImVec2(120, 0))) {
                g_rebind_listening.store(listen ? 0 : (int)slot);
            }
            if (listen) ImGui::PopStyleColor(2);
            ImGui::PopID();
        };
        row("Toggle DPS",     RebindSlot::Dps,   g_keybinds.toggle_dps);
        row("Toggle Minimap", RebindSlot::Map,   g_keybinds.toggle_minimap);
        row("Reset DPS",      RebindSlot::Reset, g_keybinds.reset_dps);

        if (g_rebind_listening.load() != 0) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.5f, 1.0f),
                               "Press any key (Esc = cancel)");
        } else {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.8f, 0.75f, 0.6f, 1.0f),
                               "Click a slot, then press a key.");
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(8);
}

LRESULT CALLBACK overlay_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto is_press = [lp]() { return (lp & (1u << 30)) == 0; };

    // Rebind capture: while listening, the next non-modifier keydown
    // becomes the new binding (ESC cancels). Must run before the
    // normal hotkey logic so pressing e.g. F10 while listening rebinds
    // rather than toggling DPS.
    int listening = g_rebind_listening.load();
    if (listening != 0) {
        bool capture = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
                       is_press();
        if (msg == WM_KEYUP || msg == WM_SYSKEYUP) return 0;
        if (capture) {
            UINT vk = (UINT)wp;
            if (vk == VK_ESCAPE) {
                g_rebind_listening.store(0);
                logf("keybinds: rebind cancelled");
                return 0;
            }
            // Skip pure modifier keys — wait for the real key.
            if (vk == VK_SHIFT   || vk == VK_CONTROL || vk == VK_MENU ||
                vk == VK_LSHIFT  || vk == VK_RSHIFT  ||
                vk == VK_LCONTROL|| vk == VK_RCONTROL||
                vk == VK_LMENU   || vk == VK_RMENU   ||
                vk == VK_LWIN    || vk == VK_RWIN) {
                return 0;
            }
            switch ((RebindSlot)listening) {
                case RebindSlot::Dps:   g_keybinds.toggle_dps     = vk; break;
                case RebindSlot::Map:   g_keybinds.toggle_minimap = vk; break;
                case RebindSlot::Reset: g_keybinds.reset_dps      = vk; break;
                default: break;
            }
            g_rebind_listening.store(0);
            keybinds_save();
            logf("keybinds: slot %d bound to %s",
                 listening, key_to_name(vk).c_str());
            return 0;
        }
    }

    // VK_F10 is the only key that arrives as WM_SYSKEYDOWN without
    // Alt being held (Windows reserves it for the menu accelerator),
    // so accept WM_SYSKEYDOWN only when the bound key is F10.
    bool is_keydown =
        (msg == WM_KEYDOWN) ||
        (msg == WM_SYSKEYDOWN && wp == VK_F10);

    if (is_keydown && is_press()) {
        UINT vk = (UINT)wp;
        if (vk == g_keybinds.toggle_dps) {
            bool now = !g_dps_visible.load();
            g_dps_visible.store(now);
            logf("overlay: %s DPS -> %s",
                 key_to_name(vk).c_str(), now ? "VISIBLE" : "HIDDEN");
            return 0;
        }
        if (vk == g_keybinds.toggle_minimap) {
            bool now = !g_minimap_visible.load();
            g_minimap_visible.store(now);
            logf("overlay: %s minimap -> %s",
                 key_to_name(vk).c_str(), now ? "VISIBLE" : "HIDDEN");
            return 0;
        }
        if (vk == g_keybinds.reset_dps) {
            aggregator_reset();
            return 0;
        }
    }

    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse) {
            switch (msg) {
                case WM_MOUSEMOVE:
                case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
                case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
                case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
                    return 0;
            }
        }
    }
    return CallWindowProcW(g_overlay.orig_wndproc, hwnd, msg, wp, lp);
}

void release_frame_targets() {
    for (auto& f : g_overlay.frames) {
        if (f.back_buffer) { f.back_buffer->Release(); f.back_buffer = nullptr; }
    }
}

void release_all() {
    release_frame_targets();
    for (auto& f : g_overlay.frames) {
        if (f.command_list) { f.command_list->Release(); f.command_list = nullptr; }
        if (f.allocator)    { f.allocator->Release();    f.allocator    = nullptr; }
    }
    g_overlay.frames.clear();
    if (g_overlay.rtv_heap)  { g_overlay.rtv_heap->Release();  g_overlay.rtv_heap  = nullptr; }
    if (g_overlay.srv_heap)  { g_overlay.srv_heap->Release();  g_overlay.srv_heap  = nullptr; }
    if (g_overlay.fence)     { g_overlay.fence->Release();     g_overlay.fence     = nullptr; }
    if (g_overlay.fence_event) {
        CloseHandle(g_overlay.fence_event);
        g_overlay.fence_event = nullptr;
    }
    if (g_overlay.device) { g_overlay.device->Release(); g_overlay.device = nullptr; }
}

bool wait_for_frame(FrameContext& frame, DWORD timeout_ms) {
    if (frame.fence_value == 0) return true;
    if (g_overlay.fence->GetCompletedValue() >= frame.fence_value) return true;
    g_overlay.fence->SetEventOnCompletion(frame.fence_value,
                                          g_overlay.fence_event);
    return WaitForSingleObject(g_overlay.fence_event, timeout_ms) ==
           WAIT_OBJECT_0;
}

bool create_back_buffer_targets(IDXGISwapChain3* swap_chain) {
    auto rtv_cpu_start =
        g_overlay.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g_overlay.back_buffer_count; ++i) {
        auto& f = g_overlay.frames[i];
        if (FAILED(swap_chain->GetBuffer(
                i, __uuidof(ID3D12Resource),
                reinterpret_cast<void**>(&f.back_buffer)))) {
            logf("overlay: GetBuffer(%u) failed", i);
            return false;
        }
        f.rtv_handle.ptr =
            rtv_cpu_start.ptr + i * g_overlay.rtv_descriptor_size;
        g_overlay.device->CreateRenderTargetView(f.back_buffer, nullptr,
                                                  f.rtv_handle);
    }
    return true;
}

bool overlay_init(IDXGISwapChain3* swap_chain, ID3D12CommandQueue* queue) {
    g_overlay.owned_swap_chain = swap_chain;
    if (FAILED(swap_chain->GetDevice(
            __uuidof(ID3D12Device),
            reinterpret_cast<void**>(&g_overlay.device)))) {
        logf("overlay: GetDevice failed");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap_chain->GetDesc(&desc))) {
        logf("overlay: swap_chain->GetDesc failed");
        return false;
    }
    g_overlay.hwnd              = desc.OutputWindow;
    g_overlay.back_buffer_count = desc.BufferCount;
    g_overlay.rt_format         = desc.BufferDesc.Format;
    if (g_overlay.back_buffer_count == 0 ||
        g_overlay.back_buffer_count > kMaxBackBuffers) {
        logf("overlay: unexpected back-buffer count %u",
             g_overlay.back_buffer_count);
        return false;
    }
    g_overlay.frames.assign(g_overlay.back_buffer_count, FrameContext{});

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = g_overlay.back_buffer_count;
    rtv_desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_overlay.device->CreateDescriptorHeap(
            &rtv_desc, __uuidof(ID3D12DescriptorHeap),
            reinterpret_cast<void**>(&g_overlay.rtv_heap)))) {
        logf("overlay: CreateDescriptorHeap(RTV) failed");
        return false;
    }
    g_overlay.rtv_descriptor_size =
        g_overlay.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
    srv_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.NumDescriptors = 64;  // 0 = ImGui font; 1-3 = minimap textures; rest reserved
    srv_desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_overlay.device->CreateDescriptorHeap(
            &srv_desc, __uuidof(ID3D12DescriptorHeap),
            reinterpret_cast<void**>(&g_overlay.srv_heap)))) {
        logf("overlay: CreateDescriptorHeap(SRV) failed");
        return false;
    }

    for (UINT i = 0; i < g_overlay.back_buffer_count; ++i) {
        auto& f = g_overlay.frames[i];
        if (FAILED(g_overlay.device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                __uuidof(ID3D12CommandAllocator),
                reinterpret_cast<void**>(&f.allocator)))) {
            logf("overlay: CreateCommandAllocator(%u) failed", i);
            return false;
        }
        if (FAILED(g_overlay.device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, f.allocator, nullptr,
                __uuidof(ID3D12GraphicsCommandList),
                reinterpret_cast<void**>(&f.command_list)))) {
            logf("overlay: CreateCommandList(%u) failed", i);
            return false;
        }
        f.command_list->Close();
    }

    if (FAILED(g_overlay.device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
            reinterpret_cast<void**>(&g_overlay.fence)))) {
        logf("overlay: CreateFence failed");
        return false;
    }
    g_overlay.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_overlay.fence_event) {
        logf("overlay: CreateEventW failed");
        return false;
    }
    g_overlay.next_fence_value = 0;

    if (!create_back_buffer_targets(swap_chain)) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.DisplaySize  = ImVec2(static_cast<float>(desc.BufferDesc.Width),
                             static_cast<float>(desc.BufferDesc.Height));
    io.DeltaTime    = 1.0f / 60.0f;
    io.LogFilename  = nullptr;

    // Persist window positions / sizes / collapsed state to disk.
    // Pointer lifetime is the whole process so we stash a static
    // UTF-8 copy. Path is <dll dir>\data\farever_layout.ini — a
    // dedicated file so we never clash with the game's own imgui.ini.
    {
        std::wstring dir = dll_dir() + L"\\data";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring wpath = dir + L"\\farever_layout.ini";
        static std::string s_ini_path;
        int n = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1,
                                    nullptr, 0, nullptr, nullptr);
        if (n > 0) {
            s_ini_path.resize(static_cast<std::size_t>(n - 1));
            WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1,
                                s_ini_path.data(), n, nullptr, nullptr);
            io.IniFilename = s_ini_path.c_str();
            logf("overlay: imgui layout file = %s", s_ini_path.c_str());
        } else {
            io.IniFilename = nullptr;
        }
    }
    ImGui::StyleColorsDark();

    // Pre-build the font atlas (lazy build inside NewFrame stack-
    // overflows the HashLink host thread).
    if (!io.Fonts->Build()) {
        logf("overlay: io.Fonts->Build() failed");
        return false;
    }

    auto srv_cpu = g_overlay.srv_heap->GetCPUDescriptorHandleForHeapStart();
    auto srv_gpu = g_overlay.srv_heap->GetGPUDescriptorHandleForHeapStart();
    if (!ImGui_ImplDX12_Init(g_overlay.device,
                             static_cast<int>(g_overlay.back_buffer_count),
                             g_overlay.rt_format, g_overlay.srv_heap,
                             srv_cpu, srv_gpu)) {
        logf("overlay: ImGui_ImplDX12_Init failed");
        return false;
    }
    if (!ImGui_ImplWin32_Init(g_overlay.hwnd)) {
        logf("overlay: ImGui_ImplWin32_Init failed");
        return false;
    }
    g_overlay.orig_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_overlay.hwnd, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(overlay_wndproc)));
    if (!g_overlay.orig_wndproc) {
        logf("overlay: SetWindowLongPtrW(GWLP_WNDPROC) failed");
        return false;
    }

    // Minimap assets — optional. Failure logs but doesn't abort init
    // (the DPS meter still works without the compass background).
    std::wstring mosaic_p = data_path(L"maps\\W1_Siagarta.preview.png");
    if (!load_texture_from_file(
            g_overlay.device, g_overlay.srv_heap, 1,
            mosaic_p.c_str(), &g_overlay.mosaic)) {
        logf("overlay: mosaic load failed; minimap will use solid bg");
    }
    std::wstring atlas_p = data_path(L"icons\\activities.png");
    if (!load_texture_from_file(
            g_overlay.device, g_overlay.srv_heap, 2,
            atlas_p.c_str(), &g_overlay.poi_atlas)) {
        logf("overlay: POI atlas load failed; falling back to shapes");
    }
    std::wstring arrow_p = data_path(L"icons\\PlayerMapArrow.png");
    if (!load_texture_from_file(
            g_overlay.device, g_overlay.srv_heap, 3,
            arrow_p.c_str(), &g_overlay.player_arrow)) {
        logf("overlay: player arrow load failed; falling back to dot");
    }
    std::wstring pois_p = data_path(L"pois_W1_Siagarta.json");
    pois_load(pois_p.c_str());

    logf("overlay: DX12+ImGui init OK (hwnd=%p, buffers=%u, fmt=%d)",
         g_overlay.hwnd, g_overlay.back_buffer_count,
         static_cast<int>(g_overlay.rt_format));
    return true;
}

void render_imgui_window() {
    // The aggregator runs unconditionally — pulls drain damage events
    // every frame so totals stay correct even with the window hidden.
    aggregator_tick();
    if (!g_dps_visible.load()) return;
    // Hold the UI back until the local Hero is locked (= we know
    // which character is the user). No point showing 0-damage rows
    // before the world is even visible.
    if (!hero_state_read().locked) return;
    AggSnapshot snap = aggregator_snapshot();

    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 360), ImGuiCond_FirstUseEver);

    ImGui::PushStyleColor(ImGuiCol_WindowBg,    kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_Border,      kColBezel);
    ImGui::PushStyleColor(ImGuiCol_TitleBg,     kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_Text,        kColText);
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, IM_COL32(48, 36, 16, 240));
    ImGui::PushStyleColor(ImGuiCol_TableRowBg,    IM_COL32( 0,  0,  0, 60));
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, IM_COL32( 0,  0,  0, 110));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, kColBezelShadow);
    ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, kColBezel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

    // No NoCollapse flag: ImGui's title-bar collapse arrow lets the
    // user shrink the window to just the title bar by clicking the
    // triangle next to "DPS Meter".
    ImGui::Begin("DPS Meter", nullptr, ImGuiWindowFlags_NoScrollbar);

    // Pick a layout tier from current content width — drives column
    // visibility, header/footer verbosity, icon scale. Thresholds tuned
    // by eyeballing default font metrics; ImGui's WindowAutoResize is
    // off here, so the user is in charge of the size and we adapt.
    float content_w = ImGui::GetContentRegionAvail().x;
    int tier = 3;
    if (content_w < 520.0f) tier = 2;
    if (content_w < 360.0f) tier = 1;
    if (content_w < 220.0f) tier = 0;

    const float kIconPx = (tier <= 1) ? 20.0f : (tier == 2 ? 22.0f : 24.0f);

    // Combat-state badge, leading the status line. Red dot while in
    // combat, dim grey circle once we've idled past the timeout.
    const ImVec4 col_active(1.0f, 0.30f, 0.30f, 1.0f);
    const ImVec4 col_idle  (0.55f, 0.55f, 0.55f, 1.0f);
    if (snap.in_combat) {
        ImGui::TextColored(col_active, "\xe2\x97\x8f");  // ●
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("in combat");
    } else {
        ImGui::TextColored(col_idle, "\xe2\x97\x8b");    // ○
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("idle");
    }
    ImGui::SameLine(0.0f, 8.0f);

    if (snap.have_fight) {
        if (tier >= 2) {
            ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.52f, 1.0f),
                               "elapsed %5.1fs", snap.elapsed_sec);
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::Text("total %.0f", snap.total_damage);
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                               "DPS %.1f", snap.dps);
        } else if (tier == 1) {
            ImGui::Text("total %.0f", snap.total_damage);
            ImGui::SameLine(0.0f, 16.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                               "DPS %.1f", snap.dps);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                               "%.0f DPS", snap.dps);
        }
    } else {
        // No fight yet — just keep the line height occupied by the
        // combat dot; no status text.
        ImGui::NewLine();
    }

    ImGui::Spacing();

    // Column set per tier. Each entry is (header, stretch_weight,
    // bitmask of what to render in the cell).
    enum Col { COL_SKILL = 0, COL_HITS, COL_TOTAL, COL_MAX,
               COL_CRIT, COL_DPS, COL_PCT };
    int  col_ids[7];
    int  col_count   = 0;
    auto add_col     = [&](int id) { col_ids[col_count++] = id; };

    add_col(COL_SKILL);
    if (tier >= 2) add_col(COL_HITS);
    if (tier >= 1) add_col(COL_TOTAL);
    if (tier >= 3) add_col(COL_MAX);
    if (tier >= 3) add_col(COL_CRIT);
    add_col(COL_DPS);
    if (tier >= 1) add_col(COL_PCT);

    ImGuiTableFlags table_flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_ScrollY;

    // Reserve bottom space only for the (optional) history block.
    static bool g_history_open = true;
    const float kLineH        = ImGui::GetTextLineHeightWithSpacing();
    const bool  show_hist     = (tier >= 2 && snap.history_count > 0);
    const float history_reserve =
        show_hist ? (g_history_open ? 130.0f : kLineH) : 0.0f;
    const float footer_h =
        (history_reserve > 0.0f) ? -history_reserve : 0.0f;

    if (ImGui::BeginTable("##skills", col_count, table_flags,
                          ImVec2(0.0f, footer_h))) {
        ImGui::TableSetupScrollFreeze(0, tier >= 2 ? 1 : 0);
        for (int c = 0; c < col_count; ++c) {
            const char* hdr  = "";
            float       wgt  = 1.0f;
            switch (col_ids[c]) {
                case COL_SKILL: hdr = "Skill"; wgt = (tier == 0) ? 0.8f : 1.4f; break;
                case COL_HITS:  hdr = "Hits";  wgt = 0.55f; break;
                case COL_TOTAL: hdr = "Total"; wgt = 1.0f;  break;
                case COL_MAX:   hdr = "Max";   wgt = 0.85f; break;
                case COL_CRIT:  hdr = "Crit%"; wgt = 0.65f; break;
                case COL_DPS:   hdr = "DPS";   wgt = 0.85f; break;
                case COL_PCT:   hdr = "%";     wgt = 0.55f; break;
            }
            ImGui::TableSetupColumn(
                hdr, ImGuiTableColumnFlags_WidthStretch, wgt);
        }
        if (tier >= 2) ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < snap.row_count; ++i) {
            const SkillRow& r = snap.rows[i];
            ImGui::TableNextRow();

            float crit_pct = r.hit_count > 0
                ? 100.0f * (float)r.crit_count / (float)r.hit_count : 0.0f;
            double row_dps = (snap.elapsed_sec > 0.001)
                ? r.total / snap.elapsed_sec : 0.0;
            float pct_total = snap.total_damage > 0.001
                ? 100.0f * (float)(r.total / snap.total_damage) : 0.0f;

            for (int c = 0; c < col_count; ++c) {
                ImGui::TableSetColumnIndex(c);
                switch (col_ids[c]) {
                    case COL_SKILL: {
                        SkillGfx sgfx{};
                        LoadedTexture* atlas_tex = nullptr;
                        if (skill_resolve_lookup(r.skill, &sgfx)) {
                            atlas_tex = get_or_load_atlas(sgfx.atlas_filename);
                        }
                        if (atlas_tex && atlas_tex->resource &&
                            sgfx.size > 0 &&
                            atlas_tex->width > 0 && atlas_tex->height > 0) {
                            float aw  = (float)atlas_tex->width;
                            float ah  = (float)atlas_tex->height;
                            float px0 = (float)(sgfx.x * sgfx.size);
                            float py0 = (float)(sgfx.y * sgfx.size);
                            float pw  = (float)(sgfx.width  * sgfx.size);
                            float ph  = (float)(sgfx.height * sgfx.size);
                            ImVec2 uv0(px0 / aw, py0 / ah);
                            ImVec2 uv1((px0 + pw) / aw, (py0 + ph) / ah);
                            ImGui::Image(
                                (ImTextureID)atlas_tex->srv_gpu.ptr,
                                ImVec2(kIconPx, kIconPx), uv0, uv1);
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("%s", r.skill);
                            }
                        } else {
                            ImGui::TextUnformatted(r.skill);
                        }
                        break;
                    }
                    case COL_HITS:  ImGui::Text("%d",  r.hit_count); break;
                    case COL_TOTAL: ImGui::Text("%.0f", r.total);     break;
                    case COL_MAX:   ImGui::Text("%.0f", r.max_hit);   break;
                    case COL_CRIT:
                        if (crit_pct > 0.0f) {
                            ImGui::TextColored(
                                ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                                "%.0f%%", crit_pct);
                        } else {
                            ImGui::TextUnformatted("-");
                        }
                        break;
                    case COL_DPS:   ImGui::Text("%.1f",  row_dps);   break;
                    case COL_PCT:   ImGui::Text("%.0f%%", pct_total); break;
                }
            }
        }
        ImGui::EndTable();
    }

    // Fight-history block: visible from tier 2 onward, collapsible.
    if (show_hist) {
        char hdr[64];
        std::snprintf(hdr, sizeof(hdr), "Fight history (%zu)###hist_hdr",
                      snap.history_count);
        ImGuiTreeNodeFlags hdr_flags =
            g_history_open ? ImGuiTreeNodeFlags_DefaultOpen : 0;
        bool was_open = g_history_open;
        g_history_open = ImGui::CollapsingHeader(hdr, hdr_flags);
        if (was_open != g_history_open) {
            // Force one extra frame so the table re-lays out with the
            // new reserve. ImGui auto-handles it next frame anyway.
        }
        if (g_history_open) {
            constexpr ImGuiTableFlags hist_flags =
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
            if (ImGui::BeginTable("##hist", 5, hist_flags,
                                  ImVec2(0.0f, history_reserve - kLineH))) {
                ImGui::TableSetupColumn("#",       ImGuiTableColumnFlags_WidthStretch, 0.35f);
                ImGui::TableSetupColumn("Time",    ImGuiTableColumnFlags_WidthStretch, 0.70f);
                ImGui::TableSetupColumn("Dur",     ImGuiTableColumnFlags_WidthStretch, 0.55f);
                ImGui::TableSetupColumn("Total",   ImGuiTableColumnFlags_WidthStretch, 0.85f);
                ImGui::TableSetupColumn("DPS",     ImGuiTableColumnFlags_WidthStretch, 0.85f);
                ImGui::TableHeadersRow();

                for (std::size_t i = 0; i < snap.history_count; ++i) {
                    const FightLogEntry& f = snap.history[i];

                    // FILETIME -> local hh:mm:ss
                    std::int64_t ft100ns =
                        f.ended_unix_ms * 10000LL + 116444736000000000LL;
                    FILETIME ft;
                    ft.dwLowDateTime  = (DWORD)(ft100ns & 0xffffffff);
                    ft.dwHighDateTime = (DWORD)(ft100ns >> 32);
                    SYSTEMTIME utc, lt;
                    FileTimeToSystemTime(&ft, &utc);
                    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &lt);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);

                    // Whole-row selectable: click opens the detail view
                    // for that fight (per-skill breakdown).
                    char sel[32];
                    std::snprintf(sel, sizeof(sel), "#%d##fight_%d",
                                  f.id, f.id);
                    bool is_sel = (g_selected_fight_id == f.id);
                    if (ImGui::Selectable(sel, is_sel,
                            ImGuiSelectableFlags_SpanAllColumns)) {
                        g_selected_fight_id =
                            is_sel ? 0 : f.id;   // toggle
                    }
                    if (f.top_skill[0] && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "top: %s   |   %d hits   |   click for details",
                            f.top_skill, f.hit_count);
                    }
                    ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%02u:%02u:%02u",
                                    lt.wHour, lt.wMinute, lt.wSecond);
                    ImGui::TableSetColumnIndex(2);
                        ImGui::Text("%.1fs", f.duration_sec);
                    ImGui::TableSetColumnIndex(3);
                        ImGui::Text("%.0f", f.total_damage);
                    ImGui::TableSetColumnIndex(4);
                        ImGui::TextColored(
                            ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                            "%.1f", f.dps);
                }
                ImGui::EndTable();
            }
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(10);
}

void overlay_render(IDXGISwapChain3* swap_chain, ID3D12CommandQueue* queue) {
    keybinds_maybe_reload();

    // Loading-screen guard: when we have no Hero lock, the game is
    // very likely between zones (loading / streaming / hxbit resync)
    // and is sometimes mid-recreation of its DX12 resources. Doing a
    // full render-and-execute through our overlay during that window
    // can crash the host's Present (observed AVs in DX12Driver.present
    // on dungeon entry). Skip the entire overlay path until we're
    // back on a known-good Hero.
    if (!hero_state_read().locked) {
        return;
    }

    UINT idx = swap_chain->GetCurrentBackBufferIndex();
    if (idx >= g_overlay.frames.size()) return;
    auto& frame = g_overlay.frames[idx];
    if (!frame.allocator || !frame.back_buffer) return;

    if (!wait_for_frame(frame, kFenceTimeoutMs)) {
        int n = ++g_consecutive_slow_frames;
        if (n == 1 || (n % 30) == 0) {
            logf("overlay: fence wait timed out (%d consecutive)", n);
        }
        if (n >= kAutoDisableSlowFrames) {
            g_overlay_enabled.store(false);
            logf("overlay: %d slow frames -> auto-disabled", n);
        }
        return;
    }
    g_consecutive_slow_frames = 0;

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    render_imgui_window();
    render_minimap_window();
    render_keys_window();
    render_fight_detail_window();
    ImGui::Render();

    frame.allocator->Reset();
    frame.command_list->Reset(frame.allocator, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = frame.back_buffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    frame.command_list->ResourceBarrier(1, &barrier);

    frame.command_list->OMSetRenderTargets(1, &frame.rtv_handle, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = {g_overlay.srv_heap};
    frame.command_list->SetDescriptorHeaps(1, heaps);

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), frame.command_list);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    frame.command_list->ResourceBarrier(1, &barrier);

    frame.command_list->Close();
    ID3D12CommandList* lists[] = {frame.command_list};
    queue->ExecuteCommandLists(1, lists);

    g_overlay.next_fence_value++;
    queue->Signal(g_overlay.fence, g_overlay.next_fence_value);
    frame.fence_value = g_overlay.next_fence_value;
}

}  // namespace

void overlay_on_present(IDXGISwapChain3* swap_chain,
                        ID3D12CommandQueue* queue) {
    if (g_overlay.init_failed) return;
    if (!queue) return;
    if (!g_overlay_enabled.load()) return;

    bool expected = false;
    if (!g_in_render.compare_exchange_strong(expected, true)) return;
    struct Scope { ~Scope() { g_in_render.store(false); } } scope;

    if (!g_overlay.initialized) {
        if (!overlay_init(swap_chain, queue)) {
            g_overlay.init_failed = true;
            release_all();
            return;
        }
        g_overlay.initialized = true;
    }
    if (swap_chain != g_overlay.owned_swap_chain) return;
    overlay_render(swap_chain, queue);
}

void overlay_on_resize(IDXGISwapChain3* swap_chain, UINT buffer_count,
                       UINT /*width*/, UINT /*height*/) {
    if (!g_overlay.initialized) return;
    if (swap_chain != g_overlay.owned_swap_chain) return;
    for (auto& f : g_overlay.frames) {
        if (!wait_for_frame(f, 1000)) {
            logf("overlay: resize drain timed out");
        }
        f.fence_value = 0;
    }
    release_frame_targets();
    if (buffer_count != 0 && buffer_count != g_overlay.back_buffer_count) {
        g_overlay.back_buffer_count = buffer_count;
        g_overlay.frames.resize(buffer_count);
    }
}

void overlay_after_resize(IDXGISwapChain3* swap_chain) {
    if (!g_overlay.initialized) return;
    if (swap_chain != g_overlay.owned_swap_chain) return;
    if (!create_back_buffer_targets(swap_chain)) {
        logf("overlay: re-creating RTVs after resize failed");
        g_overlay.init_failed = true;
    }
}

void overlay_shutdown() {
    if (!g_overlay.initialized && !g_overlay.init_failed) return;

    if (g_overlay.orig_wndproc && g_overlay.hwnd) {
        SetWindowLongPtrW(g_overlay.hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_overlay.orig_wndproc));
        g_overlay.orig_wndproc = nullptr;
    }
    if (g_overlay.initialized) {
        for (auto& f : g_overlay.frames) {
            if (!wait_for_frame(f, 1000)) {
                logf("overlay: shutdown drain timed out");
            }
        }
        release_texture(&g_overlay.mosaic);
        release_texture(&g_overlay.poi_atlas);
        release_texture(&g_overlay.player_arrow);
        for (auto& kv : g_atlas_cache) release_texture(&kv.second);
        g_atlas_cache.clear();
        g_next_atlas_slot = kSkillAtlasSlotBase;
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    release_all();
    g_overlay.initialized = false;
    g_overlay.init_failed = false;
}

}  // namespace farever
