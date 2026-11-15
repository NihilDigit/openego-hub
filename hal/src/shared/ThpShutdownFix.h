#pragma once

// EXPERIMENT ONLY; not called by the host. Faster shutdown exposed native AFE
// teardown failures. Neither SCM STOPPED nor exit code zero proved safe cleanup.
// See docs/touch-provider-lifecycle.md before reusing this candidate rewrite.

#include <windows.h>
#include <wincrypt.h>
#include <array>
#include <cstring>

namespace Gaokun::Thp {
enum class ShutdownFixResult { Applied, AlreadyApplied, Unsupported, Unavailable, Failed };
namespace Detail {

inline bool KnownDaemon(const wchar_t* path) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool ok = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES,
                                    CRYPT_VERIFYCONTEXT) != FALSE;
    if (ok) ok = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != FALSE;
    std::array<BYTE, 16384> buffer{};
    DWORD size = 0;
    while (ok) {
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &size, nullptr)) {
            ok = false;
            break;
        }
        if (!size) break;
        ok = CryptHashData(hash, buffer.data(), size, 0) != FALSE;
    }
    constexpr BYTE expected[32] = {
        0xe1,0x0c,0x06,0x94,0x19,0x62,0x13,0xe2,0x7b,0x8b,0x4c,0xc6,0x4e,0x8d,0x52,0x9e,
        0x11,0x47,0xc8,0xe9,0xbf,0x2d,0x5b,0x5d,0x59,0xd5,0x13,0xc7,0xea,0x69,0x9a,0xb0};
    BYTE digest[32]{};
    size = sizeof(digest);
    if (ok) ok = CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) != FALSE &&
                 size == sizeof(digest) && std::memcmp(digest, expected, size) == 0;
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    CloseHandle(file);
    return ok;
}

inline constexpr BYTE kOriginalShutdown[] = {
    0xb9,0x64,0,0,0,0xff,0x15,0x6f,0xc7,0x05,0,0x90,
    0x48,0x8b,0x4c,0x24,0x48,0x48,0x8b,0x01,0xff,0x50,0x38,0x90};
inline constexpr BYTE kUnlockedShutdown[] = {
    0x48,0x8b,0x4c,0x24,0x48,0x48,0x8b,0x01,0xff,0x50,0x38,
    0xb9,0x64,0,0,0,0xff,0x15,0x64,0xc7,0x05,0,0x90,0x90};

// Only before ThpFuncStart creates vendor threads: move the existing unlock call
// before Sleep(100), adjusting Sleep's RIP-relative IAT displacement. Instruction
// span, retry count and timeouts stay unchanged. Never use this on a running chain.
inline ShutdownFixResult FixShutdown(BYTE* base, const wchar_t* path) noexcept {
    const HANDLE process = GetCurrentProcess();
    if (!KnownDaemon(path)) return ShutdownFixResult::Unsupported;
    BYTE actual[sizeof(kOriginalShutdown)]{};
    SIZE_T read = 0;
    BYTE* instruction = base + 0xbe956;
    if (!ReadProcessMemory(process, instruction, actual, sizeof(actual), &read) ||
        read != sizeof(actual)) return ShutdownFixResult::Failed;
    if (!std::memcmp(actual, kUnlockedShutdown, sizeof(actual))) return ShutdownFixResult::AlreadyApplied;
    if (std::memcmp(actual, kOriginalShutdown, sizeof(actual))) return ShutdownFixResult::Unsupported;

    DWORD protection = 0;
    if (!VirtualProtectEx(process, instruction, sizeof(actual), PAGE_EXECUTE_READWRITE, &protection))
        return ShutdownFixResult::Failed;
    SIZE_T written = 0;
    const bool writeOk = WriteProcessMemory(process, instruction, kUnlockedShutdown,
                                            sizeof(actual), &written) && written == sizeof(actual);
    if (!writeOk) {
        // Do not run a partially rewritten instruction sequence.
        SIZE_T restoredBytes = 0;
        if (!WriteProcessMemory(process, instruction, kOriginalShutdown, sizeof(actual), &restoredBytes) ||
            restoredBytes != sizeof(actual)) {
            RaiseFailFastException(nullptr, nullptr, 0);
        }
    }
    DWORD ignored = 0;
    const bool restored = VirtualProtectEx(process, instruction, sizeof(actual), protection, &ignored) != FALSE;
    const bool flushed = FlushInstructionCache(process, nullptr, 0) != FALSE;
    const bool verifiedOk = ReadProcessMemory(process, instruction, actual, sizeof(actual), &read) &&
                            read == sizeof(actual) && !std::memcmp(actual, kUnlockedShutdown, sizeof(actual));
    return writeOk && restored && flushed && verifiedOk ? ShutdownFixResult::Applied
                                                       : ShutdownFixResult::Failed;
}
} // namespace Detail
} // namespace Gaokun::Thp
