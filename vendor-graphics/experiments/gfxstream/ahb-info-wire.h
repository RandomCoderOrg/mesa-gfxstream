/* SPDX-License-Identifier: MIT */

#ifndef UDROID_AHB_INFO_WIRE_H
#define UDROID_AHB_INFO_WIRE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UDROID_AHB_INFO_WIRE_VERSION 1U
#define UDROID_AHB_INFO_MAX_FDS 64U
#define UDROID_AHB_INFO_MAX_METADATA (64U * 1024U)

/*
 * Same-device, same-build internal ABI. The metadata is opaque AOSP native
 * handle state and must not be persisted or replayed across component updates.
 */

struct udroid_ahb_info {
    uint32_t resource_id;
    uint32_t generation;
    size_t num_fds;
    int fds[UDROID_AHB_INFO_MAX_FDS];
    size_t metadata_size;
    uint8_t *metadata;
};

/*
 * Send one complete AHardwareBuffer identity over a connected AF_UNIX
 * SOCK_SEQPACKET socket. File descriptors remain owned by the caller.
 */
int udroid_ahb_info_send(int socket_fd,
                         uint32_t resource_id,
                         uint32_t generation,
                         const int *fds,
                         size_t num_fds,
                         const void *metadata,
                         size_t metadata_size);

/*
 * Receive one complete AHardwareBuffer identity. On success `out` owns the
 * received descriptors and metadata until `udroid_ahb_info_release`.
 * `out` must be zero-initialized.
 */
int udroid_ahb_info_receive(int socket_fd, struct udroid_ahb_info *out);

/* Close every received descriptor, free metadata, and clear the structure. */
void udroid_ahb_info_release(struct udroid_ahb_info *info);

#ifdef __cplusplus
}
#endif

#endif
