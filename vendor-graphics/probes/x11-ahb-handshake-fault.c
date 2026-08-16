#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <xcb/dri3.h>
#include <xcb/xcb.h>

#define AHB_RGBA_SOCKET_MODIFIER 1257ULL

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000ULL + (uint64_t)now.tv_nsec / 1000000ULL;
}

static xcb_screen_t *get_screen(xcb_connection_t *connection, int number)
{
    xcb_screen_iterator_t iterator =
        xcb_setup_roots_iterator(xcb_get_setup(connection));
    while (number-- > 0)
        xcb_screen_next(&iterator);
    return iterator.data;
}

int main(void)
{
    int screen_number = 0;
    xcb_connection_t *connection = xcb_connect(NULL, &screen_number);
    if (xcb_connection_has_error(connection)) {
        fprintf(stderr, "FAIL stage=xcb-connect\n");
        return 1;
    }

    xcb_screen_t *screen = get_screen(connection, screen_number);
    if (screen == NULL) {
        fprintf(stderr, "FAIL stage=xcb-screen\n");
        return 1;
    }

    xcb_window_t window = xcb_generate_id(connection);
    xcb_create_window(connection, screen->root_depth, window, screen->root,
                      0, 0, 64, 64, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, 0, NULL);

    int sockets[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        fprintf(stderr, "FAIL stage=socketpair errno=%d\n", errno);
        return 1;
    }

    xcb_pixmap_t pixmap = xcb_generate_id(connection);
    xcb_void_cookie_t request = xcb_dri3_pixmap_from_buffers_checked(
        connection, pixmap, window, 1, 64, 64, 64 * 4,
        0, 0, 0, 0, 0, 0, 0, 24, 32, AHB_RGBA_SOCKET_MODIFIER,
        &sockets[1]);
    xcb_flush(connection);

    struct pollfd ack_poll = { .fd = sockets[0], .events = POLLIN, .revents = 0 };
    int poll_result;
    do {
        poll_result = poll(&ack_poll, 1, 1000);
    } while (poll_result < 0 && errno == EINTR);
    uint8_t ack = 0;
    if (poll_result != 1 || (ack_poll.revents & POLLIN) == 0 ||
        read(sockets[0], &ack, sizeof(ack)) != 1 || ack != 1) {
        fprintf(stderr, "FAIL stage=acknowledgement poll=%d revents=%d ack=%u\n",
                poll_result, ack_poll.revents, ack);
        return 1;
    }

    uint64_t started = monotonic_ms();
    xcb_generic_error_t *request_error = xcb_request_check(connection, request);
    uint64_t request_elapsed_ms = monotonic_ms() - started;
    close(sockets[0]);

    started = monotonic_ms();
    xcb_get_input_focus_cookie_t focus_cookie = xcb_get_input_focus(connection);
    xcb_generic_error_t *focus_error = NULL;
    xcb_get_input_focus_reply_t *focus_reply =
        xcb_get_input_focus_reply(connection, focus_cookie, &focus_error);
    uint64_t recovery_elapsed_ms = monotonic_ms() - started;

    int passed = request_error != NULL && request_elapsed_ms >= 2500 &&
                 request_elapsed_ms < 6000 && focus_reply != NULL &&
                 focus_error == NULL && recovery_elapsed_ms < 1000;

    printf("{\"server_timeout_ms\":%llu,\"recovery_ms\":%llu,"
           "\"request_error\":%u,\"server_responsive\":%s,\"passed\":%s}\n",
           (unsigned long long)request_elapsed_ms,
           (unsigned long long)recovery_elapsed_ms,
           request_error ? request_error->error_code : 0,
           focus_reply && !focus_error ? "true" : "false",
           passed ? "true" : "false");

    free(request_error);
    free(focus_error);
    free(focus_reply);
    xcb_destroy_window(connection, window);
    xcb_disconnect(connection);
    return passed ? 0 : 1;
}
