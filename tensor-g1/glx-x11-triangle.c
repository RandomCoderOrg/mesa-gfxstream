#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <unistd.h>

#define STAGE(message) do { \
   fprintf(stderr, "[glx-x11-triangle] %s\n", message); \
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
      GLX_DEPTH_SIZE, 24,
      None,
   };
   Display *display;
   XVisualInfo *visual;
   XSetWindowAttributes window_attribs;
   Colormap colormap;
   Window window;
   GLXContext context;
   GLubyte pixel[4] = { 0 };
   static const GLuint indices[] = { 0, 1, 2 };
   GLenum error;

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

   colormap = XCreateColormap(display,
                              RootWindow(display, visual->screen),
                              visual->visual, AllocNone);
   window_attribs.colormap = colormap;
   window_attribs.event_mask = ExposureMask | StructureNotifyMask;
   window = XCreateWindow(display,
                          RootWindow(display, visual->screen),
                          80, 80, 320, 240, 0,
                          visual->depth, InputOutput, visual->visual,
                          CWColormap | CWEventMask, &window_attribs);
   XStoreName(display, window, "Tensor G1 Panfrost GLX triangle");
   XMapWindow(display, window);
   XFlush(display);

   context = glXCreateContext(display, visual, NULL, True);
   if (!context || !glXMakeCurrent(display, window, context)) {
      fprintf(stderr, "GLX context creation failed\n");
      return 1;
   }

   STAGE("issue one 3-index lit fixed-function draw");
   glViewport(0, 0, 320, 240);
   {
      static const GLfloat light_position[] = { 5.0f, 5.0f, 10.0f, 0.0f };
      static const GLfloat light_diffuse[] = { 1.0f, 1.0f, 1.0f, 1.0f };
      static const GLfloat material[] = { 0.125f, 0.875f, 0.375f, 1.0f };

      glLightfv(GL_LIGHT0, GL_POSITION, light_position);
      glLightfv(GL_LIGHT0, GL_DIFFUSE, light_diffuse);
      glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, material);
   }
   glEnable(GL_LIGHTING);
   glEnable(GL_LIGHT0);
   glClearColor(0.03125f, 0.03125f, 0.03125f, 1.0f);
   glClearDepth(1.0);
   glEnable(GL_DEPTH_TEST);
   glDepthFunc(GL_LESS);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   glColor3f(0.125f, 0.875f, 0.375f);
   {
      static const GLfloat vertices[] = {
          0.0f,  0.75f,
         -0.75f, -0.75f,
          0.75f, -0.75f,
      };
      static const GLfloat normals[] = {
         0.0f, 0.0f, 1.0f,
         0.0f, 0.0f, 1.0f,
         0.0f, 0.0f, 1.0f,
      };

      glEnableClientState(GL_VERTEX_ARRAY);
      glEnableClientState(GL_NORMAL_ARRAY);
      glVertexPointer(2, GL_FLOAT, 0, vertices);
      glNormalPointer(GL_FLOAT, 0, normals);
      glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_INT, indices);
      glDisableClientState(GL_NORMAL_ARRAY);
      glDisableClientState(GL_VERTEX_ARRAY);
   }
   error = glGetError();
   printf("GL_ERROR_AFTER_DRAW=0x%04x\n", error);

   STAGE("wait for GPU and read center pixel");
   glFinish();
   error = glGetError();
   glReadPixels(160, 120, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
   printf("GL_ERROR_AFTER_FINISH=0x%04x\n", error);
   printf("GL_RENDERER=%s\n", glGetString(GL_RENDERER));
   printf("GL_VERSION=%s\n", glGetString(GL_VERSION));
   printf("PIXEL=%u,%u,%u,%u\n", pixel[0], pixel[1], pixel[2], pixel[3]);
   fflush(stdout);

   STAGE("present completed frame to X11");
   glXSwapBuffers(display, window);
   sleep(2);

   glXMakeCurrent(display, None, NULL);
   glXDestroyContext(display, context);
   XDestroyWindow(display, window);
   XFreeColormap(display, colormap);
   XFree(visual);
   XCloseDisplay(display);
   return 0;
}
