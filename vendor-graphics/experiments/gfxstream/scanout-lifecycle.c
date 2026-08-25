/* SPDX-License-Identifier: MIT */

#include "scanout-lifecycle.h"

#include <errno.h>
#include <string.h>

static struct udroid_scanout_resource *find_resource(
    struct udroid_scanout_lifecycle *lifecycle,
    uint32_t resource_id) {
    size_t i;

    for (i = 0; i < UDROID_SCANOUT_MAX_RESOURCES; ++i) {
        if (lifecycle->resources[i].occupied &&
            lifecycle->resources[i].resource_id == resource_id) {
            return &lifecycle->resources[i];
        }
    }
    return NULL;
}

static struct udroid_scanout_resource *find_free_resource(
    struct udroid_scanout_lifecycle *lifecycle) {
    size_t i;

    for (i = 0; i < UDROID_SCANOUT_MAX_RESOURCES; ++i) {
        if (!lifecycle->resources[i].occupied) {
            return &lifecycle->resources[i];
        }
    }
    return NULL;
}

int udroid_scanout_lifecycle_init(
    struct udroid_scanout_lifecycle *lifecycle,
    const struct udroid_scanout_callbacks *callbacks,
    void *userdata) {
    if (!lifecycle || !callbacks || !callbacks->import_ahb ||
        !callbacks->release_import || !callbacks->present) {
        errno = EINVAL;
        return -1;
    }

    memset(lifecycle, 0, sizeof(*lifecycle));
    lifecycle->callbacks = callbacks;
    lifecycle->userdata = userdata;
    return 0;
}

int udroid_scanout_publish_resource(
    struct udroid_scanout_lifecycle *lifecycle,
    const struct udroid_ahb_info *info) {
    struct udroid_scanout_resource *resource;
    void *imported = NULL;

    if (!lifecycle || !lifecycle->callbacks || !info ||
        info->resource_id == 0 || info->generation == 0 ||
        info->num_fds == 0 || info->metadata_size == 0 || !info->metadata) {
        errno = EINVAL;
        return -1;
    }

    resource = find_resource(lifecycle, info->resource_id);
    if (resource && resource->generation >= info->generation) {
        errno = resource->generation == info->generation ? EALREADY : ESTALE;
        return -1;
    }

    if (lifecycle->callbacks->import_ahb(lifecycle->userdata, info, &imported) !=
            0 ||
        !imported) {
        if (imported) {
            lifecycle->callbacks->release_import(lifecycle->userdata, imported);
        }
        if (errno == 0) {
            errno = EIO;
        }
        return -1;
    }

    if (!resource) {
        resource = find_free_resource(lifecycle);
        if (!resource) {
            lifecycle->callbacks->release_import(lifecycle->userdata, imported);
            errno = ENOSPC;
            return -1;
        }
    } else {
        lifecycle->callbacks->release_import(lifecycle->userdata,
                                             resource->imported);
    }

    resource->occupied = true;
    resource->resource_id = info->resource_id;
    resource->generation = info->generation;
    resource->imported = imported;
    return 0;
}

int udroid_scanout_destroy_resource(
    struct udroid_scanout_lifecycle *lifecycle,
    uint32_t resource_id) {
    struct udroid_scanout_resource *resource;
    size_t i;

    if (!lifecycle || !lifecycle->callbacks || resource_id == 0) {
        errno = EINVAL;
        return -1;
    }

    resource = find_resource(lifecycle, resource_id);
    if (!resource) {
        errno = ENOENT;
        return -1;
    }

    for (i = 0; i < UDROID_SCANOUT_MAX_OUTPUTS; ++i) {
        if (lifecycle->outputs[i].assigned &&
            lifecycle->outputs[i].resource_id == resource_id) {
            lifecycle->outputs[i].assigned = false;
            lifecycle->outputs[i].resource_id = 0;
            memset(&lifecycle->outputs[i].rect, 0,
                   sizeof(lifecycle->outputs[i].rect));
        }
    }

    lifecycle->callbacks->release_import(lifecycle->userdata,
                                         resource->imported);
    memset(resource, 0, sizeof(*resource));
    return 0;
}

int udroid_scanout_assign(struct udroid_scanout_lifecycle *lifecycle,
                          uint32_t output_id,
                          uint32_t resource_id,
                          const struct udroid_scanout_rect *rect) {
    struct udroid_scanout_output *output;

    if (!lifecycle || !lifecycle->callbacks ||
        output_id >= UDROID_SCANOUT_MAX_OUTPUTS) {
        errno = EINVAL;
        return -1;
    }

    output = &lifecycle->outputs[output_id];
    if (resource_id == 0) {
        output->assigned = false;
        output->resource_id = 0;
        memset(&output->rect, 0, sizeof(output->rect));
        return 0;
    }

    if (!rect || rect->width == 0 || rect->height == 0) {
        errno = EINVAL;
        return -1;
    }
    if (!find_resource(lifecycle, resource_id)) {
        errno = ENOENT;
        return -1;
    }

    output->assigned = true;
    output->resource_id = resource_id;
    output->rect = *rect;
    return 0;
}

int udroid_scanout_set_surface(struct udroid_scanout_lifecycle *lifecycle,
                              uint32_t output_id,
                              bool attached,
                              uint32_t surface_generation) {
    struct udroid_scanout_output *output;

    if (!lifecycle || !lifecycle->callbacks ||
        output_id >= UDROID_SCANOUT_MAX_OUTPUTS ||
        (attached && surface_generation == 0)) {
        errno = EINVAL;
        return -1;
    }

    output = &lifecycle->outputs[output_id];
    output->surface_attached = attached;
    output->surface_generation = attached ? surface_generation : 0;
    return 0;
}

int udroid_scanout_flush(struct udroid_scanout_lifecycle *lifecycle,
                         uint32_t resource_id,
                         uint32_t resource_generation) {
    struct udroid_scanout_resource *resource;
    size_t i;

    if (!lifecycle || !lifecycle->callbacks || resource_id == 0 ||
        resource_generation == 0) {
        errno = EINVAL;
        return -1;
    }

    resource = find_resource(lifecycle, resource_id);
    if (!resource) {
        errno = ENOENT;
        return -1;
    }
    if (resource->generation != resource_generation) {
        errno = ESTALE;
        return -1;
    }

    for (i = 0; i < UDROID_SCANOUT_MAX_OUTPUTS; ++i) {
        struct udroid_scanout_output *output = &lifecycle->outputs[i];

        if (!output->assigned || !output->surface_attached ||
            output->resource_id != resource_id) {
            continue;
        }
        if (lifecycle->callbacks->present(lifecycle->userdata, (uint32_t)i,
                                         output->surface_generation,
                                         resource->imported, &output->rect) != 0) {
            if (errno == 0) {
                errno = EIO;
            }
            return -1;
        }
    }
    return 0;
}

void udroid_scanout_lifecycle_release(
    struct udroid_scanout_lifecycle *lifecycle) {
    size_t i;

    if (!lifecycle || !lifecycle->callbacks) {
        return;
    }

    for (i = 0; i < UDROID_SCANOUT_MAX_RESOURCES; ++i) {
        if (lifecycle->resources[i].occupied) {
            lifecycle->callbacks->release_import(
                lifecycle->userdata, lifecycle->resources[i].imported);
        }
    }
    memset(lifecycle, 0, sizeof(*lifecycle));
}
