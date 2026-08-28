# TSACore 真值对照

本文记录从厂商实现里取到的**事实**,用来替换移植过程中的猜测,记录的是
TSACore 原实现的逐条真值。凡本文与代码注释冲突,以本文为准——移植期的注释多处
断言了后来被证伪的东西。

## 取证环境

驱动服务 `HuaweiThpService`,安装在 `C:\Program Files\Huawei\HuaweiThpService\`:

| 文件 | 内容 |
|---|---|
| `TSACore.dll` | 算法核心,**x64**,1.7 MB,**3011 个带名字的导出**,符号未裁剪 |
| `TSAPrmt.dll` | 5 MB,逐机型参数 |
| `THP_Service.dll` | 服务层 |
| `himax_thp_drv.dll` | 芯片驱动 |

工具:Ghidra 12.1.3(`C:\Tools\ghidra_12.1.3_PUBLIC`),JDK 21 aarch64。全量分析 53 秒,
识别出 4305 个函数。脚本在 `C:\Tools\re\scripts\`:

- `DumpDecomp.java` —— 按名字反编译若干函数并跟随调用链到指定深度
- `FindFieldWriters.java` —— 全量反编译后按文本搜,用来找某个结构体字段的读写方

```powershell
$env:JAVA_HOME="C:\Users\rosetta\.jdks\jbr-jcef-21-aarch64"; $env:PATH="$env:JAVA_HOME\bin;$env:PATH"
& C:\Tools\ghidra_12.1.3_PUBLIC\support\analyzeHeadless.bat C:\Tools\re\proj TSA `
    -process TSACore.dll -noanalysis -scriptPath C:\Tools\re\scripts `
    -postScript DumpDecomp.java C:\Tools\re\out\x.c 2 <函数名>...
```

首次导入去掉 `-process ... -noanalysis`,改成 `-import C:\Tools\re\TSACore.dll`。

## 可驱动的 API

不是黑盒,是一套完整的库:

```
TSA_SetFrame → TSA_Processing / TSA_ProcessingAFEFrame / TSA_ProcessingExt
             → TSA_RptTouchNum / TSA_RptTouchDim1Center / Dim2Center
               / TSA_RptTouchEdgeFlag / Event / Age / AxisMajor / AxisMinor
               / AxisAngle / SizeInMM / XGripCtd / YGripCtd / ...
```

笔侧是 `ASA_*` 一族,参数加载 `ASA_LoadProjectPrmt` / `ASA_InitPrmt`。

这使得**逐级对拍**成为可能:同一份 `.dvrbin` 喂给两边,比中间量而不只是最终坐标。

## TSACore 已可作为可执行 oracle 运行

不再只能静态阅读。宿主在 `C:\Tools\re\oracle\`(x64,`vcvarsarm64_amd64` 交叉编)。

### 参数表的来源

TSACore **自己不带参数表**。`TSAPrmt_FlashPrmtLoadProject` 通过两个函数指针取表,
而这两个指针本身是**导出的数据符号**,宿主直接写进去即可,没有注册接口这一层:

```c
*(void **)GetProcAddress(core, "g_prmtGetVersion") = GetProcAddress(prmt, "TSAFlashPrmt_GetVersion");
*(void **)GetProcAddress(core, "g_prmtCallBack")   = GetProcAddress(prmt, "TSAFlashPrmt_LoadProject");
TSA_InitProject_UI("W273AS2700");   // 返回 1
```

回调填四张表,描述符是四组 `(指针, 长度)`,长度 4 字节占 8 字节槽:

| 槽 | 目标符号 | 长度 |
|---|---|---|
| +0x00 | `g_tsaPrmtFlash` | 0x1890 |
| +0x10 | `g_tsaPrmtFlashCmfPca` | 0x132c |
| +0x20 | 姿态表(宿主自行分配) | 0x3eb20 |
| +0x30 | `g_tsaPrmtFlashAsa` | 0x0e40 |

`TSAPrmt.dll` 里有 **454 个机型**,按名字枚举。筛 60×40 只剩四个 `W273AS*`;
移植期注释(`PeakDetector.hpp` 边缘峰过滤那段)写的是 **`W273AS2700`**,采信之。

### 本机参数表的关键值(机型 `W273AS2700`)

| 位置 | 值 | 含义 |
|---|---|---|
| `flash[0x00..0x09]` | `"W273AS2700"` | 工程名,可用来确认表确实加载了 |
| `flash[0x18]`,`[0x19]` | 1,1 | 相等 ⇒ **dim1 就是 x**,dim2 是 y |
| `flash[0x1d]`,`[0x1e]` | 60,40 | dim1N / dim2N |
| `flash[0x34]` | **3** | 全内嵌工艺 ⇒ 峰检测走 `Peak_Z8Filter` 那条分支 |
| `flash[0x5a6]` 半字 | 2560 | Dim1Res |
| `flash[0x5a8]` 半字 | 1600 | Dim2Res |
| `flash[0x5b6]` 半字 | 600 | 峰阈值钳位源,实际用的是它的一半 |

**信号阈值(`flash+0x5b4` 是被复用最多的一项)**:

| 偏移 | 值 | 消费点 |
|---|---:|---|
| `flash+0x5b4` (short) | **3000** | `TouchReport_ToBeReported` bit1 分支 ×0.5 = **1500**;`GripFilter_GetSafeTouchThold` 与 `IsSignalOK` ×0.4 = **1200** |
| `flash+0x5b6` (short) | 600 | 峰检测钳位,实际用 /2 = **300** |
| `flash+0x5b8` (short) | 3000 | 未确认 |
| `flash+0x5ba` (short) | 48 | 未确认 |
| `flash+0x5d8` (short) | 1000 | `TouchMode_GetStylusNum` 的 ×3 判据 |

消费点出自 `C:\Tools\re\out\SPEC_suppress.md`;数值是本机参数表的实际字节。
注意 `+0x1ba`(触点当前信号)与我们的 `signalSum` 是否同一个量**未确认**,
前者可能是峰值而非区内求和,数量级比较之前要先确认。

**边缘补偿 profile 表(四张,各 0x11 字节)**:

```
+0x840  01 07 00 10 e0    段数 1,尺寸阈 7,LUT 16..224
+0x851  01 07 00 10 e0    同上
+0x862  01 07 00 10 60    段数 1,尺寸阈 7,LUT 16..96
+0x873  01 07 00 10 60    同上
```

即近端两张与远端两张**上界不同**(224 对 96)。我们的 `g_defaultECProfiles[4]`
四份完全相同,至少后两份是错的。

`g_tsaPrmtFlashConst` 不由该回调填,是 `TSAConstPrmt_Init()` 算出来的,须完成
初始化后再读。**其元素宽度未确认**,所以 `[0x2a]`/`[0x2c]` 究竟是第几个字节还没定,
边界真值仍待取。

### 帧怎么喂

`TSA_ProcessingAFEFrame` 在 AFE 层之后只是把 `rows*cols*2` 字节拷进内部缓冲再调
`TSA_Processing`,所以宿主**可以整个跳过 AFE**,直接:

```c
TSA_Processing(int16Grid, /*ctrlFlags*/ 0, /*ctx*/ frameIndex);
```

**轴序已由单点探针实测定死:不转置。** 在录制矩阵 `[row][col]`(40 行 60 列,列走得快)
的 `row=8, col=45` 放一个信号,上报回 `dim1=45, dim2=8`。转置喂则一个接触点都检不出。

语料由 `DvrReplay --dump-frames` 摊平导出(见 commit `d8f5743`),取的是设备原始帧,
不是 `heatmapMatrix`——后者在录制链路上已被基线与 CMF 就地改写。

### 实测:TSACore 自己够得到边界

语料 `dvr20260824_012256`(拖到四边停住 + 划出去 + 边缘带内点击),10290 帧 1.7 秒跑完:

| 量 | TSACore | 我们的移植 |
|---|---|---|
| x 取值范围 | **3 .. 2559**(面板 0..2559) | 折算约 0.42 .. 59.58 格 |
| y 取值范围 | **3 .. 1599**(面板 0..1599) | 同上量级的内缩 |
| event | 2=down 25 次,4=move 5626 次,64=up 25 次 | —— |

25 次 down 对 25 次 up,**没有多余落下、没有漏抬起**。这既是边缘行为的对照基线,
也说明「够不到边」是移植的缺陷而非物理限制。

`edgeFlag` 远不止四个方位标志:实测出现过 `0x100`、`0x200`、`0x1001`、`0x2002`、
`0x9008`、`0xA00A` 等 38 种取值,是一个多字段位域。位定义未确认。

### 更正:grip 质心由 TSACore 内部算出

先前记的「`+0x196`/`+0x197` 在 TSACore 内部只读不写,须由调用方填入」**是错的**,
那是单一文本搜法没搜到写入方所致。实测:同一份语料 5676 条上报里
**3289 条 `TSA_RptTouchXGripCtd` / `YGripCtd` 非零**,且只在部分帧非零,说明写入有条件。

顺带一条:本机 `C:\Program Files\Huawei\HuaweiThpService\` 里**没有任何模块引用
TSACore**(导入表与字符串全查过),华为的 Windows 服务走的是 `THP_Service.dll` +
`himax_thp_drv.dll` 那条路。TSACore 是随包附带但未被该服务使用的算法库。这不影响
它作为真值参照,但意味着**没有一个现成的调用方可以抄调用时序**。

## 边缘补偿:四条已确认的真值

### 1. 四条边的判据严格对称,且按峰的格索引

```c
Peak_IsOnOuterEdgeDim1Near(p)  →  peak.dim1Index == 0
Peak_IsOnOuterEdgeDim1Far(p)   →  peak.dim1Index == g_tsaPrmtFlash[0x1d] - 1
Peak_IsOnInnerEdgeDim1Near(p)  →  peak.dim1Index == 1
Peak_IsOnInnerEdgeDim1Far(p)   →  peak.dim1Index == g_tsaPrmtFlash[0x1d] - 2
Peak_GetDim1DistToEdge(p)      →  min(index, N-1-index)
```

Dim2 同构,用 `g_tsaPrmtFlash[0x1e]`。**面板尺寸从参数表读**,不是硬编码常量。

移植缺陷:`TZ_GetCentroidEdgeFlags` 把索引比较换成了浮点质心比较,近端写 `col < min+1`、
远端写 `col > max`。质心是 0..N-1 号格的加权平均,到不了 N-1 以上,**右边和下边的补偿
从未触发过**。已修(commit `6a8d053`),远端改为 `col > max-1`。

### 2. 过渡曲线的两个常量是硬编码的

```c
int CTD_ECGetFinalOffset(int rawDist, int compOff) {
    int t = rawDist - 0x100;                 // 0x100 = 1 格 (Q8)
    if (t <= 0)    return compOff;           // 全量区:距边 ≤ 1.0 格
    if (t >= 0x40) return rawDist;           // 恒等区:距边 ≥ 1.25 格
    return (rawDist*t*4 + (0x100 - 4*t)*compOff) >> 8;   // 0.25 格线性过渡
}
```

即 **full = 1.0 格、blend = 0.25 格**。我们的 `ECGetFinalOffset` 公式与之逐句一致,
只有这两个常量不同,而且历史上被反向调过两次:先把 full 从 0.5 收到 0.08(为修
「边缘坐标冻住」),再把 blend 从 0.505 放到 2.0(为把补偿够回来)。

**真值现在还不能直接用**,原因见下一条。

### 3'. 边缘补偿的完整算法(已可直接实现)

原先卡在 `+0x196`/`+0x197` 的语义上,现已查清。**它不是质心位置,是一个信号比值。**

**第一步:grip 比例**(来源:`TZ_UpdateEdgeInfo`、`TZ_CentroidGripRatio`、
`TZ_GetGripRatio`、`TZ_RegisterTouch`)

逐点扫描接触点覆盖的像素时,按坐标累加四个信号和:

| 累加器 | 条件 |
|---|---|
| `zone+0x2cc` | dim1 坐标 == `flash[0x90]` 或 `flash[0x91]` |
| `zone+0x2d0` | dim1 坐标 == `flash[0x90]+1` 或 `flash[0x91]-1` |
| `zone+0x2d4` | dim2 坐标 == `flash[0x92]` 或 `flash[0x93]` |
| `zone+0x2d8` | dim2 坐标 == `flash[0x92]+1` 或 `flash[0x93]-1` |

本机 `flash[0x90..0x93] = {0, 59, 0, 39}`,即**最外一圈**与**次外一圈**。

```c
ratio = (outer == 0 && inner == 0) ? 0
      : (inner == 0)               ? 0xffff       // 只有最外圈有信号
      : (outer == 0)               ? 0
      :                              (outer << 4) / inner;
gripRatio = min(ratio, 0xfe) 之后存成一个字节,超过 0xfe 记 0xff
```

即 **16 × 最外一圈信号和 ÷ 次外一圈信号和**。接触点被边界切得越深,最外一圈相对
次外一圈越强,比值越大。

实测印证(语料 `dvr20260824_012256`,按质心到最近边的格距分桶):

| 距边 | 样本 | 非零占比 | 中位 | 最大 |
|---:|---:|---:|---:|---:|
| 0 格 | 2880 | 100% | 108 | 255 |
| 1 格 | 284 | 100% | 7 | 20 |
| 2 格 | 210 | 45% | 0 | 7 |
| ≥3 格 | —— | 0% | 0 | 0 |

**第二步:比例映射成亚格偏移**(来源:`CTD_ECGetOffset`)

```c
// profile 表:{段数, 尺寸阈, ?, lutLo, lutHi},本机近端 {1, 7, 0, 16, 224}
seg = 0; while (seg < count-1 && profile[seg*4+1] < sizeMm) seg++;
lo = profile[seg*4+3];  hi = profile[seg*4+4];
off = (ln256[gripRatio] - ln256[lo]) * 0x100 / (ln256[hi] - ln256[lo]);
if (off > 0xff) off = 0xff;        // 只钳上界,下界不钳,off 可以为负
```

`g_ctd256Ln` 是一张 256 项 u16 表,实测 **`ln256[i] = round(256 · ln i)`**,`[0]=[1]=0`
(第 2 项 177 = 256·ln2,第 3 项 281 = 256·ln3,逐项吻合)。不必搬表,直接算。

于是 `off = 256 · ln(ratio/lo) / ln(hi/lo)`,本机 `ln(224/16) = ln 14`。

**第三步:落到坐标**(来源:`CTD_ECProcessDim1`、`CTD_ECGetFinalOffset`)

```c
// 近端(触点 flags & 1)
rawDist = coord - bound_near;                    // Q8,bound_near = flashConst[0x2a]
fin     = CTD_ECGetFinalOffset(rawDist, 0x100 - off);
coord   = bound_near + fin;

// 远端(触点 flags & 2)
rawDist = bound_far - coord;                     // bound_far = flashConst[0x2c]
fin     = CTD_ECGetFinalOffset(rawDist, 0x100 - off);
coord   = bound_far - fin;
```

`compOff = 0x100 - off`,`off ∈ (-∞, 255]`,所以 `compOff ∈ [1, +∞)` Q8,即
**≥ 0.004 格**。比例拉满时接触点被放到距边界 1/256 格处;比例最小时放到 1.0 格处。

**这就是「够不到边」的根因**:补偿量本该由 grip 比例决定,取值可以一直逼近边界;
我们的移植喂的是次轴归一化位置,在全量区里恒为常量,于是坐标停在固定的内缩位置。

**边界值**(`flashConst[0x2a]` / `[0x2c]`,按 `ushort` 读)本机为 **0 与 15360**,
Q8 折算即 **0.0 格与 60.0 格**。dim2 同构。这说明坐标空间是「面板跨 0..60 格」,
即格 `i` 的中心在 `i + 0.5`,近端与远端因而是对称的——我们的 `EdgeBounds{0,60,0,40}`
数值上正确,先前记的「两侧不对称」是误判。

### 3''. 补偿后的坐标是**另一个字段**,跟踪级看不到它

触点结构里有两个坐标:

| 偏移 | 内容 | 写入方 | 读取方 |
|---|---|---|---|
| `+0x15e` | 原始质心 | `TZ_RegisterTouch` | `Peak_IDTracking`、`IDT_GetAcc`、`IDT_IsEdgeTouch` |
| `+0x162` | 边缘补偿后的坐标 | `CTD_ECProcessDim1/2` | `Filter_*`、`Touch_CreatePostReport`、`TE_TouchDown*`、`TZ_GetMove` |

即**跟踪与 ID 关联一律用原始质心,补偿只作用于上报**(来源:对这两个偏移做全量
反编译文本扫描,78 处命中)。

**新架构里,边缘补偿属于上报语义层,不能改写跟踪级读的那份坐标。**

按这个结构改完之后**实测没有任何可测变化**:所有语料的断开数、笔画数、位置残差
逐位相同。此处原先写着「混用的代价是多出九次中途断触」,那是笔画计数把 id 回收
当成断触算出来的假象,已订正。保留这条约束的理由是它有
二进制证据且结构正确,不是因为它修好了什么可观测的东西。

### 3. LUT 索引是被补偿轴自己的 grip 质心,不是次轴位置(此节已被 3' 取代)

```c
CTD_ECProcessDim1 → CTD_ECGetOffset(touch[+0x196], touch[+0x194])
CTD_ECProcessDim2 → CTD_ECGetOffset(touch[+0x197], touch[+0x194])
```

- `+0x194` = 接触点尺寸(mm),由 `TS_UpdateCurSizeInMM` 写,`TSA_RptTouchSizeInMM` 读出。
  我们喂的 `touchSizeMm` **正确**。
- `+0x196` / `+0x197` = dim1 / dim2 的 **grip 质心**(`TSA_RptTouchXGripCtd` /
  `YGripCtd` 读出,按 `g_tsaPrmtFlash[0x18]` 与 `[0x19]` 是否相等决定轴序)。

移植缺陷:`ECDimResult` 计算处把 `subIdx` 算成**次轴归一化位置**,注释还写着
「`param_1` 是质心在垂直于补偿方向上的传感器索引」——这个假设是错的。

后果:原实现里手指往边缘陷得越深,索引越变,`compOff` 随之变化,所以「全量区」
的输出并非常量;我们喂的量沿被补偿轴移动时不变,全量区就真成了常量。实测把常量
换成真值之后,边缘 1.5 格内的不同取值从 167 塌到 47,即坐标在边缘冻住。

**所以顺序是先修 subIdx,再换常量。**

此处原先记着「`+0x196`/`+0x197` 在 TSACore 内部只被读、从不被写,由调用方填入」——
**该结论已被实测推翻**,见上文「更正:grip 质心由 TSACore 内部算出」。当时的扫描
只搜了一种文本写法,漏掉了写入方。确切算法仍待取,但取证范围回到 TSACore 内部。

### 4. 近端与远端用不同的 profile 表,且边界来自参数表

```c
// CTD_ECProcessDim1，近端分支
*(undefined8 *)_refptr_g_tsaPrmtRam = *(undefined8 *)(_refptr_g_tsaPrmtFlash + 0x840);
// 远端分支
*(undefined8 *)_refptr_g_tsaPrmtRam = *(undefined8 *)(_refptr_g_tsaPrmtFlash + 0x851);
```

近端 profile 在 flash+0x840、远端在 +0x851,各 0x11 = 17 字节;`g_tsaPrmtRam[0]` 是
分段数,`[i*4+1]` 是尺寸阈值,`[i*4+3]`/`[i*4+4]` 是 LUT 上下索引。

我们的 `g_defaultECProfiles[4]` 四份**完全相同**,是占位符而非真表。

边界取自 `g_tsaPrmtFlashConst[0x2a]`(近端)与 `[0x2c]`(远端),即**实测/配置边界**。
对应我们 `ZoneExpander::m_edgeBounds` 被读但从不被写、恒为标称 `{0,60,0,40}` 的问题。
注意 `EdgeBounds` 默认 `colMax = 60` 而格索引只到 59,近端边界与 0 号格重合、远端却
差一格,两侧本身也不对称。

## 性能:转译代价实测

把**我们自己的触摸管线**同源分别编成 ARM64 与 x64,同机跑 11 接触点满负荷帧
(基准源码在会话 scratchpad,未入库;重建见下):

| 目标 | 每帧 | 占 122 Hz 预算(8197 µs) |
|---|---:|---:|
| ARM64 原生 | 51.9 µs | 0.63% |
| x64 转译 | 66.6 µs | 0.81% |

**转译代价 1.28 倍,绝对值两个数量级富余。** 比常说的 2 倍好,因为这类整数规则循环
转译器容易处理,且两边都拿到了 SIMD(NEON / SSE)。测的是稳态,转译缓存已预热。

结论:**性能不构成选型理由**。无论自研还是调 TSACore 都跑得动,决策依据只剩功耗、
x64 依赖、正确性三条。注意这测的是 CPU 时间不是能量,功耗要另设长跑实验。

## 参数表里已读出的其余字节(机型 `W273AS2700`)

| 偏移 | 宽度 | 值 | 含义与出处 |
|---|---|---:|---|
| `flash+0x2c` | short | 2400 | 差分图长度,= 60×40(`SPEC_baseline` 一) |
| `flash+0x38` | byte | 100 | 名义上报速率,周期 `1000/100 = 10 ms`。**与实测的 8332 µs(120 Hz)不符**,未查(`SPEC_track2` 批次 3) |
| `flash+0x68` | byte | 0 | `Rawdata_CMF` 替代路径关闭,走主干 CMF(`SPEC_baseline` 一) |
| `flash+0x836` | byte | 30 | `TZ_IsLO` 的宽度阈值,其平方 900 是面积阈值,见下 |
| `flash+0x83c` | byte | 0 | bit3 未置位 ⇒ `TZ_UnitToTouch` 走类型分派分支;bit7/bit8 未置位 ⇒ 区域生长走 `TZ_NormalTraversal` |
| `flash+0x90..0x93` | byte×4 | 0,59,0,39 | 边缘带的最外一圈索引(`SPEC_grip` 零) |

## LO 是「长条形对象」,不是「过小的对象」

掌语料上厂商整帧丢弃的那条路由 `touches[0]+3`(LO 计数)驱动,而这个 LO 的判据是
`TZ_IsLO`。两个 `isSSLo*` 全局标志为 0 时(正常场景),它简化成:

> `exception < 9` 且 `面积 > DB_LOSize()` 且(`宽度 ≤ flash[0x836]` 或 `形状比 > 5`)⇒ LO

```c
ushort DB_LOSize(void) {
    size = flash[0x836] * flash[0x836];               // 30 * 30 = 900
    if (prevTouches[0][3] != 0) size = size * 3 / 4;  // 上一帧有 LO 则降到 675 —— 迟滞
    return size;
}
```

**面积大、宽度窄**——细长条。`DB_LOSize` 里的 `3/4` 是迟滞:一旦认定有长条物体压着,
就倾向于持续丢弃。

> **`TZ_IsLO` 已被完全排除,以上这条判据与实际发生的事无关。**
>
> 在 LO 确实触发的 40 帧样本上逐帧读:
>
> | `TZ_IsLO` 的出口 | 需要什么 | 实测 |
> |---|---|---|
> | 外层 `else { return 1; }` | `isSSLoSD` 或 `isSSLo` 非零 | **两者恒为 0** |
> | `DB_LOSize() < area` 之后的两条 `return 1` | `area > 900` | **40 帧里 39 帧是 104..135** |
>
> (第 40 帧是 55×37 = 2035,确实超过 900。所以面积那条分支不是「不可达」,是
> 「39/40 走不到」;`TZ_IsLO` 解释不了另外 39 帧,方向不变。)
>
> `g_subTZ` 是**逐帧刷新**的,不是陈旧内存:相邻样本之间有 17 到 39 个字节不同,
> 那两个字段在 40 帧里取过 7 种值。所以上面这些数字有效。
>
> **三条路在绝大多数帧上都通向 `return 0`。** 而真正驱动整帧清零的是
> `zone+4` 这个标志字恰好等于 5,与 `TZ_IsLO` 无关——见上面「链路与后果」。
> `TZ_IsLO` 这条线到此完全排除。
>
> 但类型 5 本身是**真实发生的**:在 LO 触发的 40 帧上逐槽位读 `TSA_RptTouchShape`
> (`touch + 0x1e0`,按下标直接读结构,上报数为 0 也能读),
> **`loNum` 恰好等于 shape == 5 的接触点个数,40 帧里 37 帧完全相等**
> (3 例不符是只读了前 8 个槽位而触点最多 20 个)。所以链条的后三段都成立,
> 缺的只是「谁把类型写成 5」。
>
> 取证侧已把 `TZ_GetType` 整条分类级联逐个函数确认过返回值域,没有一个能产出 5。
> 写入方在那棵调用树之外,**尚未找到**。另一条旁证:`DB_TouchDownNum` 与 `DB_LiftOffNum`
> 都对该字段的取值 2 和 3 有分支,**唯独没有 5**,说明 5 是较晚加入的类型。
>
> 「掌会被分成细长区、所以与多区相关」这个说法**没有证据支撑**,已撤回。
> 相关性本身是实测的(LO 帧的区数集中在 4–5),但把它归因为「细长」是推测。
> 连 LO 这个名字对应「长条」也只是命名推测,判据本身还没找到。

另一处未能区分:`g_subTZ` 的地址是 `0x6bbc7680`,取证侧从字段布局反出的数组起点
`0x6bbc7688` 正好差 8 字节,支持「前 8 字节是头部」的假设。但所有采样帧的子区下标
都是 0,此时「基址 `g_subTZ` + 0x38」与「基址 `g_subTZ+8` + 0x30」**是同一个地址**,
这个实验区分不了两种读法。要区分需要一帧下标非 0 的样本。

面积取自 `g_subTZ[g_subTZ[6]]` 的 `+0x38`(宽)× `+0x3a`(高),**不是**传入那个区
自己的宽高字段(`flash[0x83c] & 0x10` 为 0 时走这条,本机 `flash[0x83c] = 0`)。
`+0x38`/`+0x3a` 的量纲**未确认**——阈值 900 与 30 的物理含义完全取决于它。
若单位是 0.1 mm,那就是「宽 ≤ 3 mm、面积 > 9 mm²」。

出处:`TZ_IsLO`、`DB_LOSize`,反编译产物 `C:\Tools\re\out\lonum.c`。

### 链路与后果(已完整坐实)

```
zone+4 这个标志字在分类过程中累积到恰好等于 5(bit0 与 bit2 置位)
  → TZ_RegisterTouch 原样拷进 touch+0x1e0   (lVar4+0xa0,lVar4 = touchesPtr+i*0x168+0x140)
  → TZ_UpdateTouchFlag 见到它打 touch+0x1f0 |= 0x2000
  → TouchReport_ToBeReported 计入 touches[0]+3 并排除该触点
  → TSAOut_PostProcess 见 +3 非零,把整帧上报数写成 0
```

**实测完全相关,一个例外都没有**(掌语料 40 个 LO 帧、320 个槽位):

| `touch+0x1e0` 的值 | 带 `0x2000` 位 | 不带 |
|---:|---:|---:|
| 0 / 1 / 2 / 3 | **0** | 237 |
| **5** | **83** | **0** |

**注意「谁写的 5」这个问题不存在**:没有人赋值 5,是 `zone+4` 的两个独立标志位凑到一起
恰好等于 5。这也解释了为什么把 `TZ_GetType` 整条分类级联翻遍都找不到能产出 5 的返回值
——它根本不在那条链上。`touch+0x1e0` 拷贝的源头是 **`zone+4`(标志字)**,不是
`zone+8`(类型枚举),两者数值上都出现过 5,容易混。

`TSA_RptTouchShape` 读的就是 `touch+0x1e0`,所以它返回的「5」是标志字的值,不是形状分类。
`DB_TouchDownNum` 里对该字段 `== 2` / `== 3` 的判断同理,读作「标志字恰好等于 2 / 3」。

**唯一剩下的未知量:`zone+4` 的 bit0 与 bit2 各自是什么。** 这一条不影响设计决策
(我们本来就不照抄整帧否决),等真要实现这一级时再查更有针对性。

**后果是整帧连坐,不是只丢那个长条。** 实测 783 个清零帧里:

| 清零前的触点数 | 帧数 |
|---:|---:|
| 2 个及以上 | **771**(98.5%) |
| 其中 LO 计数只有 1 的 | **669** |

即**一个细长对象把同帧另外三四个正常触点一起带走**。语义上是「屏上有长条 ⇒ 这一帧
整体作废」,接近「手臂搭在屏上时整块屏拒绝输入」。

**这一条属于不该照抄的部分**(见第六节的分歧表):我们应当做成「长条本身不上报,
同帧其他接触点照常」。

## 按下确认靠信号门槛,不靠等帧数

这是掌语料里另外那 661 帧的完整机制。

`DB_TouchDownTholdCheck` 的判据(来源:同名函数):

```c
thold = *(short *)(g_tsaPrmtDynamic + 0xc);          // 字节偏移,即 short 下标 6
if (TouchThold_ToeUseSpecialThold() && 峰落在边界行/列)
    thold = *(short *)(g_tsaPrmtDynamic + 0xe);      // 边界上用另一个,更低
peakSignal = peaks[touch[+0x23c]].signal(+0x18);
if (peakSignal < thold) touch[+0x1f4] |= 0x20;
return peakSignal >= thold;
```

`DB_TouchDownNum` 里 `TholdCheck` 返回假就 `return 0xffff`,**即永远确认不了按下**,
事件码停在 `1`,`TouchReport_ToBeReported` 据此把它排除,于是一直不上报。

**实测取值:`+0xc` = 600(高负载时降到 300 / 220),`+0xe` = 420。**
`+0xc` 是字节偏移不是下标——代码写的是 `*(short *)(base + 0xc)`。

`+0xe` 那条**本机不生效**:`TouchThold_ToeUseSpecialThold()` 是按机型名前缀匹配
`P116` / `P150` / `P186`,我们是 `W273AS2700`,返回 0。

但它的语义值得记下来:那三个机型**在边界上用更低的门槛**(420 对 600),即边界上的接触点
更**容易**确认按下。我们移植里在边界用**更高**的阈值(300 对 280),方向相反。厂商没在所有
机型上开这条,但方向是清楚的,重写时值得重新考虑。

### `g_tsaPrmtDynamic` 一律按字节偏移读

反编译里对它的访问全是「裸指针 + 整数常量再解引用」,常量就是字节偏移;按 short 下标
理解会整体错一倍(这个错我犯过一次,把 `+0x10` 当成了峰阈值)。实测(掌语料 10321 帧):

| 字节偏移 | 宽度 | 取值 | 用途 |
|---|---|---|---|
| `+0x04` | short | 3000(恒定) | 与 `flash[0x5b4]` 同值 |
| `+0x06` | byte | 48(恒定) | `DB_GetNumBySignalSum` |
| `+0x08` | short | **165 .. 600**,随噪声变 | **峰检测阈值**,`TSAStatic_GetSigThold(0)`,再钳到 300 |
| `+0x0C` | short | **600** / 300 / 220 | **按下确认阈值** |
| `+0x0E` | short | 420 / 250 / 220 | 边界特例,本机不生效 |
| `+0x10` | short | 1800 / 900 / 660 | 未确认 |
| `+0x14` | int | 1800(恒定) | `DB_GetNumBySignalSum` |

整张表**每帧由 `TSADynPrmt_Process` 重算**:实测从外部改写任何一项(包括已知在用的峰阈值)
都不产生任何行为变化。这条否掉了「从宿主侧调这张表做实验」的路子。

**基础防抖帧数正常情况下是 0**:`DB_TouchDownNum` 的初值是
`(本帧峰数 − 上帧峰数) > 1 ? 1 : 0`,只有若干异常位命中时才升到 2 或 3。
**厂商不是靠等帧数把弱接触点滤掉的,是靠信号门槛。**

### 这个数能不能直接搬

比较对象是**峰的原始信号**,不是区内信号总和——我们现在的
`m_touchDownRejectMinSignal`(55)比的是总和,不是同一个量。

两边差分图的量纲**同阶但不等价**(掌语料 6283 个可比帧):

| | 中位 | p95 |
|---|---:|---:|
| 厂商 `diffMax` | 1067 | 2570 |
| 我们 `hmMax` | 1819 | 2574 |

p95 几乎相同,说明强按压的峰值在两边是一回事;中位数我们明显偏高,逐帧比值
p5 0.23、p95 2.10,散得很开。所以 600 这个数**量纲上可搬,数值上要重新标定**。

**没有实现。** 这条直接拿掌的少报去换正常点击的响应,取舍只有真机手感能定。

## 待办

- [x] flash 参数表的实际内容 —— 已取,见上;四张 EC profile 已读出
- [x] 对拍工具的基础形态 —— `C:\Tools\re\oracle\oracle.exe`,已能逐帧输出接触点
- [ ] `+0x196`/`+0x197` grip 质心的算法(写入方在 TSACore 内部,尚未定位)
- [ ] `edgeFlag` 的位域定义(实测 38 种取值)
- [ ] `g_tsaPrmtFlashConst` 的元素宽度,进而取到边界真值
- [ ] 逐级中间量 dump:`TSA_RptRawPtr` / `RptDiffPtr` / `RptBaselinePtr` /
      `RptPreCMFRawPtr` / `RptTZFlagPtr` / `RptSDPtr` 都是导出的,接上即可按级对拍
- [ ] 与我们自己的重放结果做逐帧比对的脚本
