#pragma once

#include <string>
#include <vector>

#include "CleanupPlan.h"

// 真正动系统的那一半。每个函数自己吞掉失败并记一行日志：清理是尽力而为，一项做不成
// 不该让后面几项也不做。
namespace Cleanup {

// SHGetKnownFolderPath 而不是 %ProgramFiles% / %ProgramData%：这个进程在 MSI 的
// deferred CA 里以 SYSTEM 运行，环境块不是登录用户那一份，环境变量指向哪儿没有保证。
KnownFolders ResolveKnownFolders();

void KillProcesses(const std::vector<std::wstring> &imageNames, bool dryRun);

// 先给宽限期让它们自退，到时仍在才 Terminate。
void KillProcessesAfterGrace(const std::vector<std::wstring> &imageNames, unsigned graceMs,
                             bool dryRun);

void RemoveService(const std::wstring &name, bool dryRun);

// 等 HuaweiThpService 回到 RUNNING 或 START_PENDING。超时只记一行就放行：这一步确认的是
// 旧服务的善后已经做完，不是替新服务把关，在这里卡住只会把安装拖慢。
void WaitForVendorTouchService(unsigned timeoutMs, bool dryRun);

void RemovePath(const PathTarget &target, const std::wstring &programData, bool dryRun);

}  // namespace Cleanup
