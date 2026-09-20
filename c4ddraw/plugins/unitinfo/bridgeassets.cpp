#include "bridgeassets.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace twitchstat {
namespace {

struct Asset {
    const char* path;
    WORD resource;
    const char* mime;
};

const Asset kAssets[] = {
    {"/relay.html", 101, "text/html; charset=utf-8"},
    {"/relay.mjs", 102, "text/javascript; charset=utf-8"},
    {"/broadcaster.mjs", 103, "text/javascript; charset=utf-8"},
    {"/config.html", 104, "text/html; charset=utf-8"},
    {"/control.css", 105, "text/css; charset=utf-8"},
    {"/game-text.mjs", 106, "text/javascript; charset=utf-8"},
    {"/live_config.html", 107, "text/html; charset=utf-8"},
    {"/overlay.css", 108, "text/css; charset=utf-8"},
    {"/protocol.mjs", 109, "text/javascript; charset=utf-8"},
    {"/video_overlay.html", 110, "text/html; charset=utf-8"},
    {"/viewer.mjs", 111, "text/javascript; charset=utf-8"},
};

// The host loads .c4p files as DLLs. Never look up resources in the game's EXE.
const unsigned char kModuleAnchor = 0;

} // namespace

bool localBridgeAsset(const std::string& path, const char** data,
                      size_t* size, const char** mime)
{
    if (!data || !size || !mime)
        return false;
    *data = nullptr;
    *size = 0;
    *mime = nullptr;

    for (const Asset& asset : kAssets) {
        if (path != asset.path)
            continue;
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&kModuleAnchor), &module))
            return false;
        const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(asset.resource), RT_RCDATA);
        if (!resource)
            return false;
        const DWORD bytes = SizeofResource(module, resource);
        const HGLOBAL loaded = LoadResource(module, resource);
        const void* contents = loaded ? LockResource(loaded) : nullptr;
        if (!contents || !bytes)
            return false;
        *data = static_cast<const char*>(contents);
        *size = bytes;
        *mime = asset.mime;
        return true;
    }
    return false;
}

} // namespace twitchstat
