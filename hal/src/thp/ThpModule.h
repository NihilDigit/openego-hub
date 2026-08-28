#pragma once

#include <windows.h>

// THP_Service.dll 的七个导出。签名取自原厂 .NET 服务的 P/Invoke 声明，逐项对应，
// 详见 docs/vendor-service.md。调用约定原样标注为 cdecl：x64 与 ARM64EC 下它与
// stdcall 是同一套 ABI，此处不产生差别，但写出来才能在日后换架构时立刻暴露问题。
namespace Thp {

using CallBackFunc = int(__cdecl *)(int arg);

// 原厂 PrintEventLog 回调固定用 64 字节缓冲调用 GetMESSAGE，对应 .NET 侧的
// MESSAGE_LENGTH = 0x40。
inline constexpr int kMessageLength = 64;

class Module {
public:
    Module() noexcept = default;
    ~Module() noexcept { Unload(); }

    Module(const Module &) = delete;
    Module &operator=(const Module &) = delete;

    // 按原厂方式以模块名加载，交由加载器沿标准搜索序解析；服务的工作目录即安装目录。
    [[nodiscard]] bool Load() noexcept;
    void Unload() noexcept;

    [[nodiscard]] bool IsLoaded() const noexcept { return m_dll != nullptr; }

    // 七个导出。解析失败时 Load 已经返回 false，这里不再各自判空。
    int ThpFuncStart() const noexcept { return m_thpFuncStart(); }
    int ThpFuncStop() const noexcept { return m_thpFuncStop(); }
    void GetMessage(char *buffer, int *bufferLength) const noexcept {
        m_getMessage(buffer, bufferLength);
    }
    void RegisterPrintEventLog(CallBackFunc f) const noexcept { m_registerPrintEventLog(f); }
    void RegisterEventLogStatus(CallBackFunc f) const noexcept { m_registerEventLogStatus(f); }
    void RegisterGetPenEleValue(CallBackFunc f) const noexcept { m_registerGetPenEleValue(f); }
    void RegisterSetPenEleValue(CallBackFunc f) const noexcept { m_registerSetPenEleValue(f); }

private:
    using ThpFuncStartFn = int(__cdecl *)();
    using ThpFuncStopFn = int(__cdecl *)();
    using GetMessageFn = void(__cdecl *)(char *, int *);
    using RegisterFn = void(__cdecl *)(CallBackFunc);

    HMODULE m_dll = nullptr;
    ThpFuncStartFn m_thpFuncStart = nullptr;
    ThpFuncStopFn m_thpFuncStop = nullptr;
    GetMessageFn m_getMessage = nullptr;
    RegisterFn m_registerPrintEventLog = nullptr;
    RegisterFn m_registerEventLogStatus = nullptr;
    RegisterFn m_registerGetPenEleValue = nullptr;
    RegisterFn m_registerSetPenEleValue = nullptr;
};

} // namespace Thp
