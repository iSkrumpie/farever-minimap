#include "poi_progress.h"
#include "pois.h"
#include "log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>

namespace farever {
namespace {

std::mutex                       g_mu;
std::unordered_set<std::string>  g_done;

// dll_dir is defined in overlay.cpp; we re-implement a minimal copy
// here so this TU stays standalone.
std::wstring my_dll_dir() {
    HMODULE hmod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&my_dll_dir),
        &hmod);
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(hmod, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    std::wstring s(buf);
    auto pos = s.find_last_of(L'\\');
    if (pos == std::wstring::npos) return L".";
    s.resize(pos);
    return s;
}

std::wstring done_json_path() {
    return my_dll_dir() + L"\\data\\poi_done.json";
}

}  // namespace

void poi_progress_load() {
    std::ifstream f(done_json_path());
    if (!f) {
        logf("poi_progress: no done file yet, starting fresh");
        return;
    }
    std::string text((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());

    // Tiny JSON walker — expects { "ids": ["...", "..."] }. We don't
    // need a real parser for this; just pluck string literals between
    // the array brackets.
    std::lock_guard<std::mutex> lk(g_mu);
    g_done.clear();
    auto arr_start = text.find('[');
    auto arr_end   = text.find(']', arr_start);
    if (arr_start == std::string::npos || arr_end == std::string::npos) {
        logf("poi_progress: malformed json, ignoring");
        return;
    }
    std::size_t i = arr_start + 1;
    while (i < arr_end) {
        auto q1 = text.find('"', i);
        if (q1 == std::string::npos || q1 >= arr_end) break;
        auto q2 = text.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 > arr_end) break;
        g_done.insert(text.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    logf("poi_progress: loaded %zu done IDs", g_done.size());
}

void poi_progress_save() {
    // Ensure data/ exists (same convention as keybinds_save).
    std::wstring dir = my_dll_dir() + L"\\data";
    CreateDirectoryW(dir.c_str(), nullptr);

    std::ofstream f(done_json_path());
    if (!f) {
        logf("poi_progress: failed to open done file for write");
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    f << "{\n  \"ids\": [\n";
    bool first = true;
    for (const auto& id : g_done) {
        if (!first) f << ",\n";
        f << "    \"" << id << "\"";
        first = false;
    }
    f << "\n  ]\n}\n";
}

void poi_progress_toggle(const char* poi_id) {
    if (!poi_id || !*poi_id) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_done.find(poi_id);
        if (it == g_done.end()) g_done.insert(poi_id);
        else                    g_done.erase(it);
    }
    poi_progress_save();
}

bool poi_progress_is_done(const char* poi_id) {
    if (!poi_id || !*poi_id) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    return g_done.find(poi_id) != g_done.end();
}

void poi_progress_counts(const char* kind, int* done_out, int* total_out) {
    int total = 0;
    int done  = 0;
    const auto& pois = pois_get();
    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto& p : pois) {
        if (std::strcmp(p.kind, kind) != 0) continue;
        ++total;
        if (p.id[0] && g_done.find(p.id) != g_done.end()) ++done;
    }
    if (done_out)  *done_out  = done;
    if (total_out) *total_out = total;
}

}  // namespace farever
