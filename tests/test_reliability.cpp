// Crash reports (core/crash) and Chrome trace export of profiles (core/profiler)
#include "test.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include "core/crash.h"
#include "core/json.h"
#include "core/profiler.h"
#include "core/version.h"
#include "app/updates.h"
#include "net/tcp.h"
#include "platform/platform.h"

#if !defined(_WIN32)
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::string readAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

TEST(crash_stack_trace_and_report) {
    std::string trace = crash::stackTrace();
    CHECK(trace.rfind("#0 ", 0) == 0);
    CHECK(std::count(trace.begin(), trace.end(), '\n') >= 2);   // (this test, the runner, main...)

    std::filesystem::path dir = test::tempPath("crash_report");
    crash::install(dir, "unit");
    CHECK(crash::previousReport().empty());
    CHECK(crash::reportPath() == dir / "unit-crash.txt");
    crash::writeReport("a test report");
    std::string report = readAll(dir / "unit-crash.txt");
    CHECK(report.find("Crash report: unit") != std::string::npos);
    CHECK(report.find("What: a test report") != std::string::npos);
    CHECK(report.find("#0 ") != std::string::npos);
    CHECK(report.find(std::string("Version: ") + version::string()) != std::string::npos);
    // The next run finds it, keeps it as the last report, and starts clean
    crash::install(dir, "unit");
    CHECK(crash::previousReport() == dir / "unit-crash-last.txt");
    CHECK(std::filesystem::exists(dir / "unit-crash-last.txt"));
    CHECK(!std::filesystem::exists(dir / "unit-crash.txt"));
    crash::install(dir, "unit");
    CHECK(crash::previousReport().empty());
}

#if !defined(_WIN32)
// A real crash, in a child process: the report is written and the process still dies of its signal
TEST(crash_signal_writes_a_report) {
    std::filesystem::path dir = test::tempPath("crash_signal");
    std::filesystem::create_directories(dir);
    std::fflush(nullptr);
    pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        int devnull = ::open("/dev/null", O_WRONLY);   // (the report on stderr would clutter the test output)
        if (devnull >= 0) dup2(devnull, 2);
        crash::install(dir, "child");
        raise(SIGSEGV);
        _exit(0);   // (not reached)
    }
    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    std::string report = readAll(dir / "child-crash.txt");
    CHECK(report.find("SIGSEGV") != std::string::npos);
    CHECK(report.find("Stack:") != std::string::npos);
    CHECK(report.find("Version: ") != std::string::npos);
    // The modules, with their build ids (to find this build's symbols): this program among them
    size_t modules = report.find("Modules:\n");
    REQUIRE(modules != std::string::npos);
#if defined(__linux__)
    std::string self = std::filesystem::read_symlink("/proc/self/exe").string();
    size_t at = report.find(" " + self + "\n", modules);
    REQUIRE(at != std::string::npos);
    size_t lineStart = report.rfind('\n', at) + 1;
    std::string line = report.substr(lineStart, at - lineStart);   // "  0x...-0x... <build id>"
    CHECK(line.find("0x") != std::string::npos);
    CHECK(line.size() > 40 && line.back() != '-');   // (a build id, not "-")
#endif
}
#endif

// Versions: the build's, and comparing them (update checks)
TEST(version_compare) {
    CHECK(std::string(version::string()).find('.') != std::string::npos);
    CHECK(version::full().rfind(version::string(), 0) == 0);
    CHECK(version::compare("1.2.0", "1.2.0") == 0);
    CHECK(version::compare("1.10.0", "1.9.2") > 0);
    CHECK(version::compare("v2", "1.99") > 0);
    CHECK(version::compare("1.2", "1.2.0") == 0);
    CHECK(version::compare("1.2.0-beta", "1.2.0") < 0);
    CHECK(version::compare("1.2.0-alpha", "1.2.0-beta") < 0);
    CHECK(version::compare("0.4.9", "0.5.0") < 0);
}

// Update checks: what an update URL serves (ours, or a GitHub release), compared with this version
TEST(update_check) {
    updates::Release r;
    std::string error;
    REQUIRE(updates::parseRelease(R"({"version": "1.4.0", "url": "https://example.com/get", "notes": "Faster"})", r, error));
    CHECK(r.version == "1.4.0" && r.url == "https://example.com/get" && r.notes == "Faster");
    REQUIRE(updates::parseRelease(R"({"tag_name": "v2.0.1", "html_url": "https://github.com/a/b/releases/v2.0.1", "body": "x"})", r, error));
    CHECK(r.version == "2.0.1" && r.url.find("github.com") != std::string::npos);
    CHECK(!updates::parseRelease("{}", r, error));
    CHECK(!updates::parseRelease("not json", r, error));

    auto serve = [](std::string body, bool ok = true) {
        return [body, ok](const std::string&, std::string& out, std::string& err) {
            if (!ok) err = "offline";
            out = body;
            return ok;
        };
    };
    auto wait = [](const updates::Check& c) {
        for (int i = 0; i < 500 && !c.ready(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return c.ready();
    };
    updates::Check newer, same, offline;
    newer.start("https://x", "1.3.9", serve(R"({"version": "1.4.0", "url": "u"})"));
    same.start("https://x", "1.4.0", serve(R"({"version": "1.4.0"})"));
    offline.start("https://x", "1.0", serve("", false));
    REQUIRE(wait(newer) && wait(same) && wait(offline));
    CHECK(newer.newer() && newer.release().version == "1.4.0" && newer.error().empty());
    CHECK(!same.newer() && same.error().empty());
    CHECK(!offline.newer() && offline.error() == "offline");
    updates::Check never;
    CHECK(!never.started() && !never.ready());
}

#if !defined(__APPLE__)   // (macOS: App Transport Security says no to plain http)
// platform::httpGet against a little HTTP server on this machine: a body, and a 404 that fails
TEST(http_get_local) {
    net::TcpListener server;
    std::string error;
    REQUIRE(server.listen(0, error));
    std::atomic<bool> stop{false};
    std::thread serve([&] {
        while (!stop) {
            std::unique_ptr<net::TcpConnection> c = server.accept();
            if (!c) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            std::string request;
            for (int i = 0; i < 2000 && request.find("\r\n\r\n") == std::string::npos && c->receive(request); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            bool found = request.rfind("GET /latest.json ", 0) == 0;
            std::string body = found ? R"({"version": "9.9.9", "url": "https://example.com"})" : "no";
            std::string answer = std::string(found ? "HTTP/1.1 200 OK" : "HTTP/1.1 404 Not Found") +
                                 "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                                 "\r\nConnection: close\r\n\r\n" + body;
            c->send(answer.data(), answer.size());
            c->close();
        }
    });
    std::string base = "http://127.0.0.1:" + std::to_string(server.port()), body, other;
    bool fetched = platform::httpGet(base + "/latest.json", body, error, 5);
    bool missing = platform::httpGet(base + "/nothing", other, error, 5);
    stop = true;
    serve.join();
    if (!fetched && error.find("neither curl nor wget") != std::string::npos) return;   // (a machine without them)
    CHECK(fetched);
    updates::Release r;
    CHECK(updates::parseRelease(body, r, error) && r.version == "9.9.9");
    CHECK(!missing);
    CHECK(!platform::httpGet("ftp://example.com/x", other, error, 1));
}
#endif

TEST(profiler_chrome_trace) {
    // (outside a running application: the test drives frames itself)
    auto frame = [](float gpuMs) {
        profiler::beginFrame();
        profiler::push("Update");
        profiler::push("Physics");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        profiler::pop();
        profiler::pop();
        profiler::push("Draw \"quoted\"");
        profiler::pop();
        profiler::setGpuTimes({{"Shadows", gpuMs * 0.25f}, {"Scene", gpuMs * 0.75f}});
        profiler::endFrame();
    };
    frame(1.f);
    profiler::startRecording();
    CHECK(profiler::recording());
    frame(2.f);
    frame(4.f);
    profiler::stopRecording();
    frame(8.f);
    REQUIRE(profiler::recorded().size() == 2);
    CHECK(!profiler::recording());
    CHECK(profiler::recorded()[1].begin > profiler::recorded()[0].begin);

    json::Value doc;
    std::string error;
    REQUIRE(json::parse(profiler::chromeTraceJson(profiler::recorded()), doc, error));
    const json::Value& events = doc["traceEvents"];
    int frames = 0, physics = 0, gpu = 0;
    double updateTs = 0, updateDur = 0, physicsTs = 0, physicsDur = 0;
    for (size_t i = 0; i < events.size(); ++i) {
        const json::Value& e = events[i];
        const std::string& name = e["name"].asString();
        if (name == "Frame") ++frames;
        if (name == "Update" && frames == 1) { updateTs = e["ts"].asNumber(); updateDur = e["dur"].asNumber(); }
        if (name == "Physics") {
            ++physics;
            if (physics == 1) { physicsTs = e["ts"].asNumber(); physicsDur = e["dur"].asNumber(); }
            CHECK(e["dur"].asNumber() >= 900.0);   // (microseconds: it slept 1 ms)
        }
        if (name == "Scene") {
            ++gpu;
            CHECK(e["tid"].asNumber() == 2.0);
        }
    }
    CHECK(frames == 2 && physics == 2 && gpu == 2);
    CHECK(physicsTs >= updateTs && physicsTs + physicsDur <= updateTs + updateDur + 1.0);   // nested in time
    CHECK(profiler::chromeTraceJson(profiler::recorded()).find("Draw \\\"quoted\\\"") != std::string::npos);

    std::filesystem::path file = test::tempPath("trace.json");
    REQUIRE(profiler::saveChromeTrace(file, profiler::recorded(), error));
    json::Value back;
    REQUIRE(json::parse(readAll(file), back, error));
    CHECK(back["traceEvents"].size() == events.size());
}
