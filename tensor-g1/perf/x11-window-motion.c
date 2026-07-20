#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double
seconds_between(const struct timespec *start, const struct timespec *end)
{
   return (double)(end->tv_sec - start->tv_sec) +
          (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

static void
add_ns(struct timespec *value, long nanoseconds)
{
   value->tv_nsec += nanoseconds;
   while (value->tv_nsec >= 1000000000L) {
      value->tv_nsec -= 1000000000L;
      value->tv_sec++;
   }
}

static int
compare_double(const void *left, const void *right)
{
   const double a = *(const double *)left;
   const double b = *(const double *)right;
   return (a > b) - (a < b);
}

int
main(int argc, char **argv)
{
   double duration = 2.0;
   double hold_before_destroy = 0.0;
   int fps = 60;

   if (argc > 1)
      duration = strtod(argv[1], NULL);
   if (argc > 2)
      fps = atoi(argv[2]);
   if (argc > 3)
      hold_before_destroy = strtod(argv[3], NULL);
   if (!(duration > 0.0) || fps < 1 || fps > 240 ||
       hold_before_destroy < 0.0) {
      fprintf(stderr, "usage: %s [seconds] [fps] [hold-before-destroy-seconds]\n",
              argv[0]);
      return 2;
   }

   Display *display = XOpenDisplay(NULL);
   if (!display) {
      fprintf(stderr, "cannot open X display\n");
      return 1;
   }

   const int screen = DefaultScreen(display);
   char compositor_selection_name[32];
   snprintf(compositor_selection_name, sizeof(compositor_selection_name),
            "_NET_WM_CM_S%d", screen);
   Atom compositor_selection =
      XInternAtom(display, compositor_selection_name, False);
   Window compositor_owner = XGetSelectionOwner(display, compositor_selection);
   const int display_width = DisplayWidth(display, screen);
   const int display_height = DisplayHeight(display, screen);
   const unsigned width = display_width > 1200 ? 960 : display_width * 3 / 5;
   const unsigned height = display_height > 700 ? 540 : display_height * 3 / 5;
   const int x_span = display_width - (int)width - 40;
   const int y = (display_height - (int)height) / 2;
   const unsigned long black = BlackPixel(display, screen);
   const unsigned long white = WhitePixel(display, screen);

   Window window = XCreateSimpleWindow(display, RootWindow(display, screen),
                                       20, y, width, height, 2, white, black);
   XStoreName(display, window, "Tensor G1 window-motion probe");
   XSelectInput(display, window, ExposureMask | StructureNotifyMask);
   XMapWindow(display, window);

   GC gc = XCreateGC(display, window, 0, NULL);
   XSetForeground(display, gc, white);
   for (unsigned row = 0; row < 12; row++) {
      for (unsigned column = 0; column < 20; column++) {
         if ((row + column) & 1)
            XFillRectangle(display, window, gc, column * width / 20,
                           row * height / 12, width / 20 + 1,
                           height / 12 + 1);
      }
   }
   XSync(display, False);

   const unsigned requested = (unsigned)llround(duration * fps);
   double *lateness_ms = calloc(requested, sizeof(*lateness_ms));
   if (!lateness_ms) {
      fprintf(stderr, "allocation failed\n");
      return 1;
   }

   const long frame_ns = 1000000000L / fps;
   struct timespec start;
   struct timespec deadline;
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &start);
   deadline = start;

   for (unsigned frame = 0; frame < requested; frame++) {
      const double phase = requested > 1 ? (double)frame / (requested - 1) : 0.0;
      const double triangle = phase < 0.5 ? phase * 2.0 : (1.0 - phase) * 2.0;
      const int x = 20 + (int)llround(triangle * (x_span > 0 ? x_span : 0));

      XMoveWindow(display, window, x, y);
      XFlush(display);

      add_ns(&deadline, frame_ns);
      do {
         errno = 0;
      } while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline,
                               NULL) == EINTR);
      clock_gettime(CLOCK_MONOTONIC, &now);
      lateness_ms[frame] = seconds_between(&deadline, &now) * 1000.0;
   }

   XSync(display, False);
   clock_gettime(CLOCK_MONOTONIC, &now);
   const double elapsed = seconds_between(&start, &now);
   qsort(lateness_ms, requested, sizeof(*lateness_ms), compare_double);
   const unsigned p50_index = (requested - 1) * 50 / 100;
   const unsigned p95_index = (requested - 1) * 95 / 100;

   printf("{\"schema\":\"tensor-perf-v1\",\"kind\":\"x11-window-motion\","
          "\"requested_frames\":%u,\"target_fps\":%d,"
          "\"compositor_owner\":%lu,"
          "\"hold_before_destroy_seconds\":%.3f,"
          "\"elapsed_seconds\":%.6f,\"request_fps\":%.3f,"
          "\"schedule_lateness_p50_ms\":%.3f,"
          "\"schedule_lateness_p95_ms\":%.3f,"
          "\"schedule_lateness_max_ms\":%.3f}\n",
          requested, fps, compositor_owner, hold_before_destroy, elapsed,
          requested / elapsed,
          lateness_ms[p50_index], lateness_ms[p95_index],
          lateness_ms[requested - 1]);
   fflush(stdout);

   if (hold_before_destroy > 0.0) {
      struct timespec hold = {
         .tv_sec = (time_t)hold_before_destroy,
         .tv_nsec = (long)((hold_before_destroy - (time_t)hold_before_destroy) *
                           1000000000.0),
      };
      fprintf(stderr, "lifecycle=holding-window\n");
      fflush(stderr);
      while (nanosleep(&hold, &hold) == -1 && errno == EINTR)
         ;
   }

   free(lateness_ms);
   XFreeGC(display, gc);
   fprintf(stderr, "lifecycle=destroy-request\n");
   fflush(stderr);
   XDestroyWindow(display, window);
   XSync(display, False);
   fprintf(stderr, "lifecycle=destroy-complete\n");
   fflush(stderr);
   XCloseDisplay(display);
   return 0;
}
