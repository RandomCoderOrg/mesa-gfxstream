#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef TEETH
#define TEETH 20
#endif
#define STREAM_VERTICES (4 * TEETH + 2)
#define SAMPLES (2 * TEETH)
#define INDEX_COUNT (6 * (STREAM_VERTICES / 2 - 1))

static int normal_per_vertex;
static int indexed_arrays;
static int normal_array;
static int smooth_shading;
static int expanded_indexed;
static int normalize_normals;

static void
gear_stream_position(unsigned vertex_id, GLfloat position[3])
{
   const unsigned tooth = vertex_id / 4;
   const unsigned within_tooth = vertex_id % 4;
   const float angle = (2.0f * (float) M_PI * tooth) / TEETH;
   const float da = (2.0f * (float) M_PI / TEETH) / 4.0f;
   float radius;
   float vertex_angle;

   if (vertex_id >= 4 * TEETH) {
      vertex_angle = 2.0f * (float) M_PI;
      radius = vertex_id & 1 ? 0.80f : 0.35f;
   } else {
      vertex_angle = angle;
      radius = (within_tooth == 0 || within_tooth == 2) ? 0.35f : 0.80f;

      if (within_tooth == 3)
         vertex_angle += 3.0f * da;
   }

   position[0] = radius * cosf(vertex_angle);
   position[1] = radius * sinf(vertex_angle);
   position[2] = 0.0f;
}

static void
gear_stream_vertex(unsigned vertex_id)
{
   GLfloat position[3];

   if (normal_per_vertex)
      glNormal3f(0.0f, 0.0f, 1.0f);

   gear_stream_position(vertex_id, position);
   glVertex3fv(position);
}

static void
draw_quad_strip(void)
{
   glBegin(GL_QUAD_STRIP);
   for (unsigned i = 0; i < STREAM_VERTICES; ++i)
      gear_stream_vertex(i);
   glEnd();
}

static void
draw_indexed_arrays(void)
{
   GLfloat positions[INDEX_COUNT][3];
   GLfloat normals[INDEX_COUNT][3];
   GLfloat base_positions[STREAM_VERTICES][3];
   GLfloat base_normals[STREAM_VERTICES][3];
   GLushort indices[INDEX_COUNT];
   GLushort source_indices[INDEX_COUNT];

   for (unsigned i = 0; i < STREAM_VERTICES; ++i) {
      gear_stream_position(i, base_positions[i]);
      base_normals[i][0] = 0.0f;
      base_normals[i][1] = 0.0f;
      base_normals[i][2] = 1.0f;
   }

   for (unsigned pair = 0; pair < STREAM_VERTICES / 2 - 1; ++pair) {
      const unsigned vertex = 2 * pair;
      const unsigned index = 6 * pair;
      source_indices[index + 0] = vertex;
      source_indices[index + 1] = vertex + 1;
      source_indices[index + 2] = vertex + 2;
      source_indices[index + 3] = vertex + 1;
      source_indices[index + 4] = vertex + 3;
      source_indices[index + 5] = vertex + 2;
   }

   if (expanded_indexed) {
      for (unsigned i = 0; i < INDEX_COUNT; ++i) {
         const unsigned source = source_indices[i];
         positions[i][0] = base_positions[source][0];
         positions[i][1] = base_positions[source][1];
         positions[i][2] = base_positions[source][2];
         normals[i][0] = base_normals[source][0];
         normals[i][1] = base_normals[source][1];
         normals[i][2] = base_normals[source][2];
         indices[i] = i;
      }
   } else {
      for (unsigned i = 0; i < INDEX_COUNT; ++i) {
         indices[i] = source_indices[i];
      }

      for (unsigned i = 0; i < STREAM_VERTICES; ++i) {
         positions[i][0] = base_positions[i][0];
         positions[i][1] = base_positions[i][1];
         positions[i][2] = base_positions[i][2];
         normals[i][0] = base_normals[i][0];
         normals[i][1] = base_normals[i][1];
         normals[i][2] = base_normals[i][2];
      }
   }

   glEnableClientState(GL_VERTEX_ARRAY);
   glVertexPointer(3, GL_FLOAT, 0, positions);

   if (normal_array) {
      glEnableClientState(GL_NORMAL_ARRAY);
      glNormalPointer(GL_FLOAT, 0, normals);
   }

   glDrawElements(GL_TRIANGLES, INDEX_COUNT, GL_UNSIGNED_SHORT, indices);

   if (normal_array)
      glDisableClientState(GL_NORMAL_ARRAY);

   glDisableClientState(GL_VERTEX_ARRAY);
}

static void
draw_explicit_triangles(void)
{
   glBegin(GL_TRIANGLES);
   for (unsigned i = 0; i + 3 < STREAM_VERTICES; i += 2) {
      gear_stream_vertex(i);
      gear_stream_vertex(i + 1);
      gear_stream_vertex(i + 2);

      gear_stream_vertex(i + 1);
      gear_stream_vertex(i + 3);
      gear_stream_vertex(i + 2);
   }
   glEnd();
}

static unsigned
count_white_samples(int center_x)
{
   unsigned white = 0;

   for (unsigned i = 0; i < SAMPLES; ++i) {
      const float angle = (2.0f * (float) M_PI * (i + 0.5f)) / SAMPLES;
      const int x = center_x + lroundf(104.0f * cosf(angle));
      const int y = 200 + lroundf(104.0f * sinf(angle));
      GLubyte pixel[4] = { 0 };
      glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);

      if (pixel[0] > 180 && pixel[1] > 140 && pixel[2] > 140)
         ++white;
   }

   return white;
}

static unsigned
count_white_pixels(int x)
{
   GLubyte *pixels = malloc(400 * 400 * 4);
   unsigned white = 0;

   if (!pixels)
      return 0;

   glReadPixels(x, 0, 400, 400, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   for (unsigned i = 0; i < 400 * 400; ++i) {
      const GLubyte *pixel = &pixels[i * 4];
      if (pixel[0] > 180 && pixel[1] > 140 && pixel[2] > 140)
         ++white;
   }

   free(pixels);
   return white;
}

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
   normal_per_vertex = getenv("NORMAL_PER_VERTEX") != NULL;
   indexed_arrays = getenv("INDEXED_ARRAYS") != NULL;
   normal_array = getenv("NORMAL_ARRAY") != NULL;
   smooth_shading = getenv("SMOOTH_SHADING") != NULL;
   expanded_indexed = getenv("EXPANDED_INDEXED") != NULL;
   normalize_normals = getenv("NORMALIZE_NORMALS") != NULL;
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
                                 20, 80, 800, 400, 0,
                                 visual->depth, InputOutput, visual->visual,
                                 CWColormap | CWEventMask, &attrs);
   XStoreName(display, window,
              "Tensor G1 flat lighting: quad strip | explicit triangles");
   XMapWindow(display, window);
   XFlush(display);

   GLXContext context = glXCreateContext(display, visual, NULL, True);
   if (!context || !glXMakeCurrent(display, window, context))
      return 1;

   static const GLfloat light_position[] = { 0.0f, 0.0f, 1.0f, 0.0f };
   static const GLfloat material[] = { 0.8f, 0.03f, 0.03f, 1.0f };
   glViewport(0, 0, 800, 400);
   glMatrixMode(GL_PROJECTION);
   glLoadIdentity();
   glOrtho(-2.0, 2.0, -1.0, 1.0, -1.0, 1.0);
   glMatrixMode(GL_MODELVIEW);
   glLoadIdentity();
   glLightfv(GL_LIGHT0, GL_POSITION, light_position);
   glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, material);
   glEnable(GL_LIGHTING);
   glEnable(GL_LIGHT0);
   if (normalize_normals)
      glEnable(GL_NORMALIZE);
   glShadeModel(smooth_shading ? GL_SMOOTH : GL_FLAT);
   glNormal3f(0.0f, 0.0f, 1.0f);
   glClearColor(0.03f, 0.03f, 0.03f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

   glPushMatrix();
   glTranslatef(-1.0f, 0.0f, 0.0f);
   glRotatef(20.0f, 1.0f, 0.0f, 0.0f);
   glRotatef(30.0f, 0.0f, 1.0f, 0.0f);
   if (indexed_arrays)
      draw_indexed_arrays();
   else
      draw_quad_strip();
   glPopMatrix();

   glPushMatrix();
   glTranslatef(1.0f, 0.0f, 0.0f);
   glRotatef(20.0f, 1.0f, 0.0f, 0.0f);
   glRotatef(30.0f, 0.0f, 1.0f, 0.0f);
   draw_explicit_triangles();
   glPopMatrix();
   glFinish();

   printf("GL_RENDERER=%s\n", glGetString(GL_RENDERER));
   printf("NORMAL_PER_VERTEX=%d\n", normal_per_vertex);
   printf("INDEXED_ARRAYS=%d\n", indexed_arrays);
   printf("NORMAL_ARRAY=%d\n", normal_array);
   printf("SMOOTH_SHADING=%d\n", smooth_shading);
   printf("EXPANDED_INDEXED=%d\n", expanded_indexed);
   printf("NORMALIZE_NORMALS=%d\n", normalize_normals);
   printf("GL_ERROR=0x%04x\n", glGetError());
   printf("WHITE_SAMPLES_QUAD_STRIP=%u/%u\n",
          count_white_samples(200), SAMPLES);
   printf("WHITE_SAMPLES_TRIANGLES=%u/%u\n",
          count_white_samples(600), SAMPLES);
   printf("WHITE_PIXELS_QUAD_STRIP=%u\n", count_white_pixels(0));
   printf("WHITE_PIXELS_TRIANGLES=%u\n", count_white_pixels(400));
   fflush(stdout);

   glXSwapBuffers(display, window);
   if (!getenv("NO_SLEEP"))
      sleep(8);

   glXMakeCurrent(display, None, NULL);
   glXDestroyContext(display, context);
   XDestroyWindow(display, window);
   XFreeColormap(display, colormap);
   XFree(visual);
   XCloseDisplay(display);
   return 0;
}
