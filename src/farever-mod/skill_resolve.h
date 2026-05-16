#pragma once

#include "libhl.h"

#include <cstddef>
#include <cstdint>

namespace farever {

struct SkillGfx {
    char atlas_filename[96];   // basename only, e.g. "atlas_class_Mage_96PX.png"
    int  x;        // cell index, not pixels (pixel = x * size)
    int  y;
    int  size;     // cell edge length in px (48 / 96 / ...)
    int  width;    // cell count horizontally (default 1)
    int  height;   // cell count vertically   (default 1)
};

void skill_resolve_init(const LibHL& libhl);

// Read BaseSkill.inf.gfx from a HashLink BaseSkill pointer, fill out.
// Returns false if any link in the chain is null / not HL-allocated.
// Must be called on a HashLink-registered thread (the render thread is
// fine — same constraint as damage_decode).
bool skill_resolve_query(std::uintptr_t base_skill_ptr, SkillGfx* out);

// String-keyed cache. skill_resolve_query results are stashed under the
// skill's `kind` string for the overlay to pull at render time without
// touching HashLink memory again.
void skill_resolve_cache(const char* skill_kind, const SkillGfx& gfx);
bool skill_resolve_lookup(const char* skill_kind, SkillGfx* out);

}  // namespace farever
