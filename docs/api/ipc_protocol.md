# IPC 通信协议文档

> EGoTouchRev Service ↔ App 进程间通信协议 | wire version: 2 | 日期: 2026-06-08

---

## 1. 概述

EGoTouchService (Windows Service, Session 0) 与 EGoTouchApp (桌面应用, User Session) 之间通过 **Named Pipe** 进行命令/响应通信，通过 **Shared Memory** 进行实时帧数据推送。

```
┌─────────────────────┐                    ┌─────────────────────┐
│   EGoTouchApp       │                    │   EGoTouchService   │
│   (User Session)    │                    │   (Session 0)       │
│                     │                    │                     │
│  IpcPipeClient ─────┼──── Named Pipe ────┼── IpcPipeServer     │
│       │             │  \\.\pipe\          │       │             │
│       │             │  EGoTouchControl    │       │             │
│       ▼             │                    │       ▼             │
│  SharedFrameReader  │◄── Shared Memory ──│  SharedFrameWriter  │
│                     │  Global\            │                     │
│                     │  OpenEGoHubSharedFrame│                     │
│  Config v3 IPC ─────┼──── Named Pipe ────┼── ConfigRuntime     │
│                     │  commands 46-49     │  catalog/snapshot   │
│                     │                     │  patch/persist      │
└─────────────────────┘                    └─────────────────────┘
```

---

## 2. 传输层

### 2.1 Named Pipe

| 属性 | 值 |
|------|-----|
| Pipe 名称 | `\\.\pipe\EGoTouchControl` |
| 方向 | 双向 (App 发请求, Service 发响应) |
| 并发 | 单连接 (单 App 实例) |
| 安全 | Admin-only (`D:P(A;;GA;;;SY)(A;;GA;;;BA)`) |
| App 连接超时 | 5 秒 |
| 重连策略 | App 侧主动重连 |

### 2.2 消息帧格式

**请求帧** (`IpcRequest`, 260 bytes):

```
┌──────────────┬──────────────┬─────────────────────────────────┐
│ command      │ paramLen     │ param[256]                       │
│ uint8_t      │ uint16_t     │ uint8_t[]                        │
│ 1 byte       │ 2 bytes      │ 256 bytes                        │
└──────────────┴──────────────┴─────────────────────────────────┘
```

**响应帧** (`IpcResponse`, 4100 bytes):

```
┌──────────────┬──────────────┬──────────────┬──────────────────┐
│ status       │ success      │ dataLen      │ data[4096]        │
│ uint8_t      │ bool (1B)    │ uint16_t     │ uint8_t[]         │
│ 1 byte       │ 1 byte       │ 2 bytes      │ 4096 bytes        │
└──────────────┴──────────────┴──────────────┴──────────────────┘
```

### 2.3 协议版本

| 版本 | 说明 |
|------|------|
| v1 | 初始版本 |
| v2 | 当前 public ABI。Legacy config 命令 `40-45` 保留枚举值但为 tombstone；connected config canonical path 是 v3 命令 `46-49`。 |

---

## 3. 命令参考

### 3.1 系统命令

#### Ping (0x00)

检测 Service 是否存活。

| 方向 | 字段 | 值 |
|------|------|-----|
| Req | command | `0x00` |
| Req | paramLen | `0` |
| Res | success | `true` |

---

#### EnterDebugMode (0x01)

App 请求 Service 开始推送调试帧到共享内存。

| 方向 | 字段 | 值 |
|------|------|-----|
| Req | command | `0x01` |
| Req | param | 共享内存名称 (UTF-16 wchar_t[]) |
| Req | paramLen | 名称字节数 |
| Res | success | `true` / `false` |
| Res | status (失败) | `InvalidState` (DeviceRuntime 未就绪) |

> :warning: 仅在 `_DEBUG` 构建中可用。Release 中直接返回失败。

---

#### ExitDebugMode (0x02)

App 请求 Service 停止推送调试帧。

| 方向 | 字段 | 值 |
|------|------|-----|
| Req | command | `0x02` |
| Req | paramLen | `0` |
| Res | success | `true` |

---

### 3.2 运行时控制

#### StartRuntime (0x0A)

启动 DeviceRuntime (硬件数据采集 + Pipeline 处理)。

| 方向 | 字段 | 值 |
|------|------|-----|
| Req | command | `0x0A` |
| Req | paramLen | `0` |
| Res | success | `true` (已启动或成功启动) |

> 幂等: 重复调用返回 success=true。

---

#### StopRuntime (0x0B)

停止 DeviceRuntime。

| 方向 | 字段 | 值 |
|------|------|-----|
| Req | command | `0x0B` |
| Req | paramLen | `0` |
| Res | success | `true` (已停止或成功停止) |

> 幂等: 重复调用返回 success=true。

---

### 3.3 AFE 命令

#### AfeCommand (0x14)

发送 AFE (Analog Front-End) 控制命令。

| 方向 | 字段 | 值 |
|------|------|-----|
| Req | command | `0x14` |
| Req | param[0] | AFE 命令码 (`uint8_t`) |
| Req | param[1] | 命令参数 (`uint8_t`) |
| Req | paramLen | `2` |
| Res | success | `true` / `false` |
| Res | status (失败) | `InvalidRequest` (paramLen < 2) |

---

### 3.4 VHF 控制

#### SetVhfEnabled (0x1E)

启用/禁用 VHF (Virtual HID Framework) 设备。

| 方向 | 字段 | 值 |
|------|------|-----|
| Req | command | `0x1E` |
| Req | param[0] | `0` = 禁用, 非零 = 启用 |
| Req | paramLen | `1` |
| Res | success | `true` / `false` |
| Res | status (失败) | `InvalidRequest` (paramLen < 1) / `InvalidState` (DeviceRuntime 未就绪) |

---

#### SetVhfTranspose (0x1F)

启用/禁用 VHF 坐标转置。

| 方向 | 字段 | 值 |
|------|------|-----|
| Req | command | `0x1F` |
| Req | param[0] | `0` = 禁用, 非零 = 启用 |
| Req | paramLen | `1` |
| Res | success | `true` / `false` |
| Res | status (失败) | `InvalidRequest` / `InvalidState` |

---

### 3.5 配置命令

当前 connected config 只支持 v3 paging / patch / persist。旧固定字段 ABI 仍保留命令枚举值以避免复用 wire value，但 `IsSupportedIpcCommand()` 对 `40-45` 返回 `false`，Service 应返回 `UnsupportedCommand`。

| Command | 十进制 | 十六进制 | 当前状态 |
|---------|--------|----------|----------|
| `ReloadConfig` | 40 | `0x28` | legacy config tombstone，unsupported |
| `SaveConfig` | 41 | `0x29` | legacy config tombstone，unsupported |
| `GetConfigSnapshot` | 42 | `0x2A` | legacy config tombstone，unsupported |
| `ApplyConfigPatch` | 43 | `0x2B` | legacy config tombstone，unsupported |
| `PersistConfig` | 44 | `0x2C` | legacy config tombstone，unsupported |
| `ApplyConfigTlvChunk` | 45 | `0x2D` | legacy config tombstone，unsupported |
| `GetConfigCatalogV3` | 46 | `0x2E` | v3 catalog canonical read path |
| `GetConfigSnapshotV3` | 47 | `0x2F` | v3 snapshot canonical read path |
| `ApplyConfigPatchV3` | 48 | `0x30` | v3 mutation canonical path |
| `PersistConfigV3` | 49 | `0x31` | v3 persist canonical path |

历史固定 payload 名称 `ConfigSnapshotWire`、`ApplyConfigPatchRequestWire`、`ConfigMutationResultWire`、`PersistConfigResponseWire`、`ConfigTlvChunkRequestWire`、`ReloadConfigSummaryWire` 不是当前 active public IPC payload。

#### GetConfigCatalogV3 (0x2E) / GetConfigSnapshotV3 (0x2F)

分页读取 config catalog 或当前 snapshot。请求使用 `IpcRequest::param` 携带 `ConfigV3PageRequestWire`；响应使用 `ConfigV3PageResponseHeaderWire` 后接 page bytes。

**ConfigV3PageRequestWire**:

```
Offset  Size  Field             Type      说明
------  ----  -----             ----      ----
0       2     wireVersion       uint16_t  必须为 2
2       1     payloadKind       uint8_t   1=Catalog, 2=Snapshot
3       1     flags             uint8_t   必须为 0
4       4     schemaVersion     uint32_t  App 已知 schema version，可为 0
8       4     snapshotVersion   uint32_t  App 已知 snapshot version，可为 0
12      4     offset            uint32_t  本页起始偏移
16      4     maxBytes          uint32_t  App 期望最大页大小，0 表示 Service 默认
20      4     reserved          uint32_t  必须为 0
```

**ConfigV3PageResponseHeaderWire**:

```
Offset  Size  Field             Type      说明
------  ----  -----             ----      ----
0       2     wireVersion       uint16_t  必须为 2
2       1     payloadKind       uint8_t   1=Catalog, 2=Snapshot
3       1     flags             uint8_t   当前必须为 0
4       2     headerBytes       uint16_t  sizeof(ConfigV3PageResponseHeaderWire)
6       2     pageBytes         uint16_t  本页 payload 字节数
8       4     totalBytes        uint32_t  完整 payload 字节数
12      4     schemaVersion     uint32_t  当前 schema version
16      4     snapshotVersion   uint32_t  当前 snapshot version
20      4     offset            uint32_t  本页起始偏移
24      4     checksum          uint32_t  完整 payload checksum
```

`ConfigV3PageCapacityBytes()` 等于 `4096 - sizeof(ConfigV3PageResponseHeaderWire)`。App 按 `offset + pageBytes < totalBytes` 继续请求后续页，拼接完成后校验 checksum，再反序列化 catalog/snapshot。

Catalog payload 来自 `ConfigRuntime::BuildCatalogV3Blob()`，包含每个可见配置键的 `ConfigKeyId`、YAML path、UI 类型、runtime binding、scope、apply timing、persist policy、默认值/当前值元数据。Snapshot payload 来自 `ConfigRuntime::BuildSnapshotV3Blob()`，包含当前 `ConfigStore` 中带静态 keyId 的值。

#### ApplyConfigPatchV3 (0x30)

应用 v3 `ConfigPatchTlv`。请求固定占用 `IpcRequest::param`，payload 最大 240 bytes。

**ApplyConfigPatchV3RequestWire**:

```
Offset  Size  Field                Type      说明
------  ----  -----                ----      ----
0       2     wireVersion          uint16_t  必须为 2
2       2     headerBytes          uint16_t  必须为 16
4       4     baseSchemaVersion    uint32_t  App 基于的 schema version
8       4     baseSnapshotVersion  uint32_t  App 基于的 snapshot version
12      2     payloadBytes         uint16_t  1..240
14      2     flags                uint16_t  必须为 0
16      240   bytes                uint8_t[] serialized ConfigPatchTlv
```

`ConfigPatchTlv` entry 使用静态 `ConfigKeyId` 定位 YAML path。当前支持的 `ConfigValueType` 为 `Bool=0`、`Int32=1`、`Float=2`、`String=3`、`Null=4`；connected v3 patch 不使用 legacy fieldMask。

**ConfigV3ApplyResultWire**:

```
Offset  Size  Field                 Type      说明
------  ----  -----                 ----      ----
0       2     wireVersion           uint16_t  2
2       1     status                uint8_t   ConfigV3MutationStatus
3       1     failedValueType       uint8_t   失败 entry 的 value type
4       2     changedCount          uint16_t  实际变更键数量
6       2     appliedCount          uint16_t  已 live apply 键数量
8       2     restartRequiredCount  uint16_t  已接受但需重启键数量
10      2     rejectedCount         uint16_t  被拒绝键数量
12      2     failedKeyId           uint16_t  首个失败 keyId；无失败时为 0
14      2     reserved              uint16_t  保留
```

**ConfigV3MutationStatus**:

| 值 | 枚举 | 语义 |
|----|------|------|
| 0 | `Ok` | patch accepted；可能包含 live apply 和 restart-required staged value |
| 1 | `NoChanges` | 请求合法但没有有效变更 |
| 2 | `VersionMismatch` | App 必须刷新 catalog/snapshot 后重试 |
| 3 | `Rejected` | schema、target 或 policy 语义拒绝 |
| 4 | `PersistFailed` | persist 失败，仅用于 persist response |

wire 格式错误返回 IPC failure，例如 `InvalidRequest`。语义拒绝返回 IPC success，同时 `ConfigV3ApplyResultWire.status = Rejected`。

#### PersistConfigV3 (0x31)

保留 wire 入口用于兼容旧 App/测试桩；当前 Service 不支持持久化配置文件。调用时返回 IPC failure `UnsupportedCommand`，不会写入磁盘。

**PersistConfigV3ResponseWire**:

```
Offset  Size  Field           Type      说明
------  ----  -----           ----      ----
0       2     wireVersion     uint16_t  2
2       1     status          uint8_t   ConfigV3MutationStatus
3       1     reserved        uint8_t   保留
4       2     persistedCount  uint16_t  当前固定为 0
6       2     skippedCount    uint16_t  当前不使用
8       2     failedCount     uint16_t  不支持持久化时为 1
```

当前规则：

- Service 不再提供配置持久化。
- `ApplyConfigPatchV3` 的修改只影响当前运行实例。
- `PersistConfigV3` 不创建、不读取、不写入任何配置文件。
- 旧客户端应把 `UnsupportedCommand` 视为“会话内动态调整已保留，持久化不可用”。

---

### 3.6 日志

#### GetLogs (0x32)

拉取 Service 端最近累积的日志行。

| 方向 | 字段 | 值 |
|------|------|-----|
| Req | command | `0x32` |
| Req | paramLen | `0` |
| Res | success | `true` |
| Res | data | UTF-8 文本行 (换行符分隔) |
| Res | dataLen | 实际日志字节数 (最大 4096) |

> 每次调用消费并清空缓冲区 (DrainNewLines 语义)。

---

### 3.7 诊断命令

#### GetDebugSchema (0x3D)

分页获取调试字段元数据 (字段名/类型/UI 分组等)。

| 方向 | 字段 | 值 |
|------|------|-----|
| Req | command | `0x3D` |
| Req | param | `DebugSchemaRequest` (4 bytes) |
| Req | paramLen | `sizeof(DebugSchemaRequest)` = 4 |
| Res | success | `true` |
| Res | data | `DebugSchemaResponseHeader` + N×`DebugFieldSchemaWire` |
| Res | dataLen | 实际数据长度 |

**DebugSchemaRequest** 布局 (4 bytes):

```
Offset  Size  Field    Type      说明
------  ----  -----    ----      ----
0       2     offset   uint16_t  起始索引
2       2     limit    uint16_t  最大返回数 (0 = 自动)
```

**DebugSchemaResponseHeader** 布局 (12 bytes):

```
Offset  Size  Field          Type      说明
------  ----  -----          ----      ----
0       2     schemaVersion  uint16_t  Schema 版本 (变更时递增)
2       2     totalFields    uint16_t  总字段数
4       2     returnedFields uint16_t  本次返回字段数
6       2     recordSize     uint16_t  单条记录大小 (96)
8       4     schemaHash     uint32_t  描述内容 hash
```

**DebugFieldSchemaWire** 布局 (96 bytes, 每条记录):

```
Offset  Size  Field          Type       说明
------  ----  -----          ----       ----
0       2     fieldId        uint16_t   字段 ID
2       1     valueType      uint8_t    值类型 (见 DebugValueType)
3       1     sourceKind     uint8_t    数据来源 (见 DebugSourceKind)
4       2     sourceIndex    int16_t    来源索引
6       1     uiOrder        uint8_t    UI 显示顺序
7       1     dvrTarget      uint8_t    DVR 目标
8       1     dvrPositionMode uint8_t   DVR 定位模式
9       1     _reserved0     uint8_t    保留
10      2     dvrIndex       int16_t    DVR 索引
12      2     _reserved1     int16_t    保留
14      32    key[32]        char[]     键名
46      48    displayName[48] char[]    显示名称
94      16    unit[16]       char[]     单位
110     24    uiGroup[24]    char[]     UI 分组
134     32    dvrColumnName[32] char[]  DVR 列名
166     32    dvrAnchor[32]  char[]     DVR 锚点
```

**DebugValueType**:

| 值 | 类型 |
|----|------|
| 0 | UInt32 |
| 1 | Int32 |
| 2 | Float32 |
| 3 | Bool |

**DebugSourceKind**:

| 值 | 说明 |
|----|------|
| 0 | MasterSuffixWord — Master 硬件后缀字 |
| 1 | SlaveSuffixWord — Slave 硬件后缀字 |
| 2 | StylusField — Stylus Pipeline 字段 |
| 3 | PenBridgeField — BT MCU 笔桥字段 |
| 4 | DerivedField — 派生计算字段 |

---

#### GetDebugSnapshot (0x3E)

获取一帧调试数据的快照值。

| 方向 | 字段 | 值 |
|------|------|-----|
| Req | command | `0x3E` |
| Req | paramLen | `0` |
| Res | success | `true` |
| Res | data | `DebugSnapshotHeader` + N×`DebugSnapshotValueWire` |
| Res | dataLen | 实际数据长度 |

**DebugSnapshotHeader** 布局 (8 bytes):

```
Offset  Size  Field          Type      说明
------  ----  -----          ----      ----
0       2     schemaVersion  uint16_t  Schema 版本 (必须与 GetDebugSchema 一致)
2       2     fieldCount     uint16_t  字段数
4       2     recordSize     uint16_t  单条记录大小 (16)
6       2     _reserved0     uint16_t  保留
```

**DebugSnapshotValueWire** 布局 (16 bytes, 每条记录):

```
Offset  Size  Field       Type      说明
------  ----  -----       ----      ----
0       2     fieldId     uint16_t  字段 ID (对应 schema)
2       1     valueType   uint8_t   值类型
3       1     flags       uint8_t   bit0: 值是否有效
4       4     _reserved0  uint32_t  保留
8       8     rawValue    uint64_t  原始值 (union 存储)
```

---

### 3.8 Pen Bridge 命令

#### GetPenBridgeStatus (0x3C)

查询 BT MCU 笔桥和压力传感器状态。

| 方向 | 字段 | 值 |
|------|------|-----|
| Req | command | `0x3C` |
| Req | paramLen | `0` |
| Res | success | `true` |
| Res | data | 24 bytes 二进制布局 |
| Res | dataLen | `24` |

**PenBridgeStatus** 布局 (24 bytes):

```
Offset  Size  Field         Type      说明
------  ----  -----         ----      ----
0       1     evtRunning    uint8_t   事件通道运行中 (0/1)
1       1     pressRunning  uint8_t   压力通道运行中 (0/1)
2       1     reportType    uint8_t   报告类型
3       1     freq1         uint8_t   频率通道 1
4       1     freq2         uint8_t   频率通道 2
5       8     press[4]      uint16_t[4] 缩放后压力值 ×4
13      1     pressureMode  uint8_t   压力范围模式 (0=4096, 1=16382/4)
14      2     pressureMax   uint16_t  最大压力值
16      8     rawPress[4]   uint16_t[4] 原始压力值 ×4
```

---

#### GetPenIdentityStatus (0x41)

查询当前手写笔身份状态。该命令不改变 `GetPenBridgeStatus` 的 24 bytes 固定布局。

| 方向 | 字段 | 值 |
|------|------|-----|
| Req | command | `0x41` |
| Req | paramLen | `0` |
| Res | success | `true` |
| Res | data | `PenIdentityStatusWire` |
| Res | dataLen | `400` |
| Res | status (失败) | `InvalidState` (DeviceRuntime 未就绪) |

**PenIdentityStatusWire** 布局 (400 bytes):

```
Offset  Size  Field                   Type       说明
------  ----  -----                   ----       ----
0       2     wireVersion             uint16_t   IPC 协议版本
2       1     flags                   uint8_t    bit0=stylusId 有效, bit1=modelId 有效, bit2=hardwareVersion 有效, bit3=connected, bit4=serialNumber 有效, bit5=firmwareVersion 有效
3       1     stylusId                uint8_t    PenTypeInfo payload[0]
4       4     penModuleModelId        uint32_t   PenModule 小端 modelId
8       2     hardwareVersionUtf8Len  uint16_t   UTF-8 字节长度, 不含 NUL
10      2     _reserved0              uint16_t   保留
12      128   hardwareVersionUtf8     char[128]  UTF-8 validated 字符串缓冲, 额外 NUL 仅供 C string 便利
140     2     serialNumberUtf8Len     uint16_t   UTF-8 字节长度, 不含 NUL
142     2     firmwareVersionUtf8Len   uint16_t   UTF-8 字节长度, 不含 NUL
144     128   serialNumberUtf8        char[128]  UTF-8 validated 字符串缓冲
272     128   firmwareVersionUtf8     char[128]  UTF-8 validated 字符串缓冲
```

字符串约束：`hardwareVersionUtf8Len` / `serialNumberUtf8Len` / `firmwareVersionUtf8Len` 是字节数，不是字符数；服务端按 UTF-8 边界截断，不会截断到多字节字符中间。

---

#### SetPenPressureMode (0x3F)

设置笔压力读取范围模式。

| 方向 | 字段 | 值 |
|------|------|-----|
| Req | command | `0x3F` |
| Req | param[0] | `0` = Raw12Bit4096, `1` = Raw14Bit16382 |
| Req | paramLen | `1` |
| Res | success | `true` / `false` |
| Res | status (失败) | `InvalidState` (PenPressureReader 未就绪) |

---

#### SetMasterParserOnly (0x40)

启用/禁用仅 Master Parser 模式 (调试用, 关闭下游 Pipeline 模块)。

| 方向 | 字段 | 值 |
|------|------|-----|
| Req | command | `0x40` |
| Req | param[0] | `0` = 正常, `1` = 仅 Master Parser |
| Req | paramLen | `1` |
| Res | success | `true` / `false` |
| Res | status (失败) | `InvalidState` (DeviceRuntime 未就绪) |

---

### 3.9 状态码参考

| 值 | 枚举 | 含义 |
|----|------|------|
| 0 | `Ok` | 成功 |
| 1 | `UnsupportedCommand` | 不支持的命令 |
| 2 | `InvalidRequest` | 请求参数无效 |
| 3 | `InvalidState` | 当前状态不允许此操作 (如 Runtime 未启动) |
| 4 | `NotFound` | 请求的资源不存在 |
| 5 | `PermissionDenied` | 权限不足 |
| 6 | `InternalError` | 内部错误 |

---

## 4. 共享内存 (实时帧推送)

### 4.1 SharedFrameBuffer

| 属性 | 值 |
|------|-----|
| 名称 | `Global\OpenEGoHubSharedFrame` |
| 布局 | `SharedTripleBuffer` (三缓冲) |
| ABI 版本 | 9 |
| 帧就绪事件 | `Global\EGoTouchFrameReady` (Windows Event) |
| 热图尺寸 | 40×60 (rows × cols) |
| 最大触控点数 | 10 |
| 最大 Peak 数 | 30 |

### 4.2 三缓冲读取协议

```
Writer (Service):                    Reader (App):
  1. 选取下一个 slot                  1. 读取 readyIdx
  2. slotSequence = odd (dirty)      2. 读取 slotFrameIds[readyIdx]
  3. 写入 SharedFrameData            3. 读取 slotSequences[readyIdx] → S1
  4. slotFrameIds[slot] = frameId    4. 拷贝 SharedFrameData
  5. slotSequences[slot] = even      5. 读取 slotSequences[readyIdx] → S2
  6. readyIdx = slot                 6. 读取 slotFrameIds[readyIdx]
  7. frameId++                       7. 若 S1 == S2 (均为偶数) 且
  8. SetEvent(frameReady)               slotFrameIds 与步骤2一致
                                      → 数据有效，使用拷贝
                                     否则 → 丢弃，等待下一次
```

### 4.3 SharedFrameData 关键字段

| 分类 | 字段 | 类型 | 说明 |
|------|------|------|------|
| **运行时状态** | workerState | int8_t | 工作线程状态 |
| | streaming | bool | 是否正在推送帧 |
| | lastFrameProcessUs | int64_t | 上帧处理耗时 (μs) |
| | avgFrameProcessUs | int64_t | 平均帧处理耗时 (μs) |
| | acquisitionFps | int32_t | Master FPS |
| | slaveAcquisitionFps | int32_t | Slave FPS |
| | vhfEnabled | bool | VHF 设备是否启用 |
| | vhfDeviceOpen | bool | VHF 设备是否打开 |
| **触控** | heatmapMatrix[40][60] | int16_t | 热图数据矩阵 |
| | contactCount | uint8_t | 触控点数 |
| | contacts[10] | SharedContact | 触控点详情 (id/x/y/state/area/sizeMm 等) |
| | peaks[30] | SharedPeak | 峰值点列表 |
| | touchPackets[2] | SharedTouchPacket | HID 触控报文 |
| **笔 (Stylus)** | stylusPoint | SharedStylusSolvePoint | 笔求解坐标 (x/y/pressure/tilt/confidence) |
| | stylusPacket | SharedStylusPacket | 笔 HID 报文 |
| | stylusStatus | uint32_t | 笔状态寄存器 |
| | stylusPressure | uint16_t | 笔压力值 |
| | stylusRawGrid | SharedStylusRawGrid | 笔原始网格 (TX1/TX2) |
| | diag | SharedStylusDiagnostics | 笔诊断详情 (100+ 字段) |
| **诊断** | masterSuffix | MasterSuffixView | Master 硬件后缀视图 |
| | slaveSuffix | SlaveSuffixView | Slave 硬件后缀视图 |

### 4.4 辅助事件

| 事件名 | 用途 |
|--------|------|
| `Global\EGoTouchLogReady` | Service 通知 App 有新日志 |
| `Global\OpenEGoHubPenStatusReady` | Service 通知 App 笔状态更新 |

---

## 5. ConfigDirtyFlag (共享内存)

`Global\EGoTouchConfigDirty` 是 legacy shared-memory dirty signal。它不再是 connected config mutation path；当前 connected App 必须通过 `ApplyConfigPatchV3` / `PersistConfigV3` 修改和持久化配置。

| 属性 | 值 |
|------|-----|
| 名称 | `Global\EGoTouchConfigDirty` |
| 大小 | 4 bytes (`std::atomic<uint32_t>`) |
| 安全 | Admin-only |

**协议**:

```
App (Writer):                       Service (Reader):
  1. 写入 legacy config             每帧调用 CheckAndClear()
  2. m_flag->store(1, release)      m_flag->exchange(0, acq_rel)
                                    若返回 1 → legacy reload path
```

> 仅支持 "脏/不脏" 布尔信号，不携带 keyId、schema version、apply result 或 persist result。v3 connected config 不依赖该机制。

---

## 6. 典型交互序列

### 6.1 App 启动 → 连接 Service

```
App                          Service
 │                              │
 │── Connect(5s timeout) ──────→│
 │←── Pipe connected ──────────│
 │                              │
 │── Ping ─────────────────────→│
 │←── success=true ────────────│
 │                              │
 │── GetConfigCatalogV3 ───────→│
 │←── ConfigV3 catalog pages ──│
 │                              │
 │── GetConfigSnapshotV3 ──────→│
 │←── ConfigV3 snapshot pages ─│
 │                              │
 │── EnterDebugMode ───────────→│
 │←── success=true ────────────│
 │                              │
 │  ◄══ SharedFrame 推送开始 ══│
```

### 6.2 用户修改配置

```
App                          Service
 │                              │
 │── ApplyConfigPatchV3 ───────→│
 │   keyId=SvcPenButtonMode     │
 │   value=NativeBarrel         │
 │←── ConfigV3ApplyResult ─────│
 │   changed=1, applied=1       │
 │                              │
 │── PersistConfigV3 ──────────→│
 │←── PersistConfigV3Response ─│
 │   persistedCount=N          │
```

### 6.3 拉取诊断快照

```
App                          Service
 │                              │
 │── GetDebugSchema ───────────→│
 │   offset=0, limit=0          │
 │←── SchemaResponse ──────────│
 │   totalFields=42             │
 │   schemaVersion=5            │
 │   42×DebugFieldSchemaWire    │
 │                              │
 │── GetDebugSnapshot ─────────→│
 │←── SnapshotHeader ──────────│
 │   42×SnapshotValueWire       │
 │                              │
 │── GetPenBridgeStatus ───────→│
 │←── 24 bytes PenBridgeStatus  │
 │                              │
 │── GetLogs ──────────────────→│
 │←── "2026-06-05 10:30:01..."  │
```

---

## 7. Config v3 TLV 与 KeyId 合约

### 7.1 Snapshot / Patch TLV

```
ConfigSnapshot TLV:
  [uint16_t] version
  [uint16_t] entryCount
  重复 N 次:
    [uint16_t] keyId       (ConfigKeyId enum)
    [uint8_t]  valueType   (0=bool, 1=int32, 2=float32, 3=string, 4=null)
    [uint16_t] valueLen
    [N bytes]  valuePayload

ConfigPatch TLV:
  同 Snapshot, 但仅包含变更键
```

### 7.2 ConfigKeyId 分区

| 范围 | 子系统 |
|------|--------|
| 0x0000-0x00FF | Service 配置 |
| 0x0100-0x01FF | Touch Pipeline |
| 0x0200-0x02FF | Stylus Pipeline |
| 0x0300 | `MaxKeyId` sentinel；不分配给普通键 |

`ConfigKeyId` 是 static IPC ABI，必须追加式分配，不能复用既有 ID。当前 v3 connected config 不使用 runtime dynamic key allocation。所有 patchable key 必须存在 `ConfigKeyMap` path/keyId 双向映射；`ConfigRuntimeTest` 和 App catalog/schema 测试覆盖该 gate。

---

## 8. 错误恢复

| 场景 | 行为 |
|------|------|
| Pipe 断开 | App 侧检测 `IsConnected()==false`, UI 显示断连状态 |
| Service 重启 | App 重新 Connect + Ping，重新拉取 `GetConfigCatalogV3` / `GetConfigSnapshotV3` |
| 请求超时 | `IpcPipeClient::Send()` 在 Pipe 层阻塞; 无应用层超时 |
| 并发请求 | `IpcPipeClient` 持有 `std::mutex`, 串行化所有 Send 调用 |
| 共享内存写入冲突 | Triple-buffer seqlock 保证读者不会读到半写数据 |
