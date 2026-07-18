#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>

int
main(void)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
         "eglGetPlatformDisplayEXT");
   if (!get_platform_display) {
      fputs("eglGetPlatformDisplayEXT is unavailable\n", stderr);
      return 1;
   }

   EGLDisplay display = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                              EGL_DEFAULT_DISPLAY, NULL);
   EGLint major = 0, minor = 0;
   if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
      fprintf(stderr, "surfaceless eglInitialize failed: 0x%x\n",
              eglGetError());
      return 1;
   }

   static const EGLint config_attributes[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_NONE,
   };
   static const EGLint pbuffer_attributes[] = {
      EGL_WIDTH, 64,
      EGL_HEIGHT, 64,
      EGL_NONE,
   };
   static const EGLint context_attributes[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE,
   };

   EGLConfig config = NULL;
   EGLint config_count = 0;
   if (!eglChooseConfig(display, config_attributes, &config, 1,
                        &config_count) || config_count != 1 ||
       !eglBindAPI(EGL_OPENGL_ES_API)) {
      fprintf(stderr, "surfaceless config selection failed: 0x%x\n",
              eglGetError());
      return 1;
   }

   EGLSurface surface = eglCreatePbufferSurface(display, config,
                                                 pbuffer_attributes);
   EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
                                         context_attributes);
   if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
       !eglMakeCurrent(display, surface, surface, context)) {
      fprintf(stderr, "surfaceless context creation failed: 0x%x\n",
              eglGetError());
      return 1;
   }

   printf("EGL %d.%d vendor=%s\n", major, minor,
          eglQueryString(display, EGL_VENDOR));
   printf("GL_VENDOR=%s\n", glGetString(GL_VENDOR));
   printf("GL_RENDERER=%s\n", glGetString(GL_RENDERER));
   printf("GL_VERSION=%s\n", glGetString(GL_VERSION));

   eglDestroyContext(display, context);
   eglDestroySurface(display, surface);
   eglTerminate(display);
   return 0;
}
