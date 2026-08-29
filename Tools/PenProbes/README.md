# 笔输入探针

两个一次性诊断工具，不进构建树，用得着的时候手工编译：

```powershell
. .\scripts\vsenv.ps1; Import-VsDevEnv
cl /nologo /std:c++17 /EHsc /utf-8 Tools\PenProbes\PenFlagsProbe.cpp /link user32.lib gdi32.lib
cl /nologo /std:c++17 /EHsc /utf-8 Tools\PenProbes\HidCapsProbe.cpp
```

`PenFlagsProbe` 开一个窗口，打印每个 pointer 事件的 `POINTER_PEN_INFO`——`penFlags`、
`penMask`、压力、倾角。要看的是 `INVERTED` 与 `ERASER` 两位在不在。

`HidCapsProbe` 列出本机所有 digitizer HID 集合声明了哪些 usage，用来分清「描述符里没有
这一位」和「有这一位但运行时不置」。只用 0 访问权限打开设备，不会把数字化仪从正在用它的
进程手里抢走。

## 它们回答过什么

橡皮那条线上，这两个探针把断点从三层收窄到一层：

- `HidCapsProbe` 证明厂商 VHF 的笔集合（report `0x08`）**声明了** `Invert(0x3C)` 与
  `Eraser(0x45)`，排除了描述符缺位。
- `PenFlagsProbe` 证明那两位运行时**恒为 0**，`PEN_FLAG_BARREL` 也一样，排除了「位其实
  置上了只是 OneNote 不认」。

`penFlags` 只能作否定判据：桌面版 OneNote 读的是 RealTimeStylus 的
`StylusInfo.bIsInvertedCursor` 而不是 `penFlags`。两者同源于 HID 的 Invert 位，所以
`penFlags` 没有就一定没有；真到了要验证「位置上之后 OneNote 认不认」的那一步，得换成
RTS 插件来看。详见 `docs/onenote_ink_eraser.md`。
