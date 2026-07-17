#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define GRID 10

int
main(void)
{
   static int visual_attribs[] = {
      GLX_RGBA,
      GLX_DOUBLEBUFFER,
      GLX_RED_SIZE, 8,
      GLX_GREEN_SIZE, 8,
      GLX_BLUE_SIZE, 8,
      GLX_DEPTH_SIZE, 24,
      None,
   };
   Display *display = XOpenDisplay(NULL);
   if (!display)
      return 1;

   XVisualInfo *visual =
      glXChooseVisual(display, DefaultScreen(display), visual_attribs);
   if (!visual)
      return 1;

   Colormap colormap = XCreateColormap(display,
                                       RootWindow(display, visual->screen),
                                       visual->visual, AllocNone);
   XSetWindowAttributes attrs = {
      .colormap = colormap,
      .event_mask = ExposureMask | StructureNotifyMask,
   };
   Window window = XCreateWindow(display,
                                 RootWindow(display, visual->screen),
                                 80, 80, 600, 600, 0,
                                 visual->depth, InputOutput, visual->visual,
                                 CWColormap | CWEventMask, &attrs);
   XStoreName(display, window, "Tensor G1 Panfrost lit indexed grid");
   XMapWindow(display, window);
   XFlush(display);

   GLXContext context = glXCreateContext(display, visual, NULL, True);
   if (!context || !glXMakeCurrent(display, window, context))
      return 1;

   const unsigned vertex_count = (GRID + 1) * (GRID + 1);
   const unsigned index_count = GRID * GRID * 6;
   GLfloat *vertices = calloc(vertex_count * 3, sizeof(*vertices));
   GLfloat *normals = calloc(vertex_count * 3, sizeof(*normals));
   GLuint *indices = calloc(index_count, sizeof(*indices));
   if (!vertices || !normals || !indices)
      return 1;

   for (unsigned y = 0; y <= GRID; ++y) {
      for (unsigned x = 0; x <= GRID; ++x) {
         unsigned v = y * (GRID + 1) + x;
         vertices[v * 3 + 0] = -0.9f + (1.8f * x / GRID);
         vertices[v * 3 + 1] = -0.9f + (1.8f * y / GRID);
         vertices[v * 3 + 2] = 0.0f;
         normals[v * 3 + 2] = 1.0f;
      }
   }

   unsigned i = 0;
   for (unsigned y = 0; y < GRID; ++y) {
      for (unsigned x = 0; x < GRID; ++x) {
         GLuint a = y * (GRID + 1) + x;
         GLuint b = a + 1;
         GLuint c = a + (GRID + 1);
         GLuint d = c + 1;
         indices[i++] = a;
         indices[i++] = b;
         indices[i++] = d;
         indices[i++] = a;
         indices[i++] = d;
         indices[i++] = c;
      }
   }

   static const GLfloat light_position[] = { 0.0f, 0.0f, 1.0f, 0.0f };
   static const GLfloat material[] = { 0.125f, 0.875f, 0.375f, 1.0f };
   glLightfv(GL_LIGHT0, GL_POSITION, light_position);
   glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, material);
   glEnable(GL_LIGHTING);
   glEnable(GL_LIGHT0);
   glEnable(GL_DEPTH_TEST);
   glClearColor(0.03125f, 0.03125f, 0.03125f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

   glEnableClientState(GL_VERTEX_ARRAY);
   glEnableClientState(GL_NORMAL_ARRAY);
   glVertexPointer(3, GL_FLOAT, 0, vertices);
   glNormalPointer(GL_FLOAT, 0, normals);
   glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_INT, indices);
   glFinish();

   GLubyte pixel[4] = { 0 };
   glReadPixels(300, 300, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
   printf("GL_RENDERER=%s\n", glGetString(GL_RENDERER));
   printf("GL_ERROR=0x%04x\n", glGetError());
   printf("CENTER_PIXEL=%u,%u,%u,%u\n",
          pixel[0], pixel[1], pixel[2], pixel[3]);
   fflush(stdout);

   glXSwapBuffers(display, window);
   sleep(5);

   free(indices);
   free(normals);
   free(vertices);
   glXMakeCurrent(display, None, NULL);
   glXDestroyContext(display, context);
   XDestroyWindow(display, window);
   XFreeColormap(display, colormap);
   XFree(visual);
   XCloseDisplay(display);
   return 0;
}
