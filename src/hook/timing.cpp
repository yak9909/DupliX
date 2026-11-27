#include "hook/timing.h"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <mutex>

#include "effect/duplix.h"

namespace rnd {

namespace {

struct Frame {
    bool used = false;
    int comp = 0;
    long long tKey = 0;  // frame time in microseconds
    long long first = 0, last = 0, lastSeen = 0;
    long long self[4] = {0, 0, 0, 0};
    int calls[4] = {0, 0, 0, 0};
};

constexpr int kFrames = 32;
Frame g_frames[kFrames];
std::mutex g_mu;

double Ms(long long ticks)
{
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return (double)f.QuadPart;
    }();
    return ticks * 1000.0 / freq;
}

void Emit(const Frame& f)
{
    RLog("timing: comp %d t=%.3f span=%.1fms in-plugin=%.1fms (hooks %.1fms/%d, prerender %.1fms/%d, render %.1fms/%d, "
         "copies %d)",
         f.comp, f.tKey / 1e6, Ms(f.last - f.first), Ms(f.self[0] + f.self[1] + f.self[2] + f.self[3]),
         Ms(f.self[TK_HOOK] + f.self[TK_COPY]), f.calls[TK_HOOK], Ms(f.self[TK_PRERENDER]), f.calls[TK_PRERENDER],
         Ms(f.self[TK_RENDER]), f.calls[TK_RENDER], f.calls[TK_COPY]);
}

// Frames quiet for more than 0.5 s are complete.
void FlushStale(long long now)
{
    static const long long quiet = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart / 2;
    }();
    for (auto& f : g_frames)
        if (f.used && now - f.lastSeen > quiet) {
            Emit(f);
            f = Frame();
        }
}

}  // namespace

bool TimingEnabled()
{
    static const bool on = [] {
        char v[8] = {0};
        return GetEnvironmentVariableA("DUPLIX_TIMING", v, sizeof(v)) > 0 && v[0] == '1';
    }();
    return on;
}

long long TimingNow()
{
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

void TimingMark(int compItemId, double tSec, TimingKind kind, long long t0, long long t1)
{
    if (!TimingEnabled()) return;
    long long key = std::llround(tSec * 1e6);
    std::lock_guard<std::mutex> lk(g_mu);
    FlushStale(t1);
    Frame* f = nullptr;
    Frame* free = nullptr;
    for (auto& x : g_frames) {
        if (x.used && x.comp == compItemId && x.tKey == key) { f = &x; break; }
        if (!x.used && !free) free = &x;
    }
    if (!f) {
        if (!free) {  // table full: emit the oldest
            free = &g_frames[0];
            for (auto& x : g_frames)
                if (x.lastSeen < free->lastSeen) free = &x;
            Emit(*free);
            *free = Frame();
        }
        f = free;
        f->used = true;
        f->comp = compItemId;
        f->tKey = key;
        f->first = t0;
        f->last = t1;
    }
    if (t0 < f->first) f->first = t0;
    if (t1 > f->last) f->last = t1;
    f->lastSeen = t1;
    f->self[kind] += t1 - t0;
    f->calls[kind]++;
}

void TimingFlushAll()
{
    if (!TimingEnabled()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& f : g_frames)
        if (f.used) {
            Emit(f);
            f = Frame();
        }
}

}  // namespace rnd
