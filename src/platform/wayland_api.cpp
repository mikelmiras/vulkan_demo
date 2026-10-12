// The Wayland libraries' functions, found at run time (wayland_api.h)
#define VKWORLD_WAYLAND_API_IMPLEMENTATION
#include "platform/wayland_api.h"

#include <dlfcn.h>

#include <type_traits>

namespace platform::wl::api {

#define VKWORLD_WL_DEFINE(fn) decltype(&::fn) fn = nullptr;
VKWORLD_WL_CLIENT_FUNCTIONS(VKWORLD_WL_DEFINE)
VKWORLD_WL_CURSOR_FUNCTIONS(VKWORLD_WL_DEFINE)
VKWORLD_XKB_FUNCTIONS(VKWORLD_WL_DEFINE)
#undef VKWORLD_WL_DEFINE

namespace {
int g_loaded = 0;   // 0: not tried, 1: loaded, -1: not there
bool g_cursor = false;
}  // namespace

bool load() {
    if (g_loaded) return g_loaded > 0;
    g_loaded = -1;
    void* client = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    void* xkb = dlopen("libxkbcommon.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!client || !xkb) return false;
    bool ok = true;
#define VKWORLD_WL_FIND(lib) [&](auto& fn, const char* name) { \
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name)); \
        return fn != nullptr; }
    auto fromClient = VKWORLD_WL_FIND(client);
    auto fromXkb = VKWORLD_WL_FIND(xkb);
#define VKWORLD_WL_CLIENT_GET(fn) ok = fromClient(fn, #fn) && ok;
#define VKWORLD_WL_XKB_GET(fn) ok = fromXkb(fn, #fn) && ok;
    VKWORLD_WL_CLIENT_FUNCTIONS(VKWORLD_WL_CLIENT_GET)
    VKWORLD_XKB_FUNCTIONS(VKWORLD_WL_XKB_GET)
    if (!ok) return false;   // (too old: wl_proxy_marshal_flags is libwayland 1.20)
    if (void* cursor = dlopen("libwayland-cursor.so.0", RTLD_NOW | RTLD_LOCAL)) {
        auto fromCursor = VKWORLD_WL_FIND(cursor);
        bool all = true;
#define VKWORLD_WL_CURSOR_GET(fn) all = fromCursor(fn, #fn) && all;
        VKWORLD_WL_CURSOR_FUNCTIONS(VKWORLD_WL_CURSOR_GET)
        g_cursor = all;
    }
    g_loaded = 1;
    return true;
}

bool cursorLoaded() { return g_cursor; }

}  // namespace platform::wl::api
