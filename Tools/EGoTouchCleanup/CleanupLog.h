#pragma once

#include <string>

// 清理工具自己的日志。不用 Common/Logger，也不用 hal 的 HostLog：这个 exe 由 MSI 的
// Binary 表携带，安装时释放到临时目录单独运行，链进来的东西越少，它越不会因为别处的
// 改动而在安装期崩掉。
namespace Cleanup {

// path 为空时用 ProgramData\OpenEGoHub\logs\cleanup.log；那里建不出来或打不开就退到
// %TEMP%\OpenEGoHubCleanup.log。两处都失败仍然可以调用 Log，只是没有落盘。
void OpenLog(const std::wstring &path);
void CloseLog();

// 实际写到的文件，供启动时自报一行。
const std::wstring &LogPath();

void Log(const wchar_t *format, ...);

}  // namespace Cleanup
