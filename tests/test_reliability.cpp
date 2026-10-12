// Crash reports (core/crash) and Chrome trace export of profiles (core/profiler)
#include "test.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include "core/crash.h"
#include "core/json.h"
#include "core/profiler.h"

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
    CHECK(report.find("addr2line") != std::string::npos);
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
