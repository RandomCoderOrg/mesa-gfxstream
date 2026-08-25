/* SPDX-License-Identifier: MIT */

#include "scanout-lifecycle.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct fake_import {
    uint32_t resource_id;
    uint32_t generation;
};

struct test_state {
    struct fake_import imports[8];
    size_t import_count;
    size_t release_count;
    size_t present_count;
    uint32_t last_output;
    uint32_t last_surface_generation;
    uint32_t last_resource_generation;
};

static int import_ahb(void *userdata,
                      const struct udroid_ahb_info *info,
                      void **out_import) {
    struct test_state *state = userdata;
    struct fake_import *imported;

    assert(state->import_count < 8);
    imported = &state->imports[state->import_count++];
    imported->resource_id = info->resource_id;
    imported->generation = info->generation;
    *out_import = imported;
    return 0;
}

static void release_import(void *userdata, void *imported) {
    struct test_state *state = userdata;

    assert(imported != NULL);
    state->release_count++;
}

static int present(void *userdata,
                   uint32_t output_id,
                   uint32_t surface_generation,
                   void *imported,
                   const struct udroid_scanout_rect *rect) {
    struct test_state *state = userdata;
    struct fake_import *resource = imported;

    assert(rect->width == 1280);
    assert(rect->height == 720);
    state->present_count++;
    state->last_output = output_id;
    state->last_surface_generation = surface_generation;
    state->last_resource_generation = resource->generation;
    return 0;
}

static struct udroid_ahb_info ahb(uint32_t id, uint32_t generation) {
    static uint8_t metadata[] = {0x41, 0x48, 0x42};
    struct udroid_ahb_info info;

    memset(&info, 0, sizeof(info));
    info.resource_id = id;
    info.generation = generation;
    info.num_fds = 1;
    info.fds[0] = 42;
    info.metadata_size = sizeof(metadata);
    info.metadata = metadata;
    return info;
}

int main(void) {
    static const struct udroid_scanout_callbacks callbacks = {
        .import_ahb = import_ahb,
        .release_import = release_import,
        .present = present,
    };
    struct udroid_scanout_lifecycle lifecycle;
    struct udroid_scanout_rect rect = {.width = 1280, .height = 720};
    struct udroid_ahb_info first = ahb(7, 1);
    struct udroid_ahb_info replacement = ahb(7, 2);
    struct test_state state;

    memset(&state, 0, sizeof(state));
    assert(udroid_scanout_lifecycle_init(&lifecycle, &callbacks, &state) == 0);

    assert(udroid_scanout_publish_resource(&lifecycle, &first) == 0);
    assert(udroid_scanout_assign(&lifecycle, 0, 7, &rect) == 0);

    /* No Android Surface means a valid flush is deliberately a no-op. */
    assert(udroid_scanout_flush(&lifecycle, 7, 1) == 0);
    assert(state.present_count == 0);

    assert(udroid_scanout_set_surface(&lifecycle, 0, true, 11) == 0);
    assert(udroid_scanout_flush(&lifecycle, 7, 1) == 0);
    assert(state.present_count == 1);
    assert(state.last_output == 0);
    assert(state.last_surface_generation == 11);
    assert(state.last_resource_generation == 1);

    /* Import succeeds before the live generation is replaced and released. */
    assert(udroid_scanout_publish_resource(&lifecycle, &replacement) == 0);
    assert(state.import_count == 2);
    assert(state.release_count == 1);

    errno = 0;
    assert(udroid_scanout_publish_resource(&lifecycle, &first) == -1);
    assert(errno == ESTALE);
    assert(state.import_count == 2);
    assert(state.release_count == 1);

    errno = 0;
    assert(udroid_scanout_flush(&lifecycle, 7, 1) == -1);
    assert(errno == ESTALE);
    assert(state.present_count == 1);
    assert(udroid_scanout_flush(&lifecycle, 7, 2) == 0);
    assert(state.present_count == 2);
    assert(state.last_resource_generation == 2);

    /* Surface generations change independently from guest resource identity. */
    assert(udroid_scanout_set_surface(&lifecycle, 0, false, 0) == 0);
    assert(udroid_scanout_flush(&lifecycle, 7, 2) == 0);
    assert(state.present_count == 2);
    assert(udroid_scanout_set_surface(&lifecycle, 0, true, 12) == 0);
    assert(udroid_scanout_flush(&lifecycle, 7, 2) == 0);
    assert(state.present_count == 3);
    assert(state.last_surface_generation == 12);

    assert(udroid_scanout_destroy_resource(&lifecycle, 7) == 0);
    assert(state.release_count == 2);
    errno = 0;
    assert(udroid_scanout_flush(&lifecycle, 7, 2) == -1);
    assert(errno == ENOENT);

    udroid_scanout_lifecycle_release(&lifecycle);
    assert(state.release_count == 2);

    puts("PASS scanout lifecycle rejects stale resources and surfaces");
    return 0;
}
