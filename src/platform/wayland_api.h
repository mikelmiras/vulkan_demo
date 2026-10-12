// libwayland-client, libwayland-cursor and libxkbcommon, loaded when the Wayland backend starts (dlopen) instead of
// linked: a program built with Wayland still starts on a machine without those libraries (on X11), and a missing
// libwayland-cursor only costs the themed pointer. Include this before anything that includes wayland-client.h
// (vulkan.h with VK_USE_PLATFORM_WAYLAND_KHR, the protocol headers): from here on the libraries' function names mean
// the loaded pointers, so the protocol headers' inline functions call through them too. The core protocol's
// interfaces (wl_surface_interface...) are compiled in (third_party/wayland-protocols/wayland-protocol.c).
#pragma once

#include <wayland-client-core.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

#define VKWORLD_WL_CLIENT_FUNCTIONS(X)                                                                     \
    X(wl_display_connect) X(wl_display_disconnect) X(wl_display_roundtrip) X(wl_display_dispatch_pending) \
    X(wl_display_flush) X(wl_display_prepare_read) X(wl_display_read_events) X(wl_display_cancel_read)    \
    X(wl_display_get_fd) X(wl_display_get_error) X(wl_proxy_marshal_flags) X(wl_proxy_add_listener)       \
    X(wl_proxy_destroy) X(wl_proxy_get_version) X(wl_proxy_set_user_data) X(wl_proxy_get_user_data)
#define VKWORLD_WL_CURSOR_FUNCTIONS(X)                                                                     \
    X(wl_cursor_theme_load) X(wl_cursor_theme_destroy) X(wl_cursor_theme_get_cursor) X(wl_cursor_image_get_buffer)
#define VKWORLD_XKB_FUNCTIONS(X)                                                                           \
    X(xkb_context_new) X(xkb_context_unref) X(xkb_keymap_new_from_string) X(xkb_keymap_unref)               \
    X(xkb_state_new) X(xkb_state_unref) X(xkb_state_update_mask) X(xkb_state_mod_name_is_active)          \
    X(xkb_state_key_get_utf8) X(xkb_keymap_key_get_syms_by_level) X(xkb_keysym_to_lower) X(xkb_keymap_key_repeats)

namespace platform::wl::api {

#define VKWORLD_WL_DECLARE(fn) extern decltype(&::fn) fn;
VKWORLD_WL_CLIENT_FUNCTIONS(VKWORLD_WL_DECLARE)
VKWORLD_WL_CURSOR_FUNCTIONS(VKWORLD_WL_DECLARE)
VKWORLD_XKB_FUNCTIONS(VKWORLD_WL_DECLARE)
#undef VKWORLD_WL_DECLARE

// libwayland-client and libxkbcommon (once; false if either is missing or too old), then libwayland-cursor if
// it's there (cursorLoaded())
bool load();
bool cursorLoaded();

}  // namespace platform::wl::api

#ifndef VKWORLD_WAYLAND_API_IMPLEMENTATION   // (wayland_api.cpp defines the pointers by their own names)
#define wl_display_connect platform::wl::api::wl_display_connect
#define wl_display_disconnect platform::wl::api::wl_display_disconnect
#define wl_display_roundtrip platform::wl::api::wl_display_roundtrip
#define wl_display_dispatch_pending platform::wl::api::wl_display_dispatch_pending
#define wl_display_flush platform::wl::api::wl_display_flush
#define wl_display_prepare_read platform::wl::api::wl_display_prepare_read
#define wl_display_read_events platform::wl::api::wl_display_read_events
#define wl_display_cancel_read platform::wl::api::wl_display_cancel_read
#define wl_display_get_fd platform::wl::api::wl_display_get_fd
#define wl_display_get_error platform::wl::api::wl_display_get_error
#define wl_proxy_marshal_flags platform::wl::api::wl_proxy_marshal_flags
#define wl_proxy_add_listener platform::wl::api::wl_proxy_add_listener
#define wl_proxy_destroy platform::wl::api::wl_proxy_destroy
#define wl_proxy_get_version platform::wl::api::wl_proxy_get_version
#define wl_proxy_set_user_data platform::wl::api::wl_proxy_set_user_data
#define wl_proxy_get_user_data platform::wl::api::wl_proxy_get_user_data
#define wl_cursor_theme_load platform::wl::api::wl_cursor_theme_load
#define wl_cursor_theme_destroy platform::wl::api::wl_cursor_theme_destroy
#define wl_cursor_theme_get_cursor platform::wl::api::wl_cursor_theme_get_cursor
#define wl_cursor_image_get_buffer platform::wl::api::wl_cursor_image_get_buffer
#define xkb_context_new platform::wl::api::xkb_context_new
#define xkb_context_unref platform::wl::api::xkb_context_unref
#define xkb_keymap_new_from_string platform::wl::api::xkb_keymap_new_from_string
#define xkb_keymap_unref platform::wl::api::xkb_keymap_unref
#define xkb_state_new platform::wl::api::xkb_state_new
#define xkb_state_unref platform::wl::api::xkb_state_unref
#define xkb_state_update_mask platform::wl::api::xkb_state_update_mask
#define xkb_state_mod_name_is_active platform::wl::api::xkb_state_mod_name_is_active
#define xkb_state_key_get_utf8 platform::wl::api::xkb_state_key_get_utf8
#define xkb_keymap_key_get_syms_by_level platform::wl::api::xkb_keymap_key_get_syms_by_level
#define xkb_keysym_to_lower platform::wl::api::xkb_keysym_to_lower
#define xkb_keymap_key_repeats platform::wl::api::xkb_keymap_key_repeats
#endif
