/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>

typedef struct ARect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} ARect;

typedef struct AHardwareBuffer AHardwareBuffer;

typedef struct AHardwareBuffer_Desc {
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t format;
    uint64_t usage;
    uint32_t stride;
    uint32_t rfu0;
    uint64_t rfu1;
} AHardwareBuffer_Desc;

enum {
    AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM = 1,
};

enum {
    AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN = 3UL,
    AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN = 3UL << 4,
    AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE = 1UL << 8,
    AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT = 1UL << 9,
};

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *desc, AHardwareBuffer **out_buffer);
void AHardwareBuffer_release(AHardwareBuffer *buffer);
void AHardwareBuffer_describe(const AHardwareBuffer *buffer, AHardwareBuffer_Desc *out_desc);
int AHardwareBuffer_lock(AHardwareBuffer *buffer, uint64_t usage, int32_t fence,
                         const ARect *rect, void **out_virtual_address);
int AHardwareBuffer_unlock(AHardwareBuffer *buffer, int32_t *fence);
int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer *buffer, int socket_fd);
int AHardwareBuffer_recvHandleFromUnixSocket(int socket_fd, AHardwareBuffer **out_buffer);
