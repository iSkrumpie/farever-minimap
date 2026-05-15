#pragma once

#include "live_position.h"   // LivePosition shape

namespace farevermod {

// Background-thread heap scan to find the local player's ent.Hero
// inside our own process. Replaces the find_me.py loop + JSON IPC.
//
// Lifecycle:
//   hero_scan_start()  — launches the scan thread. Returns immediately.
//   hero_scan_read()   — fast: dereferences the locked Hero pointer to
//                        read posx/y/z/rot_z. Returns valid=false until
//                        the scan completes; once locked, every call is
//                        a few f64 loads.
//   hero_scan_stop()   — joins the scan thread; safe to call after stop.
void          hero_scan_start();
void          hero_scan_stop();
LivePosition  hero_scan_read();

// True once the background scan has located the local Hero. After that,
// hero_scan_read() returns valid live coords every frame.
bool          hero_scan_locked();
bool          hero_scan_failed();

}  // namespace farevermod
