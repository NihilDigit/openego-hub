#include "power/OemWmi.h"

#include <comdef.h>
#include <wbemidl.h>

// 依赖写在这里而不是构建脚本里：这几个 obj 会被打进 GaokunHal.lib，静态库不记录导入库，
// 写成 pragma 才能让链接它的上层不必自己补一遍。
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "wbemuuid.lib")

namespace Gaokun::Power::Oem {

namespace {

void (*g_trace)(const wchar_t *, HRESULT) = nullptr;

void Step(const wchar_t *what, HRESULT hr) noexcept {
    if (g_trace) g_trace(what, hr);
}

// 只用来在一条直线的调用序列里保证释放。之所以不用 wil 或 ATL：本仓库不引入外部依赖，
// 而手写的 Release 在这条通路上已经出现过一次遗漏。
template <typename T>
class ComPtr {
public:
    ComPtr() noexcept = default;
    ~ComPtr() noexcept { if (m_ptr) m_ptr->Release(); }

    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;

    T **Receive() noexcept { return &m_ptr; }
    T *Get() const noexcept { return m_ptr; }
    T *operator->() const noexcept { return m_ptr; }
    explicit operator bool() const noexcept { return m_ptr != nullptr; }

private:
    T *m_ptr = nullptr;
};

// 库要能被服务直接调用，不要求调用方先初始化 COM。已经初始化过的进程会拿到
// RPC_E_CHANGED_MODE（例如 UI 线程是 STA），那种情形下沿用既有的套间，也不配对 Uninitialize。
class ComScope {
public:
    ComScope() noexcept : m_hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() noexcept { if (SUCCEEDED(m_hr)) CoUninitialize(); }

    ComScope(const ComScope &) = delete;
    ComScope &operator=(const ComScope &) = delete;

    [[nodiscard]] bool Usable() const noexcept {
        return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE;
    }
    [[nodiscard]] HRESULT Hr() const noexcept { return m_hr; }

private:
    HRESULT m_hr;
};

[[nodiscard]] Result Classify(HRESULT hr) noexcept {
    switch (hr) {
    case S_OK:
        return Result::Ok;
    case E_ACCESSDENIED:
    case WBEM_E_ACCESS_DENIED:
        return Result::AccessDenied;
    // 类或实例不在：这台机器上的 BIOS 没有暴露这条通道。凭空返回 0 会让上层把「不支持」
    // 显示成「阈值 0%」。
    case WBEM_E_NOT_FOUND:
    case WBEM_E_INVALID_CLASS:
    case WBEM_E_INVALID_NAMESPACE:
        return Result::Unsupported;
    default:
        return Result::Failed;
    }
}

// 从 u8Output 取回字节。方法没有失败但也没有写输出时按 Unsupported 处理，与「有输出但全零」
// 区分得开——后者是真实读数。
[[nodiscard]] Result CopyOutput(IWbemClassObject *outParams, uint8_t *response,
                                size_t responseSize, HRESULT &failure) noexcept {
    VARIANT out;
    VariantInit(&out);
    HRESULT hr = outParams->Get(L"u8Output", 0, &out, nullptr, nullptr);
    Step(L"Get u8Output", hr);
    if (FAILED(hr)) { failure = hr; return Classify(hr); }

    if (out.vt != (VT_ARRAY | VT_UI1) || !out.parray) {
        VariantClear(&out);
        failure = WBEM_E_NOT_FOUND;
        return Result::Unsupported;
    }

    LONG lower = 0, upper = 0;
    (void)SafeArrayGetLBound(out.parray, 1, &lower);
    (void)SafeArrayGetUBound(out.parray, 1, &upper);
    const size_t available = static_cast<size_t>(upper - lower + 1);

    uint8_t *bytes = nullptr;
    hr = SafeArrayAccessData(out.parray, reinterpret_cast<void **>(&bytes));
    if (FAILED(hr)) { VariantClear(&out); failure = hr; return Classify(hr); }

    ZeroMemory(response, responseSize);
    memcpy(response, bytes, available < responseSize ? available : responseSize);
    (void)SafeArrayUnaccessData(out.parray);
    VariantClear(&out);
    return Result::Ok;
}

} // namespace

void SetTrace(void (*trace)(const wchar_t *, HRESULT)) noexcept {
    g_trace = trace;
}

Result Invoke(const uint8_t *request, size_t requestSize, uint8_t *response,
              size_t responseSize, bool dryRun, HRESULT &failure) noexcept {
    failure = S_OK;
    if (requestSize > kBufferSize) { failure = E_INVALIDARG; return Result::Failed; }

    ComScope com;
    Step(L"CoInitializeEx", com.Hr());
    if (!com.Usable()) { failure = com.Hr(); return Classify(com.Hr()); }

    ComPtr<IWbemLocator> locator;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWbemLocator,
                                  reinterpret_cast<void **>(locator.Receive()));
    Step(L"CoCreateInstance", hr);
    if (FAILED(hr)) { failure = hr; return Classify(hr); }

    ComPtr<IWbemServices> services;
    hr = locator->ConnectServer(_bstr_t(L"ROOT\\WMI"), nullptr, nullptr, nullptr, 0,
                                nullptr, nullptr, services.Receive());
    Step(L"ConnectServer", hr);
    if (FAILED(hr)) { failure = hr; return Classify(hr); }

    // 没有这一步，后续调用会以 E_ACCESSDENIED 失败，即使进程本身已经提权。
    hr = CoSetProxyBlanket(services.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                           RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                           nullptr, EOAC_NONE);
    Step(L"CoSetProxyBlanket", hr);
    if (FAILED(hr)) { failure = hr; return Classify(hr); }

    // OemWMIfun 是实例方法，必须先拿到一个实例的路径；对类调用会失败。
    //
    // 用 ExecQuery 而不是 CreateInstanceEnum：后者单给 WBEM_FLAG_FORWARD_ONLY 时，本机上
    // Next 直接返回 0 条，而同一个类用 Get-CimInstance 明明能列出实例。两个标志要成对给。
    ComPtr<IEnumWbemClassObject> enumerator;
    hr = services->ExecQuery(_bstr_t(L"WQL"), _bstr_t(L"SELECT * FROM OemWMIMethod"),
                             WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                             nullptr, enumerator.Receive());
    Step(L"ExecQuery", hr);
    if (FAILED(hr)) { failure = hr; return Classify(hr); }

    ComPtr<IWbemClassObject> instance;
    ULONG returned = 0;
    hr = enumerator->Next(WBEM_INFINITE, 1, instance.Receive(), &returned);
    Step(L"Next", hr);
    if (FAILED(hr) || returned == 0) {
        // 非提升进程走到这里：枚举本身返回 S_FALSE 而不是拒绝访问，实例数为 0。整组
        // BiosWmi 调用曾因此被误判为「这台机器不支持」，见 docs/hardware-hal.md。
        failure = FAILED(hr) ? hr : WBEM_E_ACCESS_DENIED;
        return FAILED(hr) ? Classify(hr) : Result::AccessDenied;
    }

    VARIANT path;
    VariantInit(&path);
    hr = instance->Get(L"__PATH", 0, &path, nullptr, nullptr);
    Step(L"Get __PATH", hr);
    if (FAILED(hr)) { failure = hr; return Classify(hr); }

    ComPtr<IWbemClassObject> classDef;
    hr = services->GetObject(_bstr_t(L"OemWMIMethod"), 0, nullptr, classDef.Receive(), nullptr);
    Step(L"GetObject class", hr);
    if (FAILED(hr)) { VariantClear(&path); failure = hr; return Classify(hr); }

    ComPtr<IWbemClassObject> inSignature;
    hr = classDef->GetMethod(L"OemWMIfun", 0, inSignature.Receive(), nullptr);
    Step(L"GetMethod", hr);
    if (FAILED(hr)) { VariantClear(&path); failure = hr; return Classify(hr); }

    // 无入参的方法会把 in 签名给成 NULL。OemWMIfun 有 DataIn，所以这里不该是 NULL；
    // 真是 NULL 的话 SpawnInstance 会直接解引用空指针。
    if (!inSignature) {
        Step(L"in signature", WBEM_E_NOT_FOUND);
        VariantClear(&path);
        failure = WBEM_E_NOT_FOUND;
        return Result::Unsupported;
    }

    ComPtr<IWbemClassObject> inParams;
    hr = inSignature->SpawnInstance(0, inParams.Receive());
    Step(L"SpawnInstance", hr);
    if (FAILED(hr)) { VariantClear(&path); failure = hr; return Classify(hr); }

    SAFEARRAY *input = SafeArrayCreateVector(VT_UI1, 0, kBufferSize);
    if (!input) { VariantClear(&path); failure = E_OUTOFMEMORY; return Result::Failed; }
    uint8_t *bytes = nullptr;
    (void)SafeArrayAccessData(input, reinterpret_cast<void **>(&bytes));
    ZeroMemory(bytes, kBufferSize);
    memcpy(bytes, request, requestSize);
    (void)SafeArrayUnaccessData(input);

    VARIANT arg;
    VariantInit(&arg);
    arg.vt = VT_ARRAY | VT_UI1;
    arg.parray = input;
    // 参数名取自类定义：OemWMIfun(u8Input[in], u32Resrved[out], u8Output[out])。
    // 原厂脚本按位置传参，所以名字在那里看不出来；名字写错时 Put 返回 WBEM_E_NOT_FOUND,
    // 与「实例不存在」是同一个错误码，容易找错方向。
    hr = inParams->Put(L"u8Input", 0, &arg, 0);
    Step(L"Put u8Input", hr);

    Result result = Result::Ok;
    if (SUCCEEDED(hr) && !dryRun) {
        ComPtr<IWbemClassObject> outParams;
        hr = services->ExecMethod(path.bstrVal, _bstr_t(L"OemWMIfun"), 0, nullptr,
                                  inParams.Get(), outParams.Receive(), nullptr);
        Step(L"ExecMethod", hr);
        if (SUCCEEDED(hr) && responseSize > 0) {
            result = outParams ? CopyOutput(outParams.Get(), response, responseSize, failure)
                               : Result::Unsupported;
        }
    }

    VariantClear(&arg); // 连带释放 input
    VariantClear(&path);

    if (FAILED(hr)) { failure = hr; return Classify(hr); }
    return result;
}

} // namespace Gaokun::Power::Oem
