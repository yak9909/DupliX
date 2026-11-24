#include "hook/screen_plane.h"

#include <windows.h>

#include <cstring>
#include <map>
#include <mutex>
#include <utility>

namespace rnd {

namespace {
std::mutex g_mu;
struct Plane {
    double m[16];
};
std::map<std::pair<int, int>, Plane>& Planes()
{
    static std::map<std::pair<int, int>, Plane> m;
    return m;
}
}  // namespace

bool SpikeScreenEnabled()
{
    static const bool on = [] {
        char v[8] = {0};
        return GetEnvironmentVariableA("RANDAMU_SPIKE_SCREEN", v, sizeof(v)) > 0 && v[0] == '1';
    }();
    return on;
}

void SpikeSetPlane(int compItemId, int layerId, const double m4[16])
{
    std::lock_guard<std::mutex> lk(g_mu);
    std::memcpy(Planes()[{compItemId, layerId}].m, m4, sizeof(double) * 16);
}

bool SpikeGetPlane(int compItemId, int layerId, double m4[16])
{
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = Planes().find({compItemId, layerId});
    if (it == Planes().end()) return false;
    std::memcpy(m4, it->second.m, sizeof(double) * 16);
    return true;
}

}  // namespace rnd
