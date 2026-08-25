/* SPDX-License-Identifier: MIT */

#include "ahb-info-wire.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void make_packet_socketpair(int sockets[2])
{
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) == 0) {
        return;
    }
    /* Darwin lacks AF_UNIX SOCK_SEQPACKET; datagrams preserve the packet and
     * SCM_RIGHTS semantics exercised by this host-side contract test. Android
     * and Linux production builds use SOCK_SEQPACKET. */
    assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) == 0);
}

static void test_round_trip(void)
{
    int sockets[2];
    make_packet_socketpair(sockets);

    int source_fds[3];
    for (size_t index = 0; index < 3; ++index) {
        source_fds[index] = open("/dev/null", O_RDONLY);
        assert(source_fds[index] >= 0);
    }
    const uint8_t metadata[] = {0x41, 0x48, 0x42, 0x00, 0x11, 0x7f, 0xff};
    assert(udroid_ahb_info_send(sockets[0], 73, 9, source_fds, 3,
                                metadata, sizeof(metadata)) == 0);

    struct udroid_ahb_info received = {0};
    assert(udroid_ahb_info_receive(sockets[1], &received) == 0);
    assert(received.resource_id == 73);
    assert(received.generation == 9);
    assert(received.num_fds == 3);
    assert(received.metadata_size == sizeof(metadata));
    assert(memcmp(received.metadata, metadata, sizeof(metadata)) == 0);
    for (size_t index = 0; index < received.num_fds; ++index) {
        assert(fcntl(received.fds[index], F_GETFD) >= 0);
        assert((fcntl(received.fds[index], F_GETFD) & FD_CLOEXEC) != 0);
    }

    int received_fds[3];
    memcpy(received_fds, received.fds, sizeof(received_fds));
    udroid_ahb_info_release(&received);
    assert(received.metadata == NULL);
    assert(received.num_fds == 0);
    for (size_t index = 0; index < 3; ++index) {
        assert(fcntl(received_fds[index], F_GETFD) == -1);
        assert(errno == EBADF);
        assert(fcntl(source_fds[index], F_GETFD) >= 0);
        close(source_fds[index]);
    }
    udroid_ahb_info_release(&received);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_rejects_invalid_arguments(void)
{
    uint8_t metadata = 1;
    int fd = open("/dev/null", O_RDONLY);
    assert(fd >= 0);
    assert(udroid_ahb_info_send(-1, 1, 1, &fd, 1, &metadata, 1) == -EINVAL);
    assert(udroid_ahb_info_send(fd, 0, 1, &fd, 1, &metadata, 1) == -EINVAL);
    assert(udroid_ahb_info_send(fd, 1, 0, &fd, 1, &metadata, 1) == -EINVAL);
    assert(udroid_ahb_info_send(fd, 1, 1, NULL, 1, &metadata, 1) == -EINVAL);
    assert(udroid_ahb_info_send(fd, 1, 1, &fd, 0, &metadata, 1) == -EINVAL);
    assert(udroid_ahb_info_send(fd, 1, 1, &fd, 1, NULL, 1) == -EINVAL);
    assert(udroid_ahb_info_send(fd, 1, 1, &fd, 1, &metadata, 0) == -EINVAL);

    struct udroid_ahb_info nonempty = {.resource_id = 1};
    assert(udroid_ahb_info_receive(fd, &nonempty) == -EINVAL);
    close(fd);
}

static void test_rejects_malformed_packet(void)
{
    int sockets[2];
    make_packet_socketpair(sockets);
    const uint8_t garbage[] = {1, 2, 3, 4};
    assert(send(sockets[0], garbage, sizeof(garbage), 0) == sizeof(garbage));
    struct udroid_ahb_info received = {0};
    assert(udroid_ahb_info_receive(sockets[1], &received) == -EMSGSIZE);
    udroid_ahb_info_release(&received);
    close(sockets[0]);
    close(sockets[1]);
}

int main(void)
{
    test_round_trip();
    test_rejects_invalid_arguments();
    test_rejects_malformed_packet();
    puts("PASS complete AHB identity survives the process boundary");
    return 0;
}
