# The sample game's module: its behaviours, loaded by the editor and vkgame when they open this project.
# Output: bin/ in this folder (see "module" in project.json).
vkworld_add_game_module(sample_game
  PROJECT_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  SOURCES src/sample_game.cpp)
