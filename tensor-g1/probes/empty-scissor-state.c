/*
 * SPDX-License-Identifier: MIT
 *
 * Reproduce Panfrost batch-state changes after a scissor-culled draw.  The
 * first draw records provoking-vertex state but emits no job.  The second
 * draw changes that state and must be accepted as fresh zero-work state.
 */

#define GL_GLEXT_PROTOTYPES 1

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <stdio.h>
#include <string.h>

static void
draw_triangle(void)
{
   glBegin(GL_TRIANGLES);
   glColor3f(1.0f, 0.0f, 0.0f);
   glVertex2f(-0.5f, -0.5f);
   glColor3f(0.0f, 1.0f, 0.0f);
   glVertex2f(0.5f, -0.5f);
   glColor3f(0.0f, 0.0f, 1.0f);
   glVertex2f(0.0f, 0.5f);
   glEnd();
}

int
main(void)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
         "eglGetPlatformDisplayEXT");
   PFNGLPROVOKINGVERTEXPROC provoking_vertex =
      (PFNGLPROVOKINGVERTEXPROC)eglGetProcAddress("glProvokingVertex");

   EGLDisplay display = get_platform_display
                           ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                                  EGL_DEFAULT_DISPLAY, NULL)
                           : eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLint major = 0, minor = 0;
   if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
      fprintf(stderr, "EGL initialization failed: 0x%x\n", eglGetError());
      return 1;
   }

   static const EGLint config_attribs[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_NONE,
   };
   EGLConfig config = NULL;
   EGLint config_count = 0;
   if (!eglChooseConfig(display, config_attribs, &config, 1, &config_count) ||
       config_count != 1 || !eglBindAPI(EGL_OPENGL_API)) {
      fprintf(stderr, "OpenGL config selection failed: 0x%x\n", eglGetError());
      return 1;
   }

   static const EGLint pbuffer_attribs[] = {
      EGL_WIDTH, 32,
      EGL_HEIGHT, 32,
      EGL_NONE,
   };
   EGLSurface surface = eglCreatePbufferSurface(display, config,
                                                 pbuffer_attribs);
   EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
   if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
       !eglMakeCurrent(display, surface, surface, context)) {
      fprintf(stderr, "OpenGL context creation failed: 0x%x\n", eglGetError());
      return 1;
   }

   const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
   if (!provoking_vertex ||
       (!strstr(extensions ? extensions : "", "GL_ARB_provoking_vertex") &&
        !strstr(extensions ? extensions : "", "GL_EXT_provoking_vertex"))) {
      fprintf(stderr, "provoking-vertex control is unavailable\n");
      return 77;
   }

   glViewport(0, 0, 32, 32);
   glEnable(GL_SCISSOR_TEST);
   glScissor(0, 0, 0, 0);

   provoking_vertex(GL_FIRST_VERTEX_CONVENTION);
   draw_triangle();
   provoking_vertex(GL_LAST_VERTEX_CONVENTION);
   draw_triangle();
   glFinish();

   GLenum error = glGetError();
   printf("{\"probe\":\"empty-scissor-state\",\"renderer\":\"%s\","
          "\"gl_error\":%u,\"result\":\"%s\"}\n",
          glGetString(GL_RENDERER), error,
          error == GL_NO_ERROR ? "pass" : "fail");

   eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(display, context);
   eglDestroySurface(display, surface);
   eglTerminate(display);
   return error == GL_NO_ERROR ? 0 : 1;
}
