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

    // Minimap textures. SRV slot 0 is ImGui's font atlas; slots 1-3
    // are reserved for mosaic / POI-atlas / player-arrow.
    LoadedTexture               mosaic{};
    LoadedTexture               poi_atlas{};
    LoadedTexture               player_arrow{};
};

Overlay           g_overlay;
std::atomic<bool> g_in_render{false};

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
bool g_compass_collapsed = false;

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
    bezel_draw_plus    (dl, plus);
    bezel_draw_minus   (dl, minus);

    if (plus.clicked)  { g_calib.zoom += kZoomStep; if (g_calib.zoom > kZoomMax) g_calib.zoom = kZoomMax; }
    if (minus.clicked) { g_calib.zoom -= kZoomStep; if (g_calib.zoom < kZoomMin) g_calib.zoom = kZoomMin; }
    if (sizeb.clicked)    g_compass_size_idx = (g_compass_size_idx + 1) % 3;
    if (filter.clicked)   g_filter_open      = !g_filter_open;
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
    calib_maybe_reload();
    HeroSnapshot h = hero_state_read();

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

LRESULT CALLBACK overlay_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto is_press = [lp]() { return (lp & (1u << 30)) == 0; };

    // F10 → DPS meter on/off. (Arrives as WM_SYSKEYDOWN too because
    // Windows treats it as a system key.)
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
        wp == VK_F10 && is_press()) {
        bool now = !g_dps_visible.load();
        g_dps_visible.store(now);
        logf("overlay: F10 DPS -> %s", now ? "VISIBLE" : "HIDDEN");
        return 0;
    }
    // F8 → minimap on/off (historical key from minimap-dll).
    if (msg == WM_KEYDOWN && wp == VK_F8 && is_press()) {
        bool now = !g_minimap_visible.load();
        g_minimap_visible.store(now);
        logf("overlay: F8 minimap -> %s", now ? "VISIBLE" : "HIDDEN");
        return 0;
    }
    // F9 → reset the current DPS pull.
    if (msg == WM_KEYDOWN && wp == VK_F9 && is_press()) {
        aggregator_reset();
        return 0;
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
    io.IniFilename  = nullptr;
    io.LogFilename  = nullptr;
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

    if (!snap.have_fight) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.5f, 1.0f),
                           "Waiting for damage...");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.52f, 1.0f),
                           "elapsed %5.1fs", snap.elapsed_sec);
    }
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::Text("total %10.0f", snap.total_damage);
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                       "DPS %9.1f", snap.dps);

    ImGui::Spacing();

    constexpr ImGuiTableFlags table_flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_ScrollY;

    if (ImGui::BeginTable("##skills", 7, table_flags,
                          ImVec2(0.0f, -ImGui::GetTextLineHeightWithSpacing()))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Skill",  ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("Hits",   ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("Total",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Max",    ImGuiTableColumnFlags_WidthStretch, 0.85f);
        ImGui::TableSetupColumn("Crit%",  ImGuiTableColumnFlags_WidthStretch, 0.65f);
        ImGui::TableSetupColumn("DPS",    ImGuiTableColumnFlags_WidthStretch, 0.85f);
        ImGui::TableSetupColumn("%",      ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < snap.row_count; ++i) {
            const SkillRow& r = snap.rows[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.skill);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%d",  r.hit_count);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.0f", r.total);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%.0f", r.max_hit);

            ImGui::TableSetColumnIndex(4);
            float crit_pct = r.hit_count > 0
                ? 100.0f * static_cast<float>(r.crit_count) /
                           static_cast<float>(r.hit_count)
                : 0.0f;
            if (crit_pct > 0.0f) {
                ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                                   "%.0f%%", crit_pct);
            } else {
                ImGui::TextUnformatted("-");
            }

            ImGui::TableSetColumnIndex(5);
            double row_dps = (snap.elapsed_sec > 0.001)
                ? r.total / snap.elapsed_sec : 0.0;
            ImGui::Text("%.1f", row_dps);

            ImGui::TableSetColumnIndex(6);
            float pct = snap.total_damage > 0.001
                ? 100.0f * static_cast<float>(r.total / snap.total_damage)
                : 0.0f;
            ImGui::Text("%.0f%%", pct);
        }
        ImGui::EndTable();
    }

    DamageStats st = damage_stats();
    ImGui::Text("damage source: %llu allocs, %llu events, "
                "%llu dropped(uninit), %llu dropped(garbage)   "
                "F10 DPS / F8 map / F9 reset",
                static_cast<unsigned long long>(st.allocs_seen),
                static_cast<unsigned long long>(st.events_emitted),
                static_cast<unsigned long long>(st.dropped_uninit),
                static_cast<unsigned long long>(st.dropped_garbage));

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(10);
}

void overlay_render(IDXGISwapChain3* swap_chain, ID3D12CommandQueue* queue) {
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
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    release_all();
    g_overlay.initialized = false;
    g_overlay.init_failed = false;
}

}  // namespace farever
