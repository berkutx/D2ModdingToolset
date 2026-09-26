#include "simturns/coordinator_port.h"
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace hooks::simturns;
namespace p = hooks::simturns::protocol;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
p::Bytes packet(p::Op op, std::initializer_list<std::uint32_t> words)
{
    p::Bytes bytes;
    for (const auto word : words)
        for (unsigned i = 0; i != 4; ++i) bytes.push_back(static_cast<std::uint8_t>(word >> (8 * i)));
    return p::encodeFrame(op, bytes);
}
struct Fixture {
    CoordinatorPort& port{CoordinatorPort::processInstance()};
    unsigned terminal{}, faults{}, events{};
    bool sendOk{true}, throwSend{}, throwSink{};
    std::vector<p::Bytes> sent;
    CoordinatorEvent last;
    explicit Fixture(Role role = Role::Host, bool start = true) {
        port.stop();
        check(port.arm({true, role}, 123, 3, [this](const p::Bytes& bytes) {
            if (throwSend) throw std::runtime_error("send exception");
            if (sendOk) sent.push_back(bytes);
            return sendOk;
        }, [this] { ++terminal; }), "arm failed");
        if (start) {
            CoordinatorCallbacks callbacks;
            callbacks.postToUi = [this](CoordinatorEvent event) {
                if (throwSink) throw std::runtime_error("sink exception");
                ++events; last = event;
            };
            callbacks.terminalFault = [this](CoordinatorTerminalFault) { ++faults; };
            check(port.start({true, role}, callbacks), "start failed");
            check(port.bindLocalPlayer(role == Role::Host ? 1 : 2), "bind failed");
        }
    }
    ~Fixture() { port.stop(); }
    void receive(const p::Bytes& bytes) { port.receive(bytes.data(), bytes.size()); }
    void plan() { receive(packet(p::Op::SessionPlan, {123, 1, 1, 2, 3, 11, 12})); }
    void hostBootstrap() {
        plan();
        check(events == 1 && !faults, "plan not delivered");
        check(port.reportSessionActivated(), "activation failed");
        receive(packet(p::Op::EngineAction, {123, 1, 1, 2, 1, 12}));
        check(port.reportActionResult(last.engineAction, true), "cascade failed");
        receive(packet(p::Op::BootstrapCommitted, {2, 1}));
        check(port.reportBootstrap(BootstrapCheckpoint::CommitApplied, 2, 1), "commit failed");
        receive(packet(p::Op::BootstrapOperational, {2, 1}));
        check(port.reportBootstrap(BootstrapCheckpoint::OperationalApplied, 2, 1), "prepare failed");
        check(!port.operational(), "opened before release");
        receive(packet(p::Op::BootstrapReleased, {2, 1}));
        check(port.acceptBootstrapRelease(last.bootstrapReleased), "release failed");
        check(port.operational() && !faults, "bootstrap not operational");
    }
};
void malformedFrames() {
    const auto valid = packet(p::Op::SessionPlan, {123, 1, 1, 2, 3, 11, 12});
    for (std::size_t n = 0; n < valid.size(); ++n) {
        Fixture f;
        f.port.receive(valid.data(), n);
        check(f.faults == 1 && f.terminal == 1 && !f.events, "truncated frame accepted");
        f.plan();
        check(f.faults == 1 && !f.events, "faulted epoch revived");
    }
    for (auto bad : {packet(p::Op::SessionPlan, {124, 1, 1, 2, 3, 11, 12}),
                     packet(p::Op::SessionPlan, {123, 1, 1, 2, 4, 11, 12}),
                     packet(p::Op::SessionPlan, {123, 0, 1, 2, 0, 0, 0}),
                     p::encodeHello(1, Role::Host)}) {
        Fixture f; f.receive(bad);
        check(f.faults == 1 && !f.events, "Arm/plan mismatch accepted");
    }
    for (unsigned variant = 0; variant < 3; ++variant) {
        Fixture f; auto bad = valid;
        if (variant == 0) bad.push_back(0);
        if (variant == 1) bad.insert(bad.end(), valid.begin(), valid.end());
        if (variant == 2) bad[6] = 1;
        f.receive(bad);
        check(f.faults == 1 && !f.events, "invalid envelope boundary accepted");
    }
}
void lifecycleAndDelivery() {
    auto& port = CoordinatorPort::processInstance();
    port.stop();
    check(!port.arm({false, Role::Host}, 1, 3, [](const auto&) { return true; }), "opt-out armed");
    check(!port.arm({true, Role::Host}, 0, 3, [](const auto&) { return true; }), "zero epoch armed");
    check(!port.arm({true, Role::Host}, 1, 1, [](const auto&) { return true; }), "day one armed");
    {
        Fixture f(Role::Host, false); f.plan();
        check(f.terminal == 1 && !f.events, "pre-bind frame accepted");
        check(!port.start({true, Role::Host}, {}), "faulted arm started");
    }
    for (unsigned variant = 0; variant != 3; ++variant) {
        Fixture f; f.plan();
        if (variant == 0) f.sendOk = false;
        if (variant == 1) f.throwSend = true;
        if (variant == 2) {
            check(port.reportSessionActivated(), "activation before sink exception failed");
            f.throwSink = true;
            f.receive(packet(p::Op::EngineAction, {123, 1, 1, 2, 1, 12}));
        }
        if (variant != 2) check(!port.reportSessionActivated(), "rejected send accepted");
        check(f.faults == 1 && f.terminal == 1, "terminal failure not exact once");
        const auto count = f.sent.size();
        port.fail(CoordinatorFailureOrigin::UiApply, "again");
        check(!port.reportSessionActivated() && f.sent.size() == count && f.faults == 1, "failed send retried");
    }
    {
        Fixture f; f.hostBootstrap();
        std::uint32_t lease{};
        check(port.claimEndTurn(lease) && lease == 11 && port.endTurnPending(), "wrong first lease");
        check(port.reportEndTurnObserved(lease) && port.reportEndTurnApplied(1), "turn evidence failed");
        f.receive(packet(p::Op::EngineAction, {123, 2, 1, 1, 2, 13}));
        check(port.reportActionResult(f.last.engineAction, true), "apply failed");
        f.receive(packet(p::Op::EngineAction, {123, 2, 2, 1, 2, 13}));
        check(port.reportActionResult(f.last.engineAction, true), "activate failed");
        check(!port.endTurnPending() && port.claimEndTurn(lease) && lease == 13, "lease did not advance");
    }
    {
        Fixture f(Role::Join); f.plan();
        check(!f.faults && f.events == 1, "new role inherited retired map state");
        check(!port.arm({true, Role::Host}, 124, 3, [](const auto&) { return true; }), "active epoch replaced");
        check(!port.operational() && !port.endTurnPending(), "new map inherited old admission");
        check(!port.bindLocalPlayer(3) && f.faults == 1, "handle change accepted");
    }
}
void retiredCallbackCannotFaultNewSession() {
    auto& port = CoordinatorPort::processInstance();
    port.stop();
    unsigned oldFaults{}, newFaults{}, newEvents{}, terminal{}, oldDiagnostics{}, newDiagnostics{};
    const auto sender = [](const p::Bytes&) { return true; };
    check(port.arm({true, Role::Host}, 123, 3, sender, {}, {},
                   [&](const CoordinatorFaultDiagnostic&) { ++oldDiagnostics; }), "old arm failed");
    CoordinatorCallbacks oldCallbacks;
    oldCallbacks.terminalFault = [&](CoordinatorTerminalFault) { ++oldFaults; };
    oldCallbacks.postToUi = [&](CoordinatorEvent) {
        // Deterministically exercise the same unlock/callback boundary as a
        // retiring map: the old callback fails only after a new map is armed.
        port.stop();
        check(port.arm({true, Role::Join}, 124, 3, sender, [&] { ++terminal; }, {},
                       [&](const CoordinatorFaultDiagnostic&) { ++newDiagnostics; }), "replacement arm failed");
        CoordinatorCallbacks next;
        next.postToUi = [&](CoordinatorEvent) { ++newEvents; };
        next.terminalFault = [&](CoordinatorTerminalFault) { ++newFaults; };
        check(port.start({true, Role::Join}, next), "replacement start failed");
        check(port.bindLocalPlayer(2), "replacement bind failed");
        throw std::runtime_error("retired callback failed");
    };
    check(port.start({true, Role::Host}, oldCallbacks), "old start failed");
    check(port.bindLocalPlayer(1), "old bind failed");
    auto plan = packet(p::Op::SessionPlan, {123, 1, 1, 2, 3, 11, 12});
    port.receive(plan.data(), plan.size());
    check(!oldFaults && !newFaults && !terminal && !oldDiagnostics && !newDiagnostics,
          "retired callback fault crossed map generation");
    plan = packet(p::Op::SessionPlan, {124, 1, 1, 2, 3, 21, 22});
    port.receive(plan.data(), plan.size());
    check(newEvents == 1 && !newFaults && !terminal, "replacement map was poisoned by retired callback");
    port.quiesce();
    port.receive(plan.data(), plan.size());
    check(newEvents == 1 && !newFaults, "quiesced map dispatched control");
    port.stop();
}
void firstFaultDiagnostics() {
    auto& port = CoordinatorPort::processInstance();
    port.stop();
    const auto sender = [](const p::Bytes&) { return true; };
    unsigned diagnostics{}, terminal{}, faults{};
    std::vector<unsigned> order;
    CoordinatorFaultDiagnostic first;
    std::string firstMessage;
    bool reentrantPreflightRejected{};
    check(port.arm({true, Role::Join}, 321, 3, sender, [&] {
        ++terminal; order.push_back(2);
    }, {}, [&](const CoordinatorFaultDiagnostic& diagnostic) {
        ++diagnostics; order.push_back(1);
        first = diagnostic;
        firstMessage = diagnostic.message;
        first.message = nullptr; // The diagnostic message is borrowed, never retained.
        std::string error;
        reentrantPreflightRejected = !port.preflight({true, Role::Join}, error);
        port.fail(CoordinatorFailureOrigin::UiApply, "reentrant replacement reason");
    }), "diagnostic prestart arm failed");
    port.fail(CoordinatorFailureOrigin::LocalInvariant, "first pregame fault");
    port.fail(CoordinatorFailureOrigin::LocalInvariant, "later fault");
    check(diagnostics == 1 && terminal == 1 && !faults && reentrantPreflightRejected,
          "prestart diagnostic was not reentrant and exact once");
    check(order == std::vector<unsigned>({1, 2}) && firstMessage == "first pregame fault",
          "diagnostic did not preserve the first reason before terminal");
    check(first.epoch == 321 && first.generation && first.role == Role::Join && !first.started,
          "prestart diagnostic has the wrong arm snapshot");
    port.stop();

    CoordinatorFaultDiagnostic next;
    std::string nextMessage;
    order.clear();
    check(port.arm({true, Role::Host}, 322, 3, sender, [&] {
        ++terminal; order.push_back(2);
    }, {}, [&](const CoordinatorFaultDiagnostic& diagnostic) {
        ++diagnostics; order.push_back(1);
        next = diagnostic;
        nextMessage = diagnostic.message;
        next.message = nullptr;
        throw std::runtime_error("diagnostic sink failed");
    }), "diagnostic next-generation arm failed");
    CoordinatorCallbacks callbacks;
    callbacks.postToUi = [](CoordinatorEvent) {};
    callbacks.terminalFault = [&](CoordinatorTerminalFault fault) {
        ++faults; order.push_back(3);
        check(fault.message == nextMessage, "terminal fault lost the original reason");
    };
    check(port.start({true, Role::Host}, callbacks), "diagnostic started arm failed");
    port.fail(CoordinatorFailureOrigin::UiApply, nullptr);
    port.fail(CoordinatorFailureOrigin::UiApply, "later started fault");
    check(diagnostics == 2 && terminal == 2 && faults == 1
              && order == std::vector<unsigned>({1, 2, 3}),
          "throwing diagnostic suppressed or duplicated terminal notifications");
    check(next.epoch == 322 && next.generation > first.generation
              && next.role == Role::Host && next.started
              && nextMessage == "simultaneous-turn terminal failure",
          "next diagnostic inherited a prior arm or lost its fallback reason");
    port.stop();

    // Both retirement paths release the optional callback and its owned data.
    for (const bool quiesce : {false, true}) {
        auto lifetime = std::make_shared<unsigned>(0);
        const std::weak_ptr<unsigned> weak = lifetime;
        check(port.arm({true, Role::Host}, 323, 3, sender, {}, {},
                       [lifetime, &diagnostics](const CoordinatorFaultDiagnostic&) {
                           ++diagnostics;
                       }), "diagnostic lifetime arm failed");
        lifetime.reset();
        check(!weak.expired(), "armed diagnostic did not own its capture");
        if (quiesce) port.quiesce(); else port.stop();
        check(weak.expired(), "retired diagnostic retained its capture");
        port.fail(CoordinatorFailureOrigin::LocalInvariant, "retired fault");
        check(diagnostics == 2, "retired diagnostic was invoked");
        port.stop();
    }
    check(port.arm({true, Role::Join}, 324, 3, sender, [&] { ++terminal; }),
          "default diagnostic arm failed");
    port.fail(CoordinatorFailureOrigin::LocalInvariant, "default callback fault");
    check(diagnostics == 2 && terminal == 3, "old diagnostic leaked into default arm");
    port.stop();
#ifdef D2_TESTDRV
    check(port.armLocal({true, Role::Host}, sender, [&] { ++terminal; }),
          "default local diagnostic arm failed");
    port.fail(CoordinatorFailureOrigin::LocalInvariant, "local callback fault");
    check(diagnostics == 2 && terminal == 4, "diagnostic changed the local arm contract");
    port.stop();
#endif
}
#ifdef D2_TESTDRV
void localTransportUsesTheSameCore() {
    auto& port = CoordinatorPort::processInstance();
    const auto sender = [](const p::Bytes&) { return true; };
    for (unsigned variant = 0; variant != 6; ++variant) {
        port.stop();
        unsigned faults{}, terminal{}, events{};
        check(port.armLocal({true, Role::Host}, sender, [&] { ++terminal; }), "local arm failed");
        check(!port.armLocal({true, Role::Join}, sender), "active local arm replaced");
        CoordinatorCallbacks callbacks;
        callbacks.postToUi = [&](CoordinatorEvent) { ++events; };
        callbacks.terminalFault = [&](CoordinatorTerminalFault) { ++faults; };
        check(port.start({true, Role::Host}, callbacks), "local start failed");
        check(port.bindLocalPlayer(1), "local bind failed");
        auto first = packet(p::Op::SessionPlan, {912, 1, 1, 2, 7, 81, 82});
        if (variant == 1) first = packet(p::Op::SessionPlan, {912, 0, 1, 2, 0, 0, 0});
        if (variant == 2) first = packet(p::Op::SessionPlan, {0, 1, 1, 2, 7, 81, 82});
        if (variant == 3) first = packet(p::Op::EngineAction, {912, 1, 1, 2, 1, 82});
        if (variant == 4) first = packet(p::Op::SessionPlan, {912, 1, 1, 2, 1, 81, 82});
        port.receive(first.data(), first.size());
        if (variant == 0 || variant == 5) {
            check(events == 1 && !faults, "first local OH identity rejected");
            const auto next = variant == 0 ? first
                : packet(p::Op::SessionPlan, {911, 1, 1, 2, 7, 81, 82});
            port.receive(next.data(), next.size());
            check(events == 1 && faults == 1 && terminal == 1,
                  "duplicate/stale local SessionPlan replaced identity");
        } else {
            check(!events && faults == 1 && terminal == 1, "invalid first local control accepted");
        }
    }
    port.stop();
    check(!port.armLocal({false, Role::Host}, sender), "local opt-out armed");
    check(!port.arm({true, Role::Host}, 0, 0, sender), "local API weakened lobby epoch check");
    {
        Fixture f;
        f.receive(packet(p::Op::SessionPlan, {912, 1, 1, 2, 7, 81, 82}));
        check(f.faults == 1 && f.terminal == 1 && !f.events,
              "retired local identity weakened a subsequent lobby arm");
    }
}
#endif
}
int main() {
    try { malformedFrames(); lifecycleAndDelivery(); retiredCallbackCannotFaultNewSession();
        firstFaultDiagnostics();
#ifdef D2_TESTDRV
        localTransportUsesTheSameCore();
#endif
        std::cout << "lobby port: exact frames, epochs, no downgrade, write barriers, terminal failures and new-map reset passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
