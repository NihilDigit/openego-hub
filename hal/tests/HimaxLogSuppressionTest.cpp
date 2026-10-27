#include "thp/ThpModule.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// SEC_IMAGE maps the on-disk DLL without running DllMain or resolving imports.
// Copy to private writable memory: tests never modify the vendor file or live host.
int wmain(int argc, wchar_t **argv) {
    if (argc != 2) return 2;
    HANDLE file = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                            FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 3;
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY | SEC_IMAGE, 0, 0, nullptr);
    CloseHandle(file);
    if (!mapping) return 4;
    const auto *view = static_cast<const uint8_t *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!view) { CloseHandle(mapping); return 5; }
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(view);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(view + dos->e_lfanew);
    std::vector<uint8_t> image(nt->OptionalHeader.SizeOfImage);
    std::memcpy(image.data(), view, nt->OptionalHeader.SizeOfHeaders);
    const auto *section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (section[i].SizeOfRawData)
            std::memcpy(image.data() + section[i].VirtualAddress, view + section[i].VirtualAddress,
                        section[i].SizeOfRawData);
    }
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    if (image.size() < 0x199000) return 6;
    auto *level = reinterpret_cast<LONG *>(image.data() + 0x12FBC8);
    auto *handle = reinterpret_cast<uintptr_t *>(image.data() + 0x1984E0);
    auto suppress = [&] { return Thp::detail::SuppressHimaxLogInImage(
        reinterpret_cast<HMODULE>(image.data())); };
    int failures = 0;
    auto check = [&](bool ok, const char *name) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
    check(!Thp::detail::SuppressHimaxLogInImage(nullptr), "null module");
    *level = 3; *handle = 0;
    check(!suppress() && *level == 3, "wait for initialization");
    *handle = 0x1234;
    auto before = image;
    check(suppress() && *level == 0 && *handle == 0x1234, "actual DLL signatures and suppression");
    *reinterpret_cast<LONG *>(before.data() + 0x12FBC8) = 0;
    check(image == before, "only log level changes");
    check(suppress(), "idempotent");
    for (size_t offset : {size_t{0x424EA}, size_t{0x28459}}) {
        *level = 3; image[offset] ^= 1;
        check(!suppress() && *level == 3, "unknown signature rejected");
        image[offset] ^= 1;
    }
    auto *headers = reinterpret_cast<IMAGE_NT_HEADERS64 *>(image.data() +
        reinterpret_cast<IMAGE_DOS_HEADER *>(image.data())->e_lfanew);
    auto *sections = IMAGE_FIRST_SECTION(headers);
    for (WORD i = 0; i < headers->FileHeader.NumberOfSections; ++i) {
        const auto saved = sections[i].Characteristics;
        sections[i].Characteristics &= ~IMAGE_SCN_MEM_WRITE;
        if (sections[i].VirtualAddress <= 0x12FBC8 &&
            sections[i].VirtualAddress + sections[i].Misc.VirtualSize > 0x12FBC8)
            check(!suppress() && *level == 3, "read-only data rejected");
        sections[i].Characteristics = saved;
    }
    headers->OptionalHeader.SizeOfImage = 0x1000;
    check(!suppress() && *level == 3, "short image rejected");
    return failures ? 1 : 0;
}
