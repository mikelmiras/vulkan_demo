// A debugger for Lua behaviours, over the Debug Adapter Protocol (what VS Code and other editors speak): the game
// listens on a TCP port; an editor attaches (tools/vscode-vkworld-lua: a small VS Code extension that does), sets
// breakpoints in scripts/*.lua, and the game stops there: the stack, locals, upvalues and tables to look through,
// expressions to evaluate (watches, hovers), stepping over, in and out, pausing, continuing. Breakpoints can have a
// condition, a hit count, or be logpoints (a message with {expressions}, printed in the editor's console and the
// log, without stopping); with the "Script errors" exception filter on, it also stops where a script raises an
// error, before it's reported. While stopped, the game waits (the window doesn't redraw) until the editor says go on.
//
//   scripting::debugger::listen(4711, error);   // editor: Game > Lua debugger; vkgame --lua-debug [port]
//   every frame: scripting::debugger::poll();
#pragma once

#include <cstdint>
#include <string>

struct lua_State;
struct lua_Debug;

namespace scripting::debugger {

bool listen(uint16_t port, std::string& error);   // 0: any free port (port() tells which)
void stop();
bool listening();
uint16_t port();
bool attached();             // an editor is connected
std::string status();        // "Lua debugger on port 4711: attached"
void poll();                 // accepts an editor, answers it while the game runs

// (lua_script.cpp's hook) whether line events are wanted, and one of them: seconds spent stopped there
bool wantsLines();
double onLine(lua_State* L, lua_Debug* ar);
// (lua_script.cpp's error handler, where an error was raised, the stack still whole) stops there if the editor
// asked to stop on script errors: seconds spent stopped
double onError(lua_State* L, const char* message);

}  // namespace scripting::debugger
