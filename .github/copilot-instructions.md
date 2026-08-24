# Copilot Instructions

完整的构建、调试与架构说明见仓库根目录的 `CLAUDE.md`。以下是最容易踩错的几条。

## 构建

- 用 `cl.exe`，不要用 clang。设置窗口由 MSBuild 从 `.vcxproj` 构建，门控是 `if(WIN32 AND MSVC)`，clang 配置下不会产出 `OpenEGoHubSettings.exe`，打包随即失败。
- 不要给 preset 加 `CMAKE_*_COMPILER_TARGET`，它是 clang 专用，会让 CMake 发出 `cl.exe` 解析不了的 GNU 风格链接参数。
- ninja 撞到第一个失败目标就停，后续目标不重编，紧接着的 ctest 会在上一轮二进制上跑出全绿。判断构建成败要同时查 `ninja: build stopped`。
- Release 编译时 `EGOTOUCH_SERVICE_ENABLE_IPC=0` 且 `_DEBUG` 未定义。无条件调用点用到的成员必须无条件声明，否则本地 Debug 全绿而 Release 编译不过。

## 设备调试

Debug 与 Release 服务不能同时运行：两者驱动同一块 Himax 设备、创建同一个 VHF 虚拟 HID、读同一路 BT-MCU HID 报文。Debug 服务直接从构建树运行，会锁住自己的 exe，重新构建前必须停掉。

## DVR 分析

「rawdata / 原始数据」指 DVR 帧中的 heatmap 原始矩阵，不是按 `rawDataLength` / `raw.hex` 判断的独立字节块。一段录制可能只有热力图而没有解算出的 contacts / peaks。
