#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define STAGE(message) do { \
   fprintf(stderr, "[glx-x11-smoke] %s\n", message); \
   fflush(stderr); \
} while (0)

int
main(void)
{
   static int visual_attribs[] = {
      GLX_RGBA,
      GLX_DOUBLEBUFFER,
      GLX_RED_SIZE, 8,
      GLX_GREEN_SIZE, 8,
      GLX_BLUE_SIZE, 8,
      None,
   };
   Display *display;
   XVisualInfo *visual;
   XSetWindowAttributes window_attribs;
   Colormap colormap;
   Window window;
   GLXContext context;
   GLubyte pixel[4];

   STAGE("open X display and select GLX visual");
   display = XOpenDisplay(NULL);
   if (!display) {
      fprintf(stderr, "XOpenDisplay failed\n");
      return 1;
   }
   visual = glXChooseVisual(display, DefaultScreen(display), visual_attribs);
   if (!visual) {
      fprintf(stderr, "glXChooseVisual failed\n");
      return 1;
   }

   STAGE("create and map X window");
   colormap = XCreateColormap(display,
                              RootWindow(display, visual->screen),
                              visual->visual, AllocNone);
   window_attribs.colormap = colormap;
   window_attribs.event_mask = ExposureMask | StructureNotifyMask;
   window = XCreateWindow(display,
                          RootWindow(display, visual->screen),
                          64, 96, 480, 320, 0,
                          visual->depth, InputOutput, visual->visual,
                          CWColormap | CWEventMask, &window_attribs);
   XStoreName(display, window, "Tensor G1 Panfrost GLX smoke");
   XMapWindow(display, window);
   XFlush(display);

   STAGE("create desktop OpenGL context");
   context = glXCreateContext(display, visual, NULL, True);
   if (!context || !glXMakeCurrent(display, window, context)) {
      fprintf(stderr, "GLX context creation failed\n");
      return 1;
   }

   STAGE("render on Mali and copy completed frame to X11");
   glViewport(0, 0, 480, 320);
   glClearColor(0.875f, 0.375f, 0.125f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glFinish();
   glReadPixels(240, 160, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
   glXSwapBuffers(display, window);

   printf("GL_RENDERER=%s\n", glGetString(GL_RENDERER));
   printf("GL_VERSION=%s\n", glGetString(GL_VERSION));
   printf("PIXEL=%u,%u,%u,%u\n", pixel[0], pixel[1], pixel[2], pixel[3]);
   puts("GLX window presented; holding for 8 seconds");
   fflush(stdout);
   sleep(8);

   glXMakeCurrent(display, None, NULL);
   glXDestroyContext(display, context);
   XDestroyWindow(display, window);
   XFreeColormap(display, colormap);
   XFree(visual);
   XCloseDisplay(display);
   return 0;
}
