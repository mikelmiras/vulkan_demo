// Updates: is there a newer version than this one, and installing it. vkgame asks the project's updateUrl at start
// (players can turn it off: settings.checkUpdates) and puts the answer in the game state for menus; the editor's
// Help > Check for updates asks about the engine (it only says what's out and where).
//
// Installing (a game's menu: "@update", then "@restart"): the release names this platform's update pack (an export's
// .vkwupdate: the whole exported game as one pack, app/export.h) and its SHA-256. It's downloaded next to the game
// (<game>/.update/), checked against the digest (no digest, or a wrong one: nothing is installed), unpacked, and
// swapped in: each file of the game moved aside to <file>.old-update and the new one put in its place (all undone if
// one fails), so a restart runs the new version; the next start removes what was moved aside.
//
// Chunked releases (what exports make: app/export.h): each file of the release listed with its digest and cut into
// chunks by its content (a rolling hash picks the cuts, so a change only alters the chunks around it), the chunks
// served as files named by their SHA-256. Installing only downloads the chunks the installed files don't already
// have (delta updates, from any earlier version), keeps each one as it arrives (<game>/.update/chunks: an interrupted
// download resumes where it stopped), and only replaces the files that changed. In an AppImage the AppImage file
// itself is updated (the release's "linux-appimage" files: the new AppImage, rebuilt from the old one's chunks).
//
// Signed manifests: with the project's "updateKey" (an Ed25519 public key, hex: vkgame --update-keygen) a release is
// only taken if its manifest is signed with the matching secret key (vkgame --sign-manifest): the signature covers
// the digests of every file and chunk, so a compromised server can't hand out anything else.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/json.h"

namespace updates {

// A file of a chunked release: its digest, size and chunks in order
struct Chunk {
    std::string sha256;
    uint32_t size = 0;
};
struct ReleaseFile {
    std::string path;   // relative to the game's folder ("bin/Game", "game.pak")
    uint64_t size = 0;
    std::string sha256;
    bool executable = false;
    std::vector<Chunk> chunks;
};

struct Release {
    std::string version;   // "1.2.0"
    std::string url;       // where to get it (a page to open)
    std::string notes;     // what's new (may be empty)
    std::string download;  // this platform's update pack ("" if it has none: then it can only be opened)
    std::string sha256;    // ... and its digest (hex)
    std::vector<ReleaseFile> files;   // (a chunked release) this platform's files
    std::string chunkUrl;             // ... where its chunks are (chunkUrl + sha256)
    bool chunked() const { return !files.empty(); }
};

const char* platformKey();   // "linux", "windows", "macos"
// What this program updates as: platformKey(), or "linux-appimage" when it runs from an AppImage
std::string runningPlatform();
// The AppImage this runs from ("" if none: the APPIMAGE variable its runtime sets)
std::filesystem::path appImagePath();

// Content-defined chunking: where to cut (offsets and sizes, covering all of it); 16 to 256 KB, 64 KB on average
std::vector<std::pair<size_t, size_t>> cutChunks(const uint8_t* data, size_t size);
// A file described (and, given a folder, its chunks written there as <sha256>, unless already there)
bool describeFile(const std::filesystem::path& file, const std::string& path, ReleaseFile& out, std::string& error,
                  const std::filesystem::path& chunkDir = {});
json::Value filesToJson(const std::vector<ReleaseFile>& files);
std::vector<ReleaseFile> filesFromJson(const json::Value& v);

// Signing manifests: the text signed (the manifest without its "signature", as json::write gives it), a signature
// added, and checked against a public key (hex); false and why when it isn't signed, or not by that key
std::string signedText(const json::Value& manifest);
bool signManifest(json::Value& manifest, const std::string& secretKeyHex, std::string& error);
bool checkSignature(const json::Value& manifest, const std::string& publicKeyHex, std::string& error);

// What an update URL serves: {"version": "1.2.0", "url": "https://...", "notes": "...", "downloads": {"linux":
// {"url": "...", "sha256": "..."}, ...}}, or GitHub's latest release (https://api.github.com/repos/<owner>/<repo>/
// releases/latest: tag_name, html_url, body, and the asset whose name has the platform in it and ends with
// .vkwupdate, its digest from the asset's "digest": "sha256:...")
// A chunked release's platforms are {"files": [...], "chunks": "chunks/"} (relative to the manifest's URL; resolved by
// Check). With `publicKey` the manifest must be signed by it.
bool parseRelease(const std::string& json, Release& out, std::string& error, const std::string& platform = platformKey(),
                  const std::string& publicKey = "");
// A relative URL against the one it came from ("chunks/" next to "https://x/game/manifest.json")
std::string resolveUrl(const std::string& base, const std::string& relative);

// The files of `staged` (an unpacked update) swapped into `installDir` (see above); false and why if any can't be,
// with the game as it was. Unix permissions of the files replaced are kept.
bool swapIn(const std::filesystem::path& staged, const std::filesystem::path& installDir, std::string& error);
// What the last swap moved aside, and the update's leftovers, removed (at start: the old files aren't running now;
// only when <installDir>/.update is there, which an install leaves until then). An update not installed yet keeps the
// chunks it has downloaded, for the next try to resume with.
void cleanUp(const std::filesystem::path& installDir);

// One check, on a worker thread of its own (a slow network never holds up the game or its exit)
class Check {
public:
    using Fetch = std::function<bool(const std::string& url, std::string& body, std::string& error)>;
    // `current`: this program's version. `fetch`: platform::httpGet by default (tests give their own). `publicKey`:
    // the project's updateKey ("": manifests needn't be signed). `platform`: runningPlatform() by default.
    void start(const std::string& url, const std::string& current, Fetch fetch = nullptr, const std::string& publicKey = "",
               const std::string& platform = "");
    bool started() const { return state_ != nullptr; }
    bool ready() const;          // answered (or failed)
    bool newer() const;          // a version after `current` is out
    Release release() const;     // what's out (when ready and fetched)
    std::string error() const;   // why it failed ("" if it didn't)

private:
    struct State;
    std::shared_ptr<State> state_;
};

// Downloading, checking and unpacking an update, on a worker thread; apply() then swaps it in (the caller's thread)
class Install {
public:
    using Fetch = Check::Fetch;
    enum class Stage { Idle, Downloading, Verifying, Unpacking, Ready, Failed };
    // `renameTo`: (a release of one file: an AppImage) the name it has here, in installDir
    void start(const Release& release, const std::filesystem::path& installDir, Fetch fetch = nullptr,
               const std::string& renameTo = "");
    Stage stage() const;
    std::string error() const;
    float progress() const;         // (downloading) 0..1 of what has to be downloaded
    uint64_t downloaded() const;    // bytes fetched (a chunked release: only the chunks missing here)
    bool apply(std::string& error);   // (when Ready) swapIn
    static const char* stageName(Stage stage);   // "downloading", ..., for the game state

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace updates
