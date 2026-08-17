/*
 * Validate automatic EGL ES3 discovery and the instanced-array capability
 * which Mesa derives from Vulkan vertex-attribute-divisor support.
 */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>

static void
fail(const char *what)
{
   fprintf(stderr, "%s: EGL=0x%x GL=0x%x\n", what, eglGetError(), glGetError());
   exit(1);
}

int
main(void)
{
   Display *x = XOpenDisplay(NULL);
   if (!x)
      fail("XOpenDisplay");

   const int screen = DefaultScreen(x);
   Window win = XCreateSimpleWindow(x, RootWindow(x, screen), 0, 0, 256, 256,
                                    0, BlackPixel(x, screen), BlackPixel(x, screen));
   XMapWindow(x, win);

   EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)x);
   EGLint major, minor;
   if (!eglInitialize(dpy, &major, &minor))
      fail("eglInitialize");

   const EGLint config_attribs[] = {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
      EGL_NONE,
   };
   EGLConfig config;
   EGLint count;
   if (!eglChooseConfig(dpy, config_attribs, &config, 1, &count) || count != 1)
      fail("eglChooseConfig ES3");

   EGLSurface surface = eglCreateWindowSurface(dpy, config,
                                                (EGLNativeWindowType)win, NULL);
   if (surface == EGL_NO_SURFACE)
      fail("eglCreateWindowSurface");

   const EGLint context_attribs[] = {
      EGL_CONTEXT_CLIENT_VERSION, 3,
      EGL_NONE,
   };
   EGLContext context = eglCreateContext(dpy, config, EGL_NO_CONTEXT,
                                         context_attribs);
   if (context == EGL_NO_CONTEXT)
      fail("eglCreateContext ES3");
   if (!eglMakeCurrent(dpy, surface, surface, context))
      fail("eglMakeCurrent");

   const char *vs_src =
      "#version 300 es\n"
      "layout(location=0) in vec2 p;\n"
      "layout(location=1) in vec2 offset;\n"
      "void main(){ gl_Position=vec4(p+offset,0.0,1.0); }\n";
   const char *fs_src =
      "#version 300 es\n"
      "precision mediump float;\n"
      "out vec4 c;\n"
      "void main(){ c=vec4(0.1,0.8,0.25,1.0); }\n";
   GLuint vs = glCreateShader(GL_VERTEX_SHADER);
   GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
   glShaderSource(vs, 1, &vs_src, NULL);
   glCompileShader(vs);
   glShaderSource(fs, 1, &fs_src, NULL);
   glCompileShader(fs);
   GLuint program = glCreateProgram();
   glAttachShader(program, vs);
   glAttachShader(program, fs);
   glLinkProgram(program);
   glUseProgram(program);

   const GLfloat vertices[] = {
      -0.15f, -0.15f, 0.15f, -0.15f, 0.0f, 0.15f,
   };
   const GLfloat offsets[] = {
      -0.55f, 0.0f, 0.0f, 0.0f, 0.55f, 0.0f,
   };
   GLuint buffers[2];
   glGenBuffers(2, buffers);
   glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
   glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
   glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
   glBufferData(GL_ARRAY_BUFFER, sizeof(offsets), offsets, GL_STATIC_DRAW);
   glEnableVertexAttribArray(1);
   glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, NULL);
   glVertexAttribDivisor(1, 1);

   glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 3);
   glFinish();

   unsigned char center[4] = {0};
   glReadPixels(128, 128, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
   GLenum error = glGetError();
   printf("GL_VERSION=%s\nGL_RENDERER=%s\n", glGetString(GL_VERSION),
          glGetString(GL_RENDERER));
   printf("instanced_draw_gl_error=0x%x center_rgba=%u,%u,%u,%u\n",
          error, center[0], center[1], center[2], center[3]);
   if (error != GL_NO_ERROR || center[1] < 100)
      return 2;

   eglSwapBuffers(dpy, surface);
   return 0;
}
