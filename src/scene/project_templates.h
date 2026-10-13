// New projects from a template (the editor's File > New project): a folder with a project.json, a start scene
// (scenes/main.vkw) and what the template's game needs.
//
//   Empty         a sun, a ground and a camera
//   First person  a PlayerController body with its camera at eye height, a few blocks and a ramp to walk over
//   Top-down      a character moved by a Lua script (scripts/TopDownPlayer.lua) and a camera following it from
//                 above, things to collect and a score on screen
#pragma once

#include <filesystem>
#include <string>

namespace project_templates {

enum class Template { Empty, FirstPerson, TopDown, Count };

const char* name(Template t);
const char* description(Template t);

// Writes the project into `dir` (created if missing; it must not hold a project already). `projectName` "": the
// folder's name. With a .gitignore for what the engine generates there (bin/, .vkworld/, updates).
bool create(const std::filesystem::path& dir, Template t, const std::string& projectName, std::string& error);

}  // namespace project_templates
