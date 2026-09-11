# Reuse ONLY the unchanged first-party adapter from the exact Foundation release.
# This does not bundle NVIDIA SDK files or relax adapter/runtime trust checks.
# Original release: v2026.911.85650.杂鱼, source 645465c54f9615c999452ebee04f2c0046446e2d.
# The preparation script verifies the full release archive and both embedded
# trust digests. Rebuild from the SDK instead if the C ABI changes.
set(_pinned_adapter_sha "bffcf3ba0586d8ed6ffde6574689157cb291163c010c5bff4b96052710060d74")
set(_pinned_runtime_sha "9a80575f247190c05fe80eac0c4baa1d0d4d932348f26808310b5ec4bf9eeb4b")
set(_pinned_abi_sha "89bf5415feef0aa3d248bfd5f3d68feeaad37daa9f0dafa1ac9ab9f80b8563b2")
get_filename_component(_pinned_root "${SUNSHINE_PINNED_RELEASE_ADAPTER_DIR}" ABSOLUTE)
set(RTX_VIDEO_ADAPTER_DLL "${_pinned_root}/foundation_rtx_video_adapter.dll")
if (NOT EXISTS "${RTX_VIDEO_ADAPTER_DLL}")
  message(FATAL_ERROR "The pinned first-party adapter is missing; RTX HDR will not be silently disabled")
endif ()
file(SHA256 "${RTX_VIDEO_ADAPTER_DLL}" _actual_adapter)
file(SHA256 "${CMAKE_SOURCE_DIR}/src/platform/windows/hdr_enhanced/nvidia_rtx_video/adapter_abi.h" _actual_abi)
if (NOT _actual_adapter STREQUAL _pinned_adapter_sha OR NOT _actual_abi STREQUAL _pinned_abi_sha)
  message(FATAL_ERROR "Pinned RTX adapter or host ABI does not match the verified release")
endif ()
set(RTX_VIDEO_TRUST_INCLUDE "${CMAKE_BINARY_DIR}/generated/rtx_video")
set(RTX_VIDEO_TRUST_HEADER "${RTX_VIDEO_TRUST_INCLUDE}/rtx_video_trust.h")
file(MAKE_DIRECTORY "${RTX_VIDEO_TRUST_INCLUDE}")
file(WRITE "${RTX_VIDEO_TRUST_HEADER}"
  "#pragma once\n#define SUNSHINE_RTX_VIDEO_ADAPTER_SHA256 \"${_pinned_adapter_sha}\"\n#define SUNSHINE_RTX_VIDEO_RUNTIME_SHA256 \"${_pinned_runtime_sha}\"\n")
add_custom_target(sunshine_rtx_video_adapter DEPENDS "${RTX_VIDEO_ADAPTER_DLL}" "${RTX_VIDEO_TRUST_HEADER}")
set(SUNSHINE_RTX_HDR_AVAILABLE TRUE CACHE INTERNAL "RTX HDR adapter is configured" FORCE)
message(STATUS "RTX HDR enabled with the verified unchanged first-party release adapter; original NVIDIA runtime import checks retained")
