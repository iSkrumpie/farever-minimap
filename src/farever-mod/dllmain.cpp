// Entry point for the unified Farever mod (ships as dinput8.dll).
//
// Boot order:
//   1. Game starts. Windows resolves dinput8.dll from the EXE dir and
//      loads us (because we're sitting next to Farever.exe).
//   2. DllMain ATTACH: open the log, load the real dinput8 proxy,
//      spawn a worker thread.
//   3. Worker thread waits for libhl.dll, resolves hl_alloc_obj,
//      registers all module watchers (damage source + hero state),
//      installs the hooks. The render thread then drives every
//      module's tick from the D3D12 Present callback.

#include "log.h"
#include "dinput8_proxy.h"
#include "libhl.h"
#include "hl_hook.h"
#include "damage.h"
#include "hero_state.h"
#include "skill_resolve.h"
#include "entity_state.h"
#include "d3d12_hook.h"
#include "overlay.h"

#include <windows.h>

namespace fv = farever;

namespace {

HMODULE   g_self = nullptr;
fv::LibHL g_libhl{};

DWORD WINAPI worker_thread(LPVOID) {
    fv::logf("worker: started");
    if (!fv::libhl_wait_and_resolve(&g_libhl)) {
        fv::logf("worker: libhl resolution failed — aborting mod startup");
        return 1;
    }
    // Each module registers its own hl_alloc_obj watcher. Registration
    // must happen BEFORE hl_hook_install so we don't miss the first
    // burst of allocations on the way out of probe_init.
    fv::skill_resolve_init(g_libhl);
    fv::damage_start(g_libhl);
    fv::hero_state_start();
    fv::entity_state_start();

    if (!fv::hl_hook_install(g_libhl)) {
        fv::logf("worker: hl_hook_install failed");
        return 2;
    }
    if (!fv::d3d12_hook_install()) {
        fv::logf("worker: d3d12_hook_install failed — render-thread "
                 "ticks won't fire");
    }
    fv::logf("worker: live — alloc-hook + d3d12 hook armed; "
             "damage_tick + hero_state_tick drive from Present");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            g_self = module;
            DisableThreadLibraryCalls(module);
            fv::log_open();
            fv::logf("DllMain ATTACH, PID=%lu", GetCurrentProcessId());
            if (!fv::dinput8_proxy_load()) {
                fv::logf("DllMain: dinput8 proxy load failed — input may "
                         "be broken, continuing anyway");
            }
            HANDLE t = CreateThread(nullptr, 0, worker_thread, nullptr, 0,
                                    nullptr);
            if (t) CloseHandle(t);
            break;
        }
        case DLL_PROCESS_DETACH:
            fv::overlay_shutdown();
            fv::d3d12_hook_uninstall();
            fv::hero_state_stop();
            fv::damage_stop();
            fv::hl_hook_uninstall();
            fv::dinput8_proxy_unload();
            fv::log_line("DllMain DETACH");
            fv::log_close();
            break;
    }
    return TRUE;
}
