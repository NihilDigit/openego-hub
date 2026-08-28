# gaokun-hal

华为 MateBook E Go（平台代号 Gaokun，本机 `HUAWEI GK-W7X`）的硬件逆向层。
凡是需要与华为的 x64 DLL、固件表或 WMI 接口打交道的部分都收在这里，上层只消费本层的接口。

配套的 UI 与应用策略在 OpenEGoHub，它不需要了解华为的任何 ABI。

## 进程边界就是架构边界

这台机器是 ARM64，而华为的全部相关 DLL 都是 x64。纯 ARM64 进程无法加载 x64 DLL，
ARM64EC 可以，代价是整个进程都要按 ARM64EC 编译。

本层的分法是：**需要加载厂商 DLL 的组件各自成为独立进程并编译为 ARM64EC，其余保持原生 ARM64**。
上层因此不必为了用到某一项能力而把自己整体改成 ARM64EC。

goodies 的 README 记着一句「No GUI yet because no modern GUI framework supports arm64ec」，
指的正是这个约束；把 EC 关在后端进程里，这条约束就不再传导到界面层。

| 组件 | 架构 | 依赖 | 用途 |
| --- | --- | --- | --- |
| `GaokunThpHost.exe` | ARM64EC | `THP_Service.dll` 链 | 触控与笔，承载原厂完整算法链路 |
| `GaokunDisplay.exe` | ARM64EC | `qdcmlib.dll` | 出厂色彩校准（色域） |
| `GaokunKeyboardHost.exe` | ARM64EC | `KeyboardService.dll` | 键盘分离后无线连接开关 |
| `GaokunPower.exe` | 原生 ARM64 | 无 | 电池充电阈值（WMI） |
| `GaokunThpHost.lib` | 原生 ARM64 | 无 | 触控宿主的进程控制器，供上层链接 |

## 构建

需要 MSVC 的 ARM64 工具链。

```powershell
.\scripts\build.ps1 -Config Release -Verify
```

`-Verify` 会校验宿主确为 ARM64EC、控制器库确为原生 ARM64。这一步不能省：
ARM64EC 产物的 PE machine 字段是 `0x8664`（x64），凭它判断必然出错，真正的判据是
load config 中的 `CHPEMetadataPointer` 非零。而控制器库若被误编译为 ARM64EC，
要等到原生调用方链接时才会暴露。

## 各组件的接口与用法

### 触控（GaokunThpHost）

原厂 `HuaweiThpService.exe` 是编译为 x64 的 .NET 程序集，因此在 ARM64 机器上整条触控栈都跑在
模拟层内。托管代码无法编译为 ARM64EC，所以这层薄壳以原生 C++ 重写，逐条对照原厂 IL，
连同那些看起来像缺陷的行为一并保留。完整记录见 [`docs/vendor-service.md`](docs/vendor-service.md)。

宿主由上层拉起，不替换任何文件、不注册服务：

```
GaokunThpHost.exe --hosted --parent <pid> --stop-event <name>
GaokunThpHost.exe --console      # 独立运行，便于本机调试
GaokunThpHost.exe --check        # 只读自检，不加载 THP_Service.dll
```

停止有两条路径，都会走到 `ThpFuncStop`：上层置位停止事件，或上层进程本身消失。
后者不用 Job Object 的 `KILL_ON_JOB_CLOSE` 兜底，因为那是直接终止，DLL 没有机会复位 AFE。

集成方式见 [`docs/integration.md`](docs/integration.md)。

### 色域（GaokunDisplay）

```
GaokunDisplay.exe --info
GaokunDisplay.exe --preset <sRGB|DisplayP3>
GaokunDisplay.exe --reset
```

出厂校准不在任何 DLL 里，而在 ACPI 固件表 `DLUT` 中（本机 131124 字节，OEM ID `HUAWEI`，
表 ID `3DLUTTBL`），读取只需 `GetSystemFirmwareTable`。只有把 LUT 应用到面板才需要
`qdcmlib.dll`。两个细节容易出错：

- provider 签名按大端打包而表 ID 按小端，弄反时返回 0，现象与「本机没有这张表」完全一致。
- 固件表按 `i = r*289 + g*17 + b` 排列，而 `qdcmlib` 要的是 `b*289 + g*17 + r`，
  解码时必须转置。两侧用同一种顺序会让红蓝互换，而画面仍然像是正常的。

`qdcmlib.dll` 必须与 `GaokunDisplay.exe` 放在一起，不能指望系统目录里的那一份。

System32 中有 `qdcmlib.dll` 与 `qdcmlib_x64.dll` 两个文件，本机上两者都加载得上、符号也
解析得到，但 `Create_QDCMLibrary` 与 `Create_QDCMLibrary2` 双双返回 null。它们的初始化会
去解析 D3DKMT 的 thunk 指针，失败时打的正是库里那句 `Could not locate Thunk function
pointers!`。换成 goodies 随包附带的那一份就正常。

三者的代码段反汇编只差数据地址的 8 字节偏移，导入表完全一致，二进制却有一万五千字节不同，
是同一份源码的不同构建；究竟哪一处造成差别尚未查明。goodies 自带这个 DLL，作者应当也
撞上了同一件事。

排查时被误导过两次，记下来省得重来：可执行文件用 Debug 还是 Release 编译与此无关（2x2
对照过）；失败时 `GetLastError` 是 2，看着像「文件找不到」，其实文件早已加载成功，失败发生
在工厂函数里。

华为的「显示管理」（桌面右键菜单，`HwLcdEnhancement\MonitorManage.exe`）覆盖面更广，
除色域外还有色温、锐度、白点、护眼与 ICC，其色域枚举共 12 种模式，而本机 `DLUT` 只有
两个槽位有数据。本组件目前只实现色域，与 EgoTools 对齐。

### 键盘（GaokunKeyboard）

```
GaokunKeyboardHost.exe --detach-support [enable|disable]
```

`KeyboardService.dll` 随 PC Manager 的配件中心安装，路径为
`<PCManager>\components\accessories_center\accessories_app\AccessoryApp\Lib\Plugins\Depend`。
该目录同时是 HuaweiPenEraserService 建议删除的那个 Plugins 目录，若已被清理，本功能不可用。

它的调用模型是异步的：两个导出各是一个阻塞消息循环，需要各自的线程；命令由 `CommandSendXxx`
发出，结果经事先注册的回调返回。**设备打开之前发命令会访问违例而不是超时**，所以必须先等待，
判据是消息循环线程停止消耗 CPU 周期。

`StopProcPipeMsg`、`StopLoop` 与 `FreeLibrary` 一律不调用：循环线程仍停在该 DLL 内部，
调用它们同样会访问违例。原厂工具解析了这些符号却从不使用，原因即在于此。

### 充电阈值（GaokunPower）

```
GaokunPower.exe --limit <50-100> [--dry-run]
```

走 `ROOT\WMI` 的 `OemWMIMethod::OemWMIfun`，不经过任何厂商 DLL。请求是 64 字节缓冲：

```
[0] 0x03  MFID
[1] 0x15  SFID = SBCM
[2] 0x01  SBCM.CHMD
[3] 0x18  SBCM.DELY
[4]       SBCM.STCP  开始充电阈值（固定比停止阈值低 5）
[5]       SBCM.SOCP  停止充电阈值
```

方法的入参名为 `u8Input`。原厂脚本按位置传参，因而名字在那里看不出来；名字写错时
`Put` 返回 `WBEM_E_NOT_FOUND`，与「实例不存在」是同一个错误码。该方法还声明了
`u8Output` 出参，读回当前阈值可能可行，尚未验证。

`--dry-run` 会走完全部 WMI 步骤并打印请求，但不发出 `ExecMethod`。

## 致谢

色域与键盘两部分的接口形态参考了 [dantmnf/goodies](https://github.com/matebook-e-go/goodies)，
能力范围对齐 [SaKongA/EgoTools](https://github.com/SaKongA/EgoTools)。本层为独立实现，
不分发任何厂商二进制。
