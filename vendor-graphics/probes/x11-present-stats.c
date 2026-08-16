#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

#define STATS_PROPERTY "_UDROID_X11_PRESENT_STATS"
#define STATS_WORDS 15

static xcb_screen_t *get_screen(xcb_connection_t *connection, int number)
{
    xcb_screen_iterator_t iterator =
        xcb_setup_roots_iterator(xcb_get_setup(connection));
    while (iterator.rem && number-- > 0)
        xcb_screen_next(&iterator);
    return iterator.data;
}

static uint64_t read_u64(const uint32_t *words, size_t index)
{
    return (uint64_t)words[index] | ((uint64_t)words[index + 1] << 32);
}

int main(void)
{
    int screen_number = 0;
    xcb_connection_t *connection = xcb_connect(NULL, &screen_number);
    if (xcb_connection_has_error(connection)) {
        fprintf(stderr, "x11-present-stats: cannot connect to DISPLAY\n");
        xcb_disconnect(connection);
        return 1;
    }
    xcb_screen_t *screen = get_screen(connection, screen_number);
    if (screen == NULL) {
        fprintf(stderr, "x11-present-stats: X screen is unavailable\n");
        xcb_disconnect(connection);
        return 1;
    }

    xcb_intern_atom_cookie_t atom_cookie =
        xcb_intern_atom(connection, 1, strlen(STATS_PROPERTY), STATS_PROPERTY);
    xcb_intern_atom_reply_t *atom_reply =
        xcb_intern_atom_reply(connection, atom_cookie, NULL);
    if (atom_reply == NULL || atom_reply->atom == XCB_ATOM_NONE) {
        free(atom_reply);
        fprintf(stderr, "x11-present-stats: server does not publish %s\n",
                STATS_PROPERTY);
        xcb_disconnect(connection);
        return 1;
    }

    xcb_get_property_cookie_t property_cookie =
        xcb_get_property(connection, 0, screen->root, atom_reply->atom,
                         XCB_ATOM_CARDINAL, 0, STATS_WORDS);
    free(atom_reply);
    xcb_get_property_reply_t *property =
        xcb_get_property_reply(connection, property_cookie, NULL);
    if (property == NULL || property->type != XCB_ATOM_CARDINAL ||
        property->format != 32 || property->value_len != STATS_WORDS ||
        property->bytes_after != 0) {
        free(property);
        fprintf(stderr, "x11-present-stats: malformed %s property\n",
                STATS_PROPERTY);
        xcb_disconnect(connection);
        return 1;
    }

    const uint32_t *words = xcb_get_property_value(property);
    const uint32_t version = words[0];
    const uint64_t attempts = read_u64(words, 1);
    const uint64_t offloads = read_u64(words, 3);
    const uint64_t disabled = read_u64(words, 5);
    const uint64_t renderer_unavailable = read_u64(words, 7);
    const uint64_t buffer_unsupported = read_u64(words, 9);
    const uint64_t region_unsupported = read_u64(words, 11);
    const uint64_t queue_full = read_u64(words, 13);
    const uint64_t accounted = offloads + disabled + renderer_unavailable +
        buffer_unsupported + region_unsupported + queue_full;
    const int consistent = version == 1 && accounted == attempts;

    printf("{\"version\":%u,\"attempts\":%" PRIu64
           ",\"offloads\":%" PRIu64 ",\"fallbacks\":{"
           "\"disabled\":%" PRIu64 ",\"rendererUnavailable\":%" PRIu64
           ",\"bufferUnsupported\":%" PRIu64
           ",\"regionUnsupported\":%" PRIu64
           ",\"queueFull\":%" PRIu64 "},\"consistent\":%s}\n",
           version, attempts, offloads, disabled, renderer_unavailable,
           buffer_unsupported, region_unsupported, queue_full,
           consistent ? "true" : "false");

    free(property);
    xcb_disconnect(connection);
    return consistent ? 0 : 1;
}
