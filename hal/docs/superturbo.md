# SuperTurbo：进程调度引擎，不是硬件开关

电脑管家「设置 → 电脑优化」里的「Super Turbo 智能加速引擎」是 `MonSysPerfPlugin.dll`
中 Cgroup 调度引擎的总开关，手段全部是 Windows 用户态 API，与 BIOS WMI 无关。本文记录
它的落点、手段、机型门控与本机现状，全部来自对本机 PC Manager 安装目录的只读静态分析
（2026-08-31，组件版本 14.0.5.843(C233)），未运行任何厂商可执行文件。

## 开关与落点

- UI：`HwSettings.exe` 的 `btnSuperTurboAlert`（布局 `res\layout\setting2.xml` 的
  `super_turbo_layout`），处理函数名为 `SendCpuAffinitySwitch`——同布局里的
  `cpu_affinity_layout`（「应用运行智能加速」）是同一功能的旧命名，已无代码引用。
- 持久化：`HKLM\SOFTWARE\PCManager\MBAPowerManager\CpuAffinity`（REG_DWORD）。执行侧
  `CgroupImpl::MonitorCgroupSwitchStatus` 监视该键，转 `StartPolicy` / `StopPolicy`。
- 载体：`MonSysPerfPlugin.dll` 经 `config\plugincfg.xml`（clsid
  `FBEDEFED-7A27-4400-BBF9-C9902CF534C5`，process=2）装进 `MBAMessageCenter.exe`，与
  SmartChargePlugin 同宿主（调查范式见 charge-control.md）。整个组件是独立构建产物，
  PDB 路径统一为 `D:\SuperTurboArmChinaBuild\SuperTurbo\`；`SuperTurboUpgrade.exe` 只是
  它的 OTA 升级器。

## 打开后做什么

- CPU 亲和性分组：`CgroupWhiteList.xml`（183 KB，逐应用策略）注释里的四档——
  BIG_GROUP 100%、MID_GROUP 后 3/4 核、VIDEO_SMALL_GROUP 后 7/8 核、SMALL_GROUP 后
  1/4 核，另有能效核调度（`OperateEfficientCore`）。直播、会议类进程（livehime、
  wemeetapp、抖音直播伴侣等）在黑名单内不参与。
- 后台进程冻结：动态解析 `ntdll` 的 `NtSuspendProcess` / `NtResumeProcess`
  （`AppFreezeExecutor`）。
- 优先级与功耗：`SetPriorityClass`、`SetProcessInformation`；EPP 与电源方案 overlay
  （`EppControl::*`、`PowerSetActiveScheme`、`PowerWriteACValueIndex`）。
- 智能刷新率挂在这个开关下：`IDS_OPT_REFRESH_RATE_DESC` 明写「Super Turbo 智能加速
  引擎需开启」，与 osd.md 的刷新率线索衔接（同目录另有 `RefreshRateConf.dat`）。
- 场景策略 `DTTSceneConf.dat`（165 KB）：头 0x98 字节是 XOR 0x80 的明文 XML，其后
  加密未解。DTT 指 Intel Dynamic Tuning，内含 MSR PL1 等 Intel 专属项，Snapdragon 上
  走不到。

## 机型门控

GaoKun 在 `BoostPolicy::RunningSwitch` / `CpuAffinityPolicy::RunningSwitch` 共用的机型
支持表内；DLL 里有 `GK-W7X` → `GaoKun3` 的直接映射（按 `Win32_VideoController` 判出
QUALCOMM 后分流），本机 SMBIOS `HUAWEI GK-W7X` 对得上。但三条支线被排除：

- `ProductFeature.xml` 的 `<setepp>` 支持表不含 GaoKun（只有 Intel 机型）；
- 平台抽象层 `IPlatformApi` 只注册了 Intel ADL 一个实现，Qualcomm 无工厂函数，屏幕
  省电（EPSM/OPST）落到 `api object create func not reigstered!`；
- `CgroupWhiteList.xml` 的 `FreezeSupportMachineType` 是空标签，冻结支线不启用。

即本机可用的只有 CPU 亲和性调度与 Boost。

## 本机现状

没有效果。宿主 `MBAMessageCenter.exe` 只由 `MateBookService.exe`（服务
`MBAMainService`，已被本程序禁用）与 `PCManager.exe` 引用，无自启动项与计划任务；
开关值 `CpuAffinity` 当前为 0。即使写 1，也没有进程去读。唯一未验证的例外：手动打开
`PCManager.exe` 主界面可能在用户会话里把插件宿主带起来。

## 命名坑

三个带 Turbo 的东西互不相干：本开关（Cgroup 引擎）；「正在超级快充 Turbo」
（`IDS_PCOPT_SUPERTURBO_CHARGING`，充电动画）；`TurboModeConfig.xml` 的 Fn+P 性能模式
（走 `PerfCommonPlugin.dll`）。引用 `WmiUtil.dll` 的 `BiosWmi::Get/SetTurboMode`
（hardware-hal.md:280 起）的是 `PerfCommonPlugin.dll`（35 处）、
`PowerManagerUIPlugin.dll`、`SynergyService.dll`、`HwMdcTool.dll`——
`MonSysPerfPlugin.dll` 一处都没有。

## 待解决

1. `DTTSceneConf.dat` 0x98 之后的加密体未解；属性名（`SuperTurboOnEpp`、
   `EppLevelList`、`SceneStrategyList` 等）可从 DLL 字符串表读到。
2. `HKLM\SOFTWARE\PCManager\SuperTurbo` 键在 DLL 里有引用、本机不存在，用途未知。
3. 手动启动 `PCManager.exe` 是否会拉起插件宿主并使开关生效，未实测。
4. 若要在 OpenEGo Hub 里自研对应物（后台冻结 + 能效核调度），全部手段无厂商依赖；
   逐应用策略表可参考 `CgroupWhiteList.xml` 的结构。
