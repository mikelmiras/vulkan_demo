// Dear ImGui platform backend for raw XCB (Linux X11 / XWayland).
// Feeds mouse, keyboard (layout-aware via the server keymap), text input and focus into ImGui.
#pragma once

#include <xcb/xcb.h>

bool ImGui_ImplXcb_Init(xcb_connection_t* connection, xcb_window_t window);
void ImGui_ImplXcb_Shutdown();
void ImGui_ImplXcb_NewFrame(float displayWidth, float displayHeight, float deltaTime);

// Drains the X event queue. Input events go to ImGui; everything else (configure, client
// messages, ...) is forwarded to `handler`. Keyboard auto-repeat release/press pairs are merged.
void ImGui_ImplXcb_PollEvents(void (*handler)(void* user, xcb_generic_event_t* event), void* user);
