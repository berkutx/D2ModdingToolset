#pragma once
#include <stdint.h>
#include <limits.h>

namespace c4net {
/* All calls are serialized by the owning adapter. No game pointers are
 * dereferenced here. A token is reserved BEFORE PostMessage, never after it. */
struct WakeState {
    enum Phase { Idle, Queued, Deferred };
    uintptr_t controller = 0, controllerData = 0;
    uint32_t eventId = 0, generation = 0, sequence = 0, pending = 0;
    Phase phase = Idle;
    bool observed = false, armed = false, unavailable = false;

    struct Ticket { uint32_t generation, sequence; };
    void clear() { pending = 0; phase = Idle; }
    void cancel()
    {
        clear(); armed = false; observed = false;
        controller = controllerData = 0;
        // Never reuse a registration generation during the process lifetime.
        if (generation == UINT_MAX) unavailable = true;
        else ++generation;
    }
    void reject() { cancel(); unavailable = true; }
    bool registered(uintptr_t object, uintptr_t data, uint32_t id)
    {
        if (unavailable) return false;
        // Multiple listeners for this name have an unverified contract.
        if (observed || !object || !data) { reject(); return false; }
        cancel();
        if (unavailable) return false;
        controller = object; controllerData = data; eventId = id;
        observed = true;
        return true;
    }
    bool matches(uintptr_t object, uintptr_t data) const
    { return observed && !unavailable && controller == object && controllerData == data; }
    bool completed(uintptr_t object, uintptr_t data, uint32_t entryGeneration)
    {
        if (!matches(object, data) || generation != entryGeneration) return false;
        armed = true;
        return true;
    }
    bool removed(uintptr_t object, uint32_t id)
    {
        if (!observed || controller != object || eventId != id) return false;
        cancel(); return true;
    }
    bool owns(Ticket ticket) const
    {
        return armed && observed && !unavailable && phase != Idle &&
               ticket.generation == generation && ticket.sequence == pending;
    }
    bool reserve(Ticket* ticket)
    {
        if (!armed || !observed || unavailable || phase != Idle) return false;
        if (sequence == UINT_MAX) { reject(); return false; }
        pending = ++sequence; phase = Queued;
        *ticket = {generation, pending};
        return true;
    }
    void postFailed(Ticket ticket)
    {
        // A delayed failure cannot erase another generation or a request that
        // was already consumed/replaced while the API call was in progress.
        if (owns(ticket) && phase == Queued) clear();
    }
    void defer(Ticket ticket)
    { if (owns(ticket)) phase = Deferred; }
    bool requeue(Ticket* ticket)
    {
        if (!armed || !observed || unavailable || phase != Deferred) return false;
        // Allocate a NEW token: a duplicate old message cannot steal the requeue.
        clear();
        return reserve(ticket);
    }
    bool consume(Ticket ticket)
    {
        if (!owns(ticket) || phase != Queued) return false;
        clear(); return true;
    }
};
}
