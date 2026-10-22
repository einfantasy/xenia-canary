/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/dlss/dlss_super_sampling_backend.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

// The NVIDIA SDK headers are included ONLY in this translation unit (license
// isolation). They trigger /W4 warnings, so they are wrapped in a warning
// suppression.
#pragma warning(push)
#pragma warning(disable : 4100 4127 4189 4244 4267 4310 4324 4456 4458 4514 4611 4820)
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_d3d.h>
#include <nvsdk_ngx_params.h>
#pragma warning(pop)

#include "xenia/base/logging.h"

namespace xe {
namespace ui {
namespace dlss {

namespace {

// nvsdk_ngx_defs.h only forward-declares CD3DX12_HEAP_PROPERTIES. The
// complete definition comes from a newer Windows SDK's d3d12.h, where it
// publicly derives from D3D12_HEAP_PROPERTIES. The official SDK sample
// (DLSS Programming Guide, section 6.2) passes InHeap straight to
// ID3D12Device::CreateCommittedResource, proving the first
// sizeof(D3D12_HEAP_PROPERTIES) bytes are a plain D3D12_HEAP_PROPERTIES.
// This machine's Windows SDK (10.0.26100.0) lacks the definition, so we
// provide it here: an empty extension of D3D12_HEAP_PROPERTIES, which lets
// the alloc callback upcast to const D3D12_HEAP_PROPERTIES*.
class CD3DX12_HEAP_PROPERTIES : public D3D12_HEAP_PROPERTIES {};

// The default D3D12 heap properties (CPU page unknown, memory pool unknown,
// heap flags none). Defined locally so this module does not depend on the
// D3D12 provider utilities (keeping the SDK module self-contained and avoiding
// a circular dependency with the presenter target).
const D3D12_HEAP_PROPERTIES kDefaultHeapProperties = {
    D3D12_HEAP_TYPE_DEFAULT};

// The resource allocation callback is a C function pointer with no way to pass
// the device. The backend is a single instance per device, so a file-scope
// device pointer (set in Initialize, cleared in Shutdown) is safe.
ID3D12Device* g_ngx_device = nullptr;

// The app implements this so the SDK can allocate its internal resources.
// (SDK guide section 6.2.)
void NVSDK_CONV NGXResourceAllocCallback(D3D12_RESOURCE_DESC* in_desc,
                                        int in_state,
                                        CD3DX12_HEAP_PROPERTIES* in_heap,
                                        ID3D12Resource** out_resource) {
  if (g_ngx_device && in_desc && in_heap && out_resource) {
    g_ngx_device->CreateCommittedResource(
        in_heap, D3D12_HEAP_FLAG_NONE, in_desc,
        static_cast<D3D12_RESOURCE_STATES>(in_state), nullptr,
        IID_PPV_ARGS(out_resource));
  }
}

void NVSDK_CONV NGXResourceReleaseCallback(IUnknown* in_resource) {
  if (in_resource) {
    in_resource->Release();
  }
}

// ---------------------------------------------------------------------------
// SEH boundaries.
//
// The SDK can crash (access violation), not just return an error code. So every
// SDK entry point is wrapped in __try/__except. A crash is treated as a failure
// (NVSDK_NGX_Result_FAIL_PlatformError), which triggers the deterministic
// fallback. The __try scope must not contain any C++ object with a destructor,
// so these are free functions with only POD locals.
// ---------------------------------------------------------------------------

__declspec(noinline) NVSDK_NGX_Result CallGetFeatureRequirements(
    IDXGIAdapter* adapter, const NVSDK_NGX_FeatureDiscoveryInfo* discovery,
    NVSDK_NGX_FeatureRequirement* out_supported, uint32_t& seh_code) {
  seh_code = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  __try {
    result = NVSDK_NGX_D3D12_GetFeatureRequirements(adapter, discovery,
                                                   out_supported);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
    result = NVSDK_NGX_Result_FAIL_PlatformError;
  }
  return result;
}

__declspec(noinline) NVSDK_NGX_Result CallInitWithProjectId(
    const char* project_id, const wchar_t* app_data_path,
    const wchar_t* const* path_list, unsigned int path_list_length,
    ID3D12Device* device, uint32_t& seh_code) {
  seh_code = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  NVSDK_NGX_FeatureCommonInfo common_info{};
  common_info.PathListInfo.Path =
      const_cast<const wchar_t**>(const_cast<const wchar_t* const*>(path_list));
  common_info.PathListInfo.Length = path_list_length;
  __try {
    result = NVSDK_NGX_D3D12_Init_with_ProjectID(
        project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", app_data_path, device,
        &common_info, NVSDK_NGX_Version_API);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
    result = NVSDK_NGX_Result_FAIL_PlatformError;
  }
  return result;
}

__declspec(noinline) NVSDK_NGX_Result CallAllocateParameters(
    NVSDK_NGX_Parameter** out_parameters, uint32_t& seh_code) {
  seh_code = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  __try {
    result = NVSDK_NGX_D3D12_AllocateParameters(out_parameters);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
    result = NVSDK_NGX_Result_FAIL_PlatformError;
  }
  return result;
}

__declspec(noinline) NVSDK_NGX_Result CallDestroyParameters(
    NVSDK_NGX_Parameter* parameters, uint32_t& seh_code) {
  seh_code = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  __try {
    result = NVSDK_NGX_D3D12_DestroyParameters(parameters);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
    result = NVSDK_NGX_Result_FAIL_PlatformError;
  }
  return result;
}

__declspec(noinline) NVSDK_NGX_Result CallCreateDlss(
    ID3D12GraphicsCommandList* command_list, NVSDK_NGX_Handle** out_handle,
    NVSDK_NGX_Parameter* parameter_block,
    NVSDK_NGX_DLSS_Create_Params* create_params, uint32_t& seh_code) {
  seh_code = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  __try {
    result = NGX_D3D12_CREATE_DLSS_EXT(command_list, 1, 1, out_handle,
                                      parameter_block, create_params);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
    result = NVSDK_NGX_Result_FAIL_PlatformError;
  }
  return result;
}

__declspec(noinline) NVSDK_NGX_Result CallEvaluateDlss(
    ID3D12GraphicsCommandList* command_list, NVSDK_NGX_Handle* handle,
    NVSDK_NGX_Parameter* parameter_block,
    NVSDK_NGX_D3D12_DLSS_Eval_Params* eval_params, uint32_t& seh_code) {
  seh_code = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  __try {
    result = NGX_D3D12_EVALUATE_DLSS_EXT(command_list, handle, parameter_block,
                                        eval_params);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
    result = NVSDK_NGX_Result_FAIL_PlatformError;
  }
  return result;
}

__declspec(noinline) NVSDK_NGX_Result CallReleaseFeature(
    NVSDK_NGX_Handle* handle, uint32_t& seh_code) {
  seh_code = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  __try {
    result = NVSDK_NGX_D3D12_ReleaseFeature(handle);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
    result = NVSDK_NGX_Result_FAIL_PlatformError;
  }
  return result;
}

__declspec(noinline) NVSDK_NGX_Result CallShutdown1(ID3D12Device* device,
                                                    uint32_t& seh_code) {
  seh_code = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  __try {
    result = NVSDK_NGX_D3D12_Shutdown1(device);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
    result = NVSDK_NGX_Result_FAIL_PlatformError;
  }
  return result;
}

// Maps an NVSDK_NGX_Result to a BackendResult for initialization failures.
BackendResult InitFailureToBackendResult(NVSDK_NGX_Result result) {
  switch (result) {
    case NVSDK_NGX_Result_Success:
      return BackendResult::kSuccess;
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported:
    case NVSDK_NGX_Result_FAIL_PlatformError:
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature:
      // Capability / platform / init failure: cache as unavailable.
      return BackendResult::kUnavailable;
    default:
      // Any other init error (invalid parameter, out of memory, ...).
      return BackendResult::kInitFailed;
  }
}

}  // namespace

DLSSSuperSamplingBackend::DLSSSuperSamplingBackend(
    ID3D12Device* device, std::vector<std::wstring> runtime_dirs)
    : device_(device), runtime_dirs_(std::move(runtime_dirs)) {}

DLSSSuperSamplingBackend::~DLSSSuperSamplingBackend() { Shutdown(); }

bool DLSSSuperSamplingBackend::IsAvailable() const {
  return state_ == State::kAvailable;
}

bool DLSSSuperSamplingBackend::IsUnavailable() const {
  return state_ == State::kUnavailable;
}

ID3D12Resource* DLSSSuperSamplingBackend::OutputResource() const {
  return output_texture_.Get();
}

uint32_t DLSSSuperSamplingBackend::OutputWidth() const {
  return output_width_;
}

uint32_t DLSSSuperSamplingBackend::OutputHeight() const {
  return output_height_;
}

void DLSSSuperSamplingBackend::Shutdown() {
  if (state_ == State::kNotInitialized) {
    return;
  }
  XELOGI("DLSS SR: backend shutdown (input={}x{}, output={}x{}).",
         input_width_, input_height_, output_width_, output_height_);
  ReleaseFeature();
  uint32_t seh_code = 0;
  CallShutdown1(device_, seh_code);
  g_ngx_device = nullptr;
  output_texture_.Reset();
  output_in_pixel_shader_resource_ = false;
  input_width_ = 0;
  input_height_ = 0;
  output_width_ = 0;
  output_height_ = 0;
  state_ = State::kNotInitialized;
}

void DLSSSuperSamplingBackend::ReleaseFeature() {
  if (feature_handle_) {
    uint32_t seh_code = 0;
    CallReleaseFeature(feature_handle_, seh_code);
    feature_handle_ = nullptr;
  }
  if (parameter_block_) {
    uint32_t seh_code = 0;
    CallDestroyParameters(parameter_block_, seh_code);
    parameter_block_ = nullptr;
  }
}

BackendResult DLSSSuperSamplingBackend::Initialize(
    ID3D12GraphicsCommandList* command_list, uint32_t input_width,
    uint32_t input_height, uint32_t output_width, uint32_t output_height) {
  if (!device_ || !command_list) {
    return BackendResult::kInputContractFailed;
  }

  // 1. Capability check (CPU side, before any command is recorded).
  Microsoft::WRL::ComPtr<IDXGIDevice1> dxgi_device;
  if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) {
    return BackendResult::kInitFailed;
  }
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  if (FAILED(dxgi_device->GetAdapter(adapter.GetAddressOf()))) {
    return BackendResult::kInitFailed;
  }
  // Build the PathList from the runtime directories. The SDK does not search
  // recursively, so each directory that may contain an NGX runtime DLL must be
  // listed explicitly.
  std::vector<const wchar_t*> path_list;
  path_list.reserve(runtime_dirs_.size());
  for (const std::wstring& runtime_dir : runtime_dirs_) {
    path_list.push_back(runtime_dir.c_str());
  }
  const uint32_t path_list_length = static_cast<uint32_t>(path_list.size());
  if (!path_list_length) {
    return BackendResult::kInitFailed;
  }
  NVSDK_NGX_FeatureCommonInfo discovery_common{};
  discovery_common.PathListInfo.Path =
      const_cast<const wchar_t**>(path_list.data());
  discovery_common.PathListInfo.Length = path_list_length;
  NVSDK_NGX_FeatureDiscoveryInfo discovery{};
  discovery.SDKVersion = NVSDK_NGX_Version_API;
  discovery.FeatureID = NVSDK_NGX_Feature_SuperSampling;
  discovery.Identifier.IdentifierType =
      NVSDK_NGX_Application_Identifier_Type_Project_Id;
  discovery.Identifier.v.ProjectDesc.ProjectId = "xenia-dlss-sr";
  discovery.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
  discovery.Identifier.v.ProjectDesc.EngineVersion = "1.0";
  discovery.ApplicationDataPath = L".";
  discovery.FeatureInfo = &discovery_common;
  NVSDK_NGX_FeatureRequirement requirement{};
  uint32_t seh_code = 0;
  NVSDK_NGX_Result requirement_result = CallGetFeatureRequirements(
      adapter.Get(), &discovery, &requirement, seh_code);
  if (requirement_result != NVSDK_NGX_Result_Success ||
      requirement.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
    // Capability check failed: the GPU / driver does not support DLSS SR.
    XELOGE("DLSS SR: capability check failed (result={}, supported={}, "
           "seh=0x{:08X}); GPU/driver/runtime does not support DLSS SR. Caching "
           "as unavailable (no retry).",
           static_cast<int>(requirement_result),
           static_cast<int>(requirement.FeatureSupported), seh_code);
    state_ = State::kUnavailable;
    return BackendResult::kUnavailable;
  }

  // 2. Initialize the SDK session (CPU side).
  NVSDK_NGX_Result init_result = CallInitWithProjectId(
      "xenia-dlss-sr", L".", path_list.data(), path_list_length, device_,
      seh_code);
  if (init_result != NVSDK_NGX_Result_Success) {
    XELOGE("DLSS SR: SDK Init failed (result={}, seh=0x{:08X}).",
           static_cast<int>(init_result), seh_code);
    state_ = State::kUnavailable;
    return InitFailureToBackendResult(init_result);
  }
  g_ngx_device = device_;

  // 3. Allocate the parameter block (CPU side).
  NVSDK_NGX_Result alloc_result = CallAllocateParameters(&parameter_block_,
                                                        seh_code);
  if (alloc_result != NVSDK_NGX_Result_Success || !parameter_block_) {
    XELOGE("DLSS SR: parameter block allocation failed (result={}, "
           "seh=0x{:08X}).",
           static_cast<int>(alloc_result), seh_code);
    state_ = State::kUnavailable;
    return InitFailureToBackendResult(alloc_result);
  }
  // Register the resource allocation / release callbacks so the SDK can
  // allocate its internal resources. (SDK guide section 6.2.)
  parameter_block_->Set(NVSDK_NGX_Parameter_ResourceAllocCallback,
                        reinterpret_cast<unsigned long long>(
                            &NGXResourceAllocCallback));
  parameter_block_->Set(NVSDK_NGX_Parameter_ResourceReleaseCallback,
                        reinterpret_cast<unsigned long long>(
                            &NGXResourceReleaseCallback));

  // 4. Create the backend output texture (ALLOW_UNORDERED_ACCESS, UAV state).
  D3D12_RESOURCE_DESC output_desc{};
  output_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  output_desc.Alignment = 0;
  output_desc.Width = output_width;
  output_desc.Height = output_height;
  output_desc.DepthOrArraySize = 1;
  output_desc.MipLevels = 1;
  output_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  output_desc.SampleDesc.Count = 1;
  output_desc.SampleDesc.Quality = 0;
  output_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (FAILED(device_->CreateCommittedResource(
          &kDefaultHeapProperties, D3D12_HEAP_FLAG_NONE, &output_desc,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
          IID_PPV_ARGS(&output_texture_)))) {
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  output_in_pixel_shader_resource_ = false;  // starts in UAV state.

  // 5. Record CreateFeature into the command list (GPU command, first command
  //    group). IsHDR=0 (guest output is R10G10B10A2_UNORM perceived LDR),
  //    AutoExposure enabled (the SDK's estimator handles exposure).
  NVSDK_NGX_DLSS_Create_Params create_params{};
  create_params.Feature.InWidth = input_width;
  create_params.Feature.InHeight = input_height;
  create_params.Feature.InTargetWidth = output_width;
  create_params.Feature.InTargetHeight = output_height;
  create_params.Feature.InPerfQualityValue =
      NVSDK_NGX_PerfQuality_Value_Balanced;
  create_params.InFeatureCreateFlags =
      static_cast<int>(NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);
  create_params.InEnableOutputSubrects = 0;
  NVSDK_NGX_Result create_result = CallCreateDlss(command_list, &feature_handle_,
                                                 parameter_block_,
                                                 &create_params, seh_code);
  if (create_result != NVSDK_NGX_Result_Success || !feature_handle_) {
    // GPU-side CreateFeature failure: the presenter will Reset the command
    // list. Cache as unavailable so we do not retry the SDK.
    XELOGE("DLSS SR: CreateFeature (GPU) failed (result={}, seh=0x{:08X}). "
           "Caching as unavailable.",
           static_cast<int>(create_result), seh_code);
    ReleaseFeature();
    state_ = State::kUnavailable;
    return InitFailureToBackendResult(create_result);
  }

  input_width_ = input_width;
  input_height_ = input_height;
  output_width_ = output_width;
  output_height_ = output_height;
  state_ = State::kAvailable;
  XELOGI("DLSS SR: backend initialized (input={}x{}, output={}x{}, "
         "PerfQuality=Balanced).",
         input_width, input_height, output_width, output_height);
  return BackendResult::kSuccess;
}

BackendResult DLSSSuperSamplingBackend::TryProcess(
    ID3D12GraphicsCommandList* command_list, ID3D12Resource* guest_output,
    uint32_t output_width, uint32_t output_height,
    const Presenter::GuestOutputProperties& guest_output_properties,
    const DlssSrFrameParams& frame_params) {
  // 0. Cached unavailable: do not retry the SDK.
  if (state_ == State::kUnavailable) {
    return BackendResult::kUnavailable;
  }
  if (!command_list || !guest_output) {
    return BackendResult::kInputContractFailed;
  }
  const uint32_t input_width = guest_output_properties.frontbuffer_width;
  const uint32_t input_height = guest_output_properties.frontbuffer_height;
  // Input contract: all sizes must be non-zero.
  if (!input_width || !input_height || !output_width || !output_height) {
    return BackendResult::kInputContractFailed;
  }
  // 1:1 bypass: the caller (presenter) should skip the backend in this case,
  // but guard here as well.
  if (input_width == output_width && input_height == output_height) {
    return BackendResult::kInputContractFailed;
  }

  // Lazy init / re-create on size change.
  bool just_initialized = false;
  if (state_ == State::kNotInitialized ||
      input_width != input_width_ || input_height != input_height_ ||
      output_width != output_width_ || output_height != output_height_) {
    if (state_ == State::kAvailable) {
      // Size changed: release the feature and re-create.
      ReleaseFeature();
      output_texture_.Reset();
      state_ = State::kNotInitialized;
    }
    BackendResult init_result =
        Initialize(command_list, input_width, input_height, output_width,
                   output_height);
    if (init_result != BackendResult::kSuccess) {
      return init_result;
    }
    // After a fresh Initialize the output texture is in UAV state, so the first
    // evaluate does not need a "to UAV" barrier.
    output_in_pixel_shader_resource_ = false;
    just_initialized = true;
  }

  // The backend is now available and sized correctly. Record the per-frame
  // commands (barriers + EvaluateFeature) into the command list.

  // Barrier 1: guest output PIXEL_SHADER_RESOURCE -> NON_PIXEL_SHADER_RESOURCE
  // (the SDK's compute shader reads the input).
  D3D12_RESOURCE_BARRIER barrier_input{};
  barrier_input.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier_input.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier_input.Transition.pResource = guest_output;
  barrier_input.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier_input.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  barrier_input.Transition.StateAfter =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  command_list->ResourceBarrier(1, &barrier_input);

  // Barrier 2 (not the first frame): backend output PIXEL_SHADER_RESOURCE ->
  // UNORDERED_ACCESS (the SDK writes the output in UAV state).
  if (output_in_pixel_shader_resource_) {
    D3D12_RESOURCE_BARRIER barrier_output_to_uav{};
    barrier_output_to_uav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier_output_to_uav.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier_output_to_uav.Transition.pResource = output_texture_.Get();
    barrier_output_to_uav.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier_output_to_uav.Transition.StateBefore =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier_output_to_uav.Transition.StateAfter =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    command_list->ResourceBarrier(1, &barrier_output_to_uav);
    output_in_pixel_shader_resource_ = false;
  }

  // Record EvaluateFeature (GPU command). The SDK reads the input (in
  // NON_PIXEL_SHADER_RESOURCE), writes the output (in UAV), and restores the
  // input to NON_PIXEL_SHADER_RESOURCE and the output to UAV.
  NVSDK_NGX_D3D12_DLSS_Eval_Params eval_params{};
  eval_params.Feature.pInColor = guest_output;
  eval_params.Feature.pInOutput = output_texture_.Get();
  eval_params.pInDepth = nullptr;
  eval_params.pInMotionVectors = nullptr;
  eval_params.InJitterOffsetX = frame_params.jitter_x;
  eval_params.InJitterOffsetY = frame_params.jitter_y;
  eval_params.InReset = frame_params.in_reset ? 1 : 0;
  eval_params.InMVScaleX = 1.0f;
  eval_params.InMVScaleY = 1.0f;
  eval_params.InPreExposure = 1.0f;
  eval_params.InExposureScale = 1.0f;
  eval_params.InRenderSubrectDimensions.Width = input_width;
  eval_params.InRenderSubrectDimensions.Height = input_height;
  uint32_t seh_code = 0;
  NVSDK_NGX_Result eval_result =
      CallEvaluateDlss(command_list, feature_handle_, parameter_block_,
                      &eval_params, seh_code);
  if (eval_result != NVSDK_NGX_Result_Success) {
    // GPU-side EvaluateFeature failure: the presenter will Reset the command
    // list. The guest output and the backend output are still in their pre-
    // evaluate states (the barriers were recorded but not executed).
    XELOGE("DLSS SR: EvaluateFeature (GPU) failed (result={}, seh=0x{:08X}).",
           static_cast<int>(eval_result), seh_code);
    return BackendResult::kExecutionFailed;
  }

  // Barrier 3: guest output NON_PIXEL_SHADER_RESOURCE -> PIXEL_SHADER_RESOURCE
  // (the SDK restored it to NON_PIXEL_SHADER_RESOURCE; the next frame's
  // refresher expects it back in PIXEL_SHADER_RESOURCE).
  D3D12_RESOURCE_BARRIER barrier_input_back{};
  barrier_input_back.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier_input_back.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier_input_back.Transition.pResource = guest_output;
  barrier_input_back.Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier_input_back.Transition.StateBefore =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  barrier_input_back.Transition.StateAfter =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  command_list->ResourceBarrier(1, &barrier_input_back);

  // Barrier 4: backend output UAV -> PIXEL_SHADER_RESOURCE (the paint chain's
  // first effect reads it via an SRV; this is the write-after-read barrier).
  D3D12_RESOURCE_BARRIER barrier_output_back{};
  barrier_output_back.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier_output_back.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier_output_back.Transition.pResource = output_texture_.Get();
  barrier_output_back.Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier_output_back.Transition.StateBefore =
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  barrier_output_back.Transition.StateAfter =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  command_list->ResourceBarrier(1, &barrier_output_back);
  output_in_pixel_shader_resource_ = true;

  if (just_initialized) {
    XELOGI("DLSS SR: first frame evaluated (input={}x{} -> output={}x{}).",
           input_width, input_height, output_width, output_height);
  }
  return BackendResult::kSuccess;
}

}  // namespace dlss
}  // namespace ui
}  // namespace xe
