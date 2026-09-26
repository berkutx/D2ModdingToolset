#include "netintercept.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <type_traits>

using hooks::netintercept::NativeReceiveDiagnostic;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void ownedBoundedHeader() {
    static_assert(std::is_trivially_destructible_v<NativeReceiveDiagnostic>);
    static_assert(std::is_trivially_copyable_v<NativeReceiveDiagnostic>);
    char source[36] = ".?AVCConnectMsg@@";
    NativeReceiveDiagnostic d;
    d.captureHeader(0xffff, 120, source);
    std::memset(source, 'x', sizeof(source));
    check(std::strcmp(d.messageClass, ".?AVCConnectMsg@@") == 0, "header aliases native buffer");
    check(d.messageType == 0xffff && d.frameLength == 120, "header fields lost");
    d.captureHeader(0xffff, 44, source);
    check(std::strlen(d.messageClass) == 36 && d.messageClass[36] == 0, "unbounded class name");
    source[0] = '\r'; source[1] = '\n'; source[2] = '\t'; source[3] = '\x7f';
    source[4] = static_cast<char>(0xff); source[5] = 0;
    d.captureHeader(0xffff, 44, source);
    check(std::strcmp(d.messageClass, "?????") == 0, "remote class can inject a log line");
    check(d.messageClass[6] == 0 && d.messageClass[35] == 0, "reuse retained old class bytes");
    d.sender = 11; d.receiver = 22; d.threadId = 33;
    d.replay = true; d.dispatched = true; d.dispatchResult = 0;
    const auto queued = d;
    d = {};
    check(queued.sender == 11 && queued.receiver == 22 && queued.threadId == 33
          && queued.replay && queued.dispatched && queued.dispatchResult == 0
          && std::strcmp(queued.messageClass, "?????") == 0, "UI completion lost owned evidence");
}
}
int main() {
    try {
        ownedBoundedHeader();
        std::cout << "native diagnostic: bounded, sanitized, owned header and SEH-safe value passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
