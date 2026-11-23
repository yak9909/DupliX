#include "hook/bee_abi.h"

#include <Windows.h>

namespace rnd::bee {

bool VecReserve(RawVec* v, size_t elemBytes, size_t n)
{
    typedef void* (*MallocF)(size_t);
    typedef void (*FreeF)(void*);
    static HMODULE ucrt = GetModuleHandleA("ucrtbase.dll");
    static MallocF m = ucrt ? (MallocF)GetProcAddress(ucrt, "malloc") : nullptr;
    static FreeF fr = ucrt ? (FreeF)GetProcAddress(ucrt, "free") : nullptr;
    if (!m || !fr) return false;
    size_t cap = (size_t)(v->end - v->first);
    size_t bytes = elemBytes * n;
    v->last = v->first;
    if (cap >= bytes) return true;
    char* p;
    if (bytes >= 4096) {  // MSVC _Allocate_manually_vector_aligned: 32-byte aligned, raw pointer at p[-1]
        char* raw = (char*)m(bytes + 8 + 31);
        if (!raw) return false;
        p = (char*)(((uintptr_t)raw + 8 + 31) & ~(uintptr_t)31);
        ((void**)p)[-1] = raw;
    } else {
        p = (char*)m(bytes);
        if (!p) return false;
    }
    if (v->first) {
        if (cap >= 4096)
            fr(((void**)v->first)[-1]);
        else
            fr(v->first);
    }
    v->first = v->last = p;
    v->end = p + bytes;
    return true;
}

}  // namespace rnd::bee
