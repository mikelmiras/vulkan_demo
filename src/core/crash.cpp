#include "core/crash.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>

#include "core/log.h"

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#else
#include <csignal>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace crash {
namespace {

std::filesystem::path g_report, g_previous;
std::string g_name;
// The handlers can't allocate: paths copied out once, at install
char g_reportPath[1024] = {0};
char g_logPath[1024] = {0};
volatile bool g_reporting = false;   // (abort() inside a report: don't report twice)

void copyPath(char* dst, const std::filesystem::path& p) {
    std::string s = p.u8string();
    std::snprintf(dst, 1024, "%s", s.c_str());
}

#if defined(_WIN32)

// ---- Windows: an unhandled exception filter, symbols from DbgHelp (the .pdb next to the program, when there is one)
bool g_symbols = false;

std::string frames(void* const* stack, int count) {
    std::string out;
    HANDLE process = GetCurrentProcess();
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    for (int i = 0; i < count; ++i) {
        DWORD64 address = (DWORD64)stack[i];
        char line[512];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buf);
        std::memset(buf, 0, sizeof(buf));
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 offset = 0;
        char module[MAX_PATH] = "?";
        HMODULE base = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)address, &base))
            GetModuleFileNameA(base, module, MAX_PATH);
        const char* moduleName = std::strrchr(module, '\\') ? std::strrchr(module, '\\') + 1 : module;
        if (g_symbols && SymFromAddr(process, address, &offset, symbol)) {
            IMAGEHLP_LINE64 where{};
            where.SizeOfStruct = sizeof(where);
            DWORD column = 0;
            if (SymGetLineFromAddr64(process, address, &column, &where))
                std::snprintf(line, sizeof(line), "#%d %s!%s+0x%llx (%s:%lu)\n", i, moduleName, symbol->Name,
                              (unsigned long long)offset, where.FileName, where.LineNumber);
            else
                std::snprintf(line, sizeof(line), "#%d %s!%s+0x%llx\n", i, moduleName, symbol->Name, (unsigned long long)offset);
        } else {
            std::snprintf(line, sizeof(line), "#%d %s+0x%llx\n", i, moduleName,
                          (unsigned long long)(address - (DWORD64)base));
        }
        out += line;
    }
    return out;
}

std::string header(const char* what) {
    char buf[1400];
    std::time_t now = std::time(nullptr);
    char stamp[64];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::snprintf(buf, sizeof(buf), "==== Crash report: %s ====\nWhat: %s\nWhen: %s\nStack:\n", g_name.c_str(), what, stamp);
    return buf;
}

void emit(const std::string& text) {
    std::fputs(text.c_str(), stderr);
    std::fflush(stderr);
    if (g_reportPath[0])
        if (FILE* f = std::fopen(g_reportPath, "wb")) { std::fputs(text.c_str(), f); std::fclose(f); }
    if (g_logPath[0])
        if (FILE* f = std::fopen(g_logPath, "ab")) { std::fputs(text.c_str(), f); std::fclose(f); }
}

const char* exceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation (a bad memory access)";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
        default: return "unhandled exception";
    }
}

LONG WINAPI onException(EXCEPTION_POINTERS* info) {
    if (g_reporting) return EXCEPTION_CONTINUE_SEARCH;
    g_reporting = true;
    char what[160];
    std::snprintf(what, sizeof(what), "%s (code 0x%08lx at %p)", exceptionName(info->ExceptionRecord->ExceptionCode),
                  info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
    // Walk from the faulting context (the filter's own frames would hide where it happened)
    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame{};
    DWORD machine;
#if defined(_M_X64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_ARM64)
    machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = context.Pc;
    frame.AddrFrame.Offset = context.Fp;
    frame.AddrStack.Offset = context.Sp;
#else
    machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = context.Eip;
    frame.AddrFrame.Offset = context.Ebp;
    frame.AddrStack.Offset = context.Esp;
#endif
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    void* stack[64];
    int count = 0;
    while (count < 64 && StackWalk64(machine, GetCurrentProcess(), GetCurrentThread(), &frame, &context, nullptr,
                                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) && frame.AddrPC.Offset)
        stack[count++] = (void*)frame.AddrPC.Offset;
    emit(header(what) + frames(stack, count));
    return EXCEPTION_CONTINUE_SEARCH;
}

void onAbort(int) {
    if (g_reporting) return;
    writeReport("abort() (an assertion or a fatal error)");
}

void installHandlers() {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    g_symbols = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    SetUnhandledExceptionFilter(onException);
    std::signal(SIGABRT, onAbort);
}

#else

// ---- POSIX: signal handlers on their own stack (a stack overflow leaves none), backtrace() from execinfo
void writeAll(int fd, const char* s) {
    size_t n = std::strlen(s);
    while (n > 0) {
        ssize_t w = ::write(fd, s, n);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (a bad memory access)";
        case SIGBUS: return "SIGBUS (a bad memory access)";
        case SIGABRT: return "SIGABRT (abort(): an assertion or a fatal error)";
        case SIGILL: return "SIGILL (an illegal instruction)";
        case SIGFPE: return "SIGFPE (an arithmetic error: division by zero)";
        default: return "a fatal signal";
    }
}

// Async-signal-safe as far as it can be: no allocation, write() to descriptors (backtrace() was warmed up at
// install, so it doesn't load anything now)
void report(const char* what, int skip) {
    char head[512];
    std::time_t now = std::time(nullptr);
    std::snprintf(head, sizeof(head), "==== Crash report: %s ====\nWhat: %s\nWhen: %lld (seconds since 1970)\nStack:\n",
                  g_name.empty() ? "program" : g_name.c_str(), what, (long long)now);
    void* stack[64];
    int count = backtrace(stack, 64);
    int fds[3] = {2, -1, -1};
    if (g_reportPath[0]) fds[1] = ::open(g_reportPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_logPath[0]) fds[2] = ::open(g_logPath, O_WRONLY | O_APPEND);
    for (int fd : fds) {
        if (fd < 0) continue;
        writeAll(fd, head);
        for (int i = skip; i < count; ++i) {   // (numbered like stackTrace(); one frame at a time stays allocation-free)
            char num[16];
            std::snprintf(num, sizeof(num), "#%d ", i - skip);
            writeAll(fd, num);
            backtrace_symbols_fd(stack + i, 1, fd);
        }
        writeAll(fd, "(addresses into a program or library: addr2line -f -C -e <it> <offset>, or atos on macOS)\n");
        if (fd != 2) ::close(fd);
    }
}

void onSignal(int sig, siginfo_t*, void*) {
    if (!g_reporting) {
        g_reporting = true;
        report(signalName(sig), 2);   // (this handler and the signal trampoline)
    }
    // As if nothing caught it: the default action (a core dump, the shell's "Segmentation fault")
    struct sigaction dfl{};
    dfl.sa_handler = SIG_DFL;
    sigaction(sig, &dfl, nullptr);
    raise(sig);
}

void installHandlers() {
    void* warm[4];
    backtrace(warm, 4);   // (loads the unwinder now, not inside a handler)
    static char altStack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof(altStack);
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = onSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE}) sigaction(sig, &sa, nullptr);
}

#endif

void onTerminate() {
    std::string what = "an exception nothing caught";
    if (std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            what += std::string(": ") + ex.what();
        } catch (...) {
        }
    }
    if (!g_reporting) writeReport(what.c_str());
    g_reporting = true;
    std::abort();
}

}  // namespace

void install(const std::filesystem::path& folder, const std::string& name) {
    g_name = name;
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    g_report = folder / (name + "-crash.txt");
    g_previous.clear();
    if (std::filesystem::exists(g_report, ec)) {   // the last run crashed: keep its report for whoever asks
        std::filesystem::path last = folder / (name + "-crash-last.txt");
        std::filesystem::remove(last, ec);
        std::filesystem::rename(g_report, last, ec);
        if (!ec) g_previous = last;
    }
    copyPath(g_reportPath, g_report);
    copyPath(g_logPath, logfile::path());
    g_reporting = false;
    installHandlers();
    std::set_terminate(onTerminate);
}

const std::filesystem::path& previousReport() { return g_previous; }
const std::filesystem::path& reportPath() { return g_report; }

std::string stackTrace(int skip) {
#if defined(_WIN32)
    void* stack[64];
    int count = CaptureStackBackTrace((DWORD)(skip + 1), 64, stack, nullptr);
    return frames(stack, count);
#else
    void* stack[64];
    int count = backtrace(stack, 64);
    std::string out;
    if (char** names = backtrace_symbols(stack, count)) {
        for (int i = skip + 1; i < count; ++i) out += "#" + std::to_string(i - skip - 1) + " " + names[i] + "\n";
        std::free(names);
    }
    return out;
#endif
}

void writeReport(const char* what) {
#if defined(_WIN32)
    emit(header(what) + stackTrace(1));
#else
    report(what, 1);
#endif
}

}  // namespace crash
