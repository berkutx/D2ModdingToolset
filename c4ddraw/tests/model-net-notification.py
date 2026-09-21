"""Finite-state exploration of the MSS notification handshake, not a game emulator.

Python 3.7+, standard library only. Two FIFO packets suffice for a lost-wakeup
witness: packet 0 triggers a notification, packet 1 demonstrates persistent loss.
Every transition is one independently interleavable protocol operation. UI callback
reentry is allowed only while an onPacketReceived callback runs; it cannot drain
because the real callback has a `processing` guard.
"""
from collections import deque, namedtuple
import argparse
import json
from pathlib import Path

State = namedtuple("State", "arrived queue notified producer native rescue consumer delivered armed budget")
IDLE, POST, STORE_TRUE, STORE_FALSE = range(4)
UI_IDLE, DRAIN, CALLBACK_0, CALLBACK_1, FINISH = range(5)


class Model:
    def __init__(self, protocol="current", retries=0, periodic=False,
                 capacity=4, post_failure=True, stuck_callback=False):
        self.protocol = protocol
        self.retries = retries
        self.periodic = periodic
        self.capacity = capacity
        self.post_failure = post_failure
        self.stuck_callback = stuck_callback
        self.start = State(0, (), False, IDLE, 0, False, UI_IDLE, False, False, retries)

    def edges(self, s):
        if s.arrived < 2:
            yield "arrive_%d" % s.arrived, s._replace(
                arrived=s.arrived + 1, queue=s.queue + (s.arrived,))
        if s.producer == IDLE and s.queue and not s.notified:
            yield "native_observe_pending", s._replace(
                producer=POST,
                notified=True if self.protocol == "claim_before_post" else s.notified)
        if s.producer == POST:
            if s.native + int(s.rescue) < self.capacity:
                yield "native_post_success", s._replace(
                    native=s.native + 1,
                    producer=STORE_TRUE if self.protocol == "current" else IDLE)
            if self.post_failure or s.native + int(s.rescue) >= self.capacity:
                yield "native_post_failure", s._replace(producer=STORE_FALSE)
        if s.producer == STORE_TRUE:
            yield "native_late_store_true", s._replace(notified=True, producer=IDLE)
        if s.producer == STORE_FALSE:
            yield "native_store_false", s._replace(notified=False, producer=IDLE)

        if s.consumer == UI_IDLE:
            if s.native:
                yield "ui_dispatch_native", s._replace(native=s.native - 1, consumer=DRAIN)
            if s.rescue:
                yield "ui_dispatch_retry", s._replace(rescue=False, consumer=DRAIN)
        if s.consumer == DRAIN:
            if s.queue:
                p = s.queue[0]
                yield "receive_packet_%d" % p, s._replace(
                    queue=s.queue[1:], consumer=CALLBACK_0 if p == 0 else CALLBACK_1)
            else:
                yield "receive_empty", s._replace(consumer=FINISH)
        if s.consumer in (CALLBACK_0, CALLBACK_1):
            if not self.stuck_callback:
                yield "packet_callback_return", s._replace(
                    consumer=DRAIN, delivered=s.delivered or s.consumer == CALLBACK_1)
            if s.native:
                yield "nested_native_guard_return", s._replace(native=s.native - 1)
            if s.rescue:
                yield "nested_retry_guard_return", s._replace(rescue=False)
        if s.consumer == FINISH:
            yield "ui_finish_reset_false", s._replace(consumer=UI_IDLE, notified=False, armed=True)

        # A finite workaround is generously armed only AFTER a native drain has
        # returned. Waiting any finite duration is represented by letting other
        # transitions execute first. The producer may still be at STORE_TRUE.
        if s.armed and not s.rescue and (self.periodic or s.budget):
            budget = s.budget if self.periodic else s.budget - 1
            if s.native < self.capacity:
                yield "retry_post_success", s._replace(rescue=True, budget=budget)
            if self.post_failure or s.native >= self.capacity:
                yield "retry_post_failure", s._replace(budget=budget)
        # Explicit idling lets a permanent lack of wakeup be an infinite run.
        yield "idle", s

    def stopped(self, s):
        return (s.arrived == 2 and not s.delivered and 1 in s.queue
                and s.notified and s.producer == IDLE and not s.native
                and not s.rescue and s.consumer == UI_IDLE
                and not self.periodic and not s.budget)

    def explore(self):
        graph, parent = {}, {self.start: None}
        pending = deque([self.start])
        first_stopped = None
        while pending:
            s = pending.popleft()
            if first_stopped is None and self.stopped(s):
                first_stopped = s
            edges = list(self.edges(s))
            graph[s] = edges
            for action, t in edges:
                if t not in parent:
                    parent[t] = (s, action)
                    pending.append(t)
        return graph, parent, first_stopped


def witness(parent, end):
    if end is None:
        return None
    path = []
    while parent[end] is not None:
        before, action = parent[end]
        path.append({"action": action, "after": end._asdict()})
        end = before
    return list(reversed(path))


def components(graph, nodes):
    """Iterative Kosaraju, keeping only candidate liveness-violation states."""
    adjacency = {s: [t for _, t in graph[s] if t in nodes] for s in nodes}
    reverse = {s: [] for s in nodes}
    for s, ends in adjacency.items():
        for t in ends:
            reverse[t].append(s)
    seen, order = set(), []
    for root in nodes:
        if root in seen:
            continue
        seen.add(root)
        stack = [(root, iter(adjacency[root]))]
        while stack:
            top, it = stack[-1]
            nxt = next(it, None)
            if nxt is None:
                order.append(top)
                stack.pop()
            elif nxt not in seen:
                seen.add(nxt)
                stack.append((nxt, iter(adjacency[nxt])))
    assigned = set()
    for root in reversed(order):
        if root in assigned:
            continue
        group, stack = set(), [root]
        assigned.add(root)
        while stack:
            top = stack.pop()
            group.add(top)
            for nxt in reverse[top]:
                if nxt not in assigned:
                    assigned.add(nxt)
                    stack.append(nxt)
        yield group


def fair_bad_components(graph, allow_permanent_post_failure=False,
                        allow_stuck_callback=False):
    # Weak fairness of a continuously enabled action: normal producer, successful
    # notification posting, UI dispatch, Receive, and finite packet callbacks.
    # A recurring timer plus eventual PostMessage success is represented by
    # retry_post_success fairness. It is an assumption, not supplied by Python.
    fair_actions = {
        "native_observe_pending", "native_late_store_true",
        "native_store_false", "ui_dispatch_native", "ui_dispatch_retry",
        "receive_packet_0", "receive_packet_1", "receive_empty",
        "packet_callback_return", "ui_finish_reset_false",
    }
    # Post success needs strong fairness: infinitely many opportunities imply a
    # successful post eventually. Weak fairness alone permits fail/store/check
    # cycles because success is disabled between individual posting attempts.
    strong_actions = set() if allow_permanent_post_failure else {"native_post_success", "retry_post_success"}
    if allow_stuck_callback:
        fair_actions -= {"packet_callback_return"}
    bad = {s for s in graph if s.arrived == 2 and not s.delivered}
    violations = []
    pending_groups = list(components(graph, bad))
    while pending_groups:
        group = pending_groups.pop()
        # The group can be visited indefinitely (every state has idle). A fair
        # cycle can visit every state and every internal edge of a finite SCC.
        continuously_enabled = None
        taken_inside = set()
        enabled_somewhere = set()
        for s in group:
            enabled = {a for a, _ in graph[s]} & fair_actions
            continuously_enabled = enabled if continuously_enabled is None else continuously_enabled & enabled
            enabled_somewhere.update(a for a, _ in graph[s])
            taken_inside.update(a for a, t in graph[s] if t in group)
        if continuously_enabled - taken_inside:
            continue
        impossible_strong = (enabled_somewhere & strong_actions) - taken_inside
        if impossible_strong:
            # A smaller fair recurrent set can avoid states enabling an action
            # that can only exit the SCC. Recompute SCCs after removing them.
            remaining = {s for s in group
                         if not ({a for a, _ in graph[s]} & impossible_strong)}
            pending_groups.extend(components(graph, remaining))
            continue
        violations.append(group)
    return violations


def run_case(name, **options):
    model = Model(**options)
    graph, parent, stopped = model.explore()
    bad = fair_bad_components(graph)
    result = {"name": name, "options": options, "states": len(graph),
              "transitions": sum(len(e) for e in graph.values()),
              "lost_wakeup_witness": witness(parent, stopped),
              "fair_nondelivery_components": len(bad)}
    assert all(len(s.queue) <= 2 and s.native + int(s.rescue) <= model.capacity for s in graph)
    assert all(not s.delivered or 1 not in s.queue for s in graph)
    assert all(a != "receive_packet_1" or s.consumer == DRAIN
               for s in graph for a, _ in graph[s])
    if bad:
        # Show shortest reachable representative for reproducible counterexample.
        reps = [min(g, key=lambda s: len(witness(parent, s))) for g in bad]
        rep = min(reps, key=lambda s: len(witness(parent, s)))
        result["fair_nondelivery_prefix"] = witness(parent, rep)
        result["fair_nondelivery_state"] = rep._asdict()
    return result, graph


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default="model-results.json")
    args = parser.parse_args()
    results = []
    for n in (0, 1, 2, 3, 5, 10):
        # No failed posts needed: this is a protocol race even when Windows
        # accepts every notification. All failure transitions disabled here.
        result, _ = run_case("current_%d_finite_retries" % n, retries=n, post_failure=False)
        assert result["lost_wakeup_witness"] is not None
        assert result["fair_nondelivery_components"] > 0
        results.append(result)
    for capacity in (1, 2, 4):
        result, graph = run_case("current_periodic_capacity_%d" % capacity,
                                 periodic=True, capacity=capacity, post_failure=True)
        assert result["fair_nondelivery_components"] == 0
        result["without_post_success_assumption_components"] = len(
            fair_bad_components(graph, allow_permanent_post_failure=True))
        assert result["without_post_success_assumption_components"] > 0
        results.append(result)
    result, _ = run_case("claim_before_post_reference", protocol="claim_before_post", post_failure=True)
    assert result["fair_nondelivery_components"] == 0
    results.append(result)
    # To expose an already wedged processing guard, arm periodic recovery at
    # initialization and suppress callback return. This intentionally violates
    # the finite-callback assumption; no number of notifications can repair it.
    model = Model(periodic=True, stuck_callback=True, post_failure=False)
    model.start = model.start._replace(armed=True)
    graph, parent, _ = model.explore()
    bad = fair_bad_components(graph, allow_stuck_callback=True)
    assert bad
    reps = [min(g, key=lambda s: len(witness(parent, s))) for g in bad]
    rep = min(reps, key=lambda s: len(witness(parent, s)))
    results.append({"name": "permanent_processing_guard", "states": len(graph),
                    "fair_nondelivery_components": len(bad),
                    "witness_prefix": witness(parent, rep)})
    Path(args.output).write_text(json.dumps(results, indent=2), encoding="utf-8")
    for r in results:
        print("{name}: states={states}, fair_nondelivery_components={fair_nondelivery_components}".format(**r))
    print("PASS: all protocol assertions; see " + args.output)


if __name__ == "__main__":
    main()
