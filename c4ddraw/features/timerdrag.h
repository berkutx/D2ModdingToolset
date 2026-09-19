// Included by timerhost.cpp after its state, pointer probes and signature helpers.
#pragma once

// Russobit CInterface -> CDragAndDropInterf. Keep the native RTTI boundary replaceable in
// the standalone tests; all manager, list and cancellation logic below is production code.
#ifndef C4_TIMER_DRAG_DYNAMIC_CAST
#define C4_TIMER_DRAG_DYNAMIC_CAST(interf) \
    reinterpret_cast<void*(__cdecl*)(const void*, int, const void*, const void*, int)>( \
        0x66D466)(interf, 0, reinterpret_cast<const void*>(0x78E8D0), \
                 reinterpret_cast<const void*>(0x79B288), 0)
#endif

enum TimeoutDragResult
{
    TimeoutDragUnavailable = -1,
    TimeoutDragIdle = 0,
    TimeoutDragCancelled = 1,
};

bool validateNativeDragLayout()
{
    // These bytes and RTTI descriptors belong to the supported game EXE, not mss32.dll.
    const unsigned char castSignature[] = {0xFF, 0x25, 0xA4, 0xE2, 0x6C, 0x00};
    const unsigned char sourceSignature[] = {0x8B, 0x41, 0x04, 0x8B, 0x40, 0x04, 0xC3};
    const unsigned char resetSignature[] = {
        0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C, 0x53, 0x56, 0x8B, 0xF1,
        0x33, 0xDB, 0x57, 0x8B, 0x46, 0x04, 0x8B, 0x48, 0x28, 0x8B, 0x09};
    const unsigned char topSignature[] = {
        0x56, 0x8B, 0xF1, 0x8B, 0x06, 0xFF, 0x50, 0x04, 0x84, 0xC0,
        0x75, 0x0F, 0x8B, 0x4E, 0x08, 0x83, 0xC1, 0x18, 0xE8, 0xD4,
        0x1B, 0x02, 0x00, 0x8B, 0x00, 0x5E, 0xC3, 0x33, 0xC0, 0x5E, 0xC3};
    const unsigned char rememberedSignature[] = {
        0x8B, 0x41, 0x08, 0x8B, 0x80, 0x88, 0x00, 0x00, 0x00, 0xC3};
    const unsigned char interfaceType[] = ".?AVCInterface@@";
    const unsigned char dragType[] = ".?AVCDragAndDropInterf@@";
    return validateBytes(0x66D466, castSignature, sizeof(castSignature)) &&
           validateBytes(0x5A9EA2, sourceSignature, sizeof(sourceSignature)) &&
           validateBytes(0x56CF5C, resetSignature, sizeof(resetSignature)) &&
           validateBytes(0x53D357, topSignature, sizeof(topSignature)) &&
           validateBytes(0x53D6A3, rememberedSignature, sizeof(rememberedSignature)) &&
           validateBytes(0x78E8D8, interfaceType, sizeof(interfaceType)) &&
           validateBytes(0x79B290, dragType, sizeof(dragType));
}

void* timeoutDragInterfaceManager(void* interf)
{
    if (!isUserPtr(interf))
        return nullptr;
    char* data = *reinterpret_cast<char**>(reinterpret_cast<char*>(interf) + 4);
    // CInterfaceData begins with SmartPtr<CInterfManagerImpl> (counter, object).
    return isUserPtr(data) ? *reinterpret_cast<void**>(data + 4) : nullptr;
}

bool timeoutDragSourceRegistered(char* data, void* source)
{
    // List<IMidDropSource*> at +20: length, sentinel, unknown, allocator.
    // The source is non-owning. Never invoke its cleanup if native code has already removed it.
    const unsigned count = *reinterpret_cast<unsigned*>(data + 20);
    char* head = *reinterpret_cast<char**>(data + 24);
    if (!count || count > 4096 || !isUserPtr(head))
        return false;
    char* node = *reinterpret_cast<char**>(head);
    bool found = false;
    for (unsigned i = 0; i < count; ++i) {
        if (!isUserPtr(node) || node == head)
            return false;
        if (*reinterpret_cast<void**>(node + 8) == source)
            found = true;
        node = *reinterpret_cast<char**>(node);
    }
    return found && node == head;
}

int cancelTimeoutDragInterface(void* interf, void* expectedManager)
{
    if (!interf)
        return TimeoutDragIdle;
    if (!isUserPtr(interf) || timeoutDragInterfaceManager(interf) != expectedManager)
        return TimeoutDragUnavailable;

    char* owner = reinterpret_cast<char*>(C4_TIMER_DRAG_DYNAMIC_CAST(interf));
    if (!owner)
        return TimeoutDragIdle;
    if (!isUserPtr(owner) || timeoutDragInterfaceManager(owner) != expectedManager)
        return TimeoutDragUnavailable;

    char* data = *reinterpret_cast<char**>(owner + 20);
    char* manager = owner + 16;
    void** vtable = *reinterpret_cast<void***>(manager);
    if (!isUserPtr(data) || !isUserPtr(vtable) || !executableAddress(vtable[1]))
        return TimeoutDragUnavailable;
    void* source = reinterpret_cast<void*(__thiscall*)(void*)>(vtable[1])(manager);
    if (*reinterpret_cast<void**>(data + 4) != source)
        return TimeoutDragUnavailable;
    if (!source)
        return TimeoutDragIdle;
    if (!isUserPtr(source) || !timeoutDragSourceRegistered(data, source) ||
        !executableAddress(vtable[4]))
        return TimeoutDragUnavailable;
    void** sourceVtable = *reinterpret_cast<void***>(source);
    if (!isUserPtr(sourceVtable) || !executableAddress(sourceVtable[7]))
        return TimeoutDragUnavailable;

    tlog("[timer] cancelling native drag before timeout action (owner=%p source=%p)",
         owner, source);
    // resetSource notifies the targets, clears the manager and cleans the source cursor. Its
    // callbacks may replace the window. Publish the invalidation before entry and never inspect
    // any captured object after the call; the caller must reacquire everything on another tick.
    InterlockedIncrement(&g.dragCancelSerial);
    reinterpret_cast<void(__thiscall*)(void*)>(vtable[4])(manager);
    return TimeoutDragCancelled;
}

int cancelTimeoutDrag(void* anchor)
{
    if (!InterlockedExchangeAdd(&g.dragCancelAvailable, 0))
        return TimeoutDragUnavailable;
    // Target notifications can pump another timeout before resetSource clears currentSource.
    // Serialize the shared End Day / Auto Battle preparation, including that callback interval.
    static LONG volatile preparing = 0;
    if (InterlockedCompareExchange(&preparing, 1, 0) != 0)
        return TimeoutDragUnavailable;
    __try {
        __try {
            void* manager = timeoutDragInterfaceManager(anchor);
            if (!isUserPtr(manager))
                return TimeoutDragUnavailable;
            void** vtable = *reinterpret_cast<void***>(manager);
            if (!isUserPtr(vtable) || !executableAddress(vtable[11]) ||
                !executableAddress(vtable[2]))
                return TimeoutDragUnavailable;

            // CCursorImpl::getHandle selects the remembered interface before the topmost one.
            // Both can retain drag state; checking only the visible window misses native capture.
            void* remembered = reinterpret_cast<void*(__thiscall*)(void*)>(vtable[11])(manager);
            const int rememberedResult = cancelTimeoutDragInterface(remembered, manager);
            if (rememberedResult != TimeoutDragIdle)
                return rememberedResult;
            void* top = reinterpret_cast<void*(__thiscall*)(void*)>(vtable[2])(manager);
            return top == remembered ? TimeoutDragIdle : cancelTimeoutDragInterface(top, manager);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return TimeoutDragUnavailable;
        }
    } __finally {
        InterlockedExchange(&preparing, 0);
    }
}

bool prepareTimeoutInput(void* anchor)
{
    // Posted messages and WM_TIMER can be pumped recursively by native drawing/input callbacks.
    // A timeout must not destroy the object that an outer callback is still using.
    if (featuremenu_native_dispatch_active() || cursorcapture_draw_active())
        return false;
    if (cancelTimeoutDrag(anchor) != TimeoutDragIdle)
        return false;
    // Cancel a held drag immediately, but retain the timeout request until the gesture ends.
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0;
}
