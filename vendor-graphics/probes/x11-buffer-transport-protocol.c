#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>

#define PROPERTY_NAME "_UDROID_X11_BUFFER_TRANSPORT"

enum {
    CAP_AHB_SOCKET = 1U << 0,
    CAP_AHB_RGBA = 1U << 1,
    CAP_SYNC_FILE_ACQUIRE = 1U << 2,
    CAP_GPU_COPY = 1U << 3,
};

int main(void) {
    int screen_number = 0;
    xcb_connection_t *connection = xcb_connect(NULL, &screen_number);
    if (connection == NULL || xcb_connection_has_error(connection)) {
        puts("{\"connected\":false,\"compatible\":false}");
        if (connection != NULL)
            xcb_disconnect(connection);
        return 2;
    }

    const xcb_setup_t *setup = xcb_get_setup(connection);
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
    for (int index = 0; index < screen_number && screens.rem != 0; ++index)
        xcb_screen_next(&screens);
    if (screens.rem == 0) {
        puts("{\"connected\":true,\"propertyPresent\":false,\"compatible\":false}");
        xcb_disconnect(connection);
        return 3;
    }

    xcb_intern_atom_cookie_t atom_cookie =
        xcb_intern_atom(connection, 1, strlen(PROPERTY_NAME), PROPERTY_NAME);
    xcb_intern_atom_reply_t *atom_reply =
        xcb_intern_atom_reply(connection, atom_cookie, NULL);
    if (atom_reply == NULL || atom_reply->atom == XCB_ATOM_NONE) {
        puts("{\"connected\":true,\"propertyPresent\":false,\"compatible\":false}");
        free(atom_reply);
        xcb_disconnect(connection);
        return 4;
    }

    xcb_get_property_cookie_t property_cookie =
        xcb_get_property(connection, 0, screens.data->root, atom_reply->atom,
                         XCB_ATOM_CARDINAL, 0, 6);
    free(atom_reply);
    xcb_get_property_reply_t *property_reply =
        xcb_get_property_reply(connection, property_cookie, NULL);
    if (property_reply == NULL || property_reply->type != XCB_ATOM_CARDINAL ||
        property_reply->format != 32 || property_reply->value_len < 6) {
        puts("{\"connected\":true,\"propertyPresent\":false,\"compatible\":false}");
        free(property_reply);
        xcb_disconnect(connection);
        return 5;
    }

    const uint32_t *words = xcb_get_property_value(property_reply);
    const uint32_t version = words[0];
    const uint32_t capabilities = words[1];
    const uint64_t bgra_modifier =
        (uint64_t) words[2] | ((uint64_t) words[3] << 32);
    const uint64_t rgba_modifier =
        (uint64_t) words[4] | ((uint64_t) words[5] << 32);
    const uint32_t required = CAP_AHB_SOCKET | CAP_AHB_RGBA | CAP_GPU_COPY;
    const int compatible = version == 1 &&
        (capabilities & required) == required &&
        bgra_modifier != 0 && rgba_modifier != 0;

    printf("{\"connected\":true,\"propertyPresent\":true,"
           "\"version\":%" PRIu32 ",\"capabilities\":%" PRIu32 ","
           "\"ahbSocket\":%s,\"rgba\":%s,\"syncFileAcquire\":%s,"
           "\"gpuCopy\":%s,\"bgraModifier\":%" PRIu64 ","
           "\"rgbaModifier\":%" PRIu64 ",\"compatible\":%s}\n",
           version, capabilities,
           (capabilities & CAP_AHB_SOCKET) ? "true" : "false",
           (capabilities & CAP_AHB_RGBA) ? "true" : "false",
           (capabilities & CAP_SYNC_FILE_ACQUIRE) ? "true" : "false",
           (capabilities & CAP_GPU_COPY) ? "true" : "false",
           bgra_modifier, rgba_modifier, compatible ? "true" : "false");

    free(property_reply);
    xcb_disconnect(connection);
    return compatible ? 0 : 6;
}
