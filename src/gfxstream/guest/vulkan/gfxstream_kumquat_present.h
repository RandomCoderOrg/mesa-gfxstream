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

#ifdef __cplusplus
}
#endif

#endif
