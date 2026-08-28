#include "ThpModule.h"

namespace Thp {

namespace {

template <typename Fn>
[[nodiscard]] bool Resolve(HMODULE dll, const char *name, Fn &out) noexcept {
    out = reinterpret_cast<Fn>(GetProcAddress(dll, name));
    return out != nullptr;
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

} // namespace Thp
