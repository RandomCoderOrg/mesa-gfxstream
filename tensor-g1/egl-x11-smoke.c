#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define STAGE(message) do { \
   fprintf(stderr, "[egl-x11-smoke] %s\n", message); \
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
   static const EGLint config_attribs[] = {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 0,
      EGL_NONE,
   };
   static const EGLint context_attribs[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE,
   };
   Display *xdisplay;
   XVisualInfo visual_template;
   XVisualInfo *visual;
   XSetWindowAttributes window_attribs;
   EGLDisplay display;
   EGLConfig config;
   EGLSurface surface;
   EGLContext context;
   EGLint major, minor, count, visual_id;
   Window window;
   Colormap colormap;
   GLubyte pixel[4];
   int visual_count;

   STAGE("open X display");
   xdisplay = XOpenDisplay(NULL);
   if (!xdisplay) {
      fprintf(stderr, "XOpenDisplay failed\n");
      return 1;
   }

   STAGE("get EGL display");
   display = eglGetDisplay((EGLNativeDisplayType)xdisplay);
   if (display == EGL_NO_DISPLAY)
      return egl_fail("eglGetDisplay");
   STAGE("initialize EGL");
   if (!eglInitialize(display, &major, &minor))
      return egl_fail("eglInitialize");
   STAGE("choose EGL config");
   if (!eglBindAPI(EGL_OPENGL_ES_API))
      return egl_fail("eglBindAPI");
   if (!eglChooseConfig(display, config_attribs, &config, 1, &count) || count != 1)
      return egl_fail("eglChooseConfig");
   if (!eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &visual_id))
      return egl_fail("eglGetConfigAttrib");

   visual_template.visualid = visual_id;
   visual = XGetVisualInfo(xdisplay, VisualIDMask, &visual_template, &visual_count);
   if (!visual || visual_count < 1) {
      fprintf(stderr, "XGetVisualInfo failed for visual 0x%x\n", visual_id);
      return 1;
   }

   STAGE("create and map X window");
   colormap = XCreateColormap(xdisplay,
                              RootWindow(xdisplay, visual->screen),
                              visual->visual, AllocNone);
   window_attribs.colormap = colormap;
   window_attribs.event_mask = ExposureMask | StructureNotifyMask;
   window = XCreateWindow(xdisplay,
                          RootWindow(xdisplay, visual->screen),
                          32, 32, 480, 320, 0,
                          visual->depth, InputOutput, visual->visual,
                          CWColormap | CWEventMask, &window_attribs);
   XStoreName(xdisplay, window, "Tensor G1 Panfrost X11 smoke");
   XMapWindow(xdisplay, window);
   XFlush(xdisplay);

   STAGE("create EGL window surface and context");
   surface = eglCreateWindowSurface(display, config,
                                    (EGLNativeWindowType)window, NULL);
   if (surface == EGL_NO_SURFACE)
      return egl_fail("eglCreateWindowSurface");
   context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
   if (context == EGL_NO_CONTEXT)
      return egl_fail("eglCreateContext");
   if (!eglMakeCurrent(display, surface, surface, context))
      return egl_fail("eglMakeCurrent");

   STAGE("render on Mali");
   glViewport(0, 0, 480, 320);
   glClearColor(0.125f, 0.625f, 0.875f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glFinish();
   glReadPixels(240, 160, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
   STAGE("copy completed frame to X11");
   if (!eglSwapBuffers(display, surface))
      return egl_fail("eglSwapBuffers");

   printf("EGL %d.%d\n", major, minor);
   printf("GL_RENDERER=%s\n", glGetString(GL_RENDERER));
   printf("PIXEL=%u,%u,%u,%u\n", pixel[0], pixel[1], pixel[2], pixel[3]);
   puts("X11 window presented; holding for 8 seconds");
   fflush(stdout);
   sleep(8);

   eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(display, context);
   eglDestroySurface(display, surface);
   eglTerminate(display);
   XDestroyWindow(xdisplay, window);
   XFreeColormap(xdisplay, colormap);
   XFree(visual);
   XCloseDisplay(xdisplay);
   return 0;
}
