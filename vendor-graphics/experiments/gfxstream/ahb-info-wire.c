/* SPDX-License-Identifier: MIT */

#include "ahb-info-wire.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#define UDROID_AHB_INFO_MAGIC UINT32_C(0x42484155) /* "UAHB" in little endian */

struct udroid_ahb_info_header {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t resource_id;
    uint32_t generation;
    uint32_t num_fds;
    uint32_t metadata_size;
    uint32_t reserved;
};

static bool info_is_empty(const struct udroid_ahb_info *info)
{
    return info->resource_id == 0 && info->generation == 0 &&
           info->num_fds == 0 && info->metadata_size == 0 &&
           info->metadata == NULL;
}

static void close_fds(int *fds, size_t count)
{
    for (size_t index = 0; index < count; ++index) {
        if (fds[index] >= 0) {
            close(fds[index]);
            fds[index] = -1;
        }
    }
}

static int set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        return -errno;
    }
    return 0;
}

int udroid_ahb_info_send(int socket_fd,
                         uint32_t resource_id,
                         uint32_t generation,
                         const int *fds,
                         size_t num_fds,
                         const void *metadata,
                         size_t metadata_size)
{
    if (socket_fd < 0 || resource_id == 0 || generation == 0 ||
        num_fds == 0 || num_fds > UDROID_AHB_INFO_MAX_FDS ||
        fds == NULL || metadata == NULL || metadata_size == 0 ||
        metadata_size > UDROID_AHB_INFO_MAX_METADATA) {
        return -EINVAL;
    }

    for (size_t index = 0; index < num_fds; ++index) {
        if (fds[index] < 0) {
            return -EBADF;
        }
    }

    const struct udroid_ahb_info_header header = {
        .magic = UDROID_AHB_INFO_MAGIC,
        .version = UDROID_AHB_INFO_WIRE_VERSION,
        .header_size = sizeof(header),
        .resource_id = resource_id,
        .generation = generation,
        .num_fds = (uint32_t)num_fds,
        .metadata_size = (uint32_t)metadata_size,
        .reserved = 0,
    };
    struct iovec iov[2] = {
        { .iov_base = (void *)&header, .iov_len = sizeof(header) },
        { .iov_base = (void *)metadata, .iov_len = metadata_size },
    };
    char control[CMSG_SPACE(sizeof(int) * UDROID_AHB_INFO_MAX_FDS)] = {0};
    struct msghdr message = {
        .msg_iov = iov,
        .msg_iovlen = 2,
        .msg_control = control,
        .msg_controllen = CMSG_SPACE(sizeof(int) * num_fds),
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    if (cmsg == NULL) {
        return -EINVAL;
    }
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * num_fds);
    memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * num_fds);

#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    ssize_t sent;
    do {
        sent = sendmsg(socket_fd, &message, flags);
    } while (sent < 0 && errno == EINTR);

    if (sent < 0) {
        return -errno;
    }
    if ((size_t)sent != sizeof(header) + metadata_size) {
        return -EIO;
    }
    return 0;
}

int udroid_ahb_info_receive(int socket_fd, struct udroid_ahb_info *out)
{
    if (socket_fd < 0 || out == NULL || !info_is_empty(out)) {
        return -EINVAL;
    }

    const size_t packet_capacity = sizeof(struct udroid_ahb_info_header) +
                                   UDROID_AHB_INFO_MAX_METADATA;
    uint8_t *packet = malloc(packet_capacity);
    if (packet == NULL) {
        return -ENOMEM;
    }
    char control[CMSG_SPACE(sizeof(int) * UDROID_AHB_INFO_MAX_FDS)] = {0};
    struct iovec iov = { .iov_base = packet, .iov_len = packet_capacity };
    struct msghdr message = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };

#ifdef MSG_CMSG_CLOEXEC
    const int flags = MSG_CMSG_CLOEXEC;
#else
    const int flags = 0;
#endif
    ssize_t received;
    do {
        received = recvmsg(socket_fd, &message, flags);
    } while (received < 0 && errno == EINTR);
    if (received < 0) {
        int result = -errno;
        free(packet);
        return result;
    }
    if (received == 0) {
        free(packet);
        return -ECONNRESET;
    }

    int received_fds[UDROID_AHB_INFO_MAX_FDS];
    for (size_t index = 0; index < UDROID_AHB_INFO_MAX_FDS; ++index) {
        received_fds[index] = -1;
    }
    size_t received_fd_count = 0;
    bool invalid_control = false;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
         cmsg != NULL;
         cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len < CMSG_LEN(0)) {
            invalid_control = true;
            continue;
        }
        size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int) != 0 ||
            bytes / sizeof(int) > UDROID_AHB_INFO_MAX_FDS - received_fd_count) {
            invalid_control = true;
            continue;
        }
        size_t count = bytes / sizeof(int);
        memcpy(received_fds + received_fd_count, CMSG_DATA(cmsg), bytes);
        received_fd_count += count;
    }

    int result = 0;
    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 || invalid_control ||
        (size_t)received < sizeof(struct udroid_ahb_info_header)) {
        result = -EMSGSIZE;
        goto fail;
    }

    struct udroid_ahb_info_header header;
    memcpy(&header, packet, sizeof(header));
    if (header.magic != UDROID_AHB_INFO_MAGIC ||
        header.version != UDROID_AHB_INFO_WIRE_VERSION ||
        header.header_size != sizeof(header) || header.reserved != 0 ||
        header.resource_id == 0 || header.generation == 0 ||
        header.num_fds == 0 || header.num_fds > UDROID_AHB_INFO_MAX_FDS ||
        header.metadata_size == 0 ||
        header.metadata_size > UDROID_AHB_INFO_MAX_METADATA) {
        result = -EPROTO;
        goto fail;
    }
    if (header.num_fds != received_fd_count ||
        (size_t)received != sizeof(header) + header.metadata_size) {
        result = -EBADMSG;
        goto fail;
    }

#ifndef MSG_CMSG_CLOEXEC
    for (size_t index = 0; index < received_fd_count; ++index) {
        result = set_cloexec(received_fds[index]);
        if (result != 0) {
            goto fail;
        }
    }
#else
    (void)set_cloexec;
#endif

    out->metadata = malloc(header.metadata_size);
    if (out->metadata == NULL) {
        result = -ENOMEM;
        goto fail;
    }
    memcpy(out->metadata, packet + sizeof(header), header.metadata_size);
    out->resource_id = header.resource_id;
    out->generation = header.generation;
    out->num_fds = received_fd_count;
    memcpy(out->fds, received_fds, sizeof(int) * received_fd_count);
    out->metadata_size = header.metadata_size;
    free(packet);
    return 0;

fail:
    close_fds(received_fds, received_fd_count);
    free(packet);
    return result;
}

void udroid_ahb_info_release(struct udroid_ahb_info *info)
{
    if (info == NULL) {
        return;
    }
    close_fds(info->fds, info->num_fds);
    free(info->metadata);
    memset(info, 0, sizeof(*info));
}
