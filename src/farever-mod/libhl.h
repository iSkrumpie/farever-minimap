#pragma once

#include <cstdint>

namespace farever {

// Resolved exports from libhl.dll. Populated by libhl_wait_and_resolve.
// All function pointers stay valid until the process exits — libhl is
// never unloaded.
struct LibHL {
    // The classic Boehm-style allocator. Called by the HashLink VM to
    // create every heap object: vdynamic* hl_alloc_obj(hl_type* t).
    // Hooking this gives us a callback for every object birth, which
    // is the foundation of the alloc-based event source.
    void* hl_alloc_obj      = nullptr;

    // Diagnostic exports we may want later — not hooked, just resolved.
    void* hl_alloc_dynamic  = nullptr;
    void* hl_alloc_dynobj   = nullptr;
    void* hl_alloc_array    = nullptr;
    void* hl_gc_dump_memory = nullptr;

    // Thread registration for the GC. Any thread that reads HashLink
    // heap pointers must be registered or the Boehm-style collector
    // can race with it (stop-the-world signalling is per registered
    // thread, and unregistered threads' stacks/registers aren't
    // scanned as roots). Signatures:
    //   void hl_register_thread(void* stack_top);
    //   void hl_unregister_thread();
    //   void hl_blocking(bool b);   // true=we're sleeping/I/O bound, GC can advance without us
    void* hl_register_thread   = nullptr;
    void* hl_unregister_thread = nullptr;
    void* hl_blocking          = nullptr;

    void* libhl_base        = nullptr;   // HMODULE
};

// Block on a background thread until libhl.dll is loaded into the
// process, then GetProcAddress every entry above. Returns once at
// least hl_alloc_obj is resolved, or never (if libhl never loads —
// which shouldn't happen in a Farever process).
bool libhl_wait_and_resolve(LibHL* out);

}  // namespace farever
