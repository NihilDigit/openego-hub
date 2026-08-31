#pragma once

#include <string>
#include <vector>

// 清理工具里不碰系统的那一半：命令行解析、清单构造、保留判定。
//
// 单独成一个翻译单元是为了能在没有设备、没有管理员权限、也没有那些服务和目录的机器上
// 测试它。真正动手的 CleanupActions 只调用这里的判定，不自己再写一份规则。
namespace Cleanup {

struct Options {
    bool dryRun = false;
    std::wstring logPath;  // 空表示用默认位置
    bool valid = true;
    std::wstring error;
};

Options ParseCommandLine(int argc, const wchar_t *const *argv);

// SHGetKnownFolderPath 的三个结果。取参数而不是在函数里现取，测试才能喂进假路径。
struct KnownFolders {
    std::wstring programFiles;
    std::wstring programData;
    std::wstring commonPrograms;
};

enum class TargetKind { Directory, File };

struct PathTarget {
    TargetKind kind = TargetKind::Directory;
    std::wstring path;
    std::wstring reason;  // 写进日志，说明这一条为什么在清单里
};

std::vector<PathTarget> BuildRemovalPlan(const KnownFolders &folders);

// 要杀的用户面进程，按镜像名。
std::vector<std::wstring> UserFacingProcesses();

// hal 宿主。服务停掉后它们靠 --parent 机制自退，这份清单只用于收尾时确认。
std::vector<std::wstring> HalHostProcesses();

// 要停删的服务，按执行顺序。
std::vector<std::wstring> ServiceRemovalPlan();

// 保留清单。硬编码而不是从「不在删除清单里就保留」推导：删除清单将来会加条目，
// 而误删华为的服务或事件源会让原厂触控和 PC Manager 一起坏掉，这一侧要独立成立。
bool IsProtectedService(const std::wstring &name);

// programData 下的 OpenEGoHub 整棵子树是配置与日志，任何删除都必须先过这一关。
bool IsProtectedPath(const std::wstring &path, const std::wstring &programData);

// --dry-run 打印的「保留」行。
std::vector<std::wstring> PreservedItems(const KnownFolders &folders);

}  // namespace Cleanup
