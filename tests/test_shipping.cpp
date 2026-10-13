// Shipping: SHA-256, update manifests, unpacking an update and swapping it in (and back out when it fails),
// installing from a download (checked against its digest), export's update packs, crash reports made fit to send
// and sent
#include "test.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include "app/crash_reports.h"
#include "app/export.h"
#include "app/updates.h"
#include "core/project.h"
#include "core/sha256.h"
#include "core/vfs.h"
#include "platform/platform.h"

namespace fs = std::filesystem;

namespace {

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

// An update pack holding `files` (path, text)
std::string makePack(const fs::path& dir, const std::vector<std::pair<std::string, std::string>>& files) {
    std::vector<std::pair<std::string, fs::path>> list;
    for (const auto& [name, text] : files) {
        writeFile(dir / "src" / name, text);
        list.push_back({name, dir / "src" / name});
    }
    std::string error;
    fs::path pack = dir / "update.vkwupdate";
    REQUIRE(vfs::writePack(pack, list, false, error));
    return readFile(pack);
}

updates::Install::Stage waitFor(const updates::Install& install) {
    for (int i = 0; i < 500; ++i) {
        updates::Install::Stage s = install.stage();
        if (s == updates::Install::Stage::Ready || s == updates::Install::Stage::Failed) return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return install.stage();
}

}  // namespace

TEST(sha256_digests) {
    CHECK(sha256::of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256::of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";   // (two blocks)
    CHECK(sha256::of(two) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    sha256::Hasher h;   // in pieces: the same
    for (char c : two) h.add(&c, 1);
    CHECK(h.hex() == sha256::of(two));
    std::string million(1000000, 'a');
    CHECK(sha256::of(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    fs::path f = test::tempPath("sha.bin");
    writeFile(f, million);
    CHECK(sha256::ofFile(f) == sha256::of(million));
    CHECK(sha256::ofFile(test::tempPath("nothing-here")).empty());
}

TEST(update_manifests) {
    updates::Release r;
    std::string error;
    REQUIRE(updates::parseRelease(R"({"version": "1.2.0", "url": "https://example.com/game", "downloads": {
        "linux": {"url": "https://example.com/game-linux.vkwupdate", "sha256": "ABCDEF"},
        "windows": {"url": "https://example.com/game-windows.vkwupdate", "sha256": "123456"}}})", r, error, "windows"));
    CHECK(r.version == "1.2.0" && r.download == "https://example.com/game-windows.vkwupdate" && r.sha256 == "123456");
    REQUIRE(updates::parseRelease(R"({"version": "1.2.0", "downloads": {"linux": {"url": "x", "sha256": "ABCDEF"}}})", r, error, "linux"));
    CHECK(r.sha256 == "abcdef");   // (lower case)
    REQUIRE(updates::parseRelease(R"({"version": "1.2.0"})", r, error, "macos"));
    CHECK(r.download.empty());
    REQUIRE(updates::parseRelease(R"({"tag_name": "v2.0.0", "html_url": "https://github.com/a/b/releases/v2.0.0", "assets": [
        {"name": "Game-2.0.0-linux.vkwupdate", "browser_download_url": "https://dl/linux", "digest": "sha256:aaaa"},
        {"name": "Game-2.0.0-macos.dmg", "browser_download_url": "https://dl/dmg"},
        {"name": "Game-2.0.0-macos.vkwupdate", "browser_download_url": "https://dl/macos", "digest": "sha256:BBBB"}]})", r, error, "macos"));
    CHECK(r.version == "2.0.0" && r.download == "https://dl/macos" && r.sha256 == "bbbb");
    CHECK(std::string(updates::platformKey()).size() >= 5);
}

// Swapping an unpacked update in: new files in place, the old ones moved aside (removed by cleanUp), permissions
// kept; one that can't be put in place undoes the lot
TEST(update_swap_and_rollback) {
    fs::path root = test::tempPath("swap");
    fs::remove_all(root);
    fs::path install = root / "game", staged = root / "staged";
    writeFile(install / "game.bin", "old game");
    writeFile(install / "data" / "level.txt", "old level");
    writeFile(install / "keep.txt", "untouched");
#if !defined(_WIN32)
    fs::permissions(install / "game.bin", fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read);
#endif
    std::string pack = makePack(root, {{"game.bin", "new game"}, {"data/level.txt", "new level"}, {"data/extra.txt", "added"}});
    std::vector<std::string> files;
    std::string error;
    REQUIRE(vfs::extractPack(root / "update.vkwupdate", staged, files, error));
    CHECK(files == (std::vector<std::string>{"data/extra.txt", "data/level.txt", "game.bin"}));
    REQUIRE(updates::swapIn(staged, install, error));
    CHECK(readFile(install / "game.bin") == "new game");
    CHECK(readFile(install / "data" / "level.txt") == "new level");
    CHECK(readFile(install / "data" / "extra.txt") == "added");
    CHECK(readFile(install / "keep.txt") == "untouched");
    CHECK(readFile(install / "game.bin.old-update") == "old game");
#if !defined(_WIN32)
    CHECK((fs::status(install / "game.bin").permissions() & fs::perms::owner_exec) != fs::perms::none);   // (still runs)
#endif
    fs::create_directories(install / ".update");   // (as an install leaves it)
    updates::cleanUp(install);
    CHECK(!fs::exists(install / "game.bin.old-update") && !fs::exists(install / "data" / "level.txt.old-update"));
    CHECK(fs::exists(install / "game.bin"));
    // A file that can't go in (its folder would be where a file is): everything back as it was
    writeFile(root / "staged2" / "game.bin", "newer game");
    writeFile(root / "staged2" / "keep.txt" / "inside.txt", "keep.txt is a file there");
    CHECK(!updates::swapIn(root / "staged2", install, error));
    CHECK(!error.empty());
    CHECK(readFile(install / "game.bin") == "new game");
    CHECK(!fs::exists(install / "game.bin.old-update"));
    // Not a pack, or one with a path out of its folder
    writeFile(root / "junk.vkwupdate", "this isn't a pack at all, just some bytes");
    CHECK(!vfs::extractPack(root / "junk.vkwupdate", root / "out", files, error));
    (void)pack;
}

// Installing: downloaded (here: a fetch given the bytes), checked against its digest, unpacked, then applied; a wrong
// digest or none installs nothing
TEST(update_install_checks_the_digest) {
    fs::path root = test::tempPath("install");
    fs::remove_all(root);
    fs::path game = root / "game";
    writeFile(game / "game.bin", "v1");
    std::string pack = makePack(root, {{"game.bin", "v2"}, {"notes.txt", "what's new"}});
    auto fetch = [&](const std::string& url, std::string& body, std::string& error) {
        if (url != "https://example.com/u.vkwupdate") {
            error = "404";
            return false;
        }
        body = pack;
        return true;
    };
    updates::Release r;
    r.version = "2.0.0";
    r.download = "https://example.com/u.vkwupdate";
    r.sha256 = sha256::of(pack);
    std::string error;
    {   // a wrong digest
        updates::Install bad;
        updates::Release wrong = r;
        wrong.sha256 = sha256::of("something else");
        bad.start(wrong, game, fetch);
        CHECK(waitFor(bad) == updates::Install::Stage::Failed);
        CHECK(bad.error().find("doesn't match") != std::string::npos);
        CHECK(!bad.apply(error));
        updates::Install none;   // no digest at all
        updates::Release unchecked = r;
        unchecked.sha256.clear();
        none.start(unchecked, game, fetch);
        CHECK(waitFor(none) == updates::Install::Stage::Failed);
        updates::Install missing;   // the download fails
        updates::Release gone = r;
        gone.download = "https://example.com/nothing";
        missing.start(gone, game, fetch);
        CHECK(waitFor(missing) == updates::Install::Stage::Failed);
        CHECK(missing.error().find("404") != std::string::npos);
    }
    CHECK(readFile(game / "game.bin") == "v1");
    updates::Install install;
    install.start(r, game, fetch);
    REQUIRE(waitFor(install) == updates::Install::Stage::Ready);
    CHECK(std::string(updates::Install::stageName(install.stage())) == "ready");
    REQUIRE(install.apply(error));
    CHECK(readFile(game / "game.bin") == "v2");
    CHECK(readFile(game / "notes.txt") == "what's new");
    updates::cleanUp(game);   // (the next start)
    CHECK(!fs::exists(game / ".update") && !fs::exists(game / "game.bin.old-update"));
}

#ifdef VKWORLD_SAMPLE_GAME
// An export's update pack: every file of the export (the game itself too), with its digest next to it
TEST(export_update_pack) {
    fs::path sample = fs::u8path(VKWORLD_SAMPLE_GAME), out = test::tempPath("exported-update");
    fs::remove_all(out);
    ExportOptions options;
    options.layout = ExportOptions::Layout::Pack;
    options.updatePack = true;
    ExportResult result;
    std::string error;
    REQUIRE(exportGame(sample, out, platform::executableDir(), result, error, options));
    REQUIRE(fs::is_regular_file(result.updatePack));
    CHECK(result.updatePack.filename().u8string().find(updates::platformKey()) != std::string::npos);
    CHECK(result.updateSha256 == sha256::ofFile(result.updatePack));
    CHECK(readFile(fs::path(result.updatePack) += ".sha256").rfind(result.updateSha256, 0) == 0);
    std::vector<std::string> files;
    REQUIRE(vfs::extractPack(result.updatePack, test::tempPath("unpacked-update"), files, error));
    std::string exe = fs::relative(result.executable, out).generic_u8string();
    CHECK(std::find(files.begin(), files.end(), exe) != files.end());
    for (const std::string& f : files) CHECK(f.find(".vkwupdate") == std::string::npos);
}
#endif

// Crash reports: the home folder and the user's name taken out; sent once (renamed after), kept when sending fails
TEST(crash_reports_prepared_and_sent) {
    std::string report = "Crashed in /home/alice/games/Coin Hunt/libengine.so\nuser alice, host alice-pc\n";
    std::string p = crashreports::prepare(report, "/home/alice/", "alice");
    CHECK(p.find("/home/alice") == std::string::npos && p.find("alice") == std::string::npos);
    CHECK(p.find("~/games/Coin Hunt/libengine.so") != std::string::npos);
    CHECK(p.find("<user>-pc") != std::string::npos);
    CHECK(crashreports::prepare("C:\\Users\\Bob\\AppData\\x", "C:\\Users\\Bob", "Bob") == "~\\AppData\\x");
    fs::path dir = test::tempPath("crashes");
    fs::remove_all(dir);
    fs::path file = dir / "game-crash-last.txt";
    writeFile(file, "Signal: SIGSEGV\n#0 somewhere\n");
    std::string posted;
    crashreports::Upload failing;
    failing.start("https://example.com/crash", file, [](const std::string&, const std::string&, std::string& error) {
        error = "offline";
        return false;
    });
    for (int i = 0; i < 300 && !failing.done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(failing.done() && !failing.sent() && failing.error() == "offline");
    CHECK(fs::exists(file));   // (still there to send later)
    crashreports::Upload up;
    up.start("https://example.com/crash", file, [&](const std::string& url, const std::string& body, std::string&) {
        posted = url + "|" + body;
        return true;
    });
    for (int i = 0; i < 300 && !up.done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(up.sent());
    CHECK(posted == "https://example.com/crash|Signal: SIGSEGV\n#0 somewhere\n");
    CHECK(!fs::exists(file) && fs::exists(dir / "game-crash-sent.txt"));
    crashreports::Upload nowhere;   // (no URL in the project)
    writeFile(file, "again");
    nowhere.start("", file);
    for (int i = 0; i < 300 && !nowhere.done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(!nowhere.sent() && nowhere.error().find("crashReportUrl") != std::string::npos);
}
