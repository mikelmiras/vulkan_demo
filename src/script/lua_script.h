// Lua behaviours: game logic in scripts, next to (or instead of) a C++ game module.
//
// Every .lua file under a project's scripts/ folder is a behaviour type named after the file (scripts/Door.lua:
// "Door"), in the inspector's Add component menu like the C++ ones. A script returns a table: the behaviour's
// description, its fields (edited in the inspector, saved in scenes) and its callbacks, which get the instance as
// `self` (the fields are its members; self.entity is the entity it's on):
//
//   local Door = {
//       description = "Opens when the player comes near",
//       fields = {
//           {"openHeight", 3, min = 0, max = 10},        -- a number (int = true: whole numbers)
//           {"speed", 1.5},
//           {"player", "Player", ref = "entity"},       -- a string; ref: entity, prefab, scene, sound, clip, action
//           {"mode", "slide", options = {"slide", "swing"}},
//           {"locked", false},                           -- a boolean
//           {"offset", vec3(0, 1, 0)},                   -- a vector
//       },
//   }
//   function Door:start() self.closedY = self.entity.position.y end
//   function Door:update(dt)
//       local p = self.entity.position
//       p.y = p.y + self.speed * dt
//       self.entity.position = p
//   end
//   return Door
//
// Callbacks (all optional): start(), update(dt), onTriggerEnter(other), onTriggerExit(other), onMessage(msg),
// onAnimationEvent(name), drawUI(ui). The API they use (`game`, `input`, entities, vec3, ui) is listed in
// README.md ("Lua scripting"); it mirrors GameContext.
//
// Scripts are reloaded when their file changes (poll()): running instances keep their members and pick up the new
// functions, so a game can be edited while it plays. An error in a callback is logged (in the viewport, too) and
// stops that instance's callbacks until the script is changed. There is one Lua state, used on the main thread.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace scripting {

// Registers every .lua file under <project>/scripts (on disk or in the mounted pack) as a behaviour type, replacing
// the previous project's. Returns how many loaded; what failed is in errors(). Call before loading the project's
// scenes, so their script behaviours aren't left as MissingBehaviour.
int loadProject(const std::filesystem::path& projectRoot);
// Defines (or redefines, in place) a script type from source text, as if it came from a file (tests, tools)
bool define(const std::string& typeName, const std::string& source, std::string& error);
// Looks for scripts that changed, appeared or failed before (at most twice a second unless `force`) and loads
// them; running instances of a reloaded script keep their state. Returns the types that were (re)loaded.
std::vector<std::string> poll(bool force = false);
// Problems from the last loadProject / poll / define: "scripts/Door.lua:12: ..." (cleared when a script loads)
std::vector<std::string> errors();

bool isScriptType(const std::string& typeName);
std::filesystem::path scriptFile(const std::string& typeName);   // where it came from ("" if define()d)
// A new script with the callbacks stubbed out, for the editor's "New Lua script"; `typeName` must be a Lua name
std::string templateSource(const std::string& typeName);

// Drops every script type and the Lua state (tests; a new project does it itself)
void reset();

}  // namespace scripting
