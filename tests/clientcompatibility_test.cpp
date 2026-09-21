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
        cache.start([gate, &calls, mainThread] {
            ++calls;
            if (std::this_thread::get_id() == mainThread) throw std::runtime_error("hash ran on UI thread");
            gate.wait();
            return digest;
        });
        cache.start([&calls] { ++calls; return std::string("duplicate computation"); });
        const bool polling = cache.value().empty() && cache.pending();
        release.set_value(); // Always release the worker before an assertion can unwind its future.
        require(polling, "poll blocked or published before completion");
        require(cache.value(true) == digest && !cache.pending(), "host/join did not consume the login future");
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

        std::cout << "client compatibility: exact wire, one asynchronous cache, errors, teardown and bounded publication passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
