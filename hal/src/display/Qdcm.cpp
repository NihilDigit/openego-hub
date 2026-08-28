#include "Qdcm.h"

#include <windows.h>

#include <string>

namespace Gaokun::Display {

namespace {

// 虚表布局取自 goodies 的 qdcm-loader，逐槽对应。中间那六个色彩调节槽本项目不用，但必须
// 占位——少写一个槽，后面所有函数指针都会错位，而错位的表现是调用到相邻函数，不是崩溃。
struct QdcmVtable {
    int(__stdcall *QueryCaps)(void *, uint32_t, int32_t *);
    int(__stdcall *GetValidDisplays)(void *, uint32_t *, int32_t *);
    void *SetSharpness;
    void *SetWarmness;
    void *SetHue;
    void *SetSaturation;
    void *SetIntensity;
    void *SetContrast;
    int(__stdcall *SetPcc)(void *, uint32_t, const float *);
};

struct Qdcm2Vtable {
    int(__stdcall *SetIgc)(void *, uint32_t, IgcData *);
    int(__stdcall *Set3dLut)(void *, uint32_t, Lut3dData *);
    void *ScreenCapture;
    void *CaptureRead;
};

struct QdcmObject {
    const QdcmVtable *vtable;
};

struct Qdcm2Object {
    const Qdcm2Vtable *vtable;
};

using CreateFn = void *(__cdecl *)();
using DestroyFn = int(__cdecl *)(void *);

DestroyFn g_destroy1 = nullptr;
DestroyFn g_destroy2 = nullptr;

} // namespace

Qdcm::~Qdcm() noexcept {
    if (m_factory1 && g_destroy1) (void)g_destroy1(m_factory1);
    if (m_factory2 && g_destroy2) (void)g_destroy2(m_factory2);
    if (m_module) FreeLibrary(static_cast<HMODULE>(m_module));
}

bool Qdcm::Load() noexcept {
    if (m_factory1 && m_factory2) return true;

    // 按模块名加载在 ARM64EC 进程里找不到 System32 中的那一份（返回 ERROR_FILE_NOT_FOUND）：
    // 系统目录对模拟架构的进程是经过架构过滤的，而这个 qdcmlib.dll 是纯 x64。用完整路径
    // 显式加载可以绕开该过滤，从而不必把厂商 DLL 复制进本项目的产物目录。
    // 先找本程序旁边的那一份，找不到才退到系统目录。
    //
    // 顺序不能反。System32 里的 qdcmlib.dll（以及并存的 qdcmlib_x64.dll）在本机加载得上、
    // 符号也解析得到，但两个工厂函数都返回 null——它们的初始化会去解析 D3DKMT 的 thunk
    // 指针，失败时打的正是库里那句 "Could not locate Thunk function pointers!"。换成
    // goodies 随包附带的那一份就正常。两者的代码段反汇编只差数据地址的 8 字节偏移，二进制
    // 却有一万五千字节不同，是同一份源码的两个构建；具体哪一处造成差别尚未查明。
    //
    // 排查时被误导过两次，记在这里省得重来：可执行文件用 Debug 还是 Release 编译与此无关
    // （2x2 对照过），而失败时 GetLastError 是 2，看着像「文件找不到」，其实文件早就加载好了。
    const wchar_t *localNames[] = {L"qdcmlib.dll"};
    for (const wchar_t *name : localNames) {
        m_module = LoadLibraryW(name);
        if (m_module) break;
    }

    if (!m_module) {
        wchar_t system32[MAX_PATH];
        const UINT n = GetSystemDirectoryW(system32, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            const std::wstring full = std::wstring(system32) + L"\\qdcmlib.dll";
            m_module = LoadLibraryW(full.c_str());
        }
    }
    if (!m_module) return false;

    auto *dll = static_cast<HMODULE>(m_module);
    auto create1 = reinterpret_cast<CreateFn>(GetProcAddress(dll, "Create_QDCMLibrary"));
    auto create2 = reinterpret_cast<CreateFn>(GetProcAddress(dll, "Create_QDCMLibrary2"));
    g_destroy1 = reinterpret_cast<DestroyFn>(GetProcAddress(dll, "Destroy_QDCMLibrary"));
    g_destroy2 = reinterpret_cast<DestroyFn>(GetProcAddress(dll, "Destroy_QDCMLibrary2"));

    if (!create1 || !create2 || !g_destroy1 || !g_destroy2) {
        wprintf(L"  qdcmlib: loaded, but exports missing (c1=%d c2=%d d1=%d d2=%d)\n",
                create1 != nullptr, create2 != nullptr, g_destroy1 != nullptr,
                g_destroy2 != nullptr);
        return false;
    }

    m_factory1 = create1();
    m_factory2 = create2();
    if (!m_factory1 || !m_factory2) {
        // 这条几乎总是「加载到的是系统那份 qdcmlib」，见 Load 开头的说明。错误码在这里
        // 没有意义，所以直接说清楚该怎么办，而不是丢一个 GetLastError 出去。
        wprintf(L"qdcmlib loaded but refused to initialise. The copy in System32 does not work\n"
                L"on this machine; place a working qdcmlib.dll next to this executable.\n");
        return false;
    }
    return true;
}

bool Qdcm::EnumDisplays(std::vector<uint32_t> &ids) noexcept {
    if (!m_factory1) return false;
    auto *obj = static_cast<QdcmObject *>(m_factory1);

    // 必须调用两次：第一次只写回数量，并不填充 id 数组，第二次才真正填。省掉第二次会拿到
    // 一片全零的 id，而 0 恰好是个看起来合理的 display index——后续 QueryCaps 会照常返回
    // 成功但能力位全零，SetIGC 则直接失败，现象全都指向「这块屏不支持」而不是调用方式不对。
    ids.assign(64, 0);
    int32_t count = 0;
    (void)obj->vtable->GetValidDisplays(obj, ids.data(), &count);
    if (count < 0 || static_cast<size_t>(count) > ids.size()) return false;
    (void)obj->vtable->GetValidDisplays(obj, ids.data(), &count);
    if (count < 0 || static_cast<size_t>(count) > ids.size()) return false;

    ids.resize(static_cast<size_t>(count));
    return true;
}

bool Qdcm::QueryCaps(uint32_t display, int32_t &caps) noexcept {
    if (!m_factory1) return false;
    auto *obj = static_cast<QdcmObject *>(m_factory1);
    caps = 0;
    return obj->vtable->QueryCaps(obj, display, &caps) != 0;
}

int Qdcm::QueryCapsRaw(uint32_t display, int32_t &caps) noexcept {
    if (!m_factory1) return 0;
    auto *obj = static_cast<QdcmObject *>(m_factory1);
    caps = 0;
    return obj->vtable->QueryCaps(obj, display, &caps);
}

bool Qdcm::SetPcc(uint32_t display, const float matrix[9]) noexcept {
    if (!m_factory1) return false;
    auto *obj = static_cast<QdcmObject *>(m_factory1);
    return obj->vtable->SetPcc(obj, display, matrix) != 0;
}

bool Qdcm::SetIgc(uint32_t display, const RgbTable *table) noexcept {
    if (!m_factory2) return false;
    auto *obj = static_cast<Qdcm2Object *>(m_factory2);

    IgcData data{};
    if (!table) {
        data.enable = 0;
        return obj->vtable->SetIgc(obj, display, &data) != 0;
    }

    if (table->red.size() != kIgcEntries || table->green.size() != kIgcEntries ||
        table->blue.size() != kIgcEntries) {
        return false;
    }

    data.enable = 1;
    data.channel1 = const_cast<uint32_t *>(table->red.data());
    data.channel2 = const_cast<uint32_t *>(table->green.data());
    data.channel3 = const_cast<uint32_t *>(table->blue.data());
    data.numEntries = kIgcEntries;
    return obj->vtable->SetIgc(obj, display, &data) != 0;
}

bool Qdcm::SetLut3d(uint32_t display, const RgbTable *table) noexcept {
    if (!m_factory2) return false;
    auto *obj = static_cast<Qdcm2Object *>(m_factory2);

    Lut3dData data{};
    if (!table) {
        data.enable = 0;
        return obj->vtable->Set3dLut(obj, display, &data) != 0;
    }

    if (table->red.size() != kLutEntries || table->green.size() != kLutEntries ||
        table->blue.size() != kLutEntries) {
        return false;
    }

    data.enable = 1;
    data.channel1 = const_cast<uint32_t *>(table->red.data());
    data.channel2 = const_cast<uint32_t *>(table->green.data());
    data.channel3 = const_cast<uint32_t *>(table->blue.data());
    data.numFlattenEntries = kLutEntries;
    return obj->vtable->Set3dLut(obj, display, &data) != 0;
}

} // namespace Gaokun::Display
