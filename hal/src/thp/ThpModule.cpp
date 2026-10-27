#include "ThpModule.h"

#include <cstdint>
#include <cstring>

namespace Thp {

namespace {

template <typename Fn>
[[nodiscard]] bool Resolve(HMODULE dll, const char *name, Fn &out) noexcept {
    out = reinterpret_cast<Fn>(GetProcAddress(dll, name));
    return out != nullptr;
}

[[nodiscard]] bool IsWritableDataAddress(HMODULE module, const void *address, size_t size) noexcept {
    if (!module || !address || size == 0) return false;
    const auto *base = reinterpret_cast<const uint8_t *>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;

    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const uintptr_t target = reinterpret_cast<uintptr_t>(address);
    const uintptr_t imageBase = reinterpret_cast<uintptr_t>(base);
    if (target < imageBase) return false;
    const uintptr_t rva = target - imageBase;

    const auto *section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        const uintptr_t secStart = section->VirtualAddress;
        const uintptr_t secSize = section->Misc.VirtualSize ? section->Misc.VirtualSize
                                                            : section->SizeOfRawData;
        if (rva >= secStart && rva + size <= secStart + secSize) {
            constexpr DWORD kRequired = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
            return (section->Characteristics & kRequired) == kRequired;
        }
    }
    return false;
}

} // namespace

bool Module::Load() noexcept {
    if (m_dll) return true;

    m_dll = LoadLibraryW(L"THP_Service.dll");
    if (!m_dll) return false;

    // 七个导出缺一不可。原厂服务不做这项检查，因为 P/Invoke 直到首次调用才解析符号；
    // 提前一次性解析能把「DLL 版本不对」从一次崩溃变成一次启动失败。
    const bool ok =
        Resolve(m_dll, "ThpFuncStart", m_thpFuncStart) &&
        Resolve(m_dll, "ThpFuncStop", m_thpFuncStop) &&
        Resolve(m_dll, "GetMESSAGE", m_getMessage) &&
        Resolve(m_dll, "RegisterPrintEventLog", m_registerPrintEventLog) &&
        Resolve(m_dll, "RegisterEventLogStatus", m_registerEventLogStatus) &&
        Resolve(m_dll, "RegisterGetPenEleValue", m_registerGetPenEleValue) &&
        Resolve(m_dll, "RegisterSetPenEleValue", m_registerSetPenEleValue);

    if (!ok) Unload();
    return ok;
}

void Module::Unload() noexcept {
    if (m_dll) {
        FreeLibrary(m_dll);
        m_dll = nullptr;
    }
    m_thpFuncStart = nullptr;
    m_thpFuncStop = nullptr;
    m_getMessage = nullptr;
    m_registerPrintEventLog = nullptr;
    m_registerEventLogStatus = nullptr;
    m_registerGetPenEleValue = nullptr;
    m_registerSetPenEleValue = nullptr;
}

bool Module::SuppressVendorLogIfSupported() const noexcept {
    if (!m_dll || !m_thpFuncStart) return false;

    // ThpFuncStart 开头先执行 mov byte ptr [rip+disp32], 1 将 logFlag 置 1，
    // 紧接着调用 GetPenEleValue(1) 将返回值写入 logFunction（位于 logFlag - 4）。
    const auto *code = reinterpret_cast<const uint8_t *>(m_thpFuncStart);
    constexpr size_t kScanBytes = 96;

    for (size_t i = 0; i + 7 <= kScanBytes; ++i) {
        if (code[i] != 0xC6 || code[i + 1] != 0x05 || code[i + 6] != 0x01) continue;

        int32_t dispFlag = 0;
        std::memcpy(&dispFlag, code + i + 2, sizeof(dispFlag));
        auto *flagPtr = const_cast<uint8_t *>(code + i + 7 + dispFlag);
        if (!IsWritableDataAddress(m_dll, flagPtr, sizeof(uint8_t))) continue;

        for (size_t j = i + 7; j + 13 <= kScanBytes && j <= i + 40; ++j) {
            // mov ecx, 1 (B9 01 00 00 00); call rax (FF D0); mov [rip+disp32], eax (89 05 ...)
            if (code[j] == 0xB9 && code[j + 1] == 0x01 && code[j + 2] == 0x00 &&
                code[j + 3] == 0x00 && code[j + 4] == 0x00 &&
                code[j + 5] == 0xFF && code[j + 6] == 0xD0 &&
                code[j + 7] == 0x89 && code[j + 8] == 0x05) {
                int32_t dispFunc = 0;
                std::memcpy(&dispFunc, code + j + 9, sizeof(dispFunc));
                const auto *funcPtr = code + j + 13 + dispFunc;
                if (flagPtr == funcPtr + 4) {
                    *reinterpret_cast<volatile uint8_t *>(flagPtr) = 0;
                    MemoryBarrier();
                    return *reinterpret_cast<volatile uint8_t *>(flagPtr) == 0;
                }
            }
        }
    }
    return false;
}

bool Module::TrySuppressHimaxLogIfReady() noexcept {
    return detail::SuppressHimaxLogInImage(GetModuleHandleW(L"himax_thp_drv.dll"));
}

bool detail::SuppressHimaxLogInImage(HMODULE himax) noexcept {
    if (!himax) return false;

    const auto *base = reinterpret_cast<const uint8_t *>(himax);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;

    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage < 0x199000) {
        return false;
    }

    // 本机 2023-05-16 版本的导出入口 0x41950 先记录日志，真正的赋值序列在 0x424EA。
    // movzx eax, dil; mov [rip+0xED6D4], eax -> g_logLevel (0x12FBC8)。
    // 不调用导出函数：它会先进行日志 I/O；只原子改写已核实的数据，不修改代码或文件句柄。
    constexpr uint8_t kSetLevelSig[10] = {
        0x40, 0x0F, 0xB6, 0xC7, 0x89, 0x05, 0xD4, 0xD6, 0x0E, 0x00};
    // 校验 RVA 0x28459 (CloseLog): 48 8B 0D 80 00 17 00 48 85 C9 -> 读 g_hLogFile (0x1984E0)
    constexpr uint8_t kLogHandleSig[10] = {
        0x48, 0x8B, 0x0D, 0x80, 0x00, 0x17, 0x00, 0x48, 0x85, 0xC9};

    if (std::memcmp(base + 0x424EA, kSetLevelSig, sizeof(kSetLevelSig)) != 0 ||
        std::memcmp(base + 0x28459, kLogHandleSig, sizeof(kLogHandleSig)) != 0) {
        return false;
    }

    auto *logLevelPtr = reinterpret_cast<volatile LONG *>(const_cast<uint8_t *>(base + 0x12FBC8));
    auto *logHandlePtr = reinterpret_cast<void *volatile *>(const_cast<uint8_t *>(base + 0x1984E0));
    if (!IsWritableDataAddress(himax, const_cast<LONG *>(logLevelPtr), sizeof(LONG)) ||
        !IsWritableDataAddress(himax, const_cast<void **>(logHandlePtr), sizeof(void *))) {
        return false;
    }

    // thp_afe_open 在 PreTp::InitTpdLogLevel 之后打开日志文件并写入 g_hLogFile。
    // 只有看到 g_hLogFile 已建立，才说明异步初始化已完成那次 g_logLevel = 3 赋值。
    if (*logHandlePtr == nullptr) {
        return false;
    }

    InterlockedExchange(logLevelPtr, 0);
    return InterlockedCompareExchange(logLevelPtr, 0, 0) == 0;
}

} // namespace Thp
