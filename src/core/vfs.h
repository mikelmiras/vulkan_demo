// The files a game reads: from a pack (game.pak, or one appended to the game's executable) when one is mounted,
// else from the disk. A pack holds files by their path relative to the folder it stands for (the project, next to
// the executable), so a packed game finds "scenes/main.vkw" or "shaders/scene.vert.spv" exactly where they were.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace vfs {

// Mounts the pack in `file` (a .pak, or an executable with one appended) for the folder `root`; false if there's
// none. Files in it shadow the disk.
bool mount(const std::filesystem::path& file, const std::filesystem::path& root);
void unmount();
bool mounted();

bool read(const std::filesystem::path& path, std::string& out);   // the whole file
bool exists(const std::filesystem::path& path);                   // a file (in the pack or on the disk)

// Writing packs: files (path in the pack, path on the disk) into `out`, or appended to the end of `out` (an
// executable) when `append`; each file compressed when that makes it worth it (unless !compress)
bool writePack(const std::filesystem::path& out, const std::vector<std::pair<std::string, std::filesystem::path>>& files,
               bool append, std::string& error, bool compress = true);

}  // namespace vfs
