/*
 * Small contract probe for the Termux:X11 side of the standard Mesa DRI3 path.
 *
 * It verifies two independent server callbacks:
 *   1. DRI3 Open returns a render fd (/dev/mali0 on Tensor G1).
 *   2. BufferFromPixmap exports an mmap-able linear pixmap fd.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <xcb/dri3.h>
#include <xcb/xcb.h>

static xcb_screen_t *
get_screen(xcb_connection_t *connection, int screen_number)
{
   xcb_screen_iterator_t iterator =
      xcb_setup_roots_iterator(xcb_get_setup(connection));

   while (screen_number-- > 0)
      xcb_screen_next(&iterator);
   return iterator.data;
}

static void
print_x_error(const char *stage, xcb_generic_error_t *error)
{
   if (!error) {
      fprintf(stderr, "%s: no reply\n", stage);
      return;
   }

   fprintf(stderr,
           "%s: X11 error code=%u major=%u minor=%u sequence=%u\n",
           stage, error->error_code, error->major_code,
           error->minor_code, error->sequence);
}

static void
xcb_sync(xcb_connection_t *connection)
{
   xcb_get_input_focus_reply_t *reply = xcb_get_input_focus_reply(
      connection, xcb_get_input_focus(connection), NULL);
   free(reply);
}

int
main(void)
{
   xcb_connection_t *connection = NULL;
   xcb_screen_t *screen;
   xcb_dri3_query_version_reply_t *version = NULL;
   xcb_dri3_open_reply_t *open_reply = NULL;
   xcb_dri3_buffer_from_pixmap_reply_t *buffer_reply = NULL;
   xcb_generic_error_t *error = NULL;
   xcb_pixmap_t pixmap = XCB_NONE;
   xcb_gcontext_t gc = XCB_NONE;
   int *reply_fds;
   int screen_number = 0;
   int mali_fd = -1;
   int pixmap_fd = -1;
   int status = 1;

   connection = xcb_connect(NULL, &screen_number);
   if (!connection || xcb_connection_has_error(connection)) {
      fprintf(stderr, "xcb_connect failed\n");
      goto cleanup;
   }
   screen = get_screen(connection, screen_number);
   if (!screen) {
      fprintf(stderr, "X11 screen lookup failed\n");
      goto cleanup;
   }

   version = xcb_dri3_query_version_reply(
      connection, xcb_dri3_query_version(connection, 1, 2), &error);
   if (!version) {
      print_x_error("DRI3 QueryVersion", error);
      goto cleanup;
   }
   printf("DRI3_VERSION=%u.%u\n", version->major_version,
          version->minor_version);
   free(error);
   error = NULL;

   open_reply = xcb_dri3_open_reply(
      connection, xcb_dri3_open(connection, screen->root, 0), &error);
   if (!open_reply || open_reply->nfd != 1) {
      print_x_error("DRI3 Open", error);
      goto cleanup;
   }
   reply_fds = xcb_dri3_open_reply_fds(connection, open_reply);
   mali_fd = reply_fds ? reply_fds[0] : -1;
   if (mali_fd < 0 || fcntl(mali_fd, F_GETFD) < 0) {
      fprintf(stderr, "DRI3 Open returned an invalid fd\n");
      goto cleanup;
   }
   {
      struct stat info;
      if (fstat(mali_fd, &info) != 0 || !S_ISCHR(info.st_mode)) {
         fprintf(stderr, "DRI3 Open fd is not a character device\n");
         goto cleanup;
      }
      printf("DRI3_OPEN=PASS fd=%d rdev=%llu\n", mali_fd,
             (unsigned long long)info.st_rdev);
   }

   pixmap = xcb_generate_id(connection);
   xcb_create_pixmap(connection, screen->root_depth, pixmap, screen->root,
                     64, 64);
   gc = xcb_generate_id(connection);
   {
      const uint32_t foreground = 0x00d06020;
      const xcb_rectangle_t rectangle = {0, 0, 64, 64};
      xcb_create_gc(connection, gc, pixmap, XCB_GC_FOREGROUND, &foreground);
      xcb_poly_fill_rectangle(connection, pixmap, gc, 1, &rectangle);
   }
   xcb_sync(connection);

   buffer_reply = xcb_dri3_buffer_from_pixmap_reply(
      connection, xcb_dri3_buffer_from_pixmap(connection, pixmap), &error);
   if (!buffer_reply || buffer_reply->nfd != 1) {
      print_x_error("DRI3 BufferFromPixmap", error);
      goto cleanup;
   }
   reply_fds = xcb_dri3_buffer_from_pixmap_reply_fds(connection, buffer_reply);
   pixmap_fd = reply_fds ? reply_fds[0] : -1;
   if (pixmap_fd < 0) {
      fprintf(stderr, "DRI3 BufferFromPixmap returned no fd\n");
      goto cleanup;
   }
   printf("DRI3_EXPORT width=%u height=%u stride=%u size=%u depth=%u bpp=%u\n",
          buffer_reply->width, buffer_reply->height, buffer_reply->stride,
          buffer_reply->size, buffer_reply->depth, buffer_reply->bpp);

   {
      const size_t map_size = buffer_reply->size;
      uint8_t *map = mmap(NULL, map_size, PROT_READ, MAP_SHARED, pixmap_fd, 0);
      const size_t center = (size_t)32 * buffer_reply->stride + 32 * 4;

      if (map == MAP_FAILED) {
         fprintf(stderr, "mmap exported pixmap failed: %s\n", strerror(errno));
         goto cleanup;
      }
      if (center + 4 > map_size) {
         fprintf(stderr, "exported pixmap metadata is out of bounds\n");
         munmap(map, map_size);
         goto cleanup;
      }
      printf("CENTER_PIXEL_RAW=%02x%02x%02x%02x\n",
             map[center], map[center + 1], map[center + 2], map[center + 3]);
      if (map[center] != 0x20 || map[center + 1] != 0x60 ||
          map[center + 2] != 0xd0) {
         fprintf(stderr, "exported pixmap contents do not match X11 fill\n");
         munmap(map, map_size);
         goto cleanup;
      }
      munmap(map, map_size);
   }

   puts("TERMUX_X11_DRI3_EXPORT=PASS");
   status = 0;

cleanup:
   free(error);
   free(version);
   free(open_reply);
   free(buffer_reply);
   if (mali_fd >= 0)
      close(mali_fd);
   if (pixmap_fd >= 0)
      close(pixmap_fd);
   if (connection) {
      if (gc != XCB_NONE)
         xcb_free_gc(connection, gc);
      if (pixmap != XCB_NONE)
         xcb_free_pixmap(connection, pixmap);
      xcb_disconnect(connection);
   }
   return status;
}
