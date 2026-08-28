# 与上层的接口

本层对上层只暴露两样东西：一个原生 ARM64 的静态库，以及若干独立的命令行组件。
上层不需要知道任何华为的 ABI，也不需要把自己编译成 ARM64EC。

## 触控：链接 GaokunHal.lib

这是唯一需要长期驻留、因而需要生命周期管理的能力，所以它有库接口而不只是命令行。

```cpp
#include "GaokunThp.h"

Gaokun::Thp::HostController controller;

// 切换到 ARM64EC 原厂链路。调用前必须先停掉 HuaweiThpService：
// 设备同一时刻只能由一个实现持有。
const auto result = controller.Start(LR"(C:\path\to\GaokunThpHost.exe)");
if (result != Gaokun::Thp::StartResult::Started) {
    // ExitedImmediately 通常意味着设备仍被占用，或原厂目录定位失败
}

// 交还设备
if (!controller.Stop(std::chrono::seconds(15))) {
    // 超时后已强制终止，设备可能停在中间状态，记一条日志
}
// 随后再启动 HuaweiThpService
```

链接 `GaokunHal.lib`，包含 `include/GaokunThp.h`。库只用 Win32 的进程与同步原语，
因此调用方保持原生 ARM64。

### 崩溃时的收尾

宿主同时等待停止事件与调用方的进程句柄。调用方若崩溃、来不及发停止事件，宿主也会自行
走完 `ThpFuncStop` 再退出，不会留下占着设备的孤儿进程。

这一点没有用 Job Object 的 `KILL_ON_JOB_CLOSE` 实现：那是直接终止，DLL 得不到复位 AFE 的
机会，设备会停在中间状态，下一个接手者要多做一次恢复。

### 与原厂服务的互斥

`HuaweiThpService` 与本宿主二选一，切换顺序是固定的：

```
停 HuaweiThpService  ->  HostController::Start
HostController::Stop  ->  起 HuaweiThpService
```

宿主从 `HuaweiThpService` 的 `ImagePath` 推导原厂安装目录，因此该服务即便处于停止或禁用
状态也必须仍然注册在 SCM 中。这也意味着宿主自身放在哪里都可以。

## 其余能力：调用命令行组件

色域、键盘与充电阈值都是一次性动作，没有常驻状态，用进程调用即可，退出码为 0 表示成功。

```
GaokunDisplay.exe  --preset <sRGB|DisplayP3> | --reset | --info
GaokunKeyboardHost.exe --detach-support [enable|disable]
GaokunPower.exe    --limit <50-100> [--dry-run]
```

`GaokunPower.exe` 需要提权；另外两个不需要。

如果上层将来需要这几项的进程内接口，按 `HostController` 的方式补库即可，但注意色域与键盘
两个组件依赖 x64 DLL，它们的库化版本会把调用方拖成 ARM64EC，这正是当前用进程边界隔开的原因。

## 可用性探测

各组件在依赖缺失时以非零退出并说明原因，上层据此决定是否在界面上呈现对应功能：

- `GaokunDisplay.exe --info` 报告 `DLUT` 固件表是否存在、各 preset 槽位是否有数据。
- `GaokunKeyboardHost.exe --detach-support` 在 PC Manager 的 Plugins 目录被删除时退出码为 2,
  并指出该目录的位置。
- `GaokunPower.exe --limit <n> --dry-run` 走完整条 WMI 路径但不写入。

## 部署

本层不安装、不注册服务、不修改华为的任何文件。把构建产物放在上层能找到的位置即可。

在两个仓库尚未拆分之前，可以让上层通过符号链接指向本仓库的构建目录：

```powershell
New-Item -ItemType SymbolicLink -Path <上层目录>\hal -Target <本仓库>\build\Release
```

## 笔：状态与事件

笔和触控一样需要一个常驻宿主，但它同时产出两类数据，因此有两条通道。

```cpp
#include "GaokunPen.h"

Gaokun::Pen::HostController host;
host.Start(LR"(...\GaokunPenHost.exe)");

Gaokun::Pen::SnapshotReader snapshots;
snapshots.Open();          // 宿主刚起时映射可能还没建好，失败可重试

Gaokun::Pen::Snapshot state;
if (snapshots.Read(state)) { /* 电量、连接、型号、固件、序列号、按键 */ }

Gaokun::Pen::EventReader events;
events.Open();
Gaokun::Pen::Event event;
while (events.Poll(event)) { /* 侧键、连接请求、提醒 */ }
```

状态快照走共享内存加跨进程 seqlock，可重复读，漏读一次没有影响；离散事件走命名管道，
每条都是一次边沿，漏掉就没有了。把边沿压进状态位会丢失「又发生了一次」，把快照塞进管道
则会在读者慢时堆积，所以两者不能合并成一条通道。

宿主端的事件队列有界（64 条），读者未连接时丢最旧的：笔的事件都是即时语义，补投一条
几秒前的侧键按下没有意义，反而会让界面在恢复连接的瞬间连闪几下。

本机实测值可用来对照实现是否正确：

```
connected yes   battery 100%   charging no
module     65819            (CD54R)
keySupport 0x01             只支持一个按键功能
firmware   CD54 1.0.0.143
hardware   01-0400.0143-0000
```

`GaokunPenCtl.exe --watch <秒>` 走的是与上层完全相同的这条路，可用来在接入前先确认通道通。
