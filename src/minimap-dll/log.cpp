#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <share.h>
#include <string>

namespace farevermod {

namespace {
FILE* g_log = nullptr;
std::mutex g_log_mu;

std::wstring resolve_log_path() {
    // Sit the log file next to the DLL so the release ZIP is fully
    // self-contained.
    HMODULE hmod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&resolve_log_path),
        &hmod);
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(hmod, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"minimap.log";
    std::wstring s(path);
    auto pos = s.find_last_of(L'\\');
    if (pos == std::wstring::npos) return L"minimap.log";
    s.resize(pos);
    s += L"\\minimap.log";
    return s;
}

void timestamp(char* buf, size_t n) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, n, "%02d:%02d:%02d.%03d",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}
}  // namespace

void log_open() {
    std::lock_guard<std::mutex> g(g_log_mu);
    if (g_log) return;
    std::wstring log_path = resolve_log_path();
    // _wfsopen with _SH_DENYWR so other processes can still read the
    // file while the DLL holds it open. fopen "w" defaults to exclusive
    // on Windows which made live tailing impossible.
    g_log = _wfsopen(log_path.c_str(), L"w", _SH_DENYWR);
    if (g_log) {
        fputs("# minimap.dll loaded\n", g_log);
        fflush(g_log);
    }
}

void log_close() {
    std::lock_guard<std::mutex> g(g_log_mu);
    if (g_log) {
        fputs("# minimap.dll unloading\n", g_log);
        fclose(g_log);
        g_log = nullptr;
    }
}

void log_line(const std::string& s) {
    std::lock_guard<std::mutex> g(g_log_mu);
    if (!g_log) return;
    char ts[32];
    timestamp(ts, sizeof(ts));
    fprintf(g_log, "[%s] %s\n", ts, s.c_str());
    fflush(g_log);
}

void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_line(buf);
}

}  // namespace farevermod
