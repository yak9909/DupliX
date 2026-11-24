#include "hook/registry.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace rnd {

namespace {

constexpr int kCapacity = 4096;

struct Entry {
    std::atomic<uint64_t> key{0};                // 0 = free
    std::atomic<const ParamSnapshot*> snap{nullptr};  // nullptr = not registered
};

Entry g_entries[kCapacity];
std::atomic<int> g_used{0};    // entries [0, g_used) may hold a key
std::mutex g_writeMutex;       // writers only; readers never lock
std::atomic<unsigned> g_generation{0};

uint64_t Pack(LayerKey k) { return ((uint64_t)(uint32_t)k.compItemId << 32) | (uint32_t)k.layerId; }

Entry* Find(uint64_t key)
{
    int n = g_used.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (g_entries[i].key.load(std::memory_order_acquire) == key) return &g_entries[i];
    return nullptr;
}

}  // namespace

// Snapshots are never freed: a reader on a render thread may still hold the old pointer. Each
// update leaks one small struct (about 150 bytes), which only happens on effect commands.
void RegRegister(LayerKey k, const ParamSnapshot& snap)
{
    uint64_t key = Pack(k);
    if (!key) return;
    std::lock_guard<std::mutex> lk(g_writeMutex);
    Entry* e = Find(key);
    if (!e) {
        int n = g_used.load(std::memory_order_relaxed);
        if (n >= kCapacity) return;
        e = &g_entries[n];
        e->key.store(key, std::memory_order_release);
        g_used.store(n + 1, std::memory_order_release);
    }
    e->snap.store(new ParamSnapshot(snap), std::memory_order_release);
    ++g_generation;
}

void RegUnregister(LayerKey k)
{
    std::lock_guard<std::mutex> lk(g_writeMutex);
    if (Entry* e = Find(Pack(k))) e->snap.store(nullptr, std::memory_order_release);
    ++g_generation;
}

unsigned RegGeneration() { return g_generation.load(std::memory_order_acquire); }

bool RegLookup(LayerKey k, ParamSnapshot* out)
{
    Entry* e = Find(Pack(k));
    const ParamSnapshot* s = e ? e->snap.load(std::memory_order_acquire) : nullptr;
    if (!s) return false;
    *out = *s;
    return true;
}

bool RegContains(LayerKey k)
{
    Entry* e = Find(Pack(k));
    return e && e->snap.load(std::memory_order_acquire) != nullptr;
}

}  // namespace rnd
