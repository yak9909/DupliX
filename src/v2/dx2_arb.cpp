// DupliX 2 (design v4, 7): the graph data as an Arbitrary Data parameter. The handle holds the graph's stored form
// (UTF-8 text, src/graph/graph.cpp) as it is, so flattening is a copy and the saved project is the same on every
// platform. The renders parse it through a cache keyed by the text (many threads, one parse per distinct graph).
#include "v2/dx2.h"

#include <cstring>
#include <functional>
#include <map>
#include <mutex>

namespace dx2 {

namespace {

Suite<PF_HandleSuite1> Handles() { return Suite<PF_HandleSuite1>(g_pica, kPFHandleSuite, kPFHandleSuiteVersion1); }

struct Cache {
    std::mutex mu;
    std::map<std::string, std::shared_ptr<const dxg::Graph>> byText;
} g_cache;
constexpr size_t kCacheMax = 64;  // distinct graphs kept (an undo history of one session is far smaller)

bool LooksLikeGraph(const void* p, size_t n)
{
    static const char kHead[] = "DupliX graph ";
    return n >= sizeof(kHead) - 1 && !std::memcmp(p, kHead, sizeof(kHead) - 1);
}

}  // namespace

std::string DefaultGraphText()
{
    static const std::string text = dxg::Serialize(dxg::DefaultPreset().graph);
    return text;
}

PF_Handle NewGraphHandle(const std::string& text)
{
    auto hs = Handles();
    if (!hs) return nullptr;
    PF_Handle h = hs->host_new_handle(text.size());
    if (!h) return nullptr;
    if (!text.empty()) {
        std::memcpy(hs->host_lock_handle(h), text.data(), text.size());
        hs->host_unlock_handle(h);
    }
    return h;
}

std::string GraphText(PF_Handle h)
{
    auto hs = Handles();
    if (!hs || !h) return {};
    const A_HandleSize n = hs->host_get_handle_size(h);
    if (!n) return {};
    const char* p = (const char*)hs->host_lock_handle(h);
    std::string s = p ? std::string(p, (size_t)n) : std::string();
    hs->host_unlock_handle(h);
    return s;
}

bool SetGraphHandle(PF_Handle* h, const std::string& text)
{
    auto hs = Handles();
    if (!hs || !h || !*h) return false;
    if (hs->host_resize_handle(text.size(), h)) return false;
    if (!text.empty()) {
        std::memcpy(hs->host_lock_handle(*h), text.data(), text.size());
        hs->host_unlock_handle(*h);
    }
    return true;
}

std::shared_ptr<const dxg::Graph> GraphOfText(const std::string& text)
{
    {
        std::lock_guard<std::mutex> lk(g_cache.mu);
        auto it = g_cache.byText.find(text);
        if (it != g_cache.byText.end()) return it->second;
    }
    auto g = std::make_shared<dxg::Graph>();
    std::string err;
    if (!dxg::Parse(text, g.get(), &err)) Log("graph: cannot read the graph data (%s)", err.c_str());
    std::lock_guard<std::mutex> lk(g_cache.mu);
    if (g_cache.byText.size() >= kCacheMax) g_cache.byText.clear();
    g_cache.byText[text] = g;
    return g;
}

std::shared_ptr<const dxg::Graph> GraphOf(PF_Handle h) { return GraphOfText(GraphText(h)); }

PF_Err HandleArb(PF_InData* in_data, PF_OutData* out_data, PF_ArbParamsExtra* x)
{
    (void)in_data, (void)out_data;
    auto hs = Handles();
    if (!hs) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    switch (x->which_function) {
    case PF_Arbitrary_NEW_FUNC:
        *x->u.new_func_params.arbPH = NewGraphHandle(DefaultGraphText());
        return *x->u.new_func_params.arbPH ? PF_Err_NONE : PF_Err_OUT_OF_MEMORY;
    case PF_Arbitrary_DISPOSE_FUNC:
        if (x->u.dispose_func_params.arbH) hs->host_dispose_handle(x->u.dispose_func_params.arbH);
        return PF_Err_NONE;
    case PF_Arbitrary_COPY_FUNC:
        *x->u.copy_func_params.dst_arbPH = NewGraphHandle(GraphText(x->u.copy_func_params.src_arbH));
        return *x->u.copy_func_params.dst_arbPH ? PF_Err_NONE : PF_Err_OUT_OF_MEMORY;
    case PF_Arbitrary_FLAT_SIZE_FUNC:
        *x->u.flat_size_func_params.flat_data_sizePLu =
            (A_u_long)hs->host_get_handle_size(x->u.flat_size_func_params.arbH);
        return PF_Err_NONE;
    case PF_Arbitrary_FLATTEN_FUNC: {
        const std::string s = GraphText(x->u.flatten_func_params.arbH);
        if (x->u.flatten_func_params.buf_sizeLu < s.size()) return PF_Err_INTERNAL_STRUCT_DAMAGED;
        if (!s.empty()) std::memcpy(x->u.flatten_func_params.flat_dataPV, s.data(), s.size());
        return PF_Err_NONE;
    }
    case PF_Arbitrary_UNFLATTEN_FUNC: {
        const A_u_long n = x->u.unflatten_func_params.buf_sizeLu;
        const void* p = x->u.unflatten_func_params.flat_dataPV;
        // anything that is not a graph (damaged, another format) becomes the default graph
        const bool ok = p && LooksLikeGraph(p, n);
        if (!ok) Log("arb: unflatten of %lu bytes that are not a graph: the default graph instead", (unsigned long)n);
        *x->u.unflatten_func_params.arbPH = NewGraphHandle(ok ? std::string((const char*)p, n) : DefaultGraphText());
        return *x->u.unflatten_func_params.arbPH ? PF_Err_NONE : PF_Err_OUT_OF_MEMORY;
    }
    case PF_Arbitrary_INTERP_FUNC:  // CANNOT_TIME_VARY: never between two keys; the left one
        *x->u.interp_func_params.interpPH = NewGraphHandle(GraphText(x->u.interp_func_params.left_arbH));
        return PF_Err_NONE;
    case PF_Arbitrary_COMPARE_FUNC: {
        const bool same = GraphText(x->u.compare_func_params.a_arbH) == GraphText(x->u.compare_func_params.b_arbH);
        *x->u.compare_func_params.compareP = same ? PF_ArbCompare_EQUAL : PF_ArbCompare_NOT_EQUAL;
        return PF_Err_NONE;
    }
    case PF_Arbitrary_PRINT_SIZE_FUNC:
        *x->u.print_size_func_params.print_sizePLu = 64;
        return PF_Err_NONE;
    case PF_Arbitrary_PRINT_FUNC:
        if (x->u.print_func_params.print_sizeLu && x->u.print_func_params.print_bufferPC) {
            auto g = GraphOf(x->u.print_func_params.arbH);
            snprintf(x->u.print_func_params.print_bufferPC, x->u.print_func_params.print_sizeLu,
                     "DupliX graph, %u nodes, revision %u", (unsigned)g->nodes.size(), (unsigned)g->revision);
        }
        return PF_Err_NONE;
    case PF_Arbitrary_SCAN_FUNC:
        return PF_Err_UNRECOGNIZED_PARAM_TYPE;
    }
    return PF_Err_NONE;
}

}  // namespace dx2
