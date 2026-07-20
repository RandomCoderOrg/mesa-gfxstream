#ifndef TENSOR_EGL_NV12_CONSUMER_H
#define TENSOR_EGL_NV12_CONSUMER_H

#include <stdbool.h>
#include <stdint.h>

struct tmc_egl_consumer;
struct tmc_egl_surface;

struct tmc_egl_consumer *tmc_egl_consumer_create(void);
void tmc_egl_consumer_destroy(struct tmc_egl_consumer *consumer);
const char *tmc_egl_consumer_renderer(const struct tmc_egl_consumer *consumer);

struct tmc_egl_surface *tmc_egl_surface_create(
   struct tmc_egl_consumer *consumer, int dmabuf_fd, uint32_t width,
   uint32_t height, uint32_t stride, uint32_t slice_height);
void tmc_egl_surface_destroy(struct tmc_egl_consumer *consumer,
                             struct tmc_egl_surface *surface);

bool tmc_egl_surface_sample(struct tmc_egl_consumer *consumer,
                            const struct tmc_egl_surface *surface,
                            uint8_t pixel[4]);

#endif
