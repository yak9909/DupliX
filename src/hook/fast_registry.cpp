#include "hook/fast_registry.h"

#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>

namespace rnd {

namespace {

constexpr int kKeep = 64;  // MFR on many-core machines renders dozens of frames at once

struct Entry {
    long long v = 0;
    unsigned long long s = 0;  // 0 = empty
    double m[16];
};
struct Slots {
    Entry e[kKeep];
    int next = 0;  // ring position of the next new time
    int last = -1;  // entry registered last
};

std::mutex g_mu;
std::map<std::pair<int, int>, Slots>& Table()
{
    static std::map<std::pair<int, int>, Slots> t;
    return t;
}

bool SameTime(long long v1, unsigned long long s1, long long v2, unsigned long long s2)
{
    // |v| * s stays far below 2^63 for AE times (values ~1e9 at most, scales ~1e5)
    return v1 * (long long)s2 == v2 * (long long)s1;
}

int Find(const Slots& sl, long long v, unsigned long long s)
{
    for (int i = 0; i < kKeep; i++)
        if (sl.e[i].s && SameTime(sl.e[i].v, sl.e[i].s, v, s)) return i;
    return -1;
}

}  // namespace

void FastSetPlane(LayerKey k, long long tValue, unsigned long long tScale, const double m4[16])
{
    if (!tScale) tScale = 1;
    std::lock_guard<std::mutex> lk(g_mu);
    Slots& sl = Table()[{k.compItemId, k.layerId}];
    int i = Find(sl, tValue, tScale);
    if (i < 0) {
        i = sl.next;
        sl.next = (sl.next + 1) % kKeep;
    }
    sl.e[i].v = tValue;
    sl.e[i].s = tScale;
    std::memcpy(sl.e[i].m, m4, sizeof(double) * 16);
    sl.last = i;
}

bool FastGetPlane(LayerKey k, long long tValue, unsigned long long tScale, double m4[16], bool* exact)
{
    if (!tScale) tScale = 1;
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = Table().find({k.compItemId, k.layerId});
    if (it == Table().end() || it->second.last < 0) return false;
    const Slots& sl = it->second;
    int i = Find(sl, tValue, tScale);
    if (exact) *exact = i >= 0;
    if (i < 0) {  // nearest registered time (motion-blur sub-frames, or a frame not prepared yet)
        const double t = (double)tValue / (double)tScale;
        double best = 1e300;
        for (int j = 0; j < kKeep; j++) {
            if (!sl.e[j].s) continue;
            double dt = std::fabs((double)sl.e[j].v / (double)sl.e[j].s - t);
            if (dt < best) best = dt, i = j;
        }
    }
    std::memcpy(m4, sl.e[i].m, sizeof(double) * 16);
    return true;
}

void FastClear(LayerKey k)
{
    std::lock_guard<std::mutex> lk(g_mu);
    Table().erase({k.compItemId, k.layerId});
}

}  // namespace rnd
