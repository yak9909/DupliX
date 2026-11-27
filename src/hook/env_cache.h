// DupliX hooks: environment variables read in hot paths, through a cache.
// GetEnvironmentVariableA scans AE's whole environment block (several µs a call); the hooks run thousands of times a
// frame. A cached value is read again at most every kEnvRefreshMs, so a test script that changes a variable between
// renders ($.setenv) still sees the change.
#pragma once
#include <windows.h>

#include <cstring>
#include <mutex>

namespace rnd {

constexpr unsigned long long kEnvRefreshMs = 250;

class EnvCache {
public:
    explicit EnvCache(const char* name) : name_(name) {}
    // Like GetEnvironmentVariableA(name, out, cap): the length (> 0 when set), the value copied into out (cap > 0).
    int Get(char* out, int cap)
    {
        const unsigned long long now = GetTickCount64();
        std::lock_guard<std::mutex> lk(mu_);
        if (now >= next_) {
            char v[sizeof(val_)] = {0};
            const DWORD n = GetEnvironmentVariableA(name_, v, sizeof(v));
            len_ = (int)n;  // 0: not set (a longer value: its needed size, still > 0; val_ then holds nothing)
            std::memcpy(val_, v, sizeof(val_));
            val_[sizeof(val_) - 1] = 0;
            next_ = now + kEnvRefreshMs;
        }
        if (out && cap > 0) {
            strncpy_s(out, (size_t)cap, val_, _TRUNCATE);
        }
        return len_;
    }
    // set and its first character is c
    bool Is(char c)
    {
        char v[4] = {0};
        return Get(v, sizeof(v)) > 0 && v[0] == c;
    }

private:
    const char* name_;
    std::mutex mu_;
    unsigned long long next_ = 0;
    char val_[64] = {0};
    int len_ = 0;
};

}  // namespace rnd
