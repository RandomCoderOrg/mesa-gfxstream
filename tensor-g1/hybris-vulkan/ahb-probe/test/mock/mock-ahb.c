/* SPDX-License-Identifier: MIT */

#include <android/hardware_buffer.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct AHardwareBuffer {
    AHardwareBuffer_Desc desc;
    uint8_t *pixels;
    atomic_uint references;
};

static int transfer_bytes(int fd, void *data, size_t size, int writing)
{
    uint8_t *cursor = data;
    while (size > 0) {
        ssize_t transferred = writing ? write(fd, cursor, size) : read(fd, cursor, size);
        if (transferred < 0 && errno == EINTR) {
            continue;
        }
        if (transferred <= 0) {
            return transferred == 0 ? -EPIPE : -errno;
        }
        cursor += (size_t)transferred;
        size -= (size_t)transferred;
    }
    return 0;
}

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *desc, AHardwareBuffer **out_buffer)
{
    if (desc == NULL || out_buffer == NULL || desc->width == 0 || desc->height == 0) {
        return -EINVAL;
    }
    AHardwareBuffer *buffer = calloc(1, sizeof(*buffer));
    if (buffer == NULL) {
        return -ENOMEM;
    }
    buffer->desc = *desc;
    buffer->desc.stride = (desc->width + 63U) & ~63U;
    size_t allocation_size = (size_t)buffer->desc.stride * desc->height * 4;
    buffer->pixels = calloc(1, allocation_size);
    if (buffer->pixels == NULL) {
        free(buffer);
        return -ENOMEM;
    }
    atomic_init(&buffer->references, 1);
    *out_buffer = buffer;
    return 0;
}

void AHardwareBuffer_release(AHardwareBuffer *buffer)
{
    if (buffer != NULL && atomic_fetch_sub(&buffer->references, 1) == 1) {
        free(buffer->pixels);
        free(buffer);
    }
}

void AHardwareBuffer_describe(const AHardwareBuffer *buffer, AHardwareBuffer_Desc *out_desc)
{
    if (buffer != NULL && out_desc != NULL) {
        *out_desc = buffer->desc;
    }
}

int AHardwareBuffer_lock(AHardwareBuffer *buffer, uint64_t usage, int32_t fence,
                         const ARect *rect, void **out_virtual_address)
{
    (void)usage;
    (void)fence;
    (void)rect;
    if (buffer == NULL || out_virtual_address == NULL) {
        return -EINVAL;
    }
    *out_virtual_address = buffer->pixels;
    return 0;
}

int AHardwareBuffer_unlock(AHardwareBuffer *buffer, int32_t *fence)
{
    if (buffer == NULL) {
        return -EINVAL;
    }
    if (fence != NULL) {
        *fence = -1;
    }
    return 0;
}

int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer *buffer, int socket_fd)
{
    if (buffer == NULL) {
        return -EINVAL;
    }
    AHardwareBuffer *reference = (AHardwareBuffer *)buffer;
    atomic_fetch_add(&reference->references, 1);
    int result = transfer_bytes(socket_fd, &reference, sizeof(reference), 1);
    if (result != 0) {
        AHardwareBuffer_release(reference);
    }
    return result;
}

int AHardwareBuffer_recvHandleFromUnixSocket(int socket_fd, AHardwareBuffer **out_buffer)
{
    if (out_buffer == NULL) {
        return -EINVAL;
    }
    AHardwareBuffer *reference = NULL;
    int result = transfer_bytes(socket_fd, &reference, sizeof(reference), 0);
    if (result == 0) {
        *out_buffer = reference;
    }
    return result;
}
