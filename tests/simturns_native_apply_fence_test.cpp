#include "simturns/native_apply_fence.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>

using hooks::simturns::NativeApplyFence;

int main()
{
    NativeApplyFence fence;
    assert(fence.reached(0));
    const auto first = fence.issue();
    const auto second = fence.issue();
    const auto controlBarrier = fence.watermark();
    const auto later = fence.issue();
    assert(!fence.reached(controlBarrier));
    assert(fence.complete(second)); // Host and client dispatch can return out of order.
    assert(!fence.reached(controlBarrier));
    assert(!fence.complete(second));
    assert(!fence.complete(0));
    assert(!fence.complete(later + 1));
    assert(fence.complete(first));
    assert(fence.reached(controlBarrier)); // A later packet cannot hold an earlier control.
    assert(!fence.reached(later));
    assert(!fence.complete(first));
    assert(fence.complete(later));
    assert(fence.reached(fence.watermark()));

    NativeApplyFence anotherMap;
    assert(anotherMap.watermark() == 0);
    assert(!anotherMap.complete(first)); // Receipts belong to one binding, never a later map.
    for (unsigned i = 0; i != 1024; ++i) anotherMap.issue();
    for (unsigned i = 1024; i > 1; --i) assert(anotherMap.complete(i));
    assert(!anotherMap.reached(1024));
    assert(anotherMap.complete(1));
    assert(anotherMap.reached(1024));
    std::puts("simturns native apply fence: PASS");
}
