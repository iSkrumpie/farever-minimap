#include "pois.h"
#include "log.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace farevermod {

namespace {

std::vector<PoiRow> g_pois;

// Minimal JSON parser, just enough to read our pois_<world>.json:
//   - top level is an array
//   - each element is a flat object with string / number values
// `skip_value` knows to skip nested objects, arrays, strings, numbers,
// bools, null — so unknown keys / nested data is tolerated.
struct Parser {
    const char* p;
    const char* end;

    void skip_ws() {
        while (p < end &&
               (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            ++p;
    }
    bool peek(char c) { skip_ws(); return p < end && *p == c; }
    bool consume(char c) { skip_ws(); if (peek(c)) { ++p; return true; } return false; }

    bool parse_string(std::string& out) {
        skip_ws();
        if (p >= end || *p != '"') return false;
        ++p;
        out.clear();
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                char e = *p++;
                // Tolerant escape handling — these are the only ones
                // that appear in our data; anything else is passed
                // through as-is.
                switch (e) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case '"': out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    default:  out.push_back(e);    break;
                }
            } else {
                out.push_back(*p++);
            }
        }
        if (p < end) ++p;  // closing quote
        return true;
    }

    bool parse_number(double& out) {
        skip_ws();
        char* endp = nullptr;
        out = std::strtod(p, &endp);
        if (endp == p) return false;
        p = endp;
        return true;
    }

    void skip_value() {
        skip_ws();
        if (p >= end) return;
        char c = *p;
        if (c == '"') { std::string s; parse_string(s); return; }
        if (c == '{' || c == '[') {
            char open = c, close = (c == '{' ? '}' : ']');
            int depth = 0;
            while (p < end) {
                if (*p == '"') { std::string s; parse_string(s); continue; }
                if (*p == open)  ++depth;
                if (*p == close) { --depth; if (depth == 0) { ++p; return; } }
                ++p;
            }
            return;
        }
        if (std::isalpha(static_cast<unsigned char>(c))) {
            while (p < end &&
                   std::isalpha(static_cast<unsigned char>(*p))) ++p;
            return;
        }
        // Number / null fallback.
        double tmp; parse_number(tmp);
    }
};

void copy_into(char* dst, std::size_t n, const std::string& s) {
    std::size_t w = std::min(s.size(), n - 1);
    std::memcpy(dst, s.data(), w);
    dst[w] = '\0';
}

}  // namespace

bool pois_load(const wchar_t* path) {
    g_pois.clear();
    std::ifstream f(path);
    if (!f) {
        logf("pois: %ls not found", path);
        return false;
    }
    std::string s((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
    Parser ps{s.data(), s.data() + s.size()};

    if (!ps.consume('[')) {
        logf("pois: expected '[' at start of JSON");
        return false;
    }
    while (!ps.peek(']')) {
        if (!ps.consume('{')) {
            logf("pois: expected '{' inside array");
            return false;
        }
        PoiRow r{};
        r.kind[0] = r.subkind[0] = r.name[0] = '\0';
        r.x = r.y = r.z = 0.0f;

        while (!ps.peek('}')) {
            std::string key;
            if (!ps.parse_string(key)) break;
            if (!ps.consume(':'))     break;
            if (key == "kind") {
                if (ps.peek('"')) { std::string v; ps.parse_string(v); copy_into(r.kind, sizeof(r.kind), v); }
                else ps.skip_value();
            } else if (key == "subkind") {
                if (ps.peek('"')) { std::string v; ps.parse_string(v); copy_into(r.subkind, sizeof(r.subkind), v); }
                else ps.skip_value();
            } else if (key == "name") {
                if (ps.peek('"')) { std::string v; ps.parse_string(v); copy_into(r.name, sizeof(r.name), v); }
                else ps.skip_value();
            } else if (key == "x") {
                double v; if (ps.parse_number(v)) r.x = static_cast<float>(v);
                else ps.skip_value();
            } else if (key == "y") {
                double v; if (ps.parse_number(v)) r.y = static_cast<float>(v);
                else ps.skip_value();
            } else if (key == "z") {
                double v; if (ps.parse_number(v)) r.z = static_cast<float>(v);
                else ps.skip_value();
            } else {
                ps.skip_value();
            }
            if (!ps.consume(',')) break;
        }
        if (!ps.consume('}')) {
            logf("pois: expected '}' at end of object");
            return false;
        }
        g_pois.push_back(r);
        if (!ps.consume(',')) break;
    }

    logf("pois: loaded %zu rows from %ls", g_pois.size(), path);
    return true;
}

const std::vector<PoiRow>& pois_get() { return g_pois; }

// Atlas layout: UI/icons/activities.png is an 8x2 grid of diamond
// icons. Mapping inferred visually from the extracted sprite sheet —
// some assignments (TimerCollectRun, WorldPlant) are best-guess until
// we cross-check with the in-game world map.
struct AtlasIdx { int col; int row; };

AtlasIdx atlas_index_for(const PoiRow& p) {
    // Mapping verified against the in-game atlas:
    //   row 0:  skull, orb-chest, dungeon, weapon-swords, obelisk-tower,
    //           respawn (Wiedereinstieg), <unused>, <unused>
    //   row 1:  <unused>, jump-and-run, normal-chest, grey-?, grey-!,
    //           green-!, orange-!, green-check
    // Items the player hasn't seen in-game yet ((6-7,0), (0,1), (6-7,1))
    // are left out — they may correspond to event/seasonal POI types we
    // don't currently identify.
    auto eq = [](const char* a, const char* b) { return std::strcmp(a, b) == 0; };
    if (eq(p.kind, "obelisk"))  return { 4, 0 };  // tower
    if (eq(p.kind, "respawn"))  return { 5, 0 };  // Wiedereinstieg
    if (eq(p.kind, "dungeon"))  return { 2, 0 };  // dungeon
    if (eq(p.kind, "merchant")) return { 7, 0 };  // orange speech bubble
    if (eq(p.kind, "activity")) {
        if (eq(p.subkind, "WorldElite"))      return { 0, 0 };  // skull
        if (eq(p.subkind, "FightStone"))      return { 2, 1 };  // chest
        if (eq(p.subkind, "ChestOrb"))        return { 1, 0 };  // orb chest
        if (eq(p.subkind, "Ascension"))       return { 1, 1 };  // d-pad
        if (eq(p.subkind, "MountRush"))       return { 0, 1 };  // blue seal
        if (eq(p.subkind, "TimerCollectRun")) return { 0, 1 };  // blue seal
        if (eq(p.subkind, "WorldCamp"))       return { 7, 0 };  // orange chat
        if (eq(p.subkind, "WorldPlant"))      return { 7, 0 };  // orange chat
    }
    return { -1, -1 };
}

bool pois_atlas_uv(const PoiRow& p, ImVec2& uv0, ImVec2& uv1) {
    AtlasIdx ix = atlas_index_for(p);
    if (ix.col < 0) return false;
    // Atlas: 1024x384, 128x128 cells -> 8 cols x 3 rows.
    const float cw = 128.0f / 1024.0f;
    const float ch = 128.0f /  384.0f;
    uv0 = ImVec2(ix.col * cw, ix.row * ch);
    uv1 = ImVec2(uv0.x + cw,  uv0.y + ch);
    return true;
}

void pois_draw_atlas(ImDrawList* dl, ImTextureID tex, ImVec2 pos,
                     const ImVec2& uv0, const ImVec2& uv1, float size) {
    ImVec2 a(pos.x - size, pos.y - size);
    ImVec2 b(pos.x + size, pos.y + size);
    dl->AddImage(tex, a, b, uv0, uv1);
}

PoiStyle pois_style(const PoiRow& p) {
    auto eq = [](const char* a, const char* b) { return std::strcmp(a, b) == 0; };
    if (eq(p.kind, "obelisk"))  return {IM_COL32(120, 220, 255, 255), 3};
    if (eq(p.kind, "respawn"))  return {IM_COL32(120, 220, 120, 255), 4};
    if (eq(p.kind, "merchant")) return {IM_COL32(255, 200,  80, 255), 0};
    if (eq(p.kind, "activity")) {
        if (eq(p.subkind, "WorldElite"))      return {IM_COL32(255,  80,  80, 255), 2};
        if (eq(p.subkind, "FightStone"))      return {IM_COL32(220, 100, 100, 255), 0};
        if (eq(p.subkind, "ChestOrb"))        return {IM_COL32(255, 220, 100, 255), 1};
        if (eq(p.subkind, "WorldCamp"))       return {IM_COL32(200, 100,  50, 255), 0};
        if (eq(p.subkind, "WorldPlant"))      return {IM_COL32( 80, 200,  80, 255), 0};
        if (eq(p.subkind, "Ascension"))       return {IM_COL32(220, 120, 255, 255), 3};
        if (eq(p.subkind, "MountRush"))       return {IM_COL32(180, 140,  80, 255), 0};
        if (eq(p.subkind, "TimerCollectRun")) return {IM_COL32(200, 200,  80, 255), 0};
    }
    return {IM_COL32(180, 180, 180, 255), 0};
}

void pois_draw_marker(ImDrawList* dl, ImVec2 pos, PoiStyle st, float size) {
    constexpr ImU32 outline = IM_COL32(0, 0, 0, 230);
    switch (st.shape) {
        case 1: {  // square
            ImVec2 a(pos.x - size, pos.y - size);
            ImVec2 b(pos.x + size, pos.y + size);
            dl->AddRectFilled(a, b, st.color);
            dl->AddRect(a, b, outline, 0.0f, 0, 1.0f);
            break;
        }
        case 2: {  // triangle (up)
            ImVec2 a(pos.x,                pos.y - size);
            ImVec2 b(pos.x - size * 0.9f,  pos.y + size * 0.7f);
            ImVec2 c(pos.x + size * 0.9f,  pos.y + size * 0.7f);
            dl->AddTriangleFilled(a, b, c, st.color);
            dl->AddTriangle(a, b, c, outline, 1.0f);
            break;
        }
        case 3: {  // diamond
            ImVec2 a(pos.x,        pos.y - size);
            ImVec2 b(pos.x + size, pos.y);
            ImVec2 c(pos.x,        pos.y + size);
            ImVec2 d(pos.x - size, pos.y);
            dl->AddQuadFilled(a, b, c, d, st.color);
            dl->AddQuad(a, b, c, d, outline, 1.0f);
            break;
        }
        case 4: {  // cross
            float t = 2.0f;
            dl->AddLine(ImVec2(pos.x - size, pos.y),
                        ImVec2(pos.x + size, pos.y), st.color, t);
            dl->AddLine(ImVec2(pos.x, pos.y - size),
                        ImVec2(pos.x, pos.y + size), st.color, t);
            dl->AddLine(ImVec2(pos.x - size, pos.y),
                        ImVec2(pos.x + size, pos.y), outline, 0.5f);
            dl->AddLine(ImVec2(pos.x, pos.y - size),
                        ImVec2(pos.x, pos.y + size), outline, 0.5f);
            break;
        }
        case 0:
        default: {  // circle
            dl->AddCircleFilled(pos, size, st.color, 12);
            dl->AddCircle(pos, size, outline, 12, 1.0f);
            break;
        }
    }
}

}  // namespace farevermod
