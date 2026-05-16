#pragma once

#include <cstdint>

namespace farever {

// User-managed "I've already done this one" state for the minimap.
// Right-clicking a POI on the minimap toggles it. Persisted to
// data/poi_done.json so it survives restarts.
void poi_progress_load();
void poi_progress_save();

void poi_progress_toggle(const char* poi_id);
bool poi_progress_is_done(const char* poi_id);

// Count how many POIs of `kind` are marked done and how many exist
// total in the loaded POI list. Both out-pointers may be null.
void poi_progress_counts(const char* kind, int* done_out, int* total_out);

}  // namespace farever
