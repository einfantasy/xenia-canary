# Milestone 3-B：DLSS SR InPerfQualityValue UI 设计调研报告

- **文档性质**：DESIGN / 调研报告。本轮**未修改任何代码**，仅只读分析（源码、SDK 头文件、SDK 文档提取）+ 设计输出。未编译、未运行、未加载任何 DLL、未执行任何 GPU 操作。
- **仓库状态**：分支 `dev/dlss5-integration`，HEAD `6129fea`（M3-B：BeginSubmission 设备丢失 HRESULT 日志）。工作树含**未提交的 M3-B 修复**（`dlss_super_sampling_backend.h/.cc`、`d3d12_presenter.h/.cc`、`dlss/CMakeLists.txt` 及 5 个未跟踪调试报告），本轮全部原样保留，未触碰。
- **设计目标**：为 `NVSDK_NGX_Feature_Create_Params::Feature::InPerfQualityValue` 设计 UI 控件（滑块），允许用户在 "Post-processing" 对话框中修改 DLSS SR 质量预设；滑块仅在 "Super Resolution" 激活时显示。设计完全复用现有 DLSS / Super Resolution UI 的 cvar → 对话框 → Presenter 存取器链路。
- **总体状态**：**DESIGN / PASS**。设计可行，涉及 4 个文件（UI 层 2 个 + 后端 2 个），本轮不实施。

---

## A. 总结

现有 UI 链路已完整核实：`emulator_window.cc` 的 "Post-processing" 对话框内 `Resampling and sharpening` 树节点包含 "NVIDIA DLSS" 单选（master 开关）与 "Super Resolution" 复选框（SR 子开关），经 `GuestOutputEnhancementConfig` 与 `dlss_enabled` / `dlss_sr_enabled` 两个 cvar 持久化，写入 D3D12 Presenter 的增强配置。SDK 侧 `InPerfQualityValue` 的定义与取值已用权威头文件（DLSS-310.9.1）与官方编程指南双重核实：共 6 个枚举值，其中 **5 个（MaxPerf/Balanced/MaxQuality/UltraPerformance/UltraQuality）是 SDK 文档明确要求检查并暴露给用户的可选模式**，DLAA（输入=输出尺寸）为独立的 UI 概念不在本滑块范围内。当前后端在 `Initialize()` 中将质量**硬编码为 Balanced**。本设计在 "Super Resolution" 复选框之后新增一个 5 档 `SliderInt` 滑块（0=MaxPerf … 4=UltraQuality），默认 Balanced（与当前硬编码一致，避免首次启用时触发无谓重建），质量变更时 Presenter 重置并惰性重建后端。UI 层不引入任何 `nvsdk_*` 头文件，质量值以 Xenia 侧纯整数/具名枚举承载，由后端模块完成 int → SDK 枚举映射，保持许可证隔离。

## 1. 现状调查：DLSS / Super Resolution 现有 UI 设计

### 1.1 对话框与控件链路（全部为已核实的工作树代码）

| 环节 | 位置 | 内容 |
|---|---|---|
| 对话框入口 | `emulator_window.cc:356` `EmulatorWindow::DisplayConfigDialog::OnDraw` | ImGui 窗口 "Post-processing"，左上角 (20,20)，背景透明 0.6，`AlwaysAutoResize + HorizontalScrollbar` |
| 树节点 | `emulator_window.cc:446` | `ImGui::TreeNodeEx("Resampling and sharpening", Framed \| DefaultOpen)` |
| 效果单选 | `emulator_window.cc:462/465/469/474` | `RadioButton` 四选一：None/Bilinear（Effect::kBilinear=0）、CAS（1）、FSR（2）、**NVIDIA DLSS（索引 3，仅 D3D12 presenter 绘制）** |
| master 同步 | `emulator_window.cc:481-489` | 选中索引 3 → `master_enabled = true` 且强制 paint 效果为 bilinear（避免 FSR/CAS 双重重采样）；选其他项 → master 关闭 |
| SR 子开关 | `emulator_window.cc:497-500` | **仅当 `d3d12_presenter && master_enabled` 时绘制** `ImGui::Checkbox("Super Resolution", &new_dlss_sr)` |
| 写入 Presenter | `emulator_window.cc:613-616`（paint 配置）、`636-639`（增强配置） | `presenter->SetGuestOutputPaintConfigFromUIThread(...)` / `d3d12_presenter->SetGuestOutputEnhancementConfigFromUIThread(new_enhancement_config)` |
| 持久化（关闭对话框时） | `emulator_window.cc:640-655` | 仅当值实际变化时 `OVERRIDE_bool(dlss_enabled, ...)`（L643）与 `OVERRIDE_bool(dlss_sr_enabled, ...)`（L651，仅 master 开启时） |
| cvar 定义 | `emulator_window.cc:141-148`（`dlss_enabled`）、`149-158`（`dlss_sr_enabled`） | 均 `DEFINE_bool`，默认 `false`，归 "Display" 组 |
| cvar 加载 | `emulator_window.cc:1120-1127` `GetGuestOutputEnhancementConfigForCvars()` | 两个 bool 填充 `GuestOutputEnhancementConfig`；NVOF/FG/实验神经渲染子开关保持默认 OFF |
| cvar 应用 | `emulator_window.cc:1135-1158` `ApplyDisplayConfigForCvars()` | 启动时经 `dynamic_cast<D3D12Presenter*>` 写入 Presenter 增强配置 |
| Presenter 存取器 | `d3d12_presenter.h:108-110`、`d3d12_presenter.h:373/379` | `Set/GetGuestOutputEnhancementConfigFromUIThread` 在 `guest_output_enhancement_config_mutex_` 保护下读写；paint 线程每帧读取（`d3d12_presenter.cc` PaintAndPresentImpl 内 `IsSuperSamplingRequested()` 判断） |

### 1.2 后端现状（质量硬编码）

- 后端构造（工作树未提交版）：`DLSSSuperSamplingBackend(ID3D12Device* device, std::vector<std::wstring> runtime_dirs, std::wstring config_path)`（`dlss_super_sampling_backend.h` 构造声明，.cc:260）——**无质量参数**。
- 质量硬编码：`dlss_super_sampling_backend.cc:736-737` `create_params.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_Balanced;`，初始化成功日志 `PerfQuality=Balanced`（.cc:763）。
- 惰性创建点：`d3d12_presenter.cc` PaintAndPresentImpl 内（工作树 ~L831）`if (!dlss_sr_backend_) { dlss_sr_backend_ = std::make_unique<...>(device, GetDLSSRuntimeDirectories(), GetDLSSConfigPath()); }`，仅当 `IsSuperSamplingRequested()` 且非 1:1（guest 输出尺寸 ≠ 交换链尺寸）时到达。
- 生命周期：`Shutdown()`（.cc:289-313）执行 `ReleaseFeature()` + SDK `Shutdown1` + 状态复位 `kNotInitialized`；析构调用 Shutdown。因此 Presenter 侧 `dlss_sr_backend_.reset()` 即完整释放 SDK 会话，下一帧按新配置惰性重建——**这就是质量变更的重建机制**（与 1:1/尺寸变更导致的重建同一通路）。
- 前序设计文档 `ui-dlss-switch-design.md` 已预判本里程碑（Phase 2）：需向 `GuestOutputEnhancementConfig` 增加质量字段、向后端构造函数增加质量参数、质量变更时 `dlss_sr_backend_.reset()` 重建。

### 1.3 既有 UI 中"数值选择"控件范式（新滑块参照）

FSR/CAS 的数值控件（`emulator_window.cc` 树节点内，Effect 为 kFsr/kCas 时绘制）采用固定范式：
1. `ImGui::TextUnformatted("... 描述行 ...")` 说明控件含义与方向（如 "lower is sharper"）；
2. `ImGui::SliderFloat("##HiddenId", &v, min, max, label, ImGuiSliderFlags_NoInput)`，label 为格式化后的当前值文本；
3. `ImGui::SameLine()` + `ImGui::Button("Reset##...")` 一键复位到 `kDefault` 常量。

新滑块将逐条复用该范式。

## 2. 现状调查：InPerfQualityValue 的 SDK 定义（权威来源）

### 2.1 枚举定义（头文件，权威）

`C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1\include\nvsdk_ngx_defs.h:251-260`：

```c
typedef enum NVSDK_NGX_PerfQuality_Value
{
    NVSDK_NGX_PerfQuality_Value_MaxPerf,          // 0
    NVSDK_NGX_PerfQuality_Value_Balanced,         // 1
    NVSDK_NGX_PerfQuality_Value_MaxQuality,       // 2
    // Extended PerfQuality modes
    NVSDK_NGX_PerfQuality_Value_UltraPerformance, // 3
    NVSDK_NGX_PerfQuality_Value_UltraQuality,     // 4
    NVSDK_NGX_PerfQuality_Value_DLAA,             // 5
} NVSDK_NGX_PerfQuality_Value;
```

**本滑块采用的 5 个值与对应数值**（C 隐式枚举，从 0 起）：

| 档位 | 枚举 | 数值 |
|---|---|---|
| MaxPerf | `NVSDK_NGX_PerfQuality_Value_MaxPerf` | **0** |
| Balanced | `NVSDK_NGX_PerfQuality_Value_Balanced` | **1** |
| MaxQuality | `NVSDK_NGX_PerfQuality_Value_MaxQuality` | **2** |
| UltraPerformance | `NVSDK_NGX_PerfQuality_Value_UltraPerformance` | **3** |
| UltraQuality | `NVSDK_NGX_PerfQuality_Value_UltraQuality` | **4** |

DLAA=5 存在但**不在滑块内**（见 2.3 与 §3 设计决策 D6）。

### 2.2 参数流（创建期参数）

- `NVSDK_NGX_Feature_Create_Params`（`nvsdk_ngx_params.h:27-35`）：`InWidth/InHeight/InTargetWidth/InTargetHeight` + `/*** OPTIONAL ***/ NVSDK_NGX_PerfQuality_Value InPerfQualityValue;`。
- `NVSDK_NGX_DLSS_Create_Params`（`nvsdk_ngx_params.h:37-43`）= `Feature` + `InFeatureCreateFlags` + `InEnableOutputSubrects`。
- D3D12 创建辅助 `NGX_D3D12_CREATE_DLSS_EXT`（`nvsdk_ngx_helpers_d3d.h:275`）内部执行 `NVSDK_NGX_Parameter_SetI(pInParams, NVSDK_NGX_Parameter_PerfQualityValue, pInDlssCreateParams->Feature.InPerfQualityValue)`（L289）；参数名常量 `"PerfQualityValue"`（`nvsdk_ngx_defs.h:756`）。
- 后端当前经 `CallCreateDlss` → `NGX_D3D12_CREATE_DLSS_EXT`（`dlss_super_sampling_backend.cc:175-183`）走同一通路，`create_params` 由 `Initialize()` 填充。
- **语义**：质量是**创建期参数**，不是逐帧参数。SDK 文档明确：影响 DLSS 的设置变更须 "release the current feature and go back to step 3"（重新创建，见 2.3）。无"改参不重建"的文档化通路。

### 2.3 官方文档对可选模式数的确认（DLSS_Programming_Guide_Release.pdf）

- 物理页 20-21（§3.2.1 DLSS Execution Modes）：六个 PerfQualityValues（Performance / Balanced / Quality / Ultra-Performance / DLAA / Ultra Quality，分两页列出）；**"DLAA 指输入=输出尺寸（1.0 缩放比），应在游戏中以独立 UI 选项暴露"**；"只展示已启用的模式，完全隐藏其余模式"。
- 物理页 59（§5.2.8，印刷页 49）：**"The PerfQualityValue is currently defined in the NGX DLSS header as having five possible values. All five PerfQualityValue values should be checked and if they are enabled, they should be selectable in the Game UI (and hidden if disabled)."** 同页示例代码注释给出数值映射：`[0 MaxPerformance, 1 Balance, 2 MaxQuality, 3 UltraPerformance, 4 UltraQuality]`——与本设计采用的 5 档完全一致。
- RTX UI Developer Guidelines（`nvidia-dlss-reference/05-rtx-ui-guidelines.md` §4，源 `RTX UI Developer Guidelines.pdf` p.5/7）：官方 UI 模式顺序 `Off, Auto, Quality, Balanced, Performance, Ultra-Performance, DLAA`，Auto 是面向用户的分辨率映射而非 SDK 模式。**注意**：官方显示名为 "Performance"/"Quality"/"Ultra-Performance"，与枚举名 MaxPerf/MaxQuality/UltraPerformance 略有出入。本设计按任务要求采用枚举名作为滑块标签（对模拟器语境更精确、与 SDK 日志一致），不实现 Auto（Auto 需要逐分辨率查询 Optimal Settings 的完整通路，超出本里程碑范围，见 §5 风险）。

## 3. 新滑块设计（复用现有设计）

### 3.1 控件布局（"Resampling and sharpening" 树节点内）

```
Resampling and sharpening                  (Framed | DefaultOpen，不变)
  (o) None / Bilinear                      (RadioButton，不变)
  (o) AMD FidelityFX CAS                   (RadioButton，不变)
  (o) AMD FidelityFX Super Resolution 1.0  (RadioButton，不变)
  (o) NVIDIA DLSS                          (RadioButton 索引 3，不变 -> master_enabled)
      [ ] Super Resolution                 (Checkbox，不变 -> super_sampling_enabled)
      DLSS SR quality preset (lower is faster, higher is higher quality):
      [======*======] MaxQuality           [Reset]   <- 新增：SliderInt 0..4
```

- **插入点**：`emulator_window.cc:500`（`new_enhancement_config.super_sampling_enabled = new_dlss_sr;` 之后、`#endif` 之前），仍在 `if (d3d12_presenter && new_enhancement_config.master_enabled)` 块内，新增 `if (new_enhancement_config.super_sampling_enabled)` 门控。
- **可见性链**：D3D12 presenter 存在 && master 开 && SR 子开关开，三者同时满足才绘制。SR 取消勾选后滑块消失，**值保留**（与 SR 复选框在 master 关闭时的保留语义一致）；重新勾选后滑块以保留值重现。
- **D3D11 presenter**：不绘制（dynamic_cast 守卫），与 SR 复选框行为一致。

### 3.2 控件选型：`ImGui::SliderInt`（主案）

- 5 个离散命名档位，`SliderInt` 0..4，`ImGuiSliderFlags_NoInput`（与 FSR/CAS 滑块一致），隐藏 ID `"##DLSSSrPerfQuality"`，当前档位名作为 value label（`fmt::format` 生成 "MaxPerf"/"Balanced"/…）。
- 描述行（`TextUnformatted`）：说明"低档更快、高档更高质量"，并提示变更将重建 DLSS 会话（首帧可能有短暂延迟）。
- `SameLine` + `Reset` 按钮：复位到默认档 Balanced（与 cvar 默认一致）。
- **备选方案（不采用）**：`ImGui::Combo`。RTX UI 指南对质量模式推荐列表/滚动列表，但任务明确要求"滑块"且现有 FSR/CAS 数值控件均为 Slider 范式，故 SliderInt 为最终选型；Combo 可作为后续视觉调整项。

### 3.3 配置字段（`GuestOutputEnhancementConfig`）

- 新增字段：`uint8_t perf_quality_value = 1;  // Balanced（与当前硬编码一致）`。
- **设计决策 D1（对前序 Phase 2 设计表的修正）**：前序 `ui-dlss-switch-design.md` Phase 2 表曾提议字段类型直接用 `NVSDK_NGX_PerfQuality_Value`。该方案要求 `guest_output_enhancement.h` 引入 SDK 头文件，违反本项目许可证隔离约定（"UI 层不 `#include` 任何 `nvsdk_*`，DLSS 依赖只允许出现在 `dlss/*` 模块"——`ui-dlss-switch-design.md` E.3 与本文件头注释）。本设计改为 **Xenia 侧纯整数**，并建议在 `guest_output_enhancement.h` 定义具名枚举：

  ```cpp
  // Xenia-side mirror of NVSDK_NGX_PerfQuality_Value (DLSS SDK 310.9.1,
  // nvsdk_ngx_defs.h:251-260). UI/config layer never includes nvsdk_* headers;
  // the dlss/* backend maps this value to the SDK enum at feature creation.
  enum class DlssSrPerfQuality : uint8_t {
    kMaxPerf = 0, kBalanced = 1, kMaxQuality = 2,
    kUltraPerformance = 3, kUltraQuality = 4,
  };
  ```
  后端在 `Initialize()` 内做 `static_cast<NVSDK_NGX_PerfQuality_Value>(config.perf_quality)` 并校验范围（越界 → `BackendResult::kInputContractFailed` + 回退，不伪造、不钳位静默通过）。

### 3.4 cvar 与持久化

- 新增 `DEFINE_string(dlss_sr_perf_quality, "balanced", "...", "Display")`（`emulator_window.cc`，紧随 `dlss_sr_enabled` 之后）。字符串型 cvar 与 `postprocess_scaling_and_sharpening` 的命名值范式一致，配置文件人类可读。合法值：`maxperf` / `balanced` / `maxquality` / `ultraperformance` / `ultraquality`；未知值一律回退默认 `balanced`（加载侧，仿 `GetGuestOutputPaintEffectForCvarValue` 的 default 分支）。
- 新增映射函数（`emulator_window.h` 声明、`.cc` 实现）：`GetDlssSrPerfQualityForCvarValue(const std::string&)` 与反向 `GetCvarValueForDlssSrPerfQuality(uint8_t)`。
- `GetGuestOutputEnhancementConfigForCvars()`（L1120）读取新 cvar 填充 `perf_quality_value`。
- 关闭对话框持久化（L640-655 区域）：仅当 `master && super_sampling_enabled` 且值变化时 `OVERRIDE_string(dlss_sr_perf_quality, ...)`——与 SR 子开关的持久化守卫完全同构。
- **设计决策 D2（默认值）**：默认 `balanced` = 当前硬编码值。用户首次启用 SR 时不触发后端重建（质量未变），行为与 M3-B 已验证状态完全一致；只有显式拖动滑块才重建。

### 3.5 后端与 Presenter 接线（重建语义）

- 后端构造函数扩为 `(device, runtime_dirs, config_path, perf_quality)`（`perf_quality` 为 Xenia 侧 uint8_t/枚举），`Initialize()` 用它填充 `create_params.Feature.InPerfQualityValue`，替代硬编码；初始化成功日志输出实际档位名。
- **设计决策 D3（重建判定的归属）**：Presenter 持有 `uint8_t dlss_sr_backend_perf_quality_ = 0xFF;`（哨兵=从未创建）。惰性创建处：
  1. 若 `dlss_sr_backend_` 为空 → 用配置中的质量创建，记录 `dlss_sr_backend_perf_quality_`；
  2. 若非空但 `配置质量 != dlss_sr_backend_perf_quality_` → `dlss_sr_backend_.reset()`（析构 → `Shutdown()`：ReleaseFeature + SDK Shutdown1 + 状态复位）→ 下一帧（或同帧重建）以新质量惰性创建，并记录新值。
  3. 质量值本身来自每帧在 `guest_output_enhancement_config_mutex_` 下读出的 `enhancement_config`，无需额外加锁通路。
  - 不修改 `IDLSSSuperSamplingBackend` 接口（Presenter 自持"已用质量"即可判定，接口零改动）。
- **设计决策 D4（重建时机）**：在每帧创建判定处做（而非对话框关闭时）。理由：判定成本为一次 uint8 比较；重建本身是 GPU 命令（CreateFeature 录入当前帧命令列表），下一帧生效，与现有惰性创建语义一致。重建失败走既有确定性回退（`kInitFailed`/`kExecutionFailed` → `BuildOriginalPathFallbackFlow` + 命令列表 Reset），绝不标记成功。
- **1:1 绕过**：guest 输出尺寸 = 交换链尺寸时后端不创建，滑块无实际效果（与现状一致，UI 不需额外处理）。

### 3.6 设计决策 D6（排除 DLAA）

DLAA=5 表示输入=输出尺寸（1.0 缩放），在模拟器场景下无对应操作（Xenia 的 guest 输出尺寸由游戏决定，不存在"以 1:1 送入 DLSS"的用户选项），且任务明确只列 5 档；SDK 文档亦将 DLAA 单列为独立 UI 概念。滑块不暴露 DLAA。

## 4. 拟修改文件清单（下轮实施，本轮不动）

| 文件 | 修改内容 | 修改理由 |
|---|---|---|
| `src/xenia/ui/guest_output_enhancement.h`（及 `.cc`，若需） | 新增 `DlssSrPerfQuality` 枚举（或注释化 uint8 常量组）；`GuestOutputEnhancementConfig` 新增 `perf_quality_value` 字段（默认 Balanced） | 质量是配置的一部分，需经同一把 mutex 与同一存取器链路透传；纯整数承载保持 UI 层无 SDK 依赖（决策 D1） |
| `src/xenia/app/emulator_window.cc` | 新增 `dlss_sr_perf_quality` 字符串 cvar（默认 "balanced"）与两个映射函数；`GetGuestOutputEnhancementConfigForCvars()` 读取新 cvar；对话框 L500 后新增 SliderInt 0..4 + 描述行 + Reset 按钮（仅 D3D12 && master && SR 开时绘制）；关闭对话框时按守卫 `OVERRIDE_string(dlss_sr_perf_quality, ...)` | 完全复用 FSR/CAS 数值滑块范式与 DLSS 主开关的 cvar 链路（§1.1、§1.3、§3.2-3.4） |
| `src/xenia/app/emulator_window.h` | 声明 `GetDlssSrPerfQualityForCvarValue` / `GetCvarValueForDlssSrPerfQuality` | 与既有 `GetCvarValueForGuestOutputPaintEffect` 等映射函数并列 |
| `src/xenia/ui/dlss/dlss_super_sampling_backend.h/.cc` | 构造函数增加 `perf_quality` 参数；`Initialize()` 用它填充 `create_params.Feature.InPerfQualityValue`（替换硬编码 Balanced），越界值按输入契约失败处理；成功日志输出实际档位 | 质量是创建期参数，必须进入 CreateFeature（§2.2） |
| `src/xenia/ui/d3d12/d3d12_presenter.cc`（+ `.h` 成员声明） | 惰性创建处传入配置质量；新增"已用质量"哨兵成员；质量不匹配时 `dlss_sr_backend_.reset()` 触发重建（决策 D3/D4） | 质量变更 = 后端重建，Presenter 是创建方与释放方 |

`d3d12_presenter.h` 仅增加一个私有成员声明（`uint8_t dlss_sr_backend_perf_quality_ = 0xFF;`），不改 `IDLSSSuperSamplingBackend` 接口、不改任何 getter/setter 语义。

## C. 验证证据（本轮：静态调查）

- `git status` / `git diff --stat`：确认工作树未提交 M3-B 修复（6 文件 +148/-19），本轮未触碰。
- SDK 头文件逐行核实：`nvsdk_ngx_defs.h:251-260`（枚举）、`nvsdk_ngx_params.h:27-35/37-43`（Create_Params）、`nvsdk_ngx_defs.h:756`（参数名常量）、`nvsdk_ngx_helpers_d3d.h:275-289`（D3D12 创建辅助的 PerfQualityValue 传递）。
- SDK 文档文本提取（pypdf）：`DLSS_Programming_Guide_Release.pdf` 物理页 20-21（执行模式与 DLAA 独立 UI 概念）、物理页 59（"five possible values… selectable in the Game UI" + `[0 MaxPerformance, 1 Balance, 2 MaxQuality, 3 UltraPerformance, 4 UltraQuality]` 映射注释）。
- 源码链路核实：`emulator_window.cc` 全部 DLSS UI 段落（L141-158、L446-500、L613-655、L1077-1158）、`d3d12_presenter.h/.cc` 存取器与惰性创建点、`dlss_super_sampling_backend.cc:736-737` 硬编码 Balanced、`Shutdown()` 生命周期（.cc:289-313）。
- 未执行任何构建（`.\xb build`）、测试或 GPU 操作；无编译结果可呈。

## D. 环境限制

- 开发环境为 Hyper-V，无 NVIDIA 驱动、无真实 GPU。本轮为纯静态分析 + 设计输出：未编译、未运行、未加载 DLL、未执行 GPU 命令。
- 滑块的实际渲染效果、cvar 持久化、质量变更后的后端重建行为，在 RTX 4090 实机验证前一律标记 **UNVERIFIED**。

## E. 剩余风险

1. **UltraPerformance/UltraQuality 的实机可用性**：SDK 文档要求"检查五个值，禁用者在 UI 隐藏"。本设计固定暴露 5 档（不查询 Optimal Settings），若某档在 4090 + 当前输入尺寸下被 SDK 判定不可用，`CreateFeature` 将失败并走确定性回退（原始路径 + 日志），用户表现为"该档位下画面走原始渲染"。完整方案需在创建失败时记录档位并考虑 UI 灰显，属后续里程碑（需先建立"创建失败 → UI 反馈"通路）。
2. **重建开销**：质量变更触发 ReleaseFeature + Shutdown1 + 重新 Init/CreateFeature，首帧延迟不确定（UNVERIFIED）。缓解：仅值变化时重建（一次 uint8 比较），不每帧重建。
3. **许可证隔离**：`perf_quality_value` 以纯整数承载，`guest_output_enhancement.h` 不引入 `nvsdk_*`；后端映射处是唯一 int → 枚举转换点。`dlss/*` 模块外的 SDK 依赖仍为零。
4. **cvar 未知值处理**：配置文件被手工改为非法定值时，加载侧回退 `balanced`（不崩溃、不伪造档位）；该回退行为需在下轮补一条主机侧断言测试。
5. **1:1 绕过下的滑块语义**：guest 输出与交换链同尺寸时后端不创建，滑块无效果且无提示。与现状一致，暂不处理，列为 UI 文案可改进项。
6. **前序未决（继承自 M3-B）**：主开关关闭后已创建后端是否释放（`ui-dlss-switch-design.md` E.5）仍在 M3-C 实机确认范围；本设计不改变该行为（质量变更的重建只发生在 master && SR 开启时）。

## F. 下一步（最小且安全）

1. 按 §4 清单实施 4 文件修改（UI 层先行，后端参数化随后），`.\xb build --config release` 编译验证。
2. 主机侧测试：cvar 映射函数往返（5 个合法值 + 非法值回退）、`GetGuestOutputEnhancementConfigForCvars()` 默认值断言。
3. M3-C 实机验证（RTX 4090）：滑块渲染与可见性链（master/SR 开关组合）、各档位 CreateFeature 成功/回退、质量变更重建的日志序列与首帧延迟。

## G. 各项能力状态清单

- **DLSS SR**：[编译期已集成]（M3-B 后端与 Presenter 接线已交付；master + SR 子开关已在 UI 暴露；质量滑块为本设计输出，未实施；运行时 UNVERIFIED）
- **NVOF**：[未实现]
- **帧生成**：[未实现]
- **实验性神经渲染**：[未实现]
- **Presenter 集成**：[已接线且关闭]（主开关默认 OFF，后端未创建）
- **最终画质与性能**：[尚未评估]
