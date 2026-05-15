// Damage-event source. Hooks `st.skill.DamageResult` allocations via
// the hl_alloc_obj dispatcher, then reads the DR's fields once the
// Haxe constructor has populated them. The hook returns inside the
// allocator before the caller runs the constructor, so fields are
// still zero at hook time — we defer reads to a pump thread that
// retries each pending entry a few times before giving up.

#include "damage.h"
#include "hl_hook.h"
#include "mem_scan.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace farever {
namespace {

// st.skill.DamageResult layout (matches Python prototype + dpsmeter):
constexpr std::size_t OFF_DR_BASESKILL = 8;     // *Skill
constexpr std::size_t OFF_DR_AMOUNT    = 80;    // f64
constexpr std::size_t OFF_DR_HITCOUNT  = 88;    // i32
constexpr std::size_t OFF_DR_KILL      = 104;   // u8
constexpr std::size_t OFF_DR_CRITICAL  = 105;   // u8

// Skill (and BaseSkill): kind:String at +152
constexpr std::size_t OFF_SKILL_KIND   = 152;

// Haxe String: bytes:*u16 @ +8, length:i32 @ +16 (in chars)
constexpr std::size_t OFF_STR_BYTES = 8;
constexpr std::size_t OFF_STR_LEN   = 16;

// How many pump iterations to retry a pending DR before dropping it.
// 1 iteration = 50 ms; 20 retries = ~1 s window for the constructor.
constexpr int kMaxPendingRetries = 20;

constexpr std::size_t kEventRingMax = 4096;

struct Pending {
    std::uintptr_t dr_ptr;
    int            retries;
};

std::atomic<bool>          g_running{false};
std::atomic<std::uint64_t> g_allocs_seen{0};
std::atomic<std::uint64_t> g_events_emitted{0};
std::atomic<std::uint64_t> g_dropped_uninit{0};
std::atomic<std::uint64_t> g_dropped_garbage{0};
std::thread                g_pump_thread;

// HashLink GC thread-registration entry points; populated by
// damage_start and consumed only by the pump thread.
using PFN_hl_register_thread   = void (*)(void* /*stack_top*/);
using PFN_hl_unregister_thread = void (*)();
using PFN_hl_blocking          = void (*)(bool);
PFN_hl_register_thread   g_hl_register_thread   = nullptr;
PFN_hl_unregister_thread g_hl_unregister_thread = nullptr;
PFN_hl_blocking          g_hl_blocking          = nullptr;

std::mutex                              g_pending_mu;
std::deque<Pending>                     g_pending;          // FIFO of unread allocs

std::mutex                              g_events_mu;
std::deque<DamageEvent>                 g_events;           // drainable ring
std::unordered_set<std::uintptr_t>      g_seen_dr;          // dedupe across pump iterations

// --- skill-name decoder ---------------------------------------------

bool decode_skill_name(std::uintptr_t dr_ptr, char out[64]) {
    out[0] = 0;
    std::uint64_t bs_u64 = 0;
    if (!mem_read_u64(dr_ptr + OFF_DR_BASESKILL, &bs_u64)) return false;
    auto bs = static_cast<std::uintptr_t>(bs_u64);
    if (!mem_is_userland(bs)) return false;

    std::uint64_t skind_u64 = 0;
    if (!mem_read_u64(bs + OFF_SKILL_KIND, &skind_u64)) return false;
    auto skind = static_cast<std::uintptr_t>(skind_u64);
    if (!mem_is_userland(skind)) return false;

    std::uint64_t bytes_u64 = 0;
    std::int32_t  length    = 0;
    if (!mem_read_u64(skind + OFF_STR_BYTES, &bytes_u64)) return false;
    if (!mem_is_userland(static_cast<std::uintptr_t>(bytes_u64))) return false;
    if (!mem_read_i32(skind + OFF_STR_LEN, &length)) return false;
    if (length <= 0 || length > 63) return false;

    std::uint8_t buf[128];
    if (!mem_read_bytes(static_cast<std::uintptr_t>(bytes_u64),
                        buf, static_cast<std::size_t>(length) * 2))
        return false;

    for (int i = 0; i < length; ++i) {
        std::uint16_t c = static_cast<std::uint16_t>(buf[i * 2]) |
                          (static_cast<std::uint16_t>(buf[i * 2 + 1]) << 8);
        if (c >= 128) return false;
        char ch = static_cast<char>(c);
        bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '.';
        if (!ok) return false;
        out[i] = ch;
    }
    out[length] = 0;
    return true;
}

// --- alloc-hook callback --------------------------------------------

void on_dr_alloc(std::uintptr_t dr_ptr) {
    if (!dr_ptr) return;
    g_allocs_seen.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_pending_mu);
    g_pending.push_back({dr_ptr, 0});
}

// --- pump -----------------------------------------------------------

bool try_decode(std::uintptr_t dr_ptr, DamageEvent* out) {
    std::int32_t hits = 0;
    if (!mem_read_i32(dr_ptr + OFF_DR_HITCOUNT, &hits)) return false;
    if (hits == 0) return false;                  // constructor still not done
    if (hits < 1 || hits > 50) {
        g_dropped_garbage.fetch_add(1, std::memory_order_relaxed);
        return true;                              // settle but skip emit
    }
    double damage = 0;
    if (!mem_read_f64(dr_ptr + OFF_DR_AMOUNT, &damage)) return false;
    if (!(damage > 0.0 && damage < 1e8)) {
        g_dropped_garbage.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    std::uint8_t crit = 0;
    std::uint8_t kill = 0;
    mem_read_u8(dr_ptr + OFF_DR_CRITICAL, &crit);
    mem_read_u8(dr_ptr + OFF_DR_KILL, &kill);

    char skill[64];
    if (!decode_skill_name(dr_ptr, skill)) {
        g_dropped_garbage.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    out->dr_ptr    = dr_ptr;
    out->damage    = damage;
    out->hit_count = hits;
    out->is_crit   = crit ? 1 : 0;
    out->is_kill   = kill ? 1 : 0;
    std::memcpy(out->skill, skill, sizeof(out->skill));
    return true;
}

void pump_iteration() {
    // Snapshot pending under the lock, then process without holding it.
    std::vector<Pending> work;
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        work.assign(g_pending.begin(), g_pending.end());
        g_pending.clear();
    }
    std::vector<Pending> retry;
    retry.reserve(work.size());
    std::vector<DamageEvent> emit;
    emit.reserve(work.size());

    for (Pending p : work) {
        DamageEvent ev{};
        if (try_decode(p.dr_ptr, &ev)) {
            if (ev.hit_count > 0) {
                emit.push_back(ev);
            }
        } else {
            p.retries++;
            if (p.retries < kMaxPendingRetries) {
                retry.push_back(p);
            } else {
                g_dropped_uninit.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (!retry.empty()) {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        // Put unsettled items back at the front (preserve FIFO order).
        for (auto it = retry.rbegin(); it != retry.rend(); ++it) {
            g_pending.push_front(*it);
        }
    }
    if (!emit.empty()) {
        std::lock_guard<std::mutex> lk(g_events_mu);
        for (const DamageEvent& ev : emit) {
            if (!g_seen_dr.insert(ev.dr_ptr).second) continue;
            g_events.push_back(ev);
            g_events_emitted.fetch_add(1, std::memory_order_relaxed);
            while (g_events.size() > kEventRingMax) g_events.pop_front();
        }
    }
}

void pump_main() {
    logf("damage_pump: thread starting");

    // Register with HashLink's GC. Without this the Boehm collector
    // can't enumerate our stack as a set of roots and can't pause us
    // during stop-the-world — both lead to inconsistent state with
    // hxbit's mid-deserialise field writes.
    int stack_marker = 0;
    if (g_hl_register_thread) {
        g_hl_register_thread(&stack_marker);
        logf("damage_pump: hl_register_thread OK (stack_top=%p)",
             static_cast<void*>(&stack_marker));
    } else {
        logf("damage_pump: WARNING hl_register_thread not resolved — "
             "running unregistered, expect races");
    }

    std::uint64_t last_logged_events = 0;
    DWORD last_log_tick = GetTickCount();
    while (g_running.load(std::memory_order_acquire)) {
        pump_iteration();

        DWORD now = GetTickCount();
        if (now - last_log_tick >= 5000) {
            std::uint64_t n = g_events_emitted.load();
            if (n != last_logged_events) {
                logf("damage_pump: stats — allocs=%llu, events=%llu "
                     "(+%llu since last), dropped(uninit)=%llu, "
                     "dropped(garbage)=%llu",
                     static_cast<unsigned long long>(g_allocs_seen.load()),
                     static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(n - last_logged_events),
                     static_cast<unsigned long long>(g_dropped_uninit.load()),
                     static_cast<unsigned long long>(g_dropped_garbage.load()));
                last_logged_events = n;
            }
            last_log_tick = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (g_hl_unregister_thread) g_hl_unregister_thread();
    logf("damage_pump: thread exiting, %llu allocs, %llu events, "
         "%llu dropped (uninit), %llu dropped (garbage)",
         static_cast<unsigned long long>(g_allocs_seen.load()),
         static_cast<unsigned long long>(g_events_emitted.load()),
         static_cast<unsigned long long>(g_dropped_uninit.load()),
         static_cast<unsigned long long>(g_dropped_garbage.load()));
}

}  // namespace

void damage_start(const LibHL& libhl) {
    if (g_running.exchange(true)) return;
    if (g_pump_thread.joinable()) g_pump_thread.join();
    g_allocs_seen.store(0);
    g_events_emitted.store(0);
    g_dropped_uninit.store(0);
    g_dropped_garbage.store(0);
    {
        std::lock_guard<std::mutex> lk(g_pending_mu); g_pending.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_events_mu);
        g_events.clear();
        g_seen_dr.clear();
    }
    g_hl_register_thread   = reinterpret_cast<PFN_hl_register_thread>(
        libhl.hl_register_thread);
    g_hl_unregister_thread = reinterpret_cast<PFN_hl_unregister_thread>(
        libhl.hl_unregister_thread);
    g_hl_blocking          = reinterpret_cast<PFN_hl_blocking>(
        libhl.hl_blocking);

    hl_hook_register(L"st.skill.DamageResult", on_dr_alloc);
    g_pump_thread = std::thread(pump_main);
    logf("damage: watcher registered for st.skill.DamageResult, "
         "pump thread up (GC-registered)");
}

void damage_stop() {
    g_running.store(false);
    if (g_pump_thread.joinable()) g_pump_thread.join();
}

std::size_t damage_drain(DamageEvent* out, std::size_t max) {
    if (!out || max == 0) return 0;
    std::lock_guard<std::mutex> lk(g_events_mu);
    std::size_t n = 0;
    while (n < max && !g_events.empty()) {
        out[n++] = g_events.front();
        g_events.pop_front();
    }
    return n;
}

DamageStats damage_stats() {
    DamageStats s{};
    s.allocs_seen      = g_allocs_seen.load(std::memory_order_relaxed);
    s.events_emitted   = g_events_emitted.load(std::memory_order_relaxed);
    s.dropped_uninit   = g_dropped_uninit.load(std::memory_order_relaxed);
    s.dropped_garbage  = g_dropped_garbage.load(std::memory_order_relaxed);
    s.damage_result_tag = 0;   // hook learns this; not directly exposed yet
    return s;
}

}  // namespace farever
