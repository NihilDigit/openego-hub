#pragma once

// UpdateChecker — GitHub Releases 上的版本检查、安装包下载、哈希校验与静默安装。
//
// 单独成一个编译单元的理由与 ChargePolicy 相同：这里每一件事都只和网络、文件、注册表
// 打交道，与服务的生命周期无关，而 ServiceHost.cpp 已经是这棵树上最长的文件。状态机留在
// ServiceHost，这里只提供一步一步的动作，每一步都能单独失败。
//
// 只有服务做得了这些事：它在 LocalSystem 下常驻，有网络，也是唯一能把 msiexec 拉起来的
// 进程。托盘和设置窗都是中完整性，自己装不了。

#include <cstdint>
#include <optional>
#include <string>

namespace Service::Update {

// 三段版本号。每段用 uint8_t 而不是更宽的类型，是为了和 PenStatusChannel 发布的那三个
// 字节同源：通道装不下更大的值，那就在解析这一步拦住，而不是等到发布时被悄悄截断。
//
// 代价是 patch 超过 255 的发布会被判成解析失败，也就是「这次没查到更新」。发布流程允许
// 的 patch 上限是 65535（见 .github/workflows/release.yml），至今没有接近过；真要用到
// 那个区间，先改 PenStatusChannel 的字段宽度，而不是在这里放宽。
struct Version {
    uint8_t major = 0;
    uint8_t minor = 0;
    uint8_t patch = 0;

    friend auto operator<=>(const Version&, const Version&) = default;
    friend bool operator==(const Version&, const Version&) = default;
};

std::string ToString(const Version& version);

// 本机版本，取自 AppVersion.h。
[[nodiscard]] Version CurrentVersion() noexcept;

// 形如 "v0.4.0" 的 tag。不合规的一律返回 nullopt，不做任何宽容解释：宽容解析的后果是把
// 一个看不懂的 tag 当成某个版本号，然后据此判断要不要给用户推送安装。
[[nodiscard]] std::optional<Version> ParseTag(std::string_view tag);

// 一次检查的结果。地址整份留着，因为 Install 是另一次用户动作，那时不该再查一遍。
struct ReleaseInfo {
    Version version{};
    std::wstring msiUrl;
    std::wstring msiFileName;  // 校验和文件里按这个名字找行
    std::wstring checksumUrl;
};

// error 的取值：WinHTTP/Win32 错误码原样返回，另外两段是本模块自己的失败。数值只进日志，
// 不发布给界面——把一个错误码摆给用户没有可操作性。
inline constexpr uint32_t kErrorHttpStatusBase = 100000;  // 加上 HTTP 状态码
inline constexpr uint32_t kErrorMalformedResponse = 200001;
inline constexpr uint32_t kErrorChecksumMismatch = 200002;
inline constexpr uint32_t kErrorChecksumMissing = 200003;
inline constexpr uint32_t kErrorBadAssetName = 200004;

// GitHub 的 releases/latest。这个接口本身不返回预发布版本，所以调用方不必另做 prerelease
// 过滤；改成按 tag 列表查才需要。失败返回 nullopt。
//
// 接口基址可由 HKLM\SOFTWARE\OpenEGoHub\Update 的 ApiBaseUrl 覆盖，见实现处。
[[nodiscard]] std::optional<ReleaseInfo> FetchLatestRelease(uint32_t& error);

// 自动检查开关的落盘值。托盘送来的开关必须由服务这边记：那个选择原本落在 HKCU，而服务跑在
// LocalSystem 下读不到用户的 hive，于是 service.auto_update_check 永远拿不到用户的选择。
//
// 与跳过记录同一个键：更新相关的持久化状态只此一处，比再开一个 ini 少一处要对齐的路径。
// 返回 nullopt 表示用户从未设过，此时按配置里的默认值走。
[[nodiscard]] std::optional<bool> LoadAutoCheck();
void StoreAutoCheck(bool enabled);

// 「这次升级是本服务发起的，装完把托盘拉回来」的标记，同样记在更新键下。
//
// 安装包那侧做不到这件事：LaunchTray 挂在安装向导结束页的 Finish 按钮上，而 /qn 根本没有
// 那个页面。后果不止图标消失——触控租约在托盘手里，没有托盘就没人续租，装完之后触控一直
// 停在交还状态。
//
// 标记而不是「托盘不在就拉起」：用户可以主动退出托盘（RequestSafeExit、设置窗里的「退出并
// 交还原厂」），无条件重启会跟这个意图打架。只有本服务拉起 msiexec 的那一次才置标记。
void MarkTrayRelaunchPending();
void ClearTrayRelaunchPending();
// 读出标记并清掉，返回它原先是否置位。清除先于拉起发生，拉不起来也不会在此后每次启动
// 重试——那会变成一个没有出口的循环。
[[nodiscard]] bool TakeTrayRelaunchPending();

// 把托盘拉进当前交互会话的用户令牌下。
//
// 放在这个编译单元里是因为目前只有升级这一条路径需要它；出现第二个调用方时应当搬走，
// 它本身与更新无关。
//
// 不能用服务自己的 LocalSystem 令牌：托盘必须是普通的中完整性进程，否则 UIPI 会挡住中
// 完整性的设置窗向它发窗口消息，而用户改的每一项设置都走那条路（CLAUDE.md，以及
// scripts/dev-cycle.ps1 经 explorer 启动它的理由）。
//
// 没有人登录时返回 false 且 error 为 0：那不是故障，托盘的 HKCU 自启项会在下次登录时把它
// 带回来。
[[nodiscard]] bool LaunchTrayInActiveSession(const std::wstring& exePath, uint32_t& error);

// 用户跳过的版本，记在 HKLM\SOFTWARE\OpenEGoHub\Update 的 SkippedVersion（REG_SZ）。
// 记版本号而不是记一个布尔：跳过这一版不等于跳过下一版。
[[nodiscard]] std::optional<Version> LoadSkippedVersion();
void StoreSkippedVersion(const Version& version);

// 下载 MSI 与校验和文件并核对 SHA256。成功时 msiPath 是落盘的安装包。
//
// 校验不过就删掉下载的文件：留着它，用户迟早会在下载目录里双击它一次，而我们已经知道
// 那份字节不对。
[[nodiscard]] bool DownloadAndVerify(const ReleaseInfo& release,
                                     std::wstring& msiPath,
                                     uint32_t& error);

// msiexec /i <msi> /qn /norestart。
[[nodiscard]] bool LaunchInstaller(const std::wstring& msiPath, uint32_t& error);

} // namespace Service::Update
