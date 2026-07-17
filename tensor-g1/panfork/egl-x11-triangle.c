#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STAGE(message) do { \
   fprintf(stderr, "[egl-x11-triangle] %s\n", message); \
   fflush(stderr); \
} while (0)

static int
egl_fail(const char *operation)
{
   fprintf(stderr, "%s failed: EGL error 0x%04x\n", operation, eglGetError());
   return 1;
}

static GLuint
compile_shader(GLenum type, const char *source)
{
   GLuint shader = glCreateShader(type);
   GLint ok = GL_FALSE;

   glShaderSource(shader, 1, &source, NULL);
   glCompileShader(shader);
   glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[2048];
      GLsizei length = 0;

      glGetShaderInfoLog(shader, sizeof(log), &length, log);
      fprintf(stderr, "shader compile failed: %.*s\n", (int)length, log);
      glDeleteShader(shader);
      return 0;
   }

   return shader;
}

static GLuint
create_program(void)
{
   static const char vertex_source[] =
      "attribute vec2 position;\n"
      "attribute vec3 color;\n"
      "varying vec3 v_color;\n"
      "void main() {\n"
      "  gl_Position = vec4(position, 0.0, 1.0);\n"
      "  v_color = color;\n"
      "}\n";
   static const char fragment_source[] =
      "precision mediump float;\n"
      "varying vec3 v_color;\n"
      "void main() {\n"
      "  gl_FragColor = vec4(v_color, 1.0);\n"
      "}\n";
   GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
   GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
   GLuint program;
   GLint ok = GL_FALSE;

   if (!vertex || !fragment)
      return 0;

   program = glCreateProgram();
   glAttachShader(program, vertex);
   glAttachShader(program, fragment);
   glBindAttribLocation(program, 0, "position");
   glBindAttribLocation(program, 1, "color");
   glLinkProgram(program);
   glGetProgramiv(program, GL_LINK_STATUS, &ok);
   glDeleteShader(vertex);
   glDeleteShader(fragment);
   if (!ok) {
      char log[2048];
      GLsizei length = 0;

      glGetProgramInfoLog(program, sizeof(log), &length, log);
      fprintf(stderr, "program link failed: %.*s\n", (int)length, log);
      glDeleteProgram(program);
      return 0;
   }

   return program;
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
   static const GLfloat vertices[] = {
       0.0f,  0.75f,
      -0.75f, -0.75f,
       0.75f, -0.75f,
   };
   static const GLfloat colors[] = {
      0.125f, 0.875f, 0.375f,
      0.125f, 0.875f, 0.375f,
      0.125f, 0.875f, 0.375f,
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
   GLubyte pixel[4] = { 0 };
   GLuint program;
   GLenum error;
   int visual_count;

   STAGE("open X display");
   xdisplay = XOpenDisplay(NULL);
   if (!xdisplay) {
      fprintf(stderr, "XOpenDisplay failed\n");
      return 1;
   }

   display = eglGetDisplay((EGLNativeDisplayType)xdisplay);
   if (display == EGL_NO_DISPLAY)
      return egl_fail("eglGetDisplay");
   if (!eglInitialize(display, &major, &minor))
      return egl_fail("eglInitialize");
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

   colormap = XCreateColormap(xdisplay,
                              RootWindow(xdisplay, visual->screen),
                              visual->visual, AllocNone);
   window_attribs.colormap = colormap;
   window_attribs.event_mask = ExposureMask | StructureNotifyMask;
   window = XCreateWindow(xdisplay,
                          RootWindow(xdisplay, visual->screen),
                          48, 48, 320, 240, 0,
                          visual->depth, InputOutput, visual->visual,
                          CWColormap | CWEventMask, &window_attribs);
   XStoreName(xdisplay, window, "Tensor G1 Panfrost triangle");
   XMapWindow(xdisplay, window);
   XFlush(xdisplay);

   surface = eglCreateWindowSurface(display, config,
                                    (EGLNativeWindowType)window, NULL);
   if (surface == EGL_NO_SURFACE)
      return egl_fail("eglCreateWindowSurface");
   context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
   if (context == EGL_NO_CONTEXT)
      return egl_fail("eglCreateContext");
   if (!eglMakeCurrent(display, surface, surface, context))
      return egl_fail("eglMakeCurrent");

   STAGE("compile shaders and issue one draw");
   program = create_program();
   if (!program)
      return 1;
   glViewport(0, 0, 320, 240);
   glClearColor(0.03125f, 0.03125f, 0.03125f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glUseProgram(program);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
   glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, colors);
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   error = glGetError();
   printf("GL_ERROR_AFTER_DRAW=0x%04x\n", error);
   fflush(stdout);

   STAGE("wait for GPU and read center pixel");
   glFinish();
   error = glGetError();
   glReadPixels(160, 120, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
   printf("GL_ERROR_AFTER_FINISH=0x%04x\n", error);
   printf("GL_RENDERER=%s\n", glGetString(GL_RENDERER));
   printf("PIXEL=%u,%u,%u,%u\n", pixel[0], pixel[1], pixel[2], pixel[3]);
   fflush(stdout);

   STAGE("present completed frame to X11");
   if (!eglSwapBuffers(display, surface))
      return egl_fail("eglSwapBuffers");
   sleep(2);

   glDeleteProgram(program);
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
