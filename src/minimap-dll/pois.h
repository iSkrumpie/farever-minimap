#pragma once

#include <cstdint>
#include <vector>

#include "imgui.h"

namespace farevermod {

// One POI row, loaded from research/pois_<world>.json. Strings are
// small fixed-size buffers so we don't allocate per-POI on each render.
struct PoiRow {
    char  kind[24];
    char  subkind[24];
    char  name[64];
    float x;
    float y;
    float z;
};

bool pois_load(const wchar_t* path);
const std::vector<PoiRow>& pois_get();

struct PoiStyle {
    ImU32 color;
    int   shape;  // 0=circle 1=square 2=triangle 3=diamond 4=cross
};

PoiStyle pois_style(const PoiRow& p);
void pois_draw_marker(ImDrawList* dl, ImVec2 pos, PoiStyle style,
                      float size);

// Atlas sprite location: returns true and fills uv0/uv1 in [0,1] with
// the icon's region inside the activities atlas (8 cols x 2 rows).
// Returns false for POI kinds we don't have an icon for (caller should
// fall back to the geometric marker).
bool pois_atlas_uv(const PoiRow& p, ImVec2& uv0, ImVec2& uv1);

// Draw a POI icon from the activities-atlas at `pos`, sized to `size`
// (half-extent). `atlas_srv` is the ImTextureID-castable GPU SRV
// handle.
void pois_draw_atlas(ImDrawList* dl, ImTextureID atlas_srv, ImVec2 pos,
                     const ImVec2& uv0, const ImVec2& uv1, float size);

}  // namespace farevermod
