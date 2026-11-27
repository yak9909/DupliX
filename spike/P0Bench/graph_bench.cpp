// P0 (design v4 section 8): is evaluating a graph of about 20 nodes over 10,000 elements every frame fast enough?
// A synthetic stand-in, not the P1 core: struct-of-arrays elements, nodes as passes over them, values from the time t
// and the element alone. Measured per frame: the plain chain, and Motion, which evaluates the upstream Generator and
// Transform again at each element's birth time t_i (design v4 section 5), with and without a cache of those
// evaluations by frame. Build: tools/bench_p0.ps1. Throwaway code (spike/README.md).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr float kPi = 3.14159265f;

struct Elements {
    std::vector<float> x, y, z, rx, ry, rz, sc, op, r, g, b, q, ti;
    std::vector<uint32_t> id;
    void Resize(size_t n)
    {
        for (auto* v : {&x, &y, &z, &rx, &ry, &rz, &sc, &op, &r, &g, &b, &q, &ti}) v->assign(n, 0.f);
        id.assign(n, 0);
    }
};

inline uint32_t Hash(uint32_t v)
{
    v ^= v >> 16, v *= 0x7feb352dU, v ^= v >> 15, v *= 0x846ca68bU, v ^= v >> 16;
    return v;
}
inline float Rand01(uint32_t id, uint32_t seed) { return (Hash(id * 2654435761U ^ Hash(seed)) & 0xffffff) / 16777216.f; }

// value noise in 3D with smooth interpolation (a stand-in for the Noise / turbulence modulators)
inline float Lattice(int x, int y, int z) { return Rand01((uint32_t)(x * 73856093 ^ y * 19349663 ^ z * 83492791), 7) * 2 - 1; }
inline float Smooth(float t) { return t * t * (3 - 2 * t); }
float Noise(float x, float y, float z)
{
    const int xi = (int)std::floor(x), yi = (int)std::floor(y), zi = (int)std::floor(z);
    const float fx = Smooth(x - xi), fy = Smooth(y - yi), fz = Smooth(z - zi);
    auto L = [&](int a, int b, int c) { return Lattice(xi + a, yi + b, zi + c); };
    const float x00 = L(0, 0, 0) + (L(1, 0, 0) - L(0, 0, 0)) * fx, x10 = L(0, 1, 0) + (L(1, 1, 0) - L(0, 1, 0)) * fx;
    const float x01 = L(0, 0, 1) + (L(1, 0, 1) - L(0, 0, 1)) * fx, x11 = L(0, 1, 1) + (L(1, 1, 1) - L(0, 1, 1)) * fx;
    const float y0 = x00 + (x10 - x00) * fy, y1 = x01 + (x11 - x01) * fy;
    return y0 + (y1 - y0) * fz;
}

// Generator (a 100 x 100 grid on a slowly turning plane) and Transform for element i at time t: the upstream that
// Motion evaluates again at t_i
inline void Upstream(size_t i, size_t cols, float t, float* px, float* py, float* pz)
{
    const float a = (float)(i % cols) / (cols - 1), s = (float)(i / cols) / (cols - 1);
    float x = (a - 0.5f) * 800, y = (s - 0.5f) * 800, z = 0;
    const float ang = 0.3f * t, c = std::cos(ang), sn = std::sin(ang);
    *px = x * c - z * sn + 960, *py = y + 540, *pz = x * sn + z * c;
}

struct Range {
    size_t a, b;
};
template <class F>
void ParallelFor(size_t n, int threads, F f)
{
    if (threads <= 1) {
        f(Range{0, n});
        return;
    }
    std::vector<std::thread> ts;
    for (int k = 0; k < threads; k++) ts.emplace_back([&, k] { f(Range{n * k / threads, n * (k + 1) / threads}); });
    for (auto& t : ts) t.join();
}

// One frame: about 20 nodes. motion: 0 none, 1 Upstream at t_i for every element, 2 the same through a cache by frame.
void Frame(Elements& e, size_t cols, float t, int threads, int motion, std::unordered_map<int, std::vector<float>>* cache)
{
    const size_t n = e.x.size();
    ParallelFor(n, threads, [&](Range rg) {
        for (size_t i = rg.a; i < rg.b; i++) {
            // 1 Generator + 2 Transform
            Upstream(i, cols, t, &e.x[i], &e.y[i], &e.z[i]);
            e.q[i] = (float)i / (n - 1);
            e.id[i] = Hash((uint32_t)i + 1);
            // 3 Stagger (Interval 1/30 s): t_i and r
            e.ti[i] = e.q[i] * 10.f;
            const float r = std::clamp((t - e.ti[i]) / 0.5f, 0.f, 1.f);
            // 4 LFO on position (Phase by q)
            e.y[i] += 20 * std::sin(2 * kPi * (0.5f * t + e.q[i] * 3));
            // 5 Noise displacement
            const float ns = 0.004f;
            e.x[i] += 30 * Noise(e.x[i] * ns, e.y[i] * ns, t * 0.5f);
            e.y[i] += 30 * Noise(e.x[i] * ns + 31.4f, e.y[i] * ns, t * 0.5f);
            e.z[i] += 30 * Noise(e.x[i] * ns, e.y[i] * ns + 17.f, t * 0.5f);
            // 6 Field from a point
            const float dx = e.x[i] - 960, dy = e.y[i] - 540, d = std::sqrt(dx * dx + dy * dy + e.z[i] * e.z[i]);
            const float field = std::clamp(1 - d / 400, 0.f, 1.f);
            // 7 Modify scale by the field, 8 opacity by the Stagger's Remaining
            e.sc[i] = 1 + 0.5f * field;
            e.op[i] = r;
            // 9 Random jitter of the rotation (by ID)
            e.rx[i] = 30 * (Rand01(e.id[i], 1) - 0.5f);
            e.ry[i] = 30 * (Rand01(e.id[i], 2) - 0.5f);
            e.rz[i] = 360 * Rand01(e.id[i], 3) + 45 * t;
            // 10 second LFO on scale, 11 Math (max of two), 12 Cascade step
            const float l2 = 0.5f + 0.5f * std::sin(2 * kPi * (t + e.q[i]));
            e.sc[i] *= std::max(l2, field);
            const float step = std::floor(e.q[i] * 5) / 5;
            // 13 Appearance: a gradient in OKLab-like space by q, 14 Tint by the field
            const float gq = std::clamp(e.q[i] + 0.1f * step, 0.f, 1.f);
            const float L = 0.4f + 0.5f * gq, A = 0.2f * std::cos(2 * kPi * gq), B = 0.2f * std::sin(2 * kPi * gq);
            const float l_ = L + 0.396f * A + 0.216f * B, m_ = L - 0.106f * A - 0.064f * B, s_ = L - 0.089f * A - 1.291f * B;
            const float l3 = l_ * l_ * l_, m3 = m_ * m_ * m_, s3 = s_ * s_ * s_;
            e.r[i] = std::clamp(4.077f * l3 - 3.308f * m3 + 0.231f * s3, 0.f, 1.f);
            e.g[i] = std::clamp(-1.268f * l3 + 2.610f * m3 - 0.341f * s3, 0.f, 1.f);
            e.b[i] = std::clamp(-0.004f * l3 - 0.704f * m3 + 1.708f * s3, 0.f, 1.f);
            e.r[i] += (1 - e.r[i]) * 0.3f * field;
            // 15 Motion: age, initial velocity outward, gravity, linear drag (closed form), turbulence fade-in
            const float age = t - e.ti[i];
            if (age > 0 && motion) {
                float bx, by, bz;
                if (motion == 1) {
                    Upstream(i, cols, e.ti[i], &bx, &by, &bz);
                } else {
                    // the frame cache: t_i on whole frames; one upstream evaluation per (frame, element)
                    const auto& f = (*cache).at((int)std::floor(e.ti[i] * 30));
                    bx = f[i * 3], by = f[i * 3 + 1], bz = f[i * 3 + 2];
                }
                const float k = 0.8f, vx = (bx - 960) * 0.5f, vy = (by - 540) * 0.5f - 200, gy = 400;
                const float ek = (1 - std::exp(-k * age)) / k, vt = gy / k;
                e.x[i] = bx + vx * ek;
                e.y[i] = by + (vy - vt) * ek + vt * age;
                e.z[i] = bz + 20 * Noise(bx * 0.01f, by * 0.01f, age) * std::min(age, 1.f);
            }
            // 16 Life (fade over the last second), 17 Transform (global), 18 Billboard-ish rotation, 19 depth fade
            e.op[i] *= std::clamp(4 - age, 0.f, 1.f);
            e.x[i] = (e.x[i] - 960) * 1.1f + 960;
            e.ry[i] -= 0.3f * t * 180 / kPi;
            e.op[i] *= std::clamp(1 - e.z[i] / 2000, 0.f, 1.f);
        }
    });
    // 20 Sort by depth (the draw order of Fast)
    std::vector<uint32_t> order(n);
    for (size_t i = 0; i < n; i++) order[i] = (uint32_t)i;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return e.z[a] < e.z[b]; });
    volatile uint32_t sink = order[n / 2];
    (void)sink;
}

}  // namespace

int main()
{
    const size_t cols = 100, n = cols * cols;
    Elements e;
    e.Resize(n);
    // the frame cache for Motion: the upstream of every element at every whole frame of the births (10 s = 300 frames)
    std::unordered_map<int, std::vector<float>> cache;
    const auto c0 = std::chrono::steady_clock::now();
    for (int f = 0; f <= 300; f++) {
        auto& v = cache[f];
        v.resize(n * 3);
        for (size_t i = 0; i < n; i++) Upstream(i, cols, f / 30.f, &v[i * 3], &v[i * 3 + 1], &v[i * 3 + 2]);
    }
    const double cacheMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
    printf("elements %zu, nodes ~20; motion cache (301 frames x %zu, %.1f MB) built in %.1f ms\n", n, n,
           301.0 * n * 12 / 1e6, cacheMs);
    const int hw = (int)std::max(1u, std::thread::hardware_concurrency());
    for (int threads : {1, std::min(hw, 8)}) {
        for (int motion : {0, 1, 2}) {
            double best = 1e9, sum = 0;
            const int frames = 60;
            for (int k = 0; k < frames; k++) {
                const auto t0 = std::chrono::steady_clock::now();
                Frame(e, cols, 3.0f + k / 30.f, threads, motion, &cache);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                best = std::min(best, ms), sum += ms;
            }
            printf("threads %d motion %s: avg %.2f ms, best %.2f ms per frame\n", threads,
                   motion == 0 ? "off      " : motion == 1 ? "re-eval  " : "cached   ", sum / frames, best);
        }
    }
    return 0;
}
