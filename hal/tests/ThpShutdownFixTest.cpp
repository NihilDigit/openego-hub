#include "shared/ThpShutdownFix.h"
#include <cstdio>

// Device-side guard test: pass the installed, supported ApDaemon.dll. No device or
// vendor code is loaded; only a private non-executable allocation is modified.
// These checks validate rewriting guards, not the safety of vendor shutdown.
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    using namespace Gaokun::Thp;
    auto* memory = static_cast<BYTE*>(VirtualAlloc(nullptr, 0xc0000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!memory) return 3;
    const auto& instruction = Detail::kOriginalShutdown;
    auto* target = memory + 0xbe956;
    std::memcpy(target, instruction, sizeof(instruction));
    int failures = 0;
    auto check = [&](bool ok, const char* name) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; };
    check(Detail::FixShutdown(memory, L"missing-daemon.dll") == ShutdownFixResult::Unsupported &&
          target[1] == 100, "unknown binary remains untouched");
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    check(Detail::FixShutdown(memory, executable) == ShutdownFixResult::Unsupported &&
          target[1] == 100, "different file hash remains untouched");
    target[0] = 0x90;
    check(Detail::FixShutdown(memory, argv[1]) == ShutdownFixResult::Unsupported &&
          target[1] == 100, "unexpected instruction remains untouched");
    target[0] = instruction[0];
    check(Detail::FixShutdown(memory, argv[1]) == ShutdownFixResult::Applied &&
          !std::memcmp(target, Detail::kUnlockedShutdown, sizeof(instruction)), "verified unlock moved before sleep");
    MEMORY_BASIC_INFORMATION page{};
    VirtualQuery(target, &page, sizeof(page));
    check(page.Protect == PAGE_READWRITE, "page protection restored");
    check(Detail::FixShutdown(memory, argv[1]) == ShutdownFixResult::AlreadyApplied,
          "second application is idempotent");
    check(target[-1] == 0 && target[sizeof(instruction)] == 0, "surrounding instructions preserved");
    VirtualFree(memory, 0, MEM_RELEASE);
    return failures ? 1 : 0;
}
