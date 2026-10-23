# Milestone 3-B：DLSS SR InPerfQualityValue UI 实施报告

- **文档性质**：IMPLEMENTATION（实施报告）。按 [`milestone-3-b-sr-quality-ui-design.md`](milestone-3-b-sr-quality-ui-design.md) §4 清单实施代码修改，已编译通过并运行既有主机侧测试。
- **仓库状态**：分支 `dev/dlss5-integration`。在**未提交的 M3-B 修复**之上叠加本实施；两者均未提交，由用户统一处理。
- **总体状态**：**PASS WITH FINDINGS**。5 个文件全部按设计修改；Release 构建成功；既有主机侧测试全绿。质量变更后的后端重建在实机前标记 `UNVERIFIED`。

## A. 总结

按设计完成 DLSS SR 质量预设的 UI 全链路：`GuestOutputEnhancementConfig` 新增 `perf_quality` 字段与 `DlssSrPerfQuality` 枚举（Xenia 侧纯整数，UI 层零 `nvsdk_*` 依赖）；`emulator_window` 新增字符串 cvar `dlss_sr_perf_quality`（默认 `balanced`）与两个映射函数、`Super Resolution` 复选框之后的 5 档 `SliderInt` 滑块（仅 D3D12 && master && SR 开启时绘制）与关闭对话框的守卫式持久化；后端构造函数增加 `perf_quality` 参数并在 `Initialize()` 入口做越界输入契约校验、用其填充 `create_params.Feature.InPerfQualityValue`（替换硬编码 Balanced）；Presenter 惰性创建处传入配置质量、持"已用质量"哨兵、质量不匹配时 `reset()` 触发同帧重建。默认 `balanced` 与旧硬编码一致，首次启用 SR 不触发重建。

## B. 修改文件（本轮实施）

| 文件 | 修改内容 |
|---|---|
| `src/xenia/ui/guest_output_enhancement.h` | 新增 `DlssSrPerfQuality` 枚举（kMaxPerf=0 … kUltraQuality=4，无 DLAA）；`GuestOutputEnhancementConfig` 新增 `perf_quality` 字段（默认 kBalanced）。UI 层不引入 `nvsdk_*` |
| `src/xenia/app/emulator_window.h` | 声明静态映射函数 `GetCvarValueForDlssSrPerfQuality` / `GetDlssSrPerfQualityForCvarValue` |
| `src/xenia/app/emulator_window.cc` | 新增 `dlss_sr_perf_quality` 字符串 cvar（默认 `balanced`）；文件局部 `GetDlssSrPerfQualityName` 标签函数；对话框 L500 后新增 `SliderInt` 0..4 + 描述行 + Reset 按钮（仅 D3D12 && master && SR 开时绘制，值保留语义同 SR 复选框）；`GetGuestOutputEnhancementConfigForCvars()` 读取新 cvar；两个 cvar↔枚举映射函数；关闭对话框时按 `master && SR && 值变化` 守卫 `OVERRIDE_string` |
| `src/xenia/ui/dlss/dlss_super_sampling_backend.h` | 构造函数增加 `ui::DlssSrPerfQuality perf_quality` 参数；新增 `perf_quality_` 成员 |
| `src/xenia/ui/dlss/dlss_super_sampling_backend.cc` | 匿名命名空间 `PerfQualityName` 日志名函数；构造函数接收并存储质量；`Initialize()` 入口对越界质量做输入契约校验（缓存 `kUnavailable` + `kInputContractFailed`，不钳位、不伪造）；`create_params.Feature.InPerfQualityValue` 由质量映射填充（替换硬编码 Balanced）；成功日志输出实际档位名 |
| `src/xenia/ui/d3d12/d3d12_presenter.h` | 新增 `uint8_t dlss_sr_backend_perf_quality_ = 0xFF;` 哨兵成员（255=从未创建） |
| `src/xenia/ui/d3d12/d3d12_presenter.cc` | 惰性创建处：计算 `requested_quality`；质量不匹配时 `dlss_sr_backend_.reset()`（析构触发 ReleaseFeature+SDK Shutdown）并记录日志；创建时传入配置质量并更新哨兵 |

## C. 验证证据

- **构建**：`.\xb build --config release` → `Success!`（exit 0）。全部 18 个编译/链接步骤通过，含 `emulator_window.cc.obj`、`xenia-ui.lib`、`xenia-ui-d3d12.lib`。
- **测试构建**：`.\xb build --config release --build-tests --target xenia-ui-tests` → `Success!`，产出 `xenia-ui-tests.exe`。
- **主机侧测试**：`xenia-ui-tests.exe` → **All tests passed (45 assertions in 15 test cases)**。`GuestOutputEnhancementConfig` 新增字段默认值不变，既有 `SelectOutputPath` / 回退逻辑用例全绿。
- 未执行 GPU 命令、未加载任何 NVIDIA DLL、未在实机运行。

## D. 环境限制

Hyper-V 无 NVIDIA 驱动/真实 GPU。未验证：滑块实际渲染、cvar 持久化往返、质量变更触发的后端重建（ReleaseFeature+Shutdown1+重 Init+CreateFeature 同帧序列）、各档位 CreateFeature 成功/回退。全部 `UNVERIFIED`。

## E. 剩余风险

1. **重建同帧安全性**：质量变更帧内先 `Shutdown1`（旧会话）再 `Init_with_ProjectID`+`CreateFeature`（新会话）。SDK 文档未明确禁止同帧 Shutdown→Init，理论上安全，但需 RTX 4090 实机确认无驱动侧异常。
2. **UltraPerformance/UltraQuality 可用性**：未查询 Optimal Settings，固定暴露 5 档；若某档被 SDK 判定不可用，CreateFeature 失败走确定性回退（原始路径+日志），UI 暂无灰显。
3. **cvar 非法值**：加载侧回退 `balanced`（不崩溃、不伪造），但 `xenia-ui-tests` 未链接 `xenia-app`，映射函数往返测试缺失（需扩展测试 CMake 链接 `xenia-app`，超出本轮 §4 范围）。
4. **许可证隔离**：`guest_output_enhancement.h` 仍无 `nvsdk_*` 依赖；后端映射处是唯一 int→枚举转换点。`dlss/*` 外 SDK 依赖为零。
5. **1:1 绕过**：guest 输出=交换链尺寸时后端不创建，滑块无效果且无提示（与现状一致）。

## F. 下一步（最小且安全）

1. 将 `xenia-ui-tests` 的 CMake `LINKS` 增加 `xenia-app`，补 cvar 映射函数往返（5 合法值 + 非法值回退）与 `GetGuestOutputEnhancementConfigForCvars()` 默认值断言。
2. RTX 4090 实机验证：滑块渲染与可见性链、各档位 CreateFeature 成功/回退、质量变更重建的日志序列与同帧 Shutdown→Init 安全性。

## G. 各项能力状态清单

- **DLSS SR**：[编译期已集成]（质量 UI 已实施并编译通过；运行时与画质 UNVERIFIED）
- **NVOF**：[未实现]
- **帧生成**：[未实现]
- **实验性神经渲染**：[未实现]
- **Presenter 集成**：[已接线且关闭]（主开关默认 OFF，后端未创建；质量哨兵默认 255=从未创建）
- **最终画质与性能**：[尚未评估]
