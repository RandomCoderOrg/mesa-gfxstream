#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int
parse_positive(const char *value, const char *name)
{
   char *end = NULL;
   errno = 0;
   long parsed = strtol(value, &end, 10);
   if (errno || !end || *end || parsed < 1 || parsed > 16384) {
      fprintf(stderr, "invalid %s: %s\n", name, value);
      exit(2);
   }
   return (int)parsed;
}

int
main(int argc, char **argv)
{
   const int width = argc > 1 ? parse_positive(argv[1], "width") : 2240;
   const int height = argc > 2 ? parse_positive(argv[2], "height") : 256;
   const int hold_ms = argc > 3 ? parse_positive(argv[3], "hold-ms") : 1500;

   Display *display = XOpenDisplay(NULL);
   if (!display) {
      fprintf(stderr, "cannot open X display\n");
      return 1;
   }

   const int screen = DefaultScreen(display);
   XVisualInfo visual_info;
   if (!XMatchVisualInfo(display, screen, 32, TrueColor, &visual_info)) {
      fprintf(stderr, "no 32-bit TrueColor visual\n");
      XCloseDisplay(display);
      return 1;
   }

   char compositor_selection_name[32];
   snprintf(compositor_selection_name, sizeof(compositor_selection_name),
            "_NET_WM_CM_S%d", screen);
   Atom compositor_selection =
      XInternAtom(display, compositor_selection_name, False);
   Window compositor_owner = XGetSelectionOwner(display, compositor_selection);

   XSetWindowAttributes attributes = {
      .background_pixel = 0xff182030,
      .border_pixel = 0xffd8dee9,
      .colormap = XCreateColormap(display, RootWindow(display, screen),
                                 visual_info.visual, AllocNone),
   };
   const unsigned long attribute_mask =
      CWBackPixel | CWBorderPixel | CWColormap;
   Window window = XCreateWindow(
      display, RootWindow(display, screen), 0, 0, (unsigned)width,
      (unsigned)height, 0, visual_info.depth, InputOutput, visual_info.visual,
      attribute_mask, &attributes);
   if (!window) {
      fprintf(stderr, "XCreateWindow failed\n");
      XFreeColormap(display, attributes.colormap);
      XCloseDisplay(display);
      return 1;
   }

   XStoreName(display, window, "Tensor G1 ARGB stride probe");
   XMapWindow(display, window);

   GC gc = XCreateGC(display, window, 0, NULL);
   for (int row = 0; row < 8; row++) {
      XSetForeground(display, gc,
                     row & 1 ? 0xffbf616aUL : 0xff88c0d0UL);
      XFillRectangle(display, window, gc, 0,
                     row * height / 8, (unsigned)width,
                     (unsigned)(height / 8 + 1));
   }
   XSync(display, False);

   const struct timespec hold = {
      .tv_sec = hold_ms / 1000,
      .tv_nsec = (long)(hold_ms % 1000) * 1000000L,
   };
   struct timespec remaining = hold;
   while (nanosleep(&remaining, &remaining) == -1 && errno == EINTR)
      ;

   const uint64_t tight_stride = (uint64_t)width * 4;
   printf("{\"schema\":\"tensor-perf-v1\","
          "\"kind\":\"x11-argb-surface\","
          "\"width\":%d,\"height\":%d,\"depth\":%d,"
          "\"tight_stride\":%llu,\"stride_mod_64\":%llu,"
          "\"compositor_owner\":%lu,\"hold_ms\":%d}\n",
          width, height, visual_info.depth,
          (unsigned long long)tight_stride,
          (unsigned long long)(tight_stride % 64),
          compositor_owner, hold_ms);
   fflush(stdout);

   XFreeGC(display, gc);
   XDestroyWindow(display, window);
   XFreeColormap(display, attributes.colormap);
   XSync(display, False);
   XCloseDisplay(display);
   return 0;
}
