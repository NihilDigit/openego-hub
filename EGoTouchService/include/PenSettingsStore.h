#pragma once

// PenSettingsStore — 唯一一处仍然落盘的服务配置。
//
// 项目已经取消了配置文件体系（见 ConfigRuntime 里那两条 WARN），托盘配置菜单推翻了这个
// 决定，但只推翻这一个键：用户在托盘里选的侧键模式必须跨重启保留，否则每次开机都退回默认。
// 其余的键仍然只活在当前会话里，不要借这个文件把整套 YAML 复活。
//
// 位置放在 %ProgramData%\OpenEGoHub\：服务以 SYSTEM 建文件，ProgramData 的默认 ACL 让
// 普通用户读得了、改不了。写权限交给管理员就够了——真要改，路径是托盘的控制通道，不是
// 手改这个文件。

#include "PenButtonConfig.h"

#include <optional>
#include <string>

namespace Service {

class PenSettingsStore {
public:
    /// 读取持久化的侧键模式。文件缺失、键缺失或 token 非法都返回 nullopt（调用方回默认值）。
    static std::optional<PenButtonMode> LoadPenButtonMode();

    /// 写入侧键模式。目录不存在时先建。返回 false 表示没落盘，调用方只需记 WARN。
    static bool SavePenButtonMode(PenButtonMode mode);

    /// 完整文件路径，只为日志可读。
    static std::wstring FilePath();
};

} // namespace Service
