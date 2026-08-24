# EGoTouch 驱动服务 — 架构文档

> 最后更新：2026-06-10

## 1. 当前架构总览

```text
OpenEGoHubService.exe
  ServiceShell
    -> ServiceHost
       -> ConfigRuntime (code defaults + session patch)
       -> DeviceRuntime
       -> IpcPipeServer
       -> SharedFrameWriter (_DEBUG)

OpenEGoHubApp.exe
  ServiceProxy
    -> Config v3 catalog/snapshot/patch
    -> Dynamic debug schema/snapshot
    -> DVRCore import/export
```

核心边界：

1. 配置默认值只在代码/binder 中定义。
2. App 上位机动态调整只改当前 Service 会话。
3. 持久化配置文件不再是产品能力。
4. Debug data 与 config patch 分离。

## 2. Config v3 命令

| 命令 | 值 | 当前角色 |
| --- | ---: | --- |
| `ReloadConfig` - `ApplyConfigTlvChunk` | `40`-`45` | legacy tombstone，返回 `UnsupportedCommand` |
| `GetConfigCatalogV3` | `46` | 分页读取 Service catalog |
| `GetConfigSnapshotV3` | `47` | 分页读取当前会话 snapshot |
| `ApplyConfigPatchV3` | `48` | session-only 动态调整 |
| `PersistConfigV3` | `49` | 保留 wire 入口；返回 `UnsupportedCommand` |

`ConfigRuntime::ApplyConfigPatchV3()` 的事务校验和 commit 路径见 [ConfigRuntime.cpp:701-723](../EGoTouchService/source/ConfigRuntime.cpp#L701-L723)。`PersistConfigV3()` 当前拒绝持久化，见 [ConfigRuntime.cpp:694-701](../EGoTouchService/source/ConfigRuntime.cpp#L694-L701)。

## 3. Debug / runtime control IPC

| 命令 | 当前角色 |
| --- | --- |
| `EnterDebugMode` / `ExitDebugMode` | 控制 debug frame 推送 |
| `GetDebugSchema` / `GetDebugSnapshot` | 读取 dynamic debug 字段和值 |
| `AfeCommand` / `SetMasterParserOnly` / `SetPenPressureMode` | 命令式运行时控制，不持久化 |

Debug shared memory 入口在 [ServiceHost.cpp:1029-1065](../EGoTouchService/source/ServiceHost.cpp#L1029-L1065)，dynamic debug snapshot 入口在 [ServiceHost.cpp:1281-1320](../EGoTouchService/source/ServiceHost.cpp#L1281-L1320)。

## 4. DeviceRuntime 门面隔离

`DeviceRuntime` 继续只通过门面接口暴露 lifecycle、policy、pipeline apply、VHF、command 和 debug frame 能力；ServiceHost 不直接操作 pipeline 内部对象。Pipeline session config 由 `ConfigApplyActionKind::PipelineRuntime` 下发，见 [ServiceHost.cpp:1121-1129](../EGoTouchService/source/ServiceHost.cpp#L1121-L1129)。

## 小结

- 文件配置路径已被移除。
- Config v3 保留 catalog/snapshot/patch 作为 Debug 上位机会话内调整通道。
- Persist 命令仅用于兼容 wire ABI，不再表示可落盘配置。
