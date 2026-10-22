/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_DLSS_DLSS_SUPER_SAMPLING_BACKEND_H_
#define XENIA_UI_DLSS_DLSS_SUPER_SAMPLING_BACKEND_H_

#include <cstdint>
#include <string>
#include <vector>

// Must come BEFORE <wrl/client.h>: guest_output_enhancement.h -> presenter.h
// pulls in math.h (which breaks if min/max macros are defined) and later
// sets NOMINMAX before windows.h is reached. Including wrl/client.h first
// would let windows.h define the min/max macros too early (C2589).
#include "xenia/ui/guest_output_enhancement.h"

// ComPtr is available transitively through the chain above, but the backend
// owns a ComPtr member directly, so include it explicitly (after the Xenia
// header, to preserve the include order above).
#include <wrl/client.h>

// Forward declarations. The D3D12 types are used only as pointers in this
// header; the full d3d12.h is included in the .cc. The NVSDK_NGX_* types are
// SDK handles (opaque pointers) forward-declared here so that the SDK headers
// are NOT pulled into this header (license isolation: the SDK dependency is
// confined to the dlss/* module).
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace xe {
namespace ui {
namespace dlss {

// A real DLSS Super Resolution backend that wraps the NVIDIA NGX SDK.
//
// This module is the ONLY place in the codebase that depends on the NVIDIA SDK
// (license isolation). The presenter holds an instance of this class through
// the IDLSSSuperSamplingBackend interface and never includes any nvsdk_* header
// itself. If the SDK license (section 4e) is ultimately ruled incompatible with
// distribution, this entire dlss/* module can be deleted with zero changes to
// the rest of the codebase.
//
// Ownership contract:
//   - The backend OWNS its own output texture (created with
//     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, starting in UAV state) and
//     the SDK feature handle + parameter block.
//   - The backend BORROWS the guest output (read-only). It never takes over
//     guest_output_resource_paint_refs and never destroys the guest output.
//   - All SDK entry points (Init / AllocateParameters / CreateFeature /
//     EvaluateFeature / ReleaseFeature / Shutdown) are wrapped in SEH
//     boundaries, because the SDK can crash (access violation), not just return
//     an error code. A crash is treated as a failure (deterministic fallback).
//
// Lazy initialization: the first TryProcess call creates the SDK session
// (capability check + Init_with_ProjectID + AllocateParameters + CreateFeature,
// the last recorded as a GPU command into the caller's command list). If that
// first creation fails, the backend is cached as unavailable (kUnavailable) and
// subsequent frames do not retry the SDK.
//
// Command submission semantics: the command list is a recording buffer that is
// executed once (ExecuteCommandLists) at the end of the frame. On failure the
// backend does NOT reset the command list itself - that is the presenter's
// responsibility. The backend only records commands; the presenter decides when
// to execute and when to reset.
class DLSSSuperSamplingBackend final : public IDLSSSuperSamplingBackend {
 public:
  // `device` is the D3D12 device; `runtime_dir` is the directory containing the
  // NGX runtime DLLs (nvngxruntime.dll, nvngx_dlss.dll), passed to the SDK via
  // NVSDK_NGX_FeatureCommonInfo.PathListInfo.Path. Neither is used until the
  // backend is actually initialized (lazy).
  // `runtime_dirs` is the list of directories to search for the NGX runtime
  // DLLs (nvngxruntime.dll, nvngx_dlss.dll). They are passed to the SDK via
  // NVSDK_NGX_FeatureCommonInfo.PathListInfo.Path. None are used until the
  // backend is actually initialized (lazy).
  DLSSSuperSamplingBackend(ID3D12Device* device,
                           std::vector<std::wstring> runtime_dirs);
  ~DLSSSuperSamplingBackend() override;

  DLSSSuperSamplingBackend(const DLSSSuperSamplingBackend&) = delete;
  DLSSSuperSamplingBackend& operator=(const DLSSSuperSamplingBackend&) = delete;

  bool IsAvailable() const override;
  // True when the first creation failed and the backend will not retry the SDK.
  bool IsUnavailable() const;

  BackendResult TryProcess(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* guest_output,
      uint32_t output_width,
      uint32_t output_height,
      const Presenter::GuestOutputProperties& guest_output_properties,
      const DlssSrFrameParams& frame_params) override;

  // Getters for the backend output. The presenter uses these to create the SRV
  // (in its view heap) and to know the output size. The output texture is owned
  // by the backend.
  ID3D12Resource* OutputResource() const override;
  uint32_t OutputWidth() const override;
  uint32_t OutputHeight() const override;

  void Shutdown() override;

 private:
  enum class State { kNotInitialized, kAvailable, kUnavailable };

  // Creates the SDK session and the output texture. Records CreateFeature into
  // `command_list`. Returns kSuccess, or a failure (kUnavailable when the
  // capability check / Init fails, kInitFailed for other init errors).
  BackendResult Initialize(ID3D12GraphicsCommandList* command_list,
                           uint32_t input_width, uint32_t input_height,
                           uint32_t output_width, uint32_t output_height);
  // Releases the SDK feature and parameter block. Does NOT release the output
  // texture (that is done by Shutdown / the destructor).
  void ReleaseFeature();

  ID3D12Device* device_;
  std::vector<std::wstring> runtime_dirs_;

  State state_ = State::kNotInitialized;
  // The backend's identity: input size x output size. Any change requires a
  // re-create (the DLSS feature is created for a specific size pair).
  uint32_t input_width_ = 0;
  uint32_t input_height_ = 0;
  uint32_t output_width_ = 0;
  uint32_t output_height_ = 0;

  // Backend-owned output texture (ALLOW_UNORDERED_ACCESS, starts in UAV state).
  Microsoft::WRL::ComPtr<ID3D12Resource> output_texture_;
  // True after the per-frame final barrier transitions the output texture to
  // PIXEL_SHADER_RESOURCE. Used to decide whether a "to UAV" barrier is needed
  // before the next evaluate. Initially false (the output starts in UAV).
  bool output_in_pixel_shader_resource_ = false;

  // True after the first input-contract failure has been logged. The early
  // contract checks (null pointer / zero size / 1:1) fire every frame, so the
  // failure is logged only once to avoid per-frame spam. Diagnostic only -
  // the presenter separately logs the fallback this frame.
  bool contract_failure_logged_ = false;

  // SDK-owned (allocated by the SDK, released via the SDK).
  NVSDK_NGX_Handle* feature_handle_ = nullptr;
  NVSDK_NGX_Parameter* parameter_block_ = nullptr;
};

}  // namespace dlss
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_DLSS_DLSS_SUPER_SAMPLING_BACKEND_H_
