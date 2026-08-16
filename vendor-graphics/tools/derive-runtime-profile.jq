def writable_access:
  [.appDomain.deviceAccess[] | select(.exists and .readable and .writable) | .path];

def has_path_prefix($paths; $prefix):
  any($paths[]; startswith($prefix));

. as $baseline
| writable_access as $writable
| ($baseline.android.abiList | any(. == "arm64-v8a")) as $has_arm64
| ($baseline.discovery.vulkanHalCandidates | length > 0) as $has_hal
| ([$baseline.discovery.vulkanHalCandidates[].path |
     select(contains("/lib64/"))]) as $aarch64_hals
| has_path_prefix($writable; "/dev/dri/renderD") as $has_drm_render
| (has_path_prefix($writable; "/dev/mali") or
   has_path_prefix($writable; "/dev/kgsl") or
   has_path_prefix($writable; "/dev/pvr")) as $has_vendor_gpu
| (has_path_prefix($writable; "/dev/dma_heap/") or
   any($writable[]; . == "/dev/ion")) as $has_direct_allocator
| {
    schema: 1,
    sourceDeviceId: $baseline.deviceId,
    eligibility: {
      aarch64Guest: $has_arm64,
      ahardwareBufferApi: ($baseline.android.sdk >= 26),
      vulkanHalDiscovered: $has_hal,
      abiCompatibleVulkanHalDiscovered: ($aarch64_hals | length > 0),
      gpuDeviceAccessible: ($has_drm_render or $has_vendor_gpu),
      directAllocatorAccessible: $has_direct_allocator
    },
    selectedRoute: (
      if $has_arm64 and $has_drm_render then "standard-drm"
      elif $has_arm64 and ($baseline.android.sdk >= 26) and
           ($aarch64_hals | length > 0) and $has_vendor_gpu
        then "vendor-vulkan-ahb"
      else "probe-required"
      end
    ),
    gpuInterface: (
      if $has_drm_render then "drm-render-node"
      elif has_path_prefix($writable; "/dev/mali") then "mali-kbase"
      elif has_path_prefix($writable; "/dev/kgsl") then "qualcomm-kgsl"
      elif has_path_prefix($writable; "/dev/pvr") then "powervr-services"
      else "unknown"
      end
    ),
    allocationOrder: ([
      if $baseline.android.sdk >= 26 then "ahardwarebuffer" else empty end,
      if has_path_prefix($writable; "/dev/dma_heap/") then "dma-heap" else empty end,
      if any($writable[]; . == "/dev/ion") then "ion" else empty end
    ]),
    vulkanHal: {
      mode: "android-default-first",
      fallbackCandidates: $aarch64_hals
    },
    requiredRuntimeProbes: ([
      "libhybris-tls-isolation",
      if $baseline.android.sdk >= 26 then "ahardwarebuffer-provider" else empty end,
      "vulkan-device-lifecycle",
      "vulkan-xcb-present",
      "present-offload",
      "zink-feature-policy"
    ])
  }
