#pragma once

#include <cstddef>
#include <string>

namespace twitchstat {

// Data points into this plugin's read-only resources and remains valid until unload.
bool localBridgeAsset(const std::string& path, const char** data,
                      size_t* size, const char** mime);

} // namespace twitchstat
