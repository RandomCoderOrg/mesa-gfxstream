/*
 * Reproduce applications which copy pixels from a GLX window back buffer
 * into a texture (Blender 2.79 uses this while composing its UI).
 *
 * Build: cc default-fb-copy.c -o default-fb-copy -lX11 -lGL
 */

#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>

static GLenum
copy_back_buffer(const char *phase)
{
   GLint read_buffer = 0;

   while (glGetError() != GL_NO_ERROR)
      ;

   glReadBuffer(GL_BACK);
   GLenum read_error = glGetError();
   glGetIntegerv(GL_READ_BUFFER, &read_buffer);

   glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, 16, 16);
   GLenum copy_error = glGetError();

   printf("{\"phase\":\"%s\",\"read_buffer\":\"0x%x\","
          "\"read_error\":\"0x%x\",\"copy_error\":\"0x%x\"}\n",
          phase, read_buffer, read_error, copy_error);
   return copy_error;
}

int
main(void)
{
   Display *dpy = XOpenDisplay(NULL);
   if (!dpy) {
      fputs("XOpenDisplay failed\n", stderr);
      return 2;
   }

   int attributes[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8,
                       GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
   XVisualInfo *visual = glXChooseVisual(dpy, DefaultScreen(dpy), attributes);
   if (!visual) {
      fputs("glXChooseVisual failed\n", stderr);
      return 3;
   }

   Colormap cmap = XCreateColormap(dpy, RootWindow(dpy, visual->screen),
                                   visual->visual, AllocNone);
   XSetWindowAttributes window_attributes = {.colormap = cmap,
                                              .event_mask = ExposureMask};
   Window window = XCreateWindow(dpy, RootWindow(dpy, visual->screen),
                                 0, 0, 64, 64, 0, visual->depth, InputOutput,
                                 visual->visual, CWColormap | CWEventMask,
                                 &window_attributes);
   XMapWindow(dpy, window);
   XSync(dpy, False);

   GLXContext context = glXCreateContext(dpy, visual, NULL, True);
   if (!context || !glXMakeCurrent(dpy, window, context)) {
      fputs("GLX context creation failed\n", stderr);
      return 4;
   }

   printf("{\"renderer\":\"%s\"}\n", glGetString(GL_RENDERER));

   GLuint texture = 0;
   glGenTextures(1, &texture);
   glBindTexture(GL_TEXTURE_2D, texture);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 16, 16, 0, GL_RGB,
                GL_UNSIGNED_BYTE, NULL);

   GLenum first = copy_back_buffer("before-draw");

   glClearColor(0.8f, 0.1f, 0.1f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glFinish();
   GLenum after_draw = copy_back_buffer("after-draw");

   glXSwapBuffers(dpy, window);
   glFinish();
   GLenum after_swap = copy_back_buffer("after-swap");

   glDeleteTextures(1, &texture);
   glXMakeCurrent(dpy, None, NULL);
   glXDestroyContext(dpy, context);
   XDestroyWindow(dpy, window);
   XFreeColormap(dpy, cmap);
   XFree(visual);
   XCloseDisplay(dpy);

   return first == GL_NO_ERROR && after_draw == GL_NO_ERROR &&
                  after_swap == GL_NO_ERROR
             ? 0
             : 1;
}
