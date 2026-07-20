#define _POSIX_C_SOURCE 200809L
#define GL_GLEXT_PROTOTYPES

#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STAGE(message) do { \
   fprintf(stderr, "[glx-tfp-smoke] %s\n", message); \
   fflush(stderr); \
} while (0)

static int
fail(const char *message)
{
   fprintf(stderr, "[glx-tfp-smoke] FAIL: %s\n", message);
   return 1;
}

enum timing_stage {
   TIMING_X_UPDATE,
   TIMING_BIND,
   TIMING_GPU_DRAW,
   TIMING_PRESENT,
   TIMING_VERIFY,
   TIMING_RELEASE,
   TIMING_STAGE_COUNT,
};

static uint64_t
monotonic_ns(void)
{
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return (uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec;
}

static int
compare_u64(const void *left, const void *right)
{
   const uint64_t a = *(const uint64_t *)left;
   const uint64_t b = *(const uint64_t *)right;
   return (a > b) - (a < b);
}

int
main(int argc, char **argv)
{
   static const int config_attribs[] = {
      GLX_X_RENDERABLE, True,
      GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT | GLX_PIXMAP_BIT,
      GLX_RENDER_TYPE, GLX_RGBA_BIT,
      GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
      GLX_RED_SIZE, 8,
      GLX_GREEN_SIZE, 8,
      GLX_BLUE_SIZE, 8,
      GLX_DOUBLEBUFFER, True,
      GLX_BIND_TO_TEXTURE_RGB_EXT, True,
      None,
   };
   static const int pixmap_attribs[] = {
      GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGB_EXT,
      GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
      GLX_MIPMAP_TEXTURE_EXT, False,
      None,
   };
   unsigned width = argc > 1 ? strtoul(argv[1], NULL, 10) : 1024;
   unsigned height = argc > 2 ? strtoul(argv[2], NULL, 10) : 768;
   unsigned iterations = argc > 3 ? strtoul(argv[3], NULL, 10) : 16;
   if (!width || !height || !iterations || width > 8192 || height > 8192 ||
       iterations > 10000)
      return fail("usage: glx-tfp-smoke [width height iterations]");

   uint64_t *timings = calloc((size_t)iterations * TIMING_STAGE_COUNT,
                              sizeof(*timings));
   if (!timings)
      return fail("timing allocation");

   Display *display = XOpenDisplay(NULL);
   if (!display)
      return fail("XOpenDisplay");

   int config_count = 0;
   GLXFBConfig *configs = glXChooseFBConfig(display, DefaultScreen(display),
                                            config_attribs, &config_count);
   if (!configs || config_count < 1)
      return fail("no texture-from-pixmap FBConfig");

   GLXFBConfig config = configs[0];
   XVisualInfo *visual = glXGetVisualFromFBConfig(display, config);
   if (!visual)
      return fail("glXGetVisualFromFBConfig");

   Window root = RootWindow(display, visual->screen);
   const int alternate_pixmaps = getenv("TFP_ALTERNATE_PIXMAPS") != NULL;
   Pixmap sources[2] = {
      XCreatePixmap(display, root, width, height, visual->depth),
      None,
   };
   if (alternate_pixmaps)
      sources[1] = XCreatePixmap(display, root, width, height, visual->depth);
   GC gc = XCreateGC(display, sources[0], 0, NULL);
   if (!sources[0] || (alternate_pixmaps && !sources[1]) || !gc)
      return fail("source pixmap allocation");

   XSetForeground(display, gc, 0x2040c0);
   XFillRectangle(display, sources[0], gc, 0, 0, width, height);
   if (alternate_pixmaps) {
      XFillRectangle(display, sources[1], gc, 0, 0, width, height);
      XSetForeground(display, gc, 0xe08020);
      XFillRectangle(display, sources[0], gc, width / 4, height / 4,
                     width / 2, height / 2);
      XSetForeground(display, gc, 0x20c060);
      XFillRectangle(display, sources[1], gc, width / 4, height / 4,
                     width / 2, height / 2);
   }
   GLXPixmap glx_sources[2] = {
      glXCreatePixmap(display, config, sources[0], pixmap_attribs),
      None,
   };
   if (alternate_pixmaps)
      glx_sources[1] = glXCreatePixmap(display, config, sources[1],
                                       pixmap_attribs);
   if (!glx_sources[0] || (alternate_pixmaps && !glx_sources[1]))
      return fail("glXCreatePixmap");

   XSetWindowAttributes window_attribs = {
      .colormap = XCreateColormap(display, root, visual->visual, AllocNone),
      .event_mask = ExposureMask | StructureNotifyMask,
   };
   Window window = XCreateWindow(display, root, 80, 80, width, height, 0,
                                 visual->depth, InputOutput, visual->visual,
                                 CWColormap | CWEventMask, &window_attribs);
   XStoreName(display, window, "Tensor G1 GLX texture-from-pixmap probe");
   XMapWindow(display, window);
   XSync(display, False);

   GLXWindow glx_window = glXCreateWindow(display, config, window, NULL);
   if (!glx_window)
      return fail("glXCreateWindow");

   GLXContext context = glXCreateNewContext(display, config,
                                            GLX_RGBA_TYPE, NULL, True);
   if (!context || !glXMakeCurrent(display, glx_window, context))
      return fail("GLX context creation");

   PFNGLXBINDTEXIMAGEEXTPROC bind_tex_image =
      (PFNGLXBINDTEXIMAGEEXTPROC)
      glXGetProcAddressARB((const GLubyte *)"glXBindTexImageEXT");
   PFNGLXRELEASETEXIMAGEEXTPROC release_tex_image =
      (PFNGLXRELEASETEXIMAGEEXTPROC)
      glXGetProcAddressARB((const GLubyte *)"glXReleaseTexImageEXT");
   if (!bind_tex_image || !release_tex_image)
      return fail("GLX_EXT_texture_from_pixmap entrypoints");

   const char *extensions = glXQueryExtensionsString(display, visual->screen);
   if (!extensions || !strstr(extensions, "GLX_EXT_texture_from_pixmap"))
      return fail("GLX_EXT_texture_from_pixmap missing");

   GLuint texture = 0;
   GLuint sample_fbo = 0;
   GLuint sample_color = 0;
   glGenTextures(1, &texture);
   glBindTexture(GL_TEXTURE_2D, texture);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

   glGenFramebuffers(1, &sample_fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, sample_fbo);
   glGenRenderbuffers(1, &sample_color);
   glBindRenderbuffer(GL_RENDERBUFFER, sample_color);
   glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_RENDERBUFFER, sample_color);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      return fail("sample framebuffer creation");
   glBindFramebuffer(GL_FRAMEBUFFER, 0);

   unsigned long pixel = 0;
   for (unsigned frame = 0; frame < iterations; ++frame) {
      uint64_t started = monotonic_ns();
      const unsigned long expected = frame & 1 ? 0x20c060 : 0xe08020;
      const unsigned source_index = alternate_pixmaps ? frame & 1 : 0;
      if (!alternate_pixmaps) {
         XSetForeground(display, gc, expected);
         XFillRectangle(display, sources[0], gc, width / 4, height / 4,
                        width / 2, height / 2);
      }
      XSync(display, False);
      timings[TIMING_X_UPDATE * iterations + frame] =
         monotonic_ns() - started;

      if (frame == 0)
         STAGE("bind, render, present and release X pixmap texture");
      started = monotonic_ns();
      glBindTexture(GL_TEXTURE_2D, texture);
      bind_tex_image(display, glx_sources[source_index], GLX_FRONT_LEFT_EXT,
                     NULL);
      timings[TIMING_BIND * iterations + frame] = monotonic_ns() - started;

      {
         GLubyte sampled[4] = {0};
         glBindFramebuffer(GL_FRAMEBUFFER, sample_fbo);
         glViewport(0, 0, width, height);
         glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
         glClear(GL_COLOR_BUFFER_BIT);
         glEnable(GL_TEXTURE_2D);
         glBegin(GL_QUADS);
         glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, -1.0f);
         glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f, -1.0f);
         glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f,  1.0f);
         glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f,  1.0f);
         glEnd();
         glFinish();
         glReadPixels(width / 2, height / 2, 1, 1,
                      GL_RGBA, GL_UNSIGNED_BYTE, sampled);
         printf("GPU_SAMPLE frame=%u pixel=%u,%u,%u,%u\n", frame,
                sampled[0], sampled[1], sampled[2], sampled[3]);
         fflush(stdout);
         glBindFramebuffer(GL_FRAMEBUFFER, 0);
      }

      started = monotonic_ns();
      glViewport(0, 0, width, height);
      glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      glEnable(GL_TEXTURE_2D);
      glBegin(GL_QUADS);
      glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, -1.0f);
      glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f, -1.0f);
      glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f,  1.0f);
      glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f,  1.0f);
      glEnd();
      glFinish();
      timings[TIMING_GPU_DRAW * iterations + frame] =
         monotonic_ns() - started;
      if (glGetError() != GL_NO_ERROR)
         return fail("OpenGL error while sampling X pixmap");

      started = monotonic_ns();
      glXSwapBuffers(display, glx_window);
      XSync(display, False);
      timings[TIMING_PRESENT * iterations + frame] =
         monotonic_ns() - started;

      started = monotonic_ns();
      unsigned settle_attempts = 0;
      do {
         XImage *image = XGetImage(display, window, width / 2, height / 2,
                                   1, 1, AllPlanes, ZPixmap);
         if (!image)
            return fail("XGetImage after Present");
         pixel = XGetPixel(image, 0, 0) & 0x00ffffff;
         XDestroyImage(image);
         if (pixel == expected)
            break;
         const struct timespec pause = { .tv_nsec = 5000000 };
         nanosleep(&pause, NULL);
      } while (++settle_attempts < 100);
      timings[TIMING_VERIFY * iterations + frame] =
         monotonic_ns() - started;
      if (frame == 0)
         printf("PRESENT_SETTLE_ATTEMPTS=%u\n", settle_attempts);
      if (pixel != expected) {
         fprintf(stderr,
                 "[glx-tfp-smoke] FAIL: frame=%u expected=0x%08lx got=0x%08lx\n",
                 frame, expected, pixel);
         return 1;
      }

      started = monotonic_ns();
      release_tex_image(display, glx_sources[source_index],
                        GLX_FRONT_LEFT_EXT);
      timings[TIMING_RELEASE * iterations + frame] =
         monotonic_ns() - started;
   }

   printf("GL_RENDERER=%s\n", glGetString(GL_RENDERER));
   printf("SIZE=%ux%u ITERATIONS=%u\n", width, height, iterations);
   printf("SOURCE_MODE=%s\n", alternate_pixmaps ? "alternating" : "updated");
   printf("X_PIXEL=0x%08lx\n", pixel);
   printf("GLX_TEXTURE_FROM_PIXMAP=PASS\n");
   static const char *stage_names[TIMING_STAGE_COUNT] = {
      "x_update", "bind", "gpu_draw", "present", "verify", "release",
   };
   for (unsigned stage = 0; stage < TIMING_STAGE_COUNT; ++stage) {
      uint64_t *samples = timings + stage * iterations;
      uint64_t total = 0;
      uint64_t maximum = 0;
      for (unsigned frame = 0; frame < iterations; ++frame) {
         total += samples[frame];
         if (samples[frame] > maximum)
            maximum = samples[frame];
      }
      qsort(samples, iterations, sizeof(*samples), compare_u64);
      const unsigned p50 = (iterations - 1) * 50 / 100;
      const unsigned p95 = (iterations - 1) * 95 / 100;
      printf("{\"schema\":\"tensor-perf-v1\",\"kind\":\"stage\","
             "\"stage\":\"tfp_%s\",\"count\":%u,\"total_ns\":%llu,"
             "\"p50_ns\":%llu,\"p95_ns\":%llu,\"max_ns\":%llu}\n",
             stage_names[stage], iterations, (unsigned long long)total,
             (unsigned long long)samples[p50],
             (unsigned long long)samples[p95],
             (unsigned long long)maximum);
   }

   glDeleteTextures(1, &texture);
   glDeleteRenderbuffers(1, &sample_color);
   glDeleteFramebuffers(1, &sample_fbo);
   glXMakeCurrent(display, None, NULL);
   glXDestroyContext(display, context);
   glXDestroyWindow(display, glx_window);
   glXDestroyPixmap(display, glx_sources[0]);
   if (glx_sources[1])
      glXDestroyPixmap(display, glx_sources[1]);
   XDestroyWindow(display, window);
   XFreeGC(display, gc);
   XFreePixmap(display, sources[0]);
   if (sources[1])
      XFreePixmap(display, sources[1]);
   XFreeColormap(display, window_attribs.colormap);
   XFree(visual);
   XFree(configs);
   XCloseDisplay(display);
   free(timings);
   return 0;
}
