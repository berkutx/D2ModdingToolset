#pragma once

#include <cstddef>
#include <string>

namespace twitchstat {

enum class LocalBridgeState { Stopped, Starting, Listening, PortBusy, Failed };

// All entry points are safe on the game thread: they never perform network I/O
// or wait for a client. A disabled bridge never automatically takes a busy port.
void localBridgeSetEnabled(bool enabled);
void localBridgePublish(const std::string& frame, unsigned long long capturedAt, bool active);
LocalBridgeState localBridgeState();
int localBridgeError();

// Asynchronous, permanent shutdown. The plugin must remain loaded while its
// worker exits; in production its module is pinned for the process lifetime.
void localBridgeShutdown();

// Exact allowlisted, embedded assets only. Implemented by bridgeassets.cpp.
bool localBridgeAsset(const std::string& path, const char** data, size_t* size, const char** mime);

}
