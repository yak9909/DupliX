// DupliX 2 (design v4, 1.2 and 7): the editor's way into After Effects. What P0 settled (spike/P0Graph):
//   - a change is queued and written by AE's idle hook (a write from the window's own message handling never
//     returned from AEGP_SetStreamValue), as one undo group: the graph data, and the macros it set (Expose, presets)
//   - after the write, PF_RefreshAllWindows (else the Overview and UPDATE_PARAMS_UI wait for another action)
//   - the graph's revision: the idle hook compares it and reloads after AE's own undo
//   - the editor follows its instance: SEQUENCE_SETDOWN (any thread) marks it gone; its effect reference is never
//     used after that (AE crashed in P0 when it was)
//
// Tests: DUPLIX2_TEST_SCRIPT=<file> runs that editor script (graph/edit.h RunScript) on an instance when one of its
// parameters changes (a jsx sets Random Seed), through the same queue and write.
#include "v2/dx2.h"

#include <chrono>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

namespace dx2 {
namespace {

struct Write {
    AEGP_EffectRefH eff = nullptr;  // owned: disposed after the write (unless the instance went away)
    A_u_longlong instance = 0;
    std::string text;               // the new graph data
    std::vector<dxg::MacroWrite> macros;
    std::string why;
    std::chrono::steady_clock::time_point notBefore{};  // the idle hook waits until then
};

std::mutex g_mu;
std::vector<Write> g_queue;
std::set<A_u_longlong> g_gone;  // instances whose SEQUENCE_SETDOWN came

AEGP_StreamRefH Stream(AEGP_EffectRefH eff, int index)
{
    Suite<AEGP_StreamSuite6> ss(g_pica, kAEGPStreamSuite, kAEGPStreamSuiteVersion6);
    AEGP_StreamRefH st = nullptr;
    if (!ss || ss->AEGP_GetNewEffectStreamByIndex(g_pluginId, eff, index, &st)) return nullptr;
    return st;
}

// The graph data of an instance now (through AEGP).
bool ReadGraphText(AEGP_EffectRefH eff, std::string* out)
{
    Suite<AEGP_StreamSuite6> ss(g_pica, kAEGPStreamSuite, kAEGPStreamSuiteVersion6);
    AEGP_StreamRefH st = Stream(eff, P_GRAPH);
    if (!ss || !st) return false;
    AEGP_StreamValue2 v{};
    A_Time t{0, 1};
    bool ok = false;
    if (!ss->AEGP_GetNewStreamValue(g_pluginId, st, AEGP_LTimeMode_LayerTime, &t, FALSE, &v)) {
        *out = GraphText((PF_Handle)v.val.arbH);
        ok = true;
        ss->AEGP_DisposeStreamValue(&v);
    }
    ss->AEGP_DisposeStream(st);
    return ok;
}

void DisposeEffect(AEGP_EffectRefH eff)
{
    Suite<AEGP_EffectSuite4> es(g_pica, kAEGPEffectSuite, kAEGPEffectSuiteVersion4);
    if (es && eff) es->AEGP_DisposeEffect(eff);
}

// One undo step: the graph data and the macros that go with it.
void DoWrite(const Write& w)
{
    const auto t0 = std::chrono::steady_clock::now();
    Suite<AEGP_UtilitySuite6> util(g_pica, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6);
    Suite<AEGP_StreamSuite6> ss(g_pica, kAEGPStreamSuite, kAEGPStreamSuiteVersion6);
    Suite<AEGP_KeyframeSuite5> ks(g_pica, kAEGPKeyframeSuite, kAEGPKeyframeSuiteVersion5);
    if (!util || !ss) return;
    Log("write (%s): start", w.why.c_str());
    A_Err eStart = util->AEGP_StartUndoGroup("Edit DupliX Graph");
    A_Err eGraph = -1;
    AEGP_StreamRefH st = Stream(w.eff, P_GRAPH);
    AEGP_StreamValue2 v{};
    A_Time t{0, 1};
    if (st && !ss->AEGP_GetNewStreamValue(g_pluginId, st, AEGP_LTimeMode_LayerTime, &t, FALSE, &v)) {
        PF_Handle nh = NewGraphHandle(w.text);
        if (nh) {
            AEGP_StreamValue2 nv = v;
            nv.val.arbH = (AEGP_ArbBlockVal)nh;
            Log("write (%s): setting the graph", w.why.c_str());
            eGraph = ss->AEGP_SetStreamValue(g_pluginId, st, &nv);
            Log("write (%s): graph set (%d)", w.why.c_str(), (int)eGraph);
            Suite<PF_HandleSuite1> hs(g_pica, kPFHandleSuite, kPFHandleSuiteVersion1);
            if (hs) hs->host_dispose_handle(nh);  // SetStreamValue copies it (COPY_FUNC), it does not adopt it
        }
        ss->AEGP_DisposeStreamValue(&v);
    }
    if (st) ss->AEGP_DisposeStream(st);
    int macrosSet = 0;
    for (const dxg::MacroWrite& m : w.macros) {
        AEGP_StreamRefH ms = Stream(w.eff, P_MACRO + m.k);
        if (!ms) continue;
        A_long nk = 0;
        if (ks) ks->AEGP_GetStreamNumKFs(ms, &nk);
        if (nk > 0) {
            Log("write: Macro %d has keys: its value is left as it is", m.k + 1);
        } else {
            AEGP_StreamValue2 mv{};
            if (!ss->AEGP_GetNewStreamValue(g_pluginId, ms, AEGP_LTimeMode_LayerTime, &t, FALSE, &mv)) {
                AEGP_StreamValue2 nv = mv;
                nv.val.one_d = m.value;
                if (!ss->AEGP_SetStreamValue(g_pluginId, ms, &nv)) macrosSet++;
                ss->AEGP_DisposeStreamValue(&mv);
            }
        }
        ss->AEGP_DisposeStream(ms);
    }
    A_Err eEnd = util->AEGP_EndUndoGroup();
    // the Overview and UPDATE_PARAMS_UI at once (P0)
    Suite<PF_AdvAppSuite2> adv(g_pica, kPFAdvAppSuite, kPFAdvAppSuiteVersion2);
    if (adv) adv->PF_RefreshAllWindows();
    Log("write (%s): graph %zu bytes (start %d set %d end %d), %d of %zu macro(s) set, %.2f ms", w.why.c_str(), w.text.size(),
        (int)eStart, (int)eGraph, (int)eEnd, macrosSet, w.macros.size(),
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

}  // namespace

void EditorInstanceGone(A_u_longlong instance)
{
    if (!instance) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_gone.insert(instance);
}

A_Err IdleHook(AEGP_GlobalRefcon, AEGP_IdleRefcon, A_long* max_sleep)
{
    std::vector<Write> todo;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const auto now = std::chrono::steady_clock::now();
        std::vector<Write> later;
        for (Write& w : g_queue) (w.notBefore <= now ? todo : later).push_back(std::move(w));
        g_queue.swap(later);
        if (!todo.empty()) Log("idle: %zu write(s)", todo.size());
        // a write for an instance that went away is dropped, and its reference is not touched
        for (Write& w : todo)
            if (g_gone.count(w.instance)) {
                Log("write (%s): the instance went away, dropped", w.why.c_str());
                w.eff = nullptr;
            }
    }
    for (Write& w : todo) {
        if (!w.eff) continue;
        DoWrite(w);
        DisposeEffect(w.eff);
    }
    (void)max_sleep;
    return A_Err_NONE;
}

void OpenEditor(PF_InData* in_data)
{
    (void)in_data;
    Log("editor: not built yet (P1 c)");
}

void MaybeRunTestScript(PF_InData* in_data, int changed)
{
    char path[MAX_PATH] = {0};
    if (changed != P_SEED || !GetEnvironmentVariableA("DUPLIX2_TEST_SCRIPT", path, sizeof(path))) return;
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::stringstream buf;
    buf << f.rdbuf();
    const std::string script = buf.str();
    const A_u_longlong inst = InstanceId(in_data);
    static std::set<std::pair<A_u_longlong, size_t>> done;  // main thread only
    const auto key = std::make_pair(inst, std::hash<std::string>()(script));
    if (!inst || done.count(key)) return;
    done.insert(key);

    Suite<AEGP_PFInterfaceSuite1> pfi(in_data->pica_basicP, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1);
    AEGP_EffectRefH eff = nullptr;
    if (!pfi || pfi->AEGP_GetNewEffectForEffect(g_pluginId, in_data->effect_ref, &eff) || !eff) {
        Log("test script: no effect reference");
        return;
    }
    std::string text;
    dxg::Graph g;
    if (!ReadGraphText(eff, &text) || !dxg::Parse(text, &g)) g = dxg::DefaultPreset().graph;
    Write w;
    std::string err;
    if (!dxg::RunScript(g, script, &w.macros, &err)) {
        Log("test script: %s", err.c_str());
        DisposeEffect(eff);
        return;
    }
    g.revision++;
    w.eff = eff;
    w.instance = inst;
    w.text = dxg::Serialize(g);
    w.why = "test script";
    // the idle hook also runs while the script that set the seed is still running, and AEGP_SetStreamValue did
    // not return when it wrote then (11/28): wait until the script is over
    w.notBefore = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    Log("test script: %zu bytes queued (revision %u, %zu macro(s))", w.text.size(), (unsigned)g.revision, w.macros.size());
    std::lock_guard<std::mutex> lk(g_mu);
    g_queue.push_back(std::move(w));
}

}  // namespace dx2
