#include "clientcompatibility.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool yes, const char* reason) { if (!yes) throw std::runtime_error(reason); }
const std::string digest = "0123456789abcdef0123456789abcdef";
}

int main()
{
    try {
        using namespace hooks::compatibility;
        const auto payload = encode(digest);
        require(payload && payload->size() == 33 && (*payload)[0] == 1, "wire size/version mismatch");
        require(std::string(payload->begin() + 1, payload->end()) == digest, "wire changed the existing hash");
        require(!encode("") && !encode(digest.substr(1)) && !encode(digest + "0")
            && !encode("0123456789ABCDEF0123456789ABCDEF"), "invalid hash accepted");
        auto invalid = digest; invalid[10] = '\0';
        require(!encode(invalid), "embedded NUL accepted");
        invalid[10] = '/'; require(!encode(invalid), "nonhex accepted");

        std::atomic<unsigned> calls{};
        const auto mainThread = std::this_thread::get_id();
        std::promise<void> release;
        auto gate = release.get_future().share();
        FilesHashCache cache;
        require(!cache.retryUnavailable(), "unused cache reported a retry");
        cache.start([gate, &calls, mainThread] {
            ++calls;
            if (std::this_thread::get_id() == mainThread) throw std::runtime_error("hash ran on UI thread");
            gate.wait();
            return digest;
        });
        cache.start([&calls] { ++calls; return std::string("duplicate computation"); });
        const bool polling = cache.value().empty() && cache.pending();
        const bool pendingNotReset = !cache.retryUnavailable() && cache.started() && cache.pending();
        release.set_value(); // Always release the worker before an assertion can unwind its future.
        require(polling, "poll blocked or published before completion");
        require(pendingNotReset, "explicit retry reset a pending worker");
        require(cache.value(true) == digest && !cache.pending(), "host/join did not consume the login future");
        require(!cache.retryUnavailable(), "explicit retry reset a valid cache");
        cache.start([&calls] { ++calls; return std::string("changed"); });
        require(cache.value() == digest && calls == 1, "login/host/join recomputed the cache");

        FilesHashCache failing;
        failing.start([]() -> std::string { throw std::runtime_error("read failure"); });
        require(failing.value(true).empty() && failing.started() && !failing.pending(), "worker error escaped");
        failing.start([&calls] { ++calls; return digest; });
        require(failing.value(true).empty() && calls == 1, "failed hash entered a retry loop");
        FilesHashCache malformed;
        malformed.start([] { return std::string("not a digest"); });
        require(malformed.value(true).empty(), "invalid worker result was cached");
        FilesHashCache unavailable;
        unavailable.unavailable();
        unavailable.start([&calls] { ++calls; return digest; });
        require(unavailable.value(true).empty() && calls == 1, "enumeration failure retried");

        Publication retryPublication;
        retryPublication.begin(); retryPublication.stop(); // Background saw an unknown hash.
        const bool manualRetry = failing.retryUnavailable();
        if (manualRetry) retryPublication.begin(); // Same rearm used by authenticated host/join.
        require(manualRetry && !failing.started() && !failing.pending(), "manual retry did not reset failure");
        failing.start([&calls] { ++calls; return digest; });
        require(failing.value(true) == digest && calls == 2 && retryPublication.due(0),
                "failure -> manual retry did not recover/publicize the same hash");
        require(!failing.retryUnavailable(), "successful manual retry lost its cache");
        require(unavailable.retryUnavailable(), "explicit retry did not reset enumeration failure");
        unavailable.start([] { return digest; });
        require(unavailable.value(true) == digest, "enumeration failure did not recover");

        // Do not pre-consume this future: retryUnavailable must itself notice and
        // consume a ready exception, while never blocking on a pending worker.
        FilesHashCache readyFailure;
        readyFailure.start([]() -> std::string { throw std::runtime_error("ready failure"); });
        bool resetReadyFailure = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!(resetReadyFailure = readyFailure.retryUnavailable())
               && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        require(resetReadyFailure && !readyFailure.started() && !readyFailure.pending(),
                "explicit retry did not consume a ready exception");
        readyFailure.start([] { return digest; });
        require(readyFailure.value(true) == digest, "ready failure did not recover");
        std::atomic<bool> finished{};
        {
            FilesHashCache owned;
            owned.start([&finished] { finished = true; return digest; });
        }
        require(finished, "worker outlived the cache");

        Publication publication;
        require(!publication.due(0), "send before login");
        publication.begin(); require(publication.due(0), "login did not arm publication");
        publication.attempted(false, 0);
        require(!publication.due(999) && publication.due(1000), "retry flooded or not due");
        publication.attempted(false, 1000);
        require(!publication.due(1999) && publication.due(2000), "second retry interval");
        publication.attempted(false, 2000);
        require(!publication.due(100000), "more than three attempts");
        publication.begin(); require(publication.due(100000), "relogin did not arm cached hash");
        publication.attempted(true, 100000);
        require(!publication.due(200000), "reliable send resent");
        publication.begin(); publication.stop();
        require(!publication.due(200000), "logout/disconnect retained a send");

        std::cout << "client compatibility: exact wire, one asynchronous cache, manual-only error retry, teardown and bounded publication passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
