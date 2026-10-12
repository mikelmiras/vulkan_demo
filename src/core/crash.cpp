#include "core/crash.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>

#include "core/log.h"
#include "core/version.h"

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#else
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#else
#include <elf.h>
#include <link.h>
#endif
#endif

namespace crash {
namespace {

std::filesystem::path g_report, g_previous;
std::string g_name;
// The handlers can't allocate: paths copied out once, at install
char g_reportPath[1024] = {0};
char g_logPath[1024] = {0};
char g_version[128] = {0};   // "0.5.0 (e7b72ac1d2)"
char g_product[256] = {0};   // "Product: Coin Hunt 1.2.0\n" (or nothing)
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
    std::snprintf(buf, sizeof(buf), "==== Crash report: %s ====\nWhat: %s\nWhen: %s\nVersion: %s\n%sStack:\n", g_name.c_str(), what,
                  stamp, g_version, g_product);
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

// The modules loaded (programs and libraries): where each is in memory and its build id, so a stack's addresses
// can be turned into functions and lines later from the symbols kept for that build (tools/symbolicate.py).
// "  0x<start>-0x<end> <build id or UUID> <path>", one per line.
#if defined(__APPLE__)
void writeModules(int fd) {
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
        const mach_header* header = _dyld_get_image_header(i);
        const char* name = _dyld_get_image_name(i);
        if (!header || !name) continue;
        char uuid[40] = "-";
        uint64_t size = 0;
        const uint8_t* at = (const uint8_t*)header + (header->magic == MH_MAGIC_64 ? sizeof(mach_header_64) : sizeof(mach_header));
        for (uint32_t c = 0; c < header->ncmds; ++c) {
            const load_command* lc = (const load_command*)at;
            if (lc->cmd == LC_UUID) {
                const uint8_t* u = ((const uuid_command*)lc)->uuid;
                std::snprintf(uuid, sizeof(uuid), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", u[0], u[1],
                              u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
            } else if (lc->cmd == LC_SEGMENT_64 && !std::strcmp(((const segment_command_64*)lc)->segname, "__TEXT")) {
                size = ((const segment_command_64*)lc)->vmsize;
            }
            at += lc->cmdsize;
        }
        char line[1200];
        std::snprintf(line, sizeof(line), "  0x%llx-0x%llx %s %s\n", (unsigned long long)(uintptr_t)header,
                      (unsigned long long)((uintptr_t)header + size), uuid, name);
        writeAll(fd, line);
    }
}
#else
// The GNU build id in an ELF image mapped at `base` (its notes), as hex; "-" if none
void buildId(uintptr_t base, char* out, size_t size) {
    std::snprintf(out, size, "-");
    const auto* eh = (const ElfW(Ehdr)*)base;
    if (std::memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return;
    const auto* ph = (const ElfW(Phdr)*)(base + eh->e_phoff);
    uintptr_t bias = base;   // (where vaddr 0 is: the first segment's vaddr is 0 for libraries and PIE programs)
    for (int i = 0; i < eh->e_phnum; ++i)
        if (ph[i].p_type == PT_LOAD && ph[i].p_offset == 0) bias = base - ph[i].p_vaddr;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_NOTE) continue;
        const uint8_t* at = (const uint8_t*)(bias + ph[i].p_vaddr);
        const uint8_t* end = at + ph[i].p_memsz;
        while (at + sizeof(ElfW(Nhdr)) <= end) {
            const auto* note = (const ElfW(Nhdr)*)at;
            const uint8_t* name = at + sizeof(ElfW(Nhdr));
            const uint8_t* desc = name + ((note->n_namesz + 3) & ~3u);
            if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 && !std::memcmp(name, "GNU", 4)) {
                size_t n = 0;
                for (uint32_t b = 0; b < note->n_descsz && n + 3 < size; ++b, n += 2)
                    std::snprintf(out + n, 3, "%02x", desc[b]);
                return;
            }
            at = desc + ((note->n_descsz + 3) & ~3u);
        }
    }
}

// From /proc/self/maps (read() into a buffer kept for it: nothing allocated in a handler)
void writeModules(int fd) {
    static char maps[256 * 1024];
    int in = ::open("/proc/self/maps", O_RDONLY);
    if (in < 0) return;
    size_t n = 0;
    for (ssize_t r; n < sizeof(maps) - 1 && (r = ::read(in, maps + n, sizeof(maps) - 1 - n)) > 0;) n += (size_t)r;
    ::close(in);
    maps[n] = 0;
    struct Module {
        const char* path;
        uintptr_t base, end;
        bool exec;
    };
    static Module mods[256];
    int count = 0;
    for (char* line = maps; *line;) {
        char* next = std::strchr(line, '\n');
        if (next) *next = 0;
        unsigned long long start = 0, stop = 0, offset = 0;
        char perms[8] = {0};
        int pathAt = 0;
        if (std::sscanf(line, "%llx-%llx %7s %llx %*s %*s %n", &start, &stop, perms, &offset, &pathAt) >= 4 && pathAt > 0 &&
            line[pathAt] == '/') {
            const char* path = line + pathAt;
            int m = 0;
            while (m < count && std::strcmp(mods[m].path, path) != 0) ++m;
            if (m == count && count < 256) mods[count++] = {path, offset == 0 ? (uintptr_t)start : 0, (uintptr_t)stop, false};
            if (m < count) {
                if (offset == 0 && (!mods[m].base || start < mods[m].base)) mods[m].base = (uintptr_t)start;
                if (stop > mods[m].end) mods[m].end = (uintptr_t)stop;
                if (perms[2] == 'x') mods[m].exec = true;
            }
        }
        if (!next) break;
        line = next + 1;
    }
    for (int m = 0; m < count; ++m) {
        if (!mods[m].exec || !mods[m].base) continue;
        char id[80];
        buildId(mods[m].base, id, sizeof(id));
        char line[1200];
        std::snprintf(line, sizeof(line), "  0x%llx-0x%llx %s %s\n", (unsigned long long)mods[m].base,
                      (unsigned long long)mods[m].end, id, mods[m].path);
        writeAll(fd, line);
    }
}
#endif

// Async-signal-safe as far as it can be: no allocation, write() to descriptors (backtrace() was warmed up at
// install, so it doesn't load anything now)
void report(const char* what, int skip) {
    char head[1024];
    std::time_t now = std::time(nullptr);
    std::snprintf(head, sizeof(head), "==== Crash report: %s ====\nWhat: %s\nWhen: %lld (seconds since 1970)\nVersion: %s\n%sStack:\n",
                  g_name.empty() ? "program" : g_name.c_str(), what, (long long)now, g_version, g_product);
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
        writeAll(fd, "Modules:\n");
        writeModules(fd);
        writeAll(fd, "(tools/symbolicate.py turns the addresses into functions and lines, with this build's symbols)\n");
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
    std::snprintf(g_version, sizeof(g_version), "%s", version::full().c_str());
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

void setProduct(const std::string& product) {
    if (product.empty()) g_product[0] = 0;
    else std::snprintf(g_product, sizeof(g_product), "Product: %s\n", product.c_str());
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
