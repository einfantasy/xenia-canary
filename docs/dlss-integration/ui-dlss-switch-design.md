# DLSS UI 开关设计报告（参照 FSR/CAS 模式）

- **文档性质**：DESIGN（设计稿）。只读分析 + 设计输出，未修改任何源码、未编译、未加载任何 DLL、未执行任何 GPU 操作、未提交代码。
- **仓库状态**：分支 `dev/dlss5-integration`，HEAD `4dda197`（M3-B：DLSS SR 后端与 Presenter 接线），工作树干净（`git status` 无未提交修改）。
- **设计目标**：将 M3-B 已接线的 `GuestOutputEnhancementConfig`（主开关 + SR 子开关）暴露到 UI 层，遵循 FSR/CAS 的 cvar → 对话框 → Presenter 存取器链路模式。
- **总体状态**：**DESIGN / PASS**。设计可行，Phase 1 仅触及 `emulator_window.cc/.h`，完全复用 M3-B 已有结构体与 Presenter 存取器，零后端改动。Phase 2（PerfQuality）需改动后端构造函数，推迟至独立里程碑。

---

## A. 总结

M3-B（`4dda197`）已交付真实 `DLSSSuperSamplingBackend` 与 Presenter 接线：`GuestOutputEnhancementConfig` 主开关 + SR 子开关全部默认 OFF，`IsSuperSamplingRequested()` 有效请求 = `master && sub`，Presenter 在 `IsSuperSamplingRequested()` 为真时惰性创建后端、失败时确定性回退原始路径。但 `src/xenia/app/` 下**零 DLSS 引用**——没有 UI 面板、没有 cvar、没有映射函数，用户当前无法从任何界面开启 DLSS。

本设计参照 FSR/CAS 的 UI 开关链路（ImGui 对话框 → cvar → `GetGuestOutputPaintConfigFromUIThread` → 效果链构建），为 DLSS 设计一套平行的、独立的 UI 开关。**关键差异**：FSR/CAS 是预创建 PSO，配置变更只影响效果链构建，无回退概念；DLSS 是 SDK `Evaluate` GPU 命令，开关变更 = 惰性后端创建，质量变更 = 后端重建，运行时缺失时静默回退。因此不能盲目复制 FSR/CAS 的 RadioButton 互斥模式，DLSS 主开关必须是独立的复选框（放在与 FSR/CAS 同级的 `Resampling and sharpening` 树节点内），其 SR 子复选框仅在主开关启用后显示。

**Phase 1（最小实现，本设计范围）**：新增 cvar `dlss_enabled`（主开关）+ `dlss_sr_enabled`（SR 子开关）；在 "Post-processing" 对话框的 `Resampling and sharpening` 树节点内、与 FSR/CAS `RadioButton` 同级，新增 "NVIDIA DLSS" 主复选框 `master_enabled`，主复选框启用后才显示 SR 子复选框 `super_sampling_enabled`（主关闭时子复选框不显示，仅保留其值）。新增 `GetGuestOutputEnhancementConfigForCvars()`，扩展 `ApplyDisplayConfigForCvars()` 调用 `SetGuestOutputEnhancementConfigFromUIThread`。仅触及 `emulator_window.cc/.h`。

**Phase 2（推迟，独立里程碑）**：PerfQuality 选择器。需向 `GuestOutputEnhancementConfig` 添加 `PerfQuality` 字段、向后端构造函数添加 PerfQuality 参数、在质量变更时重建后端（`dlss_sr_backend_.reset()`）。

## B. 拟修改文件清单
> **Phase 1 拟修改文件**：
| 文件 | 修改内容 | 修改理由 |
|---|---|---|
| `src/xenia/app/emulator_window.cc` | 新增 cvar 定义 `dlss_enabled`（主开关，默认 `false`）、`dlss_sr_enabled`（SR 子开关，默认 `false`）；在 "Post-processing" 对话框的 `Resampling and sharpening` 树节点内、与 FSR/CAS `RadioButton` 同级新增 "NVIDIA DLSS" 主复选框 `master_enabled`，主复选框启用后才显示 SR 子复选框 `super_sampling_enabled`（主关闭时子复选框不绘制，仅保留其值）；`GetGuestOutputEnhancementConfigForCvars()` 读取两个 cvar 填充 `GuestOutputEnhancementConfig`；`ApplyDisplayConfigForCvars()` 末尾追加调用 `presenter->SetGuestOutputEnhancementConfigFromUIThread(GetGuestOutputEnhancementConfigForCvars())` | FSR/CAS 的 UI 链路模板：cvar 定义（`postprocess_scaling_and_sharpening` 等）→ 对话框 RadioButton/CheckBox → `OVERRIDE_*` 保存 → `GetGuestOutputPaintConfigForCvars()` 加载 → `ApplyDisplayConfigForCvars()` 写入 Presenter。DLSS 平行复用此链路，但用独立复选框而非 RadioButton（DLSS 不与 FSR/CAS 互斥），且放在与 FSR/CAS 同级的同一树节点内 |
| `src/xenia/app/emulator_window.h` | 声明 `GuestOutputEnhancementConfig GetGuestOutputEnhancementConfigForCvars() const;` | 映射函数声明，与现有 `GetGuestOutputPaintConfigForCvars()` 声明并列 |

> **Phase 2 拟修改文件**：
> | 文件 | 修改内容 | 修改理由 |
> |---|---|---|
> | `src/xenia/ui/guest_output_enhancement.h/.cc` | `GuestOutputEnhancementConfig` 增加 `NVSDK_NGX_PerfQuality_Value perf_quality = NVSDK_NGX_PerfQuality_Value_Balanced;` 字段 | 当前结构体仅含 bool 开关，无质量值；Phase 2 需要 UI 选择质量等级 |
> | `src/xenia/ui/dlss/dlss_super_sampling_backend.h/.cc` | 构造函数签名从 `(device, runtime_dirs)` 扩展为 `(device, runtime_dirs, perf_quality)`，`CreateFeature` 时使用传入的 `PerfQuality` | 当前质量硬编码在构造函数内部，无法外部选择 |
> | `src/xenia/ui/d3d12/d3d12_presenter.h/.cc` | 质量变更时 `dlss_sr_backend_.reset()` 后重建 | `CreateFeature` 是 GPU 命令，质量变更必须重建后端 |

## C. 设计依据（FSR/CAS 链路证据 + M3-B 证据）

### C.1 FSR/CAS UI 链路（模板，已核实）

- **对话框入口**：`emulator_window.cc:408` 附近，"Post-processing" 对话框内 `ImGui::TreeEx("Resampling and sharpening", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen)`。
- **效果选择**：`RadioButton` 三选一——`None / Bilinear`、`AMD FidelityFX Contrast Adaptive Sharpening (CAS)`、`AMD FidelityFX Super Resolution 1.0 (FSR)`，写入 `new_presenter_config.SetEffect(...)`。**互斥单选**，因为效果链是单管线（`GetGuestOutputPaintFlow` 按 `GetEffect()` 分支）。
- **cvar 定义**（`emulator_window.cc:91/108/131`）：`postprocess_scaling_and_sharpening`（string，映射 Effect 枚举）、`postprocess_ffx_cas_additional_sharpness`（double）、`postprocess_ffx_fsr_sharpness_reduction`（double）。
- **cvar 保存**（`emulator_window.cc:563-577`）：对话框关闭时 `OVERRIDE_string(...)` / `OVERRIDE_double(...)` 持久化。
- **cvar 加载**（`emulator_window.cc:1007`）：`GetGuestOutputPaintConfigForCvars()` 从 cvar 重建 `GuestOutputPaintConfig`。
- **写入 Presenter**（`emulator_window.cc:1022/1038`）：`ApplyDisplayConfigForCvars()` 调用 `presenter->SetGuestOutputPaintConfigFromUIThread(GetGuestOutputPaintConfigForCvars())`。
- **Presenter 读取**（`presenter.h:326`）：`GetGuestOutputPaintConfigFromUIThread()` 返回 `const GuestOutputPaintConfig&`。

### C.2 M3-B 接线证据（已存在，本设计复用）

- **配置结构体**（`guest_output_enhancement.h:103-133`）：`GuestOutputEnhancementConfig` 含 `master_enabled`（主开关，默认 `false`）、`super_sampling_enabled`（SR 子开关，默认 `false`）、`optical_flow_enabled`、`frame_generation_enabled`、`experimental_neural_rendering_enabled`（后三者 M3 不启用，保持 `false`）。有效请求 `IsSuperSamplingRequested()` = `master_enabled && super_sampling_enabled`。注释明确：主开关 `false` 时不请求后端、不加载 DLL、不创建 GPU 资源、不执行额外 GPU 步骤。
- **Presenter 存取器**（`d3d12_presenter.h:108-110`、`d3d12_presenter.cc:76-86`）：`SetGuestOutputEnhancementConfigFromUIThread(...)` 在 `guest_output_enhancement_config_mutex_`（`d3d12_presenter.h:373`）保护下写入；`GetGuestOutputEnhancementConfigFromUIThread()` 返回 `const GuestOutputEnhancementConfig&`。paint 线程在 `PaintAndPresentImpl` 内读取（`d3d12_presenter.cc:767`）。
- **惰性后端创建**（`d3d12_presenter.cc:767-801`）：`enhancement_config.IsSuperSamplingRequested()` 为真时，若 `dlss_sr_backend_` 为空则创建 `std::make_unique<DLSSSuperSamplingBackend>(device, GetDLSSRuntimeDirectories())`，然后 `SelectOutputPath(enhancement_config, dlss_sr_backend_.get(), command_list, ...)` 选择输出路径；成功则 paint 输入 = `dlss_sr_backend_->OutputResource()`，失败走 `BuildOriginalPathFallbackFlow` 原始路径。
- **运行时目录**（`d3d12_presenter.cc:87`）：`GetDLSSRuntimeDirectories()` 返回 `std::vector<std::wstring>`，供后端 `PathList` 加载 `nvngxruntime.dll`。

### C.3 设计决策与 FSR/CAS 的关键差异

| 维度 | FSR/CAS（模板） | DLSS（本设计） |
|---|---|---|
| 效果选择 UI | `RadioButton` 互斥单选（单管线） | **独立复选框**，与 FSR/CAS 同级放在 `Resampling and sharpening` 树节点内（DLSS 是平行增强路径，不与 FSR/CAS 互斥）；SR 子复选框仅在主复选框启用后显示 |
| 配置变更影响 | 仅效果链构建（PSO 预创建） | 开关 ON → **惰性后端创建**（首帧 `CreateFeature` GPU 命令）；质量变更 → **后端重建** |
| 回退概念 | 无（PSO 总是可用） | **确定性回退**：运行时缺失/初始化失败/输入无效 → `BuildOriginalPathFallbackFlow` 原始链（`effect_count=1`, `kBilinear`）+ `CommandList::Reset`+重录 |
| 可用性 | 总是可用（纯 Xenia 内建） | 需要运行时 DLL + GPU；缺失时静默回退，不弹窗报错 |
| 主开关语义 | 无（Effect 枚举直接控制） | `master_enabled` 为总闸：`false` 时不加载 DLL、不创建 GPU 资源（`guest_output_enhancement.h:104-106` 注释） |
| 子开关 | 无 | SR / NVOF / FG / 实验性神经渲染四独立子开关，全部默认 OFF，有效 = `master && sub` |

**设计注记（同级放置带来的两点语义）**：

1. **显示/隐藏而非禁用**：主复选框 `master_enabled` 关闭时，SR 子复选框**不绘制**（仅保留其值），而非禁用。由于有效请求 `IsSuperSamplingRequested()` = `master && sub`，子开关即使保留旧值也不会触发后端创建。主开关重新启用后，子复选框以先前状态重现。这与 `master && sub` 语义一致，无需在关闭主开关时重置子开关。
2. **重采样正交性**：DLSS SR 与 FSR/CAS 都是重采样/超采样方法，同级放置意味着用户可同时启用（例如选择 FSR 作为 paint 效果的同时启用 DLSS SR）。按架构独立性原则，二者是正交模块，本设计保持正交（不做互斥约束）；同时启用会产生双重重采样，画质影响留给 M3-C / M5 实机评估，不在本设计阶段做取舍。

**Phase 1 UI 布局**（"Post-processing" 对话框内，`Resampling and sharpening` 树节点内，与 FSR/CAS 同级）：

```
▾ Resampling and sharpening          (DefaultOpen)
    ○ None / Bilinear                (RadioButton，不变)
    ○ AMD FidelityFX ... CAS         (RadioButton，不变)
    ○ AMD FidelityFX ... FSR 1.0     (RadioButton，不变)
    [✓] NVIDIA DLSS                  (master_enabled，与 FSR/CAS 同级)
        [ ] Super Resolution         (super_sampling_enabled，仅主复选框启用后显示)
      (NVOF / Frame Generation / Neural Rendering 子开关 Phase 2+ 再增)
```

## D. 环境限制

开发环境运行于 Hyper-V，无法安装 NVIDIA 驱动或执行真实 GPU 渲染。本设计为纯静态分析 + 设计输出，未编译、未运行、未加载 DLL。所有 UI 运行时行为（对话框渲染、cvar 读写、后端惰性创建、回退路径）在 RTX 4090 实机验证前标记为 **UNVERIFIED**。

## E. 剩余风险

1. **Phase 2 后端重建成本**：`CreateFeature` 是 GPU 命令，质量变更时 `dlss_sr_backend_.reset()` + 重建可能耗时。缓解：仅在质量值实际变更时重建，不每帧重建。
2. **PerfQuality 映射**：`NVSDK_NGX_PerfQuality_Value` 无 `Off` 值（`MaxPerf`/`Balanced`/`MaxQuality`/`UltraPerformance`/`UltraQuality`/`DLAA`），关闭由 Xenia 侧 `GuestOutputEnhancementConfig` 控制，UI 不直接暴露 SDK 枚举。
3. **许可证 §4e**：`LICENSE REVIEW REQUIRED`，阻塞公开发布宣称，不阻塞本地开发。DLSS 依赖只允许出现在 `dlss/*` 模块，UI 层不 `#include` 任何 `nvsdk_*`。
4. **UI 线程安全**：配置在 paint 线程读取，已有 `guest_output_enhancement_config_mutex_` 保护，与 FSR/CAS 的 `GuestOutputPaintConfig` 存取器同模式，风险低。
5. **主开关语义边界**：`master_enabled = false` 时后端指针必须为 `nullptr`（不加载 DLL、不创建资源）。Phase 1 UI 切换主开关为 OFF 后，Presenter 侧需确认后端被释放或保留不激活——当前 M3-B 代码在 `IsSuperSamplingRequested()` 为假时不创建后端，但**已创建的后端在主开关 OFF 后是否释放**需 M3-C 实机确认。

## F. 下一步

1. **Phase 1 实施**：在 `emulator_window.cc/.h` 中按本设计新增 cvar、对话框树节点、映射函数、`ApplyDisplayConfigForCvars` 扩展。仅触及 UI 层，零后端改动。
2. **编译验证**：`.\xb build --config release` 确认编译通过。
3. **主机侧测试**：`.\xb build --config release --build-tests --target xenia-app` 运行既有主机侧测试（UI 层无新增测试目标，cvar 默认值验证由现有框架覆盖）。
4. **M3-C 实机验证**：RTX 4090 上验证对话框渲染、cvar 持久化、主开关 ON → 后端惰性创建、主开关 OFF → 回退原始路径、运行时缺失 → 静默回退。
5. **Phase 2（独立里程碑）**：PerfQuality 选择器，需改动 `GuestOutputEnhancementConfig`、后端构造函数、Presenter 重建逻辑。

## G. 各项能力状态清单

- **DLSS SR**：[编译期已集成]（M3-B `4dda197` 已接线，UI 未暴露，运行时 UNVERIFIED）
- **NVOF**：[未实现]
- **帧生成**：[未实现]
- **实验性神经渲染**：[未实现]
- **Presenter 集成**：[已接线且关闭]（M3-B，主开关默认 OFF，后端未创建）
- **最终画质与性能**：[尚未评估]
