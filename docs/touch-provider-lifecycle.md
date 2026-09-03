# 触控提供方的时间常数与生命周期

触控提供方在 EGoTouch 与 HuaweiTHP 之间切换，切换由托盘的租约驱动。这份文档记录这套机制
在真机上的实测数字，以及据此确定的设计。

测量环境：GK-W7X（MateBook E，GaoKun 平台），Windows 11 ARM64，2026-09-03。以下每个数字都是
本机实测，不是推算。

## 服务与提供方的切换耗时

| 动作 | 实测 |
|---|---|
| `HuaweiThpService` 启动到 RUNNING | 323 / 325 / 341 ms |
| `HuaweiThpService` 停止到 STOPPED | 1640 / 3350 / 3368 ms |
| 本服务正常停止（含交还华为） | 6331 ms |
| 一次完整接管占用控制线程 | 4888 / 5129 ms |
| THP 宿主停止 | 约 4.4 s |

两个数字决定了不少设计：**华为起来只要 0.32 秒**，交还它的代价远低于此前的估计；而**一次接管
本身要花掉 5 秒左右**，比租约超时（5000 ms）还长。

`HuaweiThpService` 处于 START_PENDING 时发停止命令会被 SCM 拒绝，服务继续走到 RUNNING。这个
窗口里 `SwitchToEGo` 会走 `stopHuawei` 失败分支，接管失败但华为确实在跑，状态没有失真。

## 崩溃后的自愈边界

服务进程被强制终止（接管态）后的时间线：

```
  137 ms   服务消失，THP 宿主还在
 2217 ms   THP 宿主退出（它等父进程句柄）
 5198 ms   SCM 按 SC_ACTION_RESTART 拉回服务
 5777 ms   HuaweiThpService RUNNING
 7499 ms   托盘续租，重新接管
```

0.137 到 5.198 这 5 秒没有任何提供方在驱动设备。

自愈依赖 SCM 的恢复动作，而它配置为 24 小时内 3 次（`ServiceEntry.cpp`）。配额之外没有兜底：
在没有恢复动作的服务上重复同一实验，90 秒内 `svc=STOPPED hw=STOPPED thp=0` 没有任何变化。
用户报告的「触摸失灵，华为驱动也没启动上」就是这个状态。

两处相关的配置事实：

- `ChangeServiceConfig2W` 只写了 `SERVICE_CONFIG_FAILURE_ACTIONS`，没有写
  `SERVICE_CONFIG_FAILURE_ACTIONS_FLAG`。默认 `fFailureActionsOnNonCrashFailures` 为 FALSE，
  因此服务以错误码正常停止（启动失败走的正是这条路）不触发恢复动作。
- `HuaweiThpService` 自己配了 3 次重启、延迟 1 秒，启动类型 AUTO_START。它被我们停止属于正常
  停止，不触发它自己的恢复动作。

## 托盘续租的抖动

24 分钟连续采样，每分钟一条汇总：

```
n=60  avg=999 ms   max=1012~1017 ms     稳态
少数峰值：1083 / 1108 / 1326 / 1927 ms
```

托盘心跳是 UI 线程上的 `WM_TIMER`，周期 1000 ms。最坏一次 1927 ms，离 5000 ms 超时还有很大
余量。同期唯一越过警戒线的两次都是服务自己占住控制线程造成的（4888 ms 和 5129 ms），托盘那侧
没有迟到。

**租约超时的真正压力来自服务自身的同步切换，不是托盘卡顿。**

## THP 宿主没有可用的活性信号

宿主运行约 30 分钟累计 CPU 5.98 秒；静止 5 秒采样，8 个线程的 CPU 增量全部为 0，全部处于
Wait。THP 链是中断驱动的，进程内部没有周期性节拍，因此宿主无法自发产生反映触控链状态的心跳。
`THP_Service.dll` 只导出七个函数（`ThpFuncStart`/`ThpFuncStop`/`GetMESSAGE` 与四个 Register），
不提供任何状态查询。

结论是宿主判活只能停留在进程级：`egoAlive` 现在检查进程是否存在，短期内没有更强的判据可用。

## 面板断电会让宿主内部死亡

一次息屏与唤醒的完整记录：

```
15:06:05.679  DISPLAY_STATE = 0 -> DisplayOff
15:06:12.249  THP host stopped              2 s 去抖 + 4.5 s 停止耗时
15:06:12.253  provider state=6 (EGoSuspended)
15:07:20.402  DISPLAY_STATE = 1 -> DisplayOn
15:07:20.463  Pen host is not responding (running=1, exit=-1); restarting it
15:07:21.984  THP host started
15:07:21.988  provider state=3 (EGoTouch)
```

倒数第三行是关键：pen 宿主在唤醒瞬间被判定为进程还在（`running=1`）而心跳已停。这是「面板断电
之后宿主进程存活、内部已死」的直接证据，它之所以能被发现，是因为 pen 宿主有心跳，而 THP 宿主
没有（见上一节）。

所以挂起时主动停掉 THP 宿主是必要的，不是保守做法：那是在用「主动停」绕开「发现不了」。

## 据此确定的设计

**租约的 deadline 从真正开始持有算起。** `AcquireOrRenew` 原先在进入时设 deadline，随后在同一
调用里同步跑完可能超过整个租期的切换，控制线程返回后紧跟的 `Tick` 会立刻判定超时。真机上这
表现为接管成功 4 毫秒后被自己撤销，随后重新接管，来回 15 秒：

```
15:14:28.462  state=3 EGoTouch            接管成功
15:14:28.466  state=4 SwitchingToHuawei   4 ms 后自己撤销
15:14:33.383  state=2 SwitchingToEGo      下一拍续租，从头再来
15:14:38.268  state=3 EGoTouch
```

**挂起时交还华为，不保留零提供方状态。** `EGoSuspended` 期间两个提供方都不在跑，实测一次息屏
即达 68 秒；`Tick` 与 `AcquireOrRenew` 都对该状态短路，退出它只有 `OnResume` 一条路，Resume 事件
一旦丢失就无限期停住。用 0.32 秒把华为请回来换掉这个状态。`TouchProviderState::EGoSuspended`
作为跨进程 ABI 保留在枚举中，不再发布。

**宿主看护的条件是「华为正被我们停着」，不是「我们持有租约」。** 租约超时会先清掉
`m_hasLease` 再切换，切换失败回滚后宿主将不再被任何人看护，而此时托盘通常已经不在。

**宿主在父进程消失时把华为请回来。** 它是这条路径上最后一个活着的进程，运行在 LocalSystem 下，
`RunHosted` 本来就分得清「被要求停」和「父进程没了」两种来路。这样不依赖 SCM 的次数配额，零
提供方窗口从 5 秒压到 2 秒出头。

## 复现方法

```powershell
# 服务时序：停掉两个自研服务后单独测华为
sc.exe stop HuaweiThpService ; sc.exe start HuaweiThpService

# 崩溃恢复：接管态下强杀服务进程，按 250 ms 采样服务与宿主状态
taskkill /F /PID (Get-CimInstance Win32_Service -Filter "Name='OpenEGoHubService'").ProcessId

# 宿主线程画像：需要提权，进程属 LocalSystem
Get-Process GaokunThpHost | ForEach-Object { $_.Threads }

# 电源事件序列：直接读服务日志
Select-String 'Power|System event|Provider' C:\ProgramData\OpenEGoHub\logs\OpenEGoHubService.txt
```

租约抖动需要在服务侧临时记录续租的到达间隔。基准点要取上一条命令处理完成的时刻，而不是它到达
的时刻：`AcquireOrRenew` 会同步跑完整个切换，按到达时刻计算量到的是服务自己忙，不是托盘迟到。
