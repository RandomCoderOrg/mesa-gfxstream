/*
 * Copyright 2026 Google
 * SPDX-License-Identifier: MIT
 */

#ifndef GFXSTREAM_KUMQUAT_PRESENT_H
#define GFXSTREAM_KUMQUAT_PRESENT_H

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Experimental uDroid WSI boundary. The function always consumes a non-negative acquire fence.
 * On success, the caller owns the returned non-negative release fence and must wait/close it before
 * reusing the image. This is not a Vulkan extension. Development probes obtain it from the
 * gfxstream ICD's instance proc-address entrypoint by this exact function name.
 */
VKAPI_ATTR VkResult VKAPI_CALL gfxstream_kumquat_present_image(VkImage image, int acquireFenceFd,
                                                               int* releaseFenceFd);

/* Development probe for host-to-guest coherence of an exported Kumquat resource. */
VKAPI_ATTR VkResult VKAPI_CALL
gfxstream_kumquat_sync_memory_from_host(VkDeviceMemory memory, uint64_t size);

VKAPI_ATTR VkResult VKAPI_CALL gfxstream_kumquat_sync_image_from_host(
    VkDeviceMemory memory, uint32_t width, uint32_t height);

VKAPI_ATTR VkResult VKAPI_CALL
gfxstream_kumquat_send_memory_ahb_to_socket(VkDeviceMemory memory, int socketFd);

#ifdef __cplusplus
}
#endif

#endif
