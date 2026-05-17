#pragma once

#include "libhl.h"

#include <cstdint>

namespace farever {

// Callback signature for hl_alloc_obj watchers. `obj` is the freshly
// allocated object pointer (return value of hl_alloc_obj). The
// hl_type is implicit — by the time a callback fires, the dispatcher
// has already matched it against the class name the watcher
// registered for.
//
// IMPORTANT: callbacks run on whatever HashLink thread does the
// allocation (main + GC threads). Keep them cheap and lock-light.
using AllocCallback = void(*)(std::uintptr_t obj);

// Install MinHook on hl_alloc_obj. Idempotent.
bool hl_hook_install(const LibHL& libhl);
void hl_hook_uninstall();

// v0.4.15: surgical disable of only the hl_alloc_obj hook (the
// regular hl_hook_uninstall calls MH_DisableHook(MH_ALL_HOOKS) which
// would also kill the D3D12 hooks). Used by the anticrash mode that
// removes our alloc-hook trampoline once the Hero lock is stable, so
// the game's HashLink allocator runs without any of our overhead.
// Idempotent. The trampoline is removed; future hl_alloc_obj calls
// from the game bypass us entirely.
void hl_hook_disable_alloc();

// v0.4.15.1: re-enable the hl_alloc_obj hook after a previous
// disable_alloc. Used by the anticrash self-heal path: when polling
// loses the lock (zone transition replaced both Hero AND Player),
// we re-arm the alloc-hook so the watcher catches the new Hero
// allocation, then disarm again 5 s after the next stable lock.
// The MinHook patch and watcher registrations are still in place
// from hl_hook_install; this just re-arms the trampoline.
bool hl_hook_re_enable_alloc();

// Register a watcher for a Haxe class. The dispatcher reads the class
// name from `hl_type.obj.name` on first allocation per type, matches
// it against registered names, and caches the hl_type* → watcher map
// so the steady-state path is one hash lookup. Register watchers
// BEFORE the hook installs (or accept a few allocations missed until
// the cache learns).
void hl_hook_register(const wchar_t* class_name, AllocCallback cb);

// Look up the hl_type* cached for a registered class. Returns 0 until
// at least one allocation of that class has flowed through dispatch.
// Used by hero_state to drive a heap rescan after a lock drop —
// dungeon exits can reuse an existing Hero without firing a fresh
// alloc, so the rescan is the only way to find it again.
std::uintptr_t hl_hook_get_type(const wchar_t* class_name);

}  // namespace farever
