#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Keep this probe buildable with the runtime libXtst package alone. Ubuntu's
 * minimal PRoot does not install the development header or unversioned link. */
extern int XTestFakeButtonEvent(Display *, unsigned int, Bool, unsigned long);
extern int XTestFakeMotionEvent(Display *, int, int, int, unsigned long);

static void
sleep_ns(long nanoseconds)
{
   struct timespec delay = {
      .tv_sec = nanoseconds / 1000000000L,
      .tv_nsec = nanoseconds % 1000000000L,
   };

   while (nanosleep(&delay, &delay) != 0)
      ;
}

int
main(int argc, char **argv)
{
   double duration = argc > 1 ? strtod(argv[1], NULL) : 5.0;
   int fps = argc > 2 ? atoi(argv[2]) : 60;
   if (!(duration > 0.0) || fps < 1 || fps > 240) {
      fprintf(stderr, "usage: %s [seconds] [fps]\n", argv[0]);
      return 2;
   }

   Display *display = XOpenDisplay(NULL);
   if (!display) {
      fprintf(stderr, "cannot open X display\n");
      return 1;
   }

   const int screen = DefaultScreen(display);
   const int width = DisplayWidth(display, screen);
   const int height = DisplayHeight(display, screen);
   const int launcher_x = 52;
   const int launcher_y = height - 24;

   XTestFakeMotionEvent(display, screen, launcher_x, launcher_y, CurrentTime);
   XTestFakeButtonEvent(display, 1, True, CurrentTime);
   XTestFakeButtonEvent(display, 1, False, CurrentTime);
   XSync(display, False);
   sleep_ns(750000000L);

   const unsigned frames = (unsigned)llround(duration * fps);
   const long frame_ns = 1000000000L / fps;
   for (unsigned frame = 0; frame < frames; frame++) {
      const double phase = frames > 1 ? (double)frame / (frames - 1) : 0.0;
      const double sweep = phase * 8.0;
      const unsigned row = (unsigned)sweep;
      const double fraction = sweep - row;
      const bool reverse = row & 1;
      const double horizontal = reverse ? 1.0 - fraction : fraction;
      const int x = 90 + (int)llround(horizontal * 470.0);
      const int y = height - 120 - (int)(row % 8) * 70;

      XTestFakeMotionEvent(display, screen, x, y, CurrentTime);
      XFlush(display);
      sleep_ns(frame_ns);
   }

   printf("{\"schema\":\"tensor-perf-v1\",\"kind\":\"x11-menu-hover\","
          "\"requested_frames\":%u,\"target_fps\":%d,"
          "\"display_width\":%d,\"display_height\":%d}\n",
          frames, fps, width, height);
   XCloseDisplay(display);
   return 0;
}
