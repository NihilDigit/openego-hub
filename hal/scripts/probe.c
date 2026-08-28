// ARM64EC 可行性探针：只加载 THP_Service.dll 并解析七个导出，不调用 ThpFuncStart。
// 启动整条链路会去抢正在运行的原厂服务持有的设备，探针的目的仅仅是证明跨架构加载成立。
#include <windows.h>
#include <stdio.h>

static const char *kExports[] = {
    "ThpFuncStart", "ThpFuncStop", "GetMESSAGE",
    "RegisterPrintEventLog", "RegisterEventLogStatus",
    "RegisterGetPenEleValue", "RegisterSetPenEleValue",
};

int main(void) {
    USHORT process = 0, machine = 0;
    if (IsWow64Process2(GetCurrentProcess(), &process, &machine))
        printf("host machine=0x%04x  process=0x%04x\n", machine, process);

    SetDllDirectoryA("C:\\Program Files\\Huawei\\HuaweiThpService");
    HMODULE h = LoadLibraryA("C:\\Program Files\\Huawei\\HuaweiThpService\\THP_Service.dll");
    if (!h) { printf("LoadLibrary failed: %lu\n", GetLastError()); return 1; }
    printf("THP_Service.dll loaded at %p\n", (void *)h);

    int bad = 0;
    for (int i = 0; i < 7; ++i) {
        FARPROC p = GetProcAddress(h, kExports[i]);
        printf("  %-24s %p\n", kExports[i], (void *)p);
        if (!p) bad = 1;
    }

    // 依赖链是否真的被拉起来了，看这几个是否已在进程内。
    const char *deps[] = {"TSACore.dll", "TSAPrmt.dll", "SpiModule.dll", "himax_thp_drv.dll", "ApDaemon.dll"};
    for (int i = 0; i < 5; ++i)
        printf("  dep %-20s %s\n", deps[i], GetModuleHandleA(deps[i]) ? "loaded" : "-");

    return bad;
}
