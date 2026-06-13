// Precompiled header — build-time optimization only (no behavioral change).
//
// Collects heavy, stable third-party headers that dominate parse time so MSVC
// parses them once into the .pch and reuses it across all ~480 C++ TUs. It is
// force-included into every C++ TU via <ForcedIncludeFiles> in mss32.vcxproj,
// so no source files change.
//
// Deliberately conservative — only include-order-insensitive, self-contained
// library headers:
//  - sol2 (sol/sol.hpp), a ~25k-line single header and the single biggest parse
//    cost; it pulls only the lua headers.
//  - Common STL containers/utilities pulled by nearly every TU.
//  - NO <windows.h>: some networking TUs (SLikeNet) need <winsock2.h> before
//    <windows.h>; force-including windows.h first would break that ordering.
//  - NO fmt/spdlog: spdlog bundles its own fmt (SPDLOG_FMT_EXTERNAL is not set)
//    and the pinned spdlog needs fmt >= 8 while the project pins fmt 7, so
//    force-including both clashes. Revisit if fmt is bumped.
//  - The C sources (lua) and fmt's own .cc opt out of the PCH per-file in the
//    project, so this C++ header is never forced into a C compile.
#pragma once

#ifdef __cplusplus

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sol/sol.hpp>

#endif // __cplusplus
