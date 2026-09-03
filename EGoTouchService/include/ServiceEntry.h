#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Service {

class IServiceEntryActions {
public:
    virtual ~IServiceEntryActions() = default;

#if EGOTOUCH_SERVICE_ENABLE_IPC
    virtual bool InstallService() = 0;
    virtual bool UninstallService() = 0;
#endif
    // 只把华为的后台服务还回去，不碰服务注册：安装包用 ServiceControl 原生删除服务，走不到
    // UninstallService。留在上面那个宏之外是必须的——它在当前构建里没有定义，宏内的东西
    // 一概不进二进制。
    virtual bool RestoreVendorServices() = 0;
    virtual void InitializeServiceProcess() = 0;
#if EGOTOUCH_SERVICE_ENABLE_IPC
    virtual void RunConsole() = 0;
#endif
    virtual bool StartScmDispatcher() = 0;
    virtual DWORD LastErrorCode() const = 0;
};

int ServiceEntryMain(int argc, wchar_t* argv[], IServiceEntryActions& actions);

} // namespace Service
