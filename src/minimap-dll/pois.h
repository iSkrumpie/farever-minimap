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

}  // namespace farevermod
