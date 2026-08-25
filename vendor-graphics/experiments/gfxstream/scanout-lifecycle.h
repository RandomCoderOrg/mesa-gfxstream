/* SPDX-License-Identifier: MIT */

#ifndef UDROID_SCANOUT_LIFECYCLE_H
#define UDROID_SCANOUT_LIFECYCLE_H

#include "ahb-info-wire.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UDROID_SCANOUT_MAX_RESOURCES 256U
#define UDROID_SCANOUT_MAX_OUTPUTS 16U

struct udroid_scanout_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

struct udroid_scanout_callbacks {
    int (*import_ahb)(void *userdata,
                      const struct udroid_ahb_info *info,
                      void **out_import);
    void (*release_import)(void *userdata, void *imported);
    /*
     * If presentation outlives this call, the backend must retain its own
     * reference before returning and release it after Android signals buffer
     * completion. The lifecycle cache may retire its reference immediately
     * after a resource replacement or destroy event.
     */
    int (*present)(void *userdata,
                   uint32_t output_id,
                   uint32_t surface_generation,
                   void *imported,
                   const struct udroid_scanout_rect *rect);
};

struct udroid_scanout_resource {
    bool occupied;
    uint32_t resource_id;
    uint32_t generation;
    void *imported;
};

struct udroid_scanout_output {
    bool assigned;
    bool surface_attached;
    uint32_t resource_id;
    uint32_t surface_generation;
    struct udroid_scanout_rect rect;
};

struct udroid_scanout_lifecycle {
    const struct udroid_scanout_callbacks *callbacks;
    void *userdata;
    struct udroid_scanout_resource resources[UDROID_SCANOUT_MAX_RESOURCES];
    struct udroid_scanout_output outputs[UDROID_SCANOUT_MAX_OUTPUTS];
};

int udroid_scanout_lifecycle_init(
    struct udroid_scanout_lifecycle *lifecycle,
    const struct udroid_scanout_callbacks *callbacks,
    void *userdata);

/* Import first, then atomically replace a strictly older generation. */
int udroid_scanout_publish_resource(
    struct udroid_scanout_lifecycle *lifecycle,
    const struct udroid_ahb_info *info);

/* Release one imported resource and detach every output that selected it. */
int udroid_scanout_destroy_resource(
    struct udroid_scanout_lifecycle *lifecycle,
    uint32_t resource_id);

/* A resource_id of zero disables the output, matching virtio-gpu semantics. */
int udroid_scanout_assign(struct udroid_scanout_lifecycle *lifecycle,
                          uint32_t output_id,
                          uint32_t resource_id,
                          const struct udroid_scanout_rect *rect);

/* A new generation invalidates presents queued against an older Android Surface. */
int udroid_scanout_set_surface(struct udroid_scanout_lifecycle *lifecycle,
                              uint32_t output_id,
                              bool attached,
                              uint32_t surface_generation);

/* Present only outputs selecting exactly this live resource generation. */
int udroid_scanout_flush(struct udroid_scanout_lifecycle *lifecycle,
                         uint32_t resource_id,
                         uint32_t resource_generation);

void udroid_scanout_lifecycle_release(
    struct udroid_scanout_lifecycle *lifecycle);

#ifdef __cplusplus
}
#endif

#endif
