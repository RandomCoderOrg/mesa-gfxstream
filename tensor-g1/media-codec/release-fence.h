#ifndef TENSOR_RELEASE_FENCE_H
#define TENSOR_RELEASE_FENCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TMC_RELEASE_FENCE_SLOTS 64

struct tmc_release_fence_slot {
   bool active;
   int fence_fd;
};

struct tmc_release_fence_context {
   int mali_fd;
   int stream_fd;
   size_t page_size;
   void *tracking;
   volatile uint8_t *events;
   uint8_t next_atom;
   struct tmc_release_fence_slot slots[TMC_RELEASE_FENCE_SLOTS];
};

struct tmc_release_fence {
   struct tmc_release_fence_context *context;
   int slot;
};

void tmc_release_fence_context_reset(struct tmc_release_fence_context *context);
bool tmc_release_fence_context_init(struct tmc_release_fence_context *context);
void tmc_release_fence_context_destroy(struct tmc_release_fence_context *context);
bool tmc_release_fence_arm(struct tmc_release_fence_context *context,
                           int dmabuf_fd,
                           struct tmc_release_fence *release_fence);
bool tmc_release_fence_signal(struct tmc_release_fence *release_fence);

#endif
