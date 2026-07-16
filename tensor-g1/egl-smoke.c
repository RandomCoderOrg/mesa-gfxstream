#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdio.h>
#include <stdlib.h>

#define STAGE(message) do { \
   fprintf(stderr, "[egl-smoke] %s\n", message); \
   fflush(stderr); \
} while (0)

static int
egl_fail(const char *operation)
{
   fprintf(stderr, "%s failed: EGL error 0x%04x\n", operation, eglGetError());
   return 1;
}

int
main(void)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)
      eglGetProcAddress("eglGetPlatformDisplayEXT");
   const EGLint config_attribs[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_NONE,
   };
   const EGLint surface_attribs[] = {
      EGL_WIDTH, 32,
      EGL_HEIGHT, 32,
      EGL_NONE,
   };
   const EGLint context_attribs[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE,
   };
   EGLDisplay display;
   EGLConfig config;
   EGLSurface surface;
   EGLContext context;
   EGLint major, minor, count;
   GLubyte pixel[4];

   if (!get_platform_display) {
      fprintf(stderr, "eglGetPlatformDisplayEXT is unavailable\n");
      return 1;
   }

   STAGE("get surfaceless display");
   display = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                  EGL_DEFAULT_DISPLAY, NULL);
   if (display == EGL_NO_DISPLAY)
      return egl_fail("eglGetPlatformDisplayEXT");

   STAGE("initialize EGL");
   if (!eglInitialize(display, &major, &minor))
      return egl_fail("eglInitialize");

   STAGE("bind OpenGL ES API");
   if (!eglBindAPI(EGL_OPENGL_ES_API))
      return egl_fail("eglBindAPI");

   STAGE("choose config");
   if (!eglChooseConfig(display, config_attribs, &config, 1, &count) ||
       count != 1)
      return egl_fail("eglChooseConfig");

   STAGE("create pbuffer");
   surface = eglCreatePbufferSurface(display, config, surface_attribs);
   if (surface == EGL_NO_SURFACE)
      return egl_fail("eglCreatePbufferSurface");

   STAGE("create context");
   context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
   if (context == EGL_NO_CONTEXT)
      return egl_fail("eglCreateContext");

   STAGE("make context current");
   if (!eglMakeCurrent(display, surface, surface, context))
      return egl_fail("eglMakeCurrent");

   printf("EGL %d.%d\n", major, minor);
   printf("GL_RENDERER=%s\n", glGetString(GL_RENDERER));
   printf("GL_VERSION=%s\n", glGetString(GL_VERSION));

   STAGE("submit clear");
   glClearColor(0.125f, 0.25f, 0.5f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   STAGE("wait for GPU");
   glFinish();

   STAGE("read back rendered pixel");
   glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);

   if (glGetError() != GL_NO_ERROR) {
      fprintf(stderr, "render failed with a GL error\n");
      return 1;
   }

   printf("PIXEL=%u,%u,%u,%u\n",
          pixel[0], pixel[1], pixel[2], pixel[3]);
   if (abs((int) pixel[0] - 32) > 1 ||
       abs((int) pixel[1] - 64) > 1 ||
       abs((int) pixel[2] - 128) > 1 ||
       pixel[3] != 255) {
      fprintf(stderr, "rendered pixel does not match the clear colour\n");
      return 1;
   }

   puts("Kbase render smoke test: PASS");
   return 0;
}
