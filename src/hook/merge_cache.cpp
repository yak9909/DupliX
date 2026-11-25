#include "hook/merge_cache.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

#include "effect/randamu.h"
#include "hook/bee_hooks.h"
#include "hook/merge_scene.h"

namespace rnd {

namespace {

MergePlanCache g_plans;  // by comp and comp time

// The plan last found for a layer at a layer time (the hooks after Count reuse it without reading the comp).
std::mutex g_lastMu;
std::map<std::pair<long long, long long>, std::shared_ptr<const MergePlan>> g_last;

// Diagnostics: RANDAMU_MERGE_DUMP names a file that gets the first plan of each log generation (camera, other
// surfaces, every copy's screen quad / world corners / group, the groups' depths and rects), one record per line.
void DumpPlan(const MergeSceneInput& in, const MergePlan& p)
{
    char path[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("RANDAMU_MERGE_DUMP", path, sizeof(path)) <= 0 || !path[0]) return;
    static LogLimit once;
    if (!once.Take(1)) return;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") || !f) return;
    fprintf(f, "cam dist %.6f w %.0f h %.0f m", p.cam.dist, p.cam.compW, p.cam.compH);
    for (double v : p.cam.camToWorld) fprintf(f, " %.9g", v);
    fprintf(f, "\n");
    for (const ScenePlane& s : in.planes) {
        fprintf(f, "plane unbounded %d rect %.3f %.3f %.3f %.3f m", (int)s.unbounded, s.rect[0], s.rect[1], s.rect[2], s.rect[3]);
        for (double v : s.toWorld) fprintf(f, " %.9g", v);
        fprintf(f, "\n");
    }
    for (const auto& c : p.debugCopies) {
        fprintf(f, "copy %d %d group %d z %.4f %.4f q", c.layer, c.copy, c.group, c.zMin, c.zMax);
        for (int k = 0; k < 4; k++) fprintf(f, " %.4f %.4f", c.x[k], c.y[k]);
        fprintf(f, " w");
        for (int k = 0; k < 4; k++) fprintf(f, " %.6f %.6f %.6f", c.corners[k][0], c.corners[k][1], c.corners[k][2]);
        fprintf(f, "\n");
    }
    for (const auto& kv : p.layers)
        for (const auto& cl : kv.second.clusters) {
            fprintf(f, "cluster depth %.6f rect %.0f %.0f %.0f %.0f copies", cl.depth, cl.rect[0], cl.rect[1], cl.rect[2], cl.rect[3]);
            for (int c : cl.copies) fprintf(f, " %d", c);
            fprintf(f, "\n");
        }
    fclose(f);
}

long long TimeKey(const A_Time& t)
{
    const double s = t.scale ? (double)t.value / (double)t.scale : 0.0;
    return (long long)std::llround(s * 1e6);
}

bool LayerKeyOf(const void* layer, int* comp, int* id)
{
    SPBasicSuite* sp = HookPica();
    AEGP_LayerSuite9* ls = nullptr;
    AEGP_CompSuite12* cs = nullptr;
    AEGP_ItemSuite9* is = nullptr;
    if (!sp || sp->AcquireSuite(kAEGPLayerSuite, kAEGPLayerSuiteVersion9, (const void**)&ls) || !ls) return false;
    bool ok = false;
    if (!sp->AcquireSuite(kAEGPCompSuite, kAEGPCompSuiteVersion12, (const void**)&cs) && cs) {
        if (!sp->AcquireSuite(kAEGPItemSuite, kAEGPItemSuiteVersion9, (const void**)&is) && is) {
            AEGP_CompH c = nullptr;
            AEGP_ItemH item = nullptr;
            A_long itemId = 0;
            AEGP_LayerIDVal lid = 0;
            ok = !ls->AEGP_GetLayerParentComp((AEGP_LayerH)layer, &c) && c && !cs->AEGP_GetItemFromComp(c, &item) &&
                 !is->AEGP_GetItemID(item, &itemId) && !ls->AEGP_GetLayerID((AEGP_LayerH)layer, &lid);
            *comp = (int)itemId;
            *id = (int)lid;
            sp->ReleaseSuite(kAEGPItemSuite, kAEGPItemSuiteVersion9);
        }
        sp->ReleaseSuite(kAEGPCompSuite, kAEGPCompSuiteVersion12);
    }
    sp->ReleaseSuite(kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    return ok;
}

}  // namespace

std::shared_ptr<const MergePlan> MergePlanFor(const void* layer, const A_Time& layerTime, bool reread)
{
    int comp = 0, id = 0;
    if (!LayerKeyOf(layer, &comp, &id)) return nullptr;
    const auto lastKey = std::make_pair(MergeLayerKey(comp, id), TimeKey(layerTime));
    if (!reread) {
        std::lock_guard<std::mutex> lk(g_lastMu);
        auto it = g_last.find(lastKey);
        if (it != g_last.end()) return it->second;
    }
    MergeSceneInput in;
    if (!ReadMergeScene(layer, layerTime, &in)) {
        static LogLimit warned;
        if (warned.Take(5)) RLog("merge: scene read failed (comp %d layer %d): no merging", comp, id);
        return nullptr;
    }
    // the comp time is the camera's frame: key by comp and the scene's own hash (the comp time is in it through
    // every transform); one entry per comp keeps the latest frame
    // the log generation (changes only when a test switches logs): each test render builds and logs its own plan
    const unsigned long long h = HashMergeScene(in) ^ ((unsigned long long)RLogGeneration() * 0x9E3779B97F4A7C15ull);
    auto plan = g_plans.Get(((long long)comp << 20) ^ (long long)(h & 0xFFFFF), h, [&in] {
        MergePlan p = BuildMergePlan(in);
        // test only (tests/ae/merge.jsx "seam"): every group plane at the camera depth of a world point "x,y,z"
        char fv[96] = {0};
        double wp[3];
        if (GetEnvironmentVariableA("RANDAMU_MERGE_FORCE_DW", fv, sizeof(fv)) > 0 &&
            sscanf_s(fv, "%lf,%lf,%lf", &wp[0], &wp[1], &wp[2]) == 3) {
            double inv[16];
            if (Invert4(p.cam.camToWorld, inv)) {
                const double z = wp[0] * inv[2] + wp[1] * inv[6] + wp[2] * inv[10] + inv[14];
                for (auto& kv : p.layers)
                    for (auto& c : kv.second.clusters) c.depth = z;
                RLog("merge: test: group depth forced to %.2f", z);
            }
        }
        DumpPlan(in, p);
        static LogLimit logged;
        if (p.overBudget) RLog("merge: plan over budget (%d rounds): no merging this frame", p.rounds);
        if (logged.Take(20)) {
            RLog("merge: read scene ok=1 (plan: %d rounds, %.1f ms)", p.rounds, p.ms[0] + p.ms[1] + p.ms[2] + p.ms[3]);
            LogMergeScene(in);
            for (const auto& kv : p.layers)
                RLog("merge: plan layer %d/%d subs=%zu groups=%zu%s", kv.second.in.compItemId, kv.second.in.layerId,
                     kv.second.subs.size(), kv.second.clusters.size(),
                     kv.second.clusters.empty() ? "" : " (first group: depth and rect follow)");
            for (const auto& kv : p.layers)
                for (const auto& c : kv.second.clusters)
                    RLog("merge:   group copies=%zu depth=%.2f rect=(%.0f,%.0f,%.0f,%.0f)", c.copies.size(), c.depth, c.rect[0],
                         c.rect[1], c.rect[2], c.rect[3]);
        }
        return p;
    });
    {
        std::lock_guard<std::mutex> lk(g_lastMu);
        if (g_last.size() > 4096) g_last.clear();
        g_last[lastKey] = plan;
    }
    return plan;
}

const MergeLayerPlan* MergeLayerOf(const MergePlan& plan, int compItemId, int layerId)
{
    auto it = plan.layers.find(MergeLayerKey(compItemId, layerId));
    if (it == plan.layers.end() || it->second.clusters.empty()) return nullptr;
    return &it->second;
}

}  // namespace rnd
