# 充电控制：链路、语义与已验证结论

充电阈值经 OEM WMI 写入 EC，截止动作由固件执行。手动固定上限已实测生效；智能充电在
禁用 PC Manager 的机器上没有执行者，等于不限充。本文记录整条链路、字段语义、证据与
待解决项，供后续改动直接引用。

## 链路

写（设置上限 / 切模式）：

```
EGoTouchSettings（非提权）
  ChargeLimitSlider / SmartChargeToggle  → SendTrayCommand(SetChargeLimit, percent)
    smart 用哨兵 0，非 smart 用滑块值           MainWindow.xaml.cpp:1431 / :1484
EGoTouchTray
  SubmitChargeLimitCommand                    EGoTouchTray.cpp:1654
    ↓ PenControl 共享内存（校验 percent==0 || 50..100，PenControlChannel.cpp:415）
OpenEGoHubService（LocalSystem）
  HandlePenControlCommand 的 ChargeLimit 分支  ServiceHost.cpp:1487-1521
    ↓ RunHalTool 拉起 GaokunPower.exe（--smart / --limit <n>），等退出码，15 s 上限
GaokunPower.exe（继承 LocalSystem，提权由此而来）
  Detail::SetChargeLimit / SetSmartCharge     ChargeLimit.cpp:61 / :80
    ↓ Oem::Invoke → ROOT\WMI 的 OemWMIMethod::OemWMIfun → ACPI PNP0C14\HWMI_0 → EC
```

读走另一条路：服务进程内直调 GaokunHal.lib 发 `03 16`（ServiceHost.cpp:110-119 的
RefreshChargeLimit），结果经 PenStatus 共享内存到托盘与设置窗。读需要提权，设置窗自己
读不到（实测 0x80041003）。读的时机只有三处：AccessoryLoop 首轮、touch_only 启动时一次、
每次写之后；没有周期重读。

## SBCM 字段

写请求（`03 15`）六字节，字段名照搬 goodies 的 Set-ChargeLimit.ps1（ChargeLimit.cpp:3）：

| 偏移 | 字段 | 语义 |
|---|---|---|
| [2] | CHMD | 充电模式：1 = 手动阈值（硬性生效），4 = 智能充电（阈值不强制） |
| [3] | DELY | 未知，写路径恒用 0x18；EC 实测读回 72。唯一可能藏着行为开关的字段 |
| [4] | STCP | 起充阈值，恒为停充 - 5 |
| [5] | SOCP | 停充阈值 |

UI/IPC 层的哨兵 0 与 CHMD 是两套值：0 只存在于 EGoTouchTrayIpc / PenControlChannel，
被服务译成 `--smart`，hal 写的是 CHMD=4。链路上没有一处把 0 当 CHMD 下发。

读路径（`03 16`）是本仓库从 WmiUtil.dll 的 `BiosWmi::GetSmartChargeMode`（RVA 0x5600）
静态反汇编得到并实测确认的（hardware-hal.md:190-219）。

## 已验证结论

**手动模式在工作。**

- 写 80 后读回 STCP/SOCP = 75/80，CHMD 由 4 变 1（ChargeLimit.cpp:11-13）。
- 一次 84%→95% 充电曲线实测：硬件停在 34861 mWh，95% 满充为 35042 mWh
  （GaokunPower.h:90-96）。截止由 EC 执行，写入后不需要任何后台服务维护。

**智能模式在禁用 PC Manager 的机器上等于不限充。** 「阈值写着 70 电池充到 100%」
（GaokunPower.h:141-143、PowerMain.cpp:41-45）描述的是 CHMD=4 这一支，不是写入失败。
动态调整 STCP/SOCP 的学习逻辑在 PC Manager 的 HwPCCoreService / MBAMainService 里，两者
都在 VendorServices.cpp:18-26 的禁用名单内；禁用后没有任何进程再更新这两个字节，本仓库
也明确不管（ChargeLimit.cpp:77-79）。2026-08-31 本机实证：EC 读回 start 65 / stop 70 /
mode 4，电池 100%——阈值是 08-29 手动写 70 的残值，模式是智能，无人执行。

**厂商用的可能不是同一条写命令。** HardwareHal 的写路径是 `03 10`，只带两个阈值字节、
不动 CHMD（hardware-hal.md:212-214）；本仓库用 `03 15` 连模式一起写。两条命令是否作用于
同一份 EC 记录未验证。厂商 `Battery::GetChargeThreshold` 本身是坏的
（hardware-hal.md:157-188），这是读路径自行逆向的原因。

## 验证命令

`GaokunPower.exe` 是阈值唯一的命令行入口（GaokunCtl 没有充电相关命令）：

```powershell
GaokunPower.exe --info               # 电池实况，不需要提权
GaokunPower.exe --query              # 读阈值与模式，需管理员
GaokunPower.exe --limit 80 --dry-run # 走完全部 COM 步骤不下发，打印每步 HRESULT
GaokunPower.exe --limit 80           # 写手动阈值
GaokunPower.exe --smart              # 交还智能模式（CHMD=4）
```

## 待解决

1. **UI 误导**：vendor 服务被禁时「智能充电」仍显示为正常选项（「由智能充电决定」，
   MainWindow.xaml.cpp:1205-1207），实际等于不限充。应在此状态下明示，或改称「不限制」。
2. **写入无回读校验**：RunHalTool 只看退出码；写后虽会重读（ServiceHost.cpp:1517-1519）
   但只更新缓存，不与写入值比较。ExecMethod 成功而 EC 拒绝的情形没有任何一层能发现。
3. **缓存陈旧**：无周期重读，外部改了模式（恢复 PC Manager、固件行为）后界面一直显示
   服务启动那一刻的值，直到下次写入或重启服务。
4. `DELY` 的语义；`03 10` 与 `03 15` 的关系（用 0x10 只写阈值不动 CHMD 会怎样）。
5. 在未禁用 PC Manager 的机器上确认智能模式是否正常工作，以坐实或证伪第 1 条的前提。
