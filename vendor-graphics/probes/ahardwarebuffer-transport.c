/* SPDX-License-Identifier: MIT */

#ifndef _Nullable
#define _Nullable
#endif
#ifndef _Nonnull
#define _Nonnull
#endif
#ifndef __INTRODUCED_IN
#define __INTRODUCED_IN(api_level)
#endif

#include <android/hardware_buffer.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const uint32_t default_widths[] = { 63, 64, 65, 127, 128, 129, 1919, 1920, 1921 };

static uint64_t monotonic_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static uint64_t fnv1a(const uint8_t *data, size_t size, uint64_t hash)
{
    for (size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void fill_pattern(uint8_t *pixels, const AHardwareBuffer_Desc *desc, uint32_t generation)
{
    size_t row_bytes = (size_t)desc->stride * 4;
    for (uint32_t y = 0; y < desc->height; ++y) {
        uint8_t *row = pixels + (size_t)y * row_bytes;
        for (uint32_t x = 0; x < desc->width; ++x) {
            row[x * 4 + 0] = (uint8_t)(x + generation * 17U);
            row[x * 4 + 1] = (uint8_t)(y + generation * 29U);
            row[x * 4 + 2] = (uint8_t)(x ^ y ^ generation * 43U);
            row[x * 4 + 3] = 255;
        }
    }
}

static uint64_t visible_hash(const uint8_t *pixels, const AHardwareBuffer_Desc *desc)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t row_bytes = (size_t)desc->stride * 4;
    size_t visible_bytes = (size_t)desc->width * 4;
    for (uint32_t y = 0; y < desc->height; ++y) {
        hash = fnv1a(pixels + (size_t)y * row_bytes, visible_bytes, hash);
    }
    return hash;
}

static int wait_and_close_fence(int fence)
{
    if (fence < 0) {
        return 0;
    }
    struct pollfd poll_fd = { .fd = fence, .events = POLLIN };
    int result;
    do {
        result = poll(&poll_fd, 1, 5000);
    } while (result < 0 && errno == EINTR);
    int saved_errno = errno;
    close(fence);
    if (result > 0) {
        return 0;
    }
    return result == 0 ? -ETIMEDOUT : -saved_errno;
}

static int run_case(uint32_t width, uint32_t height, uint32_t generation)
{
    const int source_only = getenv("UDROID_AHB_SOURCE_ONLY") != NULL;
    const int transport_only = source_only || getenv("UDROID_AHB_TRANSPORT_ONLY") != NULL;
    const int keep_source = getenv("UDROID_AHB_KEEP_SOURCE") != NULL;
    AHardwareBuffer_Desc requested = {
        .width = width,
        .height = height,
        .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                 AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                 AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
    };
    AHardwareBuffer_Desc allocated_desc = {0};
    AHardwareBuffer_Desc received_desc = {0};
    AHardwareBuffer *allocated = NULL;
    AHardwareBuffer *received = NULL;
    void *mapped = NULL;
    int sockets[2] = {-1, -1};
    int unlock_fence = -1;
    uint64_t expected_hash = 0;
    uint64_t actual_hash = 0;
    uint64_t started = monotonic_ns();
    uint64_t allocated_at = 0;
    uint64_t written_at = 0;
    uint64_t sent_at = 0;
    uint64_t received_at = 0;
    uint64_t read_at = 0;
    const char *stage = "allocate";
    int result = AHardwareBuffer_allocate(&requested, &allocated);
    if (result != 0 || allocated == NULL) {
        goto done;
    }
    allocated_at = monotonic_ns();
    AHardwareBuffer_describe(allocated, &allocated_desc);
    if (allocated_desc.width != width || allocated_desc.height != height ||
        allocated_desc.layers != 1 || allocated_desc.stride < width) {
        result = -EINVAL;
        stage = "describe-allocated";
        goto done;
    }

    if (transport_only) {
        written_at = allocated_at;
        goto transport;
    }

    stage = "lock-write";
    result = AHardwareBuffer_lock(allocated, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &mapped);
    if (result != 0 || mapped == NULL) {
        goto done;
    }
    fill_pattern(mapped, &allocated_desc, generation);
    expected_hash = visible_hash(mapped, &allocated_desc);
    stage = "unlock-write";
    result = AHardwareBuffer_unlock(allocated, &unlock_fence);
    mapped = NULL;
    if (result != 0) {
        goto done;
    }
    stage = "wait-write-fence";
    result = wait_and_close_fence(unlock_fence);
    unlock_fence = -1;
    if (result != 0) {
        goto done;
    }
    written_at = monotonic_ns();

transport:
    stage = "socketpair";
    int socket_type = SOCK_STREAM;
#ifdef SOCK_CLOEXEC
    socket_type |= SOCK_CLOEXEC;
#endif
    if (socketpair(AF_UNIX, socket_type, 0, sockets) != 0) {
        result = -errno;
        goto done;
    }
    stage = "send";
    result = AHardwareBuffer_sendHandleToUnixSocket(allocated, sockets[0]);
    if (result != 0) {
        goto done;
    }
    sent_at = monotonic_ns();
    if (source_only) {
        received_at = sent_at;
        read_at = sent_at;
        result = 0;
        stage = "ok-source-send";
        goto done;
    }
    stage = "receive";
    result = AHardwareBuffer_recvHandleFromUnixSocket(sockets[1], &received);
    if (result != 0 || received == NULL) {
        goto done;
    }
    received_at = monotonic_ns();

    if (!keep_source) {
        AHardwareBuffer_release(allocated);
        allocated = NULL;
    }
    AHardwareBuffer_describe(received, &received_desc);
    if (received_desc.width != allocated_desc.width || received_desc.height != allocated_desc.height ||
        received_desc.layers != allocated_desc.layers || received_desc.format != allocated_desc.format ||
        received_desc.stride != allocated_desc.stride) {
        result = -EINVAL;
        stage = "describe-received";
        goto done;
    }

    if (transport_only) {
        read_at = received_at;
        result = 0;
        stage = "ok-transport";
        goto done;
    }

    stage = "lock-read";
    result = AHardwareBuffer_lock(received, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &mapped);
    if (result != 0 || mapped == NULL) {
        goto done;
    }
    actual_hash = visible_hash(mapped, &received_desc);
    stage = "unlock-read";
    result = AHardwareBuffer_unlock(received, NULL);
    mapped = NULL;
    if (result != 0) {
        goto done;
    }
    read_at = monotonic_ns();
    if (actual_hash != expected_hash) {
        result = -EILSEQ;
        stage = "hash";
        goto done;
    }
    stage = "ok";

done:
    if (mapped != NULL) {
        AHardwareBuffer_unlock(received != NULL ? received : allocated, NULL);
    }
    if (unlock_fence >= 0) {
        close(unlock_fence);
    }
    if (sockets[0] >= 0) {
        close(sockets[0]);
    }
    if (sockets[1] >= 0) {
        close(sockets[1]);
    }
    if (received != NULL) {
        AHardwareBuffer_release(received);
    }
    if (allocated != NULL) {
        AHardwareBuffer_release(allocated);
    }

    uint64_t finished = monotonic_ns();
    printf("{\"mode\":\"%s\",\"width\":%u,\"height\":%u,\"generation\":%u,\"stride\":%u,"
           "\"result\":%d,\"stage\":\"%s\",\"expected_hash\":\"%016" PRIx64 "\","
           "\"actual_hash\":\"%016" PRIx64 "\",\"allocate_us\":%.3f,\"write_us\":%.3f,"
           "\"send_us\":%.3f,\"receive_us\":%.3f,\"read_us\":%.3f,\"total_us\":%.3f}\n",
           source_only ? "source" : (transport_only ? "transport" : "mapped"),
           width, height, generation,
           allocated_desc.stride, result, stage,
           expected_hash, actual_hash,
           allocated_at ? (allocated_at - started) / 1000.0 : 0.0,
           written_at ? (written_at - allocated_at) / 1000.0 : 0.0,
           sent_at ? (sent_at - written_at) / 1000.0 : 0.0,
           received_at ? (received_at - sent_at) / 1000.0 : 0.0,
           read_at ? (read_at - received_at) / 1000.0 : 0.0,
           (finished - started) / 1000.0);
    fflush(stdout);
    return result;
}

int main(int argc, char **argv)
{
    uint32_t iterations = 10;
    uint32_t height = 32;
    uint32_t single_width = 0;
    if (argc > 1) {
        iterations = (uint32_t)strtoul(argv[1], NULL, 10);
    }
    if (argc > 2) {
        height = (uint32_t)strtoul(argv[2], NULL, 10);
    }
    if (argc > 3) {
        single_width = (uint32_t)strtoul(argv[3], NULL, 10);
    }
    if (iterations == 0 || height == 0 || (argc > 3 && single_width == 0)) {
        fprintf(stderr, "usage: %s [iterations>0] [height>0] [single-width>0]\n", argv[0]);
        return 2;
    }

    uint64_t cases = 0;
    uint64_t failures = 0;
    for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
        if (single_width != 0) {
            ++cases;
            if (run_case(single_width, height, iteration + 1) != 0) {
                ++failures;
            }
            continue;
        }
        for (size_t index = 0; index < sizeof(default_widths) / sizeof(default_widths[0]); ++index) {
            ++cases;
            if (run_case(default_widths[index], height, iteration + 1) != 0) {
                ++failures;
            }
        }
    }
    fprintf(stderr, "ahb-probe: cases=%" PRIu64 " failures=%" PRIu64 "\n", cases, failures);
    return failures == 0 ? 0 : 1;
}
