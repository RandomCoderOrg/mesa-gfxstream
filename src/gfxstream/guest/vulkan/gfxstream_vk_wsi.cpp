/*
 * Copyright 2023 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "gfxstream_vk_entrypoints.h"
#include "gfxstream_kumquat_present.h"
#include "gfxstream_vk_private.h"
#include "wsi_common.h"

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
gfxstream_vk_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char* pName) {
    VK_FROM_HANDLE(gfxstream_vk_physical_device, pdevice, physicalDevice);
    return vk_instance_get_proc_addr_unchecked(&pdevice->instance->vk, pName);
}

static VkResult gfxstream_vk_wsi_send_memory_ahb_to_socket(
    VkDevice device, VkDeviceMemory memory, int socket_fd) {
    (void)device;
    return gfxstream_kumquat_send_memory_ahb_to_socket(memory, socket_fd);
}

static VkResult gfxstream_vk_wsi_sync_image_from_host(
    VkDevice device, VkDeviceMemory memory, uint32_t width, uint32_t height) {
    (void)device;
    return gfxstream_kumquat_sync_image_from_host(memory, width, height);
}

VkResult gfxstream_vk_wsi_init(struct gfxstream_vk_physical_device* physical_device) {
    VkResult result = (VkResult)0;

    const struct wsi_device_options options = {.sw_device = false};
    result = wsi_device_init(
        &physical_device->wsi_device, gfxstream_vk_physical_device_to_handle(physical_device),
        gfxstream_vk_wsi_proc_addr, &physical_device->instance->vk.alloc, -1, NULL, &options);
    if (result != VK_SUCCESS) return result;

    // Gfxstream exports image-backed DMA-BUFs but does not expose native DRM
    // format modifiers to the guest WSI.
    physical_device->wsi_device.supports_modifiers = false;
    // Native images are handed to socket-based X servers as their underlying
    // AHardwareBuffer instead of as CPU-readable DMA-BUFs.
    physical_device->wsi_device.supports_scanout = true;
    physical_device->wsi_device.x11.send_memory_ahb_to_socket =
        gfxstream_vk_wsi_send_memory_ahb_to_socket;
    physical_device->wsi_device.x11.sync_image_from_host =
        gfxstream_vk_wsi_sync_image_from_host;
    physical_device->wsi_device.x11.needs_external_image_ownership = true;

    // The generic dma-buf sync-file capability probe allocates anonymous
    // exportable memory. Gfxstream can export only image- or buffer-backed
    // resources, so reject that probe up front and let common WSI use the
    // image-backed implicit-sync path.
    physical_device->wsi_device.semaphore_export_handle_types &=
        ~VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;

    physical_device->vk.wsi_device = &physical_device->wsi_device;

    return result;
}

void gfxstream_vk_wsi_finish(struct gfxstream_vk_physical_device* physical_device) {
    physical_device->vk.wsi_device = NULL;
    wsi_device_finish(&physical_device->wsi_device, &physical_device->instance->vk.alloc);
}
