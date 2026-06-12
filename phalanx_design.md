# Phalanx — 设计文档

> 最后更新：2026-06-10
> 引擎：UE5.6 | 语言：C++ | Sprint：10天（6-15 ~ 6-24）
> 刻意练习模式：代码全由本人写，AI 只引导 / review / 考核

---

## 定位与叙事

**一句话：** 在 UE5 里找到确定性 Agent 仿真的天花板，并把这个过程做成任何人都愿意看的画面。

**工程成就（给技术面试官看）：**
- 10,000+ 单位，每个都有真实状态（位置 / 血量 / 行为 / 目标）
- 固定步长 SimClock 完全独立于 UE 渲染帧——RTS 确定性 / lockstep 的正确前提
- Bit-identical 确定性 replay，CI 强制验证
- 自制 SpatialGrid O(1) 查询，不依赖 UE PhysicsScene

**视觉成就（让任何人都停下来看）：**
- 两支万人军队碰撞的画面，UE5 Lumen 光照下有体积感和重量感
- 性能数据实时覆盖层："10,247 units · 9.1ms sim tick" 作为画面的一部分而不是道歉
- Replay 模式：完全相同的战局重演，作为视频的结尾 beat

**Blackbird JD 逐词对应：**

| JD 关键词 | Phalanx 实现 |
|---|---|
| `unit behaviors / movement / combat` | SteeringProcessor + CombatProcessor |
| `determinism / lockstep / replay` | SimClock（固定步长）+ ReplaySystem（bit-identical）|
| `simulation and systems architecture` | 仿真层 / 视觉层两层解耦架构 |
| `performance and stability / desync risk` | SpatialGrid + BenchmarkMode + CI PerfGuard |

---

## 核心架构：两层完全解耦

```
┌─────────────────────────────────────────────────────┐
│  仿真层 — CPU — 确定性                               │
│                                                     │
│  SimClock (62.5Hz 固定步长)                          │
│      │                                              │
│      ├──► Mass Entity Manager (SoA Archetype)       │
│      │        ├──► SteeringProcessor                │
│      │        │        └──► SpatialGrid (O(1))      │
│      │        └──► CombatProcessor                  │
│      │                 └──► SpatialGrid (O(1))      │
│      └──► ReplaySystem (输入磁带)                   │
└─────────────────────────────────────────────────────┘
         │ 每渲染帧读取位置（只读，不写）
         ▼
┌─────────────────────────────────────────────────────┐
│  视觉层 — GPU — 可传播                               │
│                                                     │
│  SimToVisualBridge (插值 + 批量写 ISM)               │
│      ├──► UInstancedStaticMesh (两队 GPU Instancing) │
│      │        └──► UE5 Lumen (动态光照 / 长影)       │
│      └──► PerformanceHUD (Units / SimMs / RenderMs) │
└─────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────┐
│  验证层                                              │
│  BenchmarkMode (headless, Units vs ms → CSV)        │
│  DeterminismTest (两次跑同场景，断言 bit-identical)   │
│  PerfGuardTest (CI: 5K units ≤ 8ms)                │
└─────────────────────────────────────────────────────┘
```

**两层解耦的关键意义：** 仿真层以 16ms 固定步长运行，渲染帧可以是任意频率。SimBridge 每帧从仿真层读取最新位置并插值更新 ISM——视觉丝滑，仿真确定。这是 Homeworld、StarCraft 等 RTS 确定性网络同步的工程前提。

---

## 数据设计

### 仿真层 Fragment（SoA，cache-friendly）

| Fragment | 字段 | 说明 |
|---|---|---|
| `FSimPositionFragment` | `int32 X, Y` | Q16.16 定点整数，与 cpp_core Q16 oracle 同族 |
| `FSimVelocityFragment` | `int32 VX, VY` | 定点速度，低 16 位为小数 |
| `FHealthFragment` | `int32 HP, MaxHP` | 纯整数，无浮点百分比 |
| `FTeamFragment` | `uint8 TeamID` | 0 = 红方，1 = 蓝方 |
| `FBehaviorFragment` | `FMassEntityHandle TargetHandle` | 当前攻击目标，Invalid = 空闲 |
| `FDeadTag` | （无数据）| 死亡后添加，批量销毁 |

**为什么仿真侧全程整数：** 浮点 FMA 指令和 denormal 刷新策略在不同 CPU 上结果可能不一致。整数运算在任意机器上 bit-identical，这是确定性 replay 的硬性前提。

**定点数值域：** Q16.16 最大表示 ±32,767 个仿真单位（约 ±3.2km），地图尺寸保持在此范围内。速度最小分辨率约 0.015 UU/tick，足够精度。

### SpatialGrid 设计

- 均匀网格，`cellSize = max(攻击范围, 感知范围)`，通常 200-300 UU
- 每格预分配 `TArray<FMassEntityHandle>`，避免运行时分配
- `cellIndex = (x / cellSize) + (y / cellSize) * gridWidth`
- `Insert / Remove / QueryRadius` 全部 O(1)
- 不依赖 UE PhysicsScene：确定性可控，benchmark 可复现

---

## 客户端架构层（框架，非内容）

**原则：用 UE5 本身的架构机制，接缝留对，内容填空。不另造框架。**

```
UPhalanxGameInstance
├── UPhalanxSimSubsystem      ← SimClock 的宿主（Phase 1 实现）
├── UPhalanxUISubsystem       ← 管理 Widget 栈的统一入口（Phase 1 建桩）
│   └── ShowWidget / HideWidget / GetActiveWidget
└── UPhalanxAudioSubsystem    ← 包装 UAudioComponent 调用（Phase 1 建桩）
    └── PlaySound / StopAll / SetMusicTrack

APhalanxPlayerController
└── Enhanced Input 已配置     ← InputMappingContext + InputAction 资产（Phase 1）
    今后加新命令只加 IA 资产，不动 C++

UPhalanxBaseWidget（UMG 基类）
├── UBattleHUD                ← PerformanceHUD + Replay 控制（Phase 5 实现）
└── UBattleConfigWidget       ← 单位数 / 队伍配置滑条（Phase 5 实现）
```

**Phase 1 只建桩：** UISubsystem 和 AudioSubsystem 的 `.h` 定义接口，`.cpp` 方法体为空或单行日志。Enhanced Input 的 InputMappingContext 和 InputAction 资产建好但绑定空 Lambda。这半天的投入让整个项目后续不需要架构级重构。

---

## 模块接口契约（C++ 草图，Phase 1 前锁定）

接口在实现之前定义，防止模块间隐式耦合。

```cpp
// === SimClock（UGameInstanceSubsystem 宿主） ===
DECLARE_MULTICAST_DELEGATE_OneParam(FOnSimTick, int32 /*TickIndex*/);

class UPhalanxSimSubsystem : public UGameInstanceSubsystem
{
public:
    FOnSimTick OnSimTick;               // 仿真层所有 Processor 订阅此委托
    int32  GetCurrentTick() const;
    float  GetInterpolationAlpha() const; // 视觉层插值用：[0,1) 当前渲染帧在两仿真步之间的位置
    void   SetPaused(bool bPaused);
    void   SetTimeScale(float Scale);   // Replay 倍速控制
};

// === SpatialGrid（纯 C++，不依赖 UE） ===
template<int32 CellSize>
class TPhalanxSpatialGrid
{
public:
    void Insert(FMassEntityHandle Handle, int32 WorldX, int32 WorldY);
    void Remove(FMassEntityHandle Handle, int32 WorldX, int32 WorldY);
    void Move  (FMassEntityHandle Handle, int32 OldX, int32 OldY, int32 NewX, int32 NewY);
    // 返回值为栈上临时 span，调用方不得跨 tick 持有
    TConstArrayView<FMassEntityHandle> QueryRadius(int32 CX, int32 CY, int32 Radius) const;
    void Clear();
};

// === ReplaySystem ===
enum class ESimCommandType : uint8 { MoveFormation, SetTimeScale, Pause };

struct FSimCommand
{
    int32           Tick;
    ESimCommandType Type;
    int32           ParamX, ParamY;  // 仿真空间整数坐标
    uint8           TeamID;
};

class UReplaySystem : public UGameInstanceSubsystem
{
public:
    void        RecordCommand(FSimCommand Cmd);     // 仅 Record 模式有效
    bool        HasCommandForTick(int32 Tick) const;
    FSimCommand PopCommand(int32 Tick);
    void        SaveTape(const FString& Path) const;
    bool        LoadTape(const FString& Path);
    void        Reset();
    bool        IsReplaying() const;
};

// === SimToVisualBridge（每渲染帧调用，读仿真写 ISM） ===
class USimToVisualBridge : public UActorComponent
{
public:
    int32  LastUnitCount = 0;   // 由 PerformanceHUD 消费
    double LastSimMs     = 0.0;
    double LastBridgeMs  = 0.0;
private:
    void TickComponent(...) override; // 遍历 Fragment，插值，批量写 ISM
};
```

**约束：** 仿真层代码（SimClock / SpatialGrid / Processor / ReplaySystem）不得 `#include` 任何 Visual 层头文件。依赖方向单向：Visual → Simulation，不得反转。

---

## 确定性不变量（设计红线，DeterminismTest 逐条验证）

以下任意一条被违反，replay 将产生 desync。

1. **无浮点仿真路径** — 从 SimClock OnSimTick 触发到所有 Processor 执行完毕，不得调用任何 `float` / `double` 算术。坐标、速度、伤害全部用 `int32` Q16.16 定点。
2. **无外部时间查询** — Processor 内不得调用 `FApp::GetDeltaTime()`、`FPlatformTime::Seconds()` 或任何帧时间相关 API。时间只由 `TickIndex` 决定。
3. **PRNG 确定性** — 每次用随机数前以 `(TickIndex ^ EntityHandle.Index)` 作为种子重置，不维护全局随机状态。
4. **死亡处理顺序确定** — `FDeadTag` 收集后按 `EntityHandle.Index` 升序处理，不依赖 Mass 内部迭代顺序。
5. **Processor 执行顺序显式声明** — 所有依赖关系通过 `ExecuteBefore` / `ExecuteAfter` 在 `ConfigureQueries` 里声明，不依赖注册顺序。
6. **ReplaySystem 是唯一外部输入来源** — 玩家输入只在 PlayerController 里转为 `FSimCommand` 送入 ReplaySystem，Processor 只从 ReplaySystem 消费指令，不直接读取输入状态。

---

## 性能预算

| 组件 | 目标（5K 单位）| 目标（10K 单位）| CI 门禁阈值 |
|---|---|---|---|
| SpatialGrid 全量更新 | ≤ 1ms | ≤ 2ms | — |
| SteeringProcessor | ≤ 2ms | ≤ 5ms | — |
| CombatProcessor | ≤ 1ms | ≤ 2ms | — |
| 死亡销毁批处理 | ≤ 0.5ms | ≤ 1ms | — |
| **Sim tick 总计** | **≤ 6ms** | **≤ 12ms** | **5K ≤ 8ms（CI）** |
| SimToVisualBridge | ≤ 2ms | ≤ 4ms | — |
| ISM 渲染（GPU） | ≤ 3ms | ≤ 5ms | — |

*基于单线程、中端 PC（Ryzen 5 / i5 级别）估算。Phase 4 benchmark 结果为准，超出预算则先优化再封顶。*

---

## 视觉方向

**单位造型：** 几何极简风，两队用颜色 + 形状区分——有意识的设计，不是占位方块。参考：Homeworld 舰队的抽象感。具体造型 Day 9 决定（可以是扁平六边形柱体 vs 锥体，只要在俯视镜头下两队可读）。

**环境：** 平坦开阔地形（沙漠 / 石板），单色调背景，让单位密度成为唯一视觉焦点。UE5 Lumen，日落侧逆光，长影。

**相机：** 两个镜头——宏观俯视（展示规模）+ 近地飞越（展示个体密度）。

**HUD 覆盖层：** 右上角常驻 `Units: 10,247 | Sim: 9.1ms | Render: 4.2ms`。这是 demo 的技术背书，不是 debug 信息。

**Demo 视频结构（90 秒）：**
- 0-30s：俯视全景——大军从两侧集结、冲锋、碰撞
- 30-60s：近地飞越——穿越战场，个体密度和战斗撕咬
- 60-90s：Replay 模式——完全相同的战局重演（叙事上证明确定性）

---

## 项目结构

```
Phalanx/
├── Source/Phalanx/
│   ├── Core/
│   │   ├── PhalanxGameInstance.h/.cpp        Phase 1
│   │   ├── PhalanxPlayerController.h/.cpp    Phase 1
│   │   ├── PhalanxUISubsystem.h/.cpp         Phase 1（桩）
│   │   └── PhalanxAudioSubsystem.h/.cpp      Phase 1（桩）
│   ├── Simulation/
│   │   ├── SimClock.h/.cpp                   Phase 1
│   │   ├── SpatialGrid.h/.cpp                Phase 2
│   │   ├── ReplaySystem.h/.cpp               Phase 3
│   │   └── BenchmarkMode.h/.cpp              Phase 4
│   ├── Mass/
│   │   ├── UnitFragments.h                   Phase 1
│   │   ├── SteeringProcessor.h/.cpp          Phase 2
│   │   └── CombatProcessor.h/.cpp            Phase 2
│   ├── Visual/
│   │   ├── SimToVisualBridge.h/.cpp          Phase 1（框架）/ Phase 5（打磨）
│   │   └── PhalanxBaseWidget.h/.cpp          Phase 1（UMG 基类）
│   └── Tests/
│       ├── DeterminismTest.cpp               Phase 3
│       └── PerfGuardTest.cpp                 Phase 4
├── Content/Input/
│   ├── IMC_Phalanx.uasset                   Phase 1
│   └── IA_ReplayPlay/Pause/ConfigOpen.uasset Phase 1
├── ARCHITECTURE.md                           Phase 6
└── README.md                                 Phase 6
```

---

## Sprint 计划（10天，6-15 ~ 6-24）

### Phase 1 — 基础 + 框架桩（Day 1-2）

- UE5.6 C++ 项目，启用 MassEntity / MassGameplay / MassRepresentation / EnhancedInput 插件
- 建立 Core 层：`PhalanxGameInstance`、`PhalanxPlayerController`、`PhalanxUISubsystem`（桩）、`PhalanxAudioSubsystem`（桩）
- Enhanced Input：创建 IMC 和 IA 资产（ReplayPlay / ReplayPause / ConfigOpen），绑定空 Lambda
- `UPhalanxBaseWidget`：UMG 基类，空实现
- 定义 Unit Archetype 所有 Fragment（见数据设计）
- 实现 `SimClock`：固定 62.5Hz（16ms 步长），`OnSimTick` 多播委托，与 `AActor::Tick` 完全解耦
- 实现 `SimToVisualBridge` 框架：每渲染帧读取 Fragment 位置，批量写 ISM PerInstance Transform
- **验收：** 1000 个单位静止，ISM 正确显示，SimClock 独立运行，Core 层所有桩编译通过

**AI 考核点：**
- SimClock 如何处理渲染帧比仿真步长短 / 长两种情况？
- ISM 批量更新（`UpdateInstanceTransforms`）和逐个更新（`UpdateInstanceTransform`）性能差异？
- UISubsystem 和直接调用 `CreateWidget` 的区别？

---

### Phase 2 — 空间与行为（Day 3-5）

- 实现 `SpatialGrid`（纯 C++，模板 cellSize，O(1) insert / remove / query）
- 实现 `SteeringProcessor`：Separation / Cohesion + Formation 目标点驱动
- 实现 `CombatProcessor`：SpatialGrid 查敌 → 攻击范围判断 → HP 扣除 → FDeadTag → 批量销毁
- **验收：** 两军各 500 单位从两侧对冲，聚合行为和战斗撕咬视觉可辨

**AI 考核点：**
- Processor 执行顺序如何保证（`FMassProcessorDependencies`）？
- SpatialGrid 在并行 Processor 中的线程安全策略？

---

### Phase 3 — 确定性（Day 6-7）

- 实现 `ReplaySystem`：所有 Formation 指令以 `(tick, command)` 记录入磁带；回放时磁带代替实时输入
- 实现 `DeterminismTest`：相同 seed 跑两次，tick 500 时断言全量单位位置 / 血量 bit-identical
- GitHub Actions CI：DeterminismTest 设为 required check
- **验收：** CI 绿；手动触发 Replay 能重演同一场战局

**AI 考核点：**
- PRNG 如何纳入确定性约束？
- 死亡顺序是否影响确定性？怎么证明？

---

### Phase 4 — 性能边界（Day 8）

- 实现 `BenchmarkMode`：headless（禁用渲染），固定 1000 tick，输出 `units,sim_ms` 到 CSV
- Unreal Insights 定位热点：SpatialGrid vs SteeringProcessor vs CombatProcessor 各占比
- 针对性优化直到 CPU 瓶颈明确
- `PerfGuardTest`：断言 5000 单位 sim tick ≤ 8ms（CI）
- 记录性能曲线（1K / 2K / 5K / 10K / 20K 各点）——README 的核心图表
- **验收：** PerfGuardTest 绿；10K 单位实测数据记录在案

**AI 考核点：**
- Cache miss 如何在 Unreal Insights 里定位？
- 当前 SoA 布局是否还有优化空间？

---

### Phase 5 — 视觉打磨（Day 9）

- 设计并制作两队单位 Mesh（简单几何体，两队视觉可辨）
- 配置 Lumen 光照：日落侧逆光 + 长影 + 适当雾
- 实现 `UBattleHUD`：继承 `PhalanxBaseWidget`，显示性能覆盖层 + Replay 控制（播放 / 暂停 / 倍速）
- 实现 `UBattleConfigWidget`：单位数滑条 + 队伍配置，触发新战局
- 调试相机：俯视全景 + 近地飞越两个镜头
- **验收：** 10K 单位下视觉帧率流畅（渲染层独立）；HUD 数据实时准确

**AI 考核点：**
- ISM 在 10K 单位下的 draw call 数量？
- LOD 设置如何影响远距离单位渲染开销？

---

### Phase 6 — 交付（Day 10）

- 录制 90 秒 demo 视频（见视觉方向 → Demo 视频结构）
- 写 `ARCHITECTURE.md`（含本文档架构图的文字版）
- 写 `README.md`：一句话定位 + 性能曲线图 + 架构说明 + 运行方式
- 更新简历 v2 加 Phalanx 条目
- 投递 Blackbird + UE 系岗

---

## 已知风险与缓解策略

| 风险 | 可能性 | 缓解 |
|---|---|---|
| UE5.6 Mass Entity API 与文档不符 | 中 | Phase 1 第一天先用最小用例验证 Fragment 注册和 Processor 触发，卡住立即查 UE5.6 Changelog |
| ISM 批量更新成 CPU 瓶颈（>10K 单位）| 中 | 先测 `UpdateInstanceTransforms()`，如不够改用 `FInstancedStaticMeshInstanceData` 直写 buffer |
| SpatialGrid 在并行 Processor 中产生数据竞争 | 高 | Phase 2 初期用单线程 Processor，确定性验证通过后再评估是否需要并行 |
| DeterminismTest 在 CI 机器上失败但本地通过 | 低 | CI 强制编译 flag 规避 FMA 差异；仿真侧无 float 已从源头消除主要风险 |
| Day 9 视觉效果不达预期 | 中 | 硬时间盒：超时则降低 Mesh 精细度，demo 视频节奏优先于画质 |

---

## 关键设计决策

**为什么仿真侧用定点整数而不是 float？**
浮点 FMA 指令和 denormal 刷新策略在不同 CPU 上结果可能不同。定点整数在任意机器上 bit-identical——这是确定性 replay 的硬性前提，与 cpp_core 的 Q16 oracle 同一套哲学。

**为什么不用 UE 内置 Navigation / Avoidance？**
NavMesh 查询结果依赖帧时序，不确定。自制 SteeringProcessor 让每个决策都在固定步长内发生，可复现、可测试。

**为什么视觉层用 ISM 而不是 Niagara？**
ISM 每个实例有独立 Transform 可直接写入，逻辑最简单，Phase 1 就能跑通。Niagara 适合死亡爆炸等粒子效果，Phase 5 可叠加，但主体不依赖它。

**规模上限预估：**
Mass Entity 单线程 + 自制 SpatialGrid：5K-15K 单位 @16ms。开启 Mass Processor 并行可进一步突破。实际上限由 Phase 4 benchmark 数据说话，不预设结论。

---

## 面试叙事连接

> "In cpp_core I enforced determinism at the microsecond level — audio clock-driven timing, Q16 fixed-point oracle, CI-verified zero-allocation hot path. Phalanx applies the same discipline at a different scale: a fixed-timestep SimClock fully decoupled from UE's render loop, integer-only simulation state, and a replay system that produces bit-identical results across 10,000 units. The visual layer is deliberately separate — it reads simulation state each frame but never influences it. That's the architecture an RTS needs for lockstep multiplayer, and it's the architecture Blackbird is running in production."
