"""Shared release-time contract for the rootless vendor graphics component."""

SOURCE_NAMES = ("mesa", "libhybris", "sysvk", "ginkage")

PROFILE_NAMES = (
    "vendor-vulkan:ginkage-ahb",
    "vendor-vulkan:ginkage-dmabuf-copy",
    "vendor-vulkan:ginkage-dmabuf-zero",
    "vendor-vulkan:shm",
)

REQUIRED_PATHS = (
    "sources.json",
    "THIRD_PARTY_NOTICES.md",
    "bin/udroid-gpu-run",
    "bin/udroid-gpu-probe",
    "lib/mesa/libEGL.so.1",
    "lib/mesa/libGL.so.1",
    "lib/mesa/libGLESv1_CM.so.1",
    "lib/mesa/libGLESv2.so.2",
    "lib/mesa/libglapi.so.0",
    "lib/mesa/dri/zink_dri.so",
    "lib/bridge/libahb-wrapper.so",
    "lib/bridge/libhardware.so.2",
    "lib/bridge/libhybris-common.so.1",
    "lib/bridge/libsysvk.so",
    "lib/bridge/libvulkan.so.1",
    "lib/bridge/libhybris/linker/q.so",
    "share/vulkan/icd.d/sysvk.json",
    "share/vulkan/explicit_layer.d/VkLayer_window_system_integration.json",
    "share/vulkan/explicit_layer.d/libVkLayer_window_system_integration.so",
    "libexec/egl-gles3-instancing",
    "libexec/vulkan-core-features",
    "libexec/run-wsi-qualification.sh",
    "libexec/x11-ahb-present-probe",
    "libexec/x11-buffer-transport-protocol",
    "libexec/x11-present-stats",
)

REQUIRED_EXECUTABLES = (
    "bin/udroid-gpu-run",
    "bin/udroid-gpu-probe",
    "libexec/egl-gles3-instancing",
    "libexec/vulkan-core-features",
    "libexec/run-wsi-qualification.sh",
    "libexec/x11-ahb-present-probe",
    "libexec/x11-buffer-transport-protocol",
    "libexec/x11-present-stats",
)
