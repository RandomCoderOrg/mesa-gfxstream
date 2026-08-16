#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct delayed_write {
    int fd;
    unsigned delay_ms;
    uint8_t value;
};

static uint64_t monotonic_us(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000000ULL + (uint64_t)now.tv_nsec / 1000ULL;
}

static int wait_for_socket_event(int fd, short events, int timeout_ms)
{
    struct pollfd descriptor = { .fd = fd, .events = events, .revents = 0 };
    int result;
    do {
        result = poll(&descriptor, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);

    return result == 1 && (descriptor.revents & events) != 0 &&
           (descriptor.revents & (POLLERR | POLLNVAL)) == 0;
}

static void *write_after_delay(void *opaque)
{
    struct delayed_write *request = opaque;
    struct timespec delay = {
        .tv_sec = request->delay_ms / 1000,
        .tv_nsec = (long)(request->delay_ms % 1000) * 1000000L,
    };
    nanosleep(&delay, NULL);
    (void)write(request->fd, &request->value, sizeof(request->value));
    return NULL;
}

static int report(const char *case_name, int passed, uint64_t elapsed_us)
{
    printf("{\"case\":\"%s\",\"passed\":%s,\"elapsed_us\":%llu}\n",
           case_name, passed ? "true" : "false",
           (unsigned long long)elapsed_us);
    return passed;
}

int main(void)
{
    int passed = 0;
    int sockets[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        return 1;
    uint64_t started = monotonic_us();
    int ready = wait_for_socket_event(sockets[0], POLLIN, 100);
    uint64_t elapsed = monotonic_us() - started;
    passed += report("silent-peer-timeout", !ready && elapsed >= 80000 && elapsed < 500000, elapsed);
    close(sockets[0]);
    close(sockets[1]);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        return 1;
    close(sockets[1]);
    started = monotonic_us();
    ready = wait_for_socket_event(sockets[0], POLLIN, 100);
    uint8_t closed_value = 0;
    ssize_t closed_count = ready ? read(sockets[0], &closed_value, sizeof(closed_value)) : -1;
    elapsed = monotonic_us() - started;
    passed += report("closed-peer-rejected", ready && closed_count == 0 && elapsed < 80000, elapsed);
    close(sockets[0]);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        return 1;
    struct delayed_write delayed = { .fd = sockets[1], .delay_ms = 20, .value = 1 };
    pthread_t writer;
    if (pthread_create(&writer, NULL, write_after_delay, &delayed) != 0)
        return 1;
    started = monotonic_us();
    ready = wait_for_socket_event(sockets[0], POLLIN, 200);
    uint8_t ack = 0;
    ssize_t count = ready ? read(sockets[0], &ack, sizeof(ack)) : -1;
    elapsed = monotonic_us() - started;
    pthread_join(writer, NULL);
    passed += report("delayed-valid-ack", ready && count == 1 && ack == 1 && elapsed >= 10000 && elapsed < 200000,
                     elapsed);
    close(sockets[0]);
    close(sockets[1]);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        return 1;
    uint8_t invalid_ack = 2;
    if (write(sockets[1], &invalid_ack, sizeof(invalid_ack)) != 1)
        return 1;
    started = monotonic_us();
    ready = wait_for_socket_event(sockets[0], POLLIN, 100);
    ack = 0;
    count = ready ? read(sockets[0], &ack, sizeof(ack)) : -1;
    elapsed = monotonic_us() - started;
    passed += report("invalid-ack-rejected", ready && count == 1 && ack != 1, elapsed);
    close(sockets[0]);
    close(sockets[1]);

    printf("{\"summary\":true,\"passed\":%d,\"total\":4}\n", passed);
    return passed == 4 ? 0 : 1;
}
