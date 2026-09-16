#pragma once

#include <string>

// 把 ProgramData 下的日志目录打成一个 zip，供用户提交排障材料。
namespace LogExport {

enum class Status {
    Success,
    NoLogs,   // 目录不存在或一个文件都没有
    Failed,
};

struct Result {
    Status status = Status::Failed;
    std::wstring detail;  // 失败原因，直接作为提示正文
};

// 形如 OpenEGoHub-logs-20260831-0912.zip，取本地时间。
std::wstring SuggestedFileName();

// 目录里有没有值得导出的东西。UI 用它在弹出选择器之前就给出提示。
bool HasLogs();

// 把日志目录、logging.ini 和一份 export-info.txt 打包到 destinationZip。
// 阻塞若干百毫秒（复制加起子进程），调用方应放在后台线程。
//
// includeVendorLogs 额外收进华为触控服务与面板驱动的日志。触控完全不工作时，原因只存在
// 于那份日志里：我们这侧只看得到宿主起没起来，面板 ID、总线状态、固件刷写都在厂商那边。
// 默认不收，因为它按天分文件、目录里常年堆着上百 MB，而多数反馈用不到。
Result WriteArchive(const std::wstring& destinationZip, bool includeVendorLogs = false);

} // namespace LogExport
