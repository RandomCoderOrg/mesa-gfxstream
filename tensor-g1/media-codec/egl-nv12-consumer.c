#define _GNU_SOURCE

#include "egl-nv12-consumer.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FOURCC_CODE(a, b, c, d) \
   ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
    ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_FORMAT_R8 FOURCC_CODE('R', '8', ' ', ' ')
#define DRM_FORMAT_GR88 FOURCC_CODE('G', 'R', '8', '8')

struct tmc_egl_consumer {
   EGLDisplay display;
   EGLSurface pbuffer;
   EGLContext context;
   PFNEGLCREATEIMAGEKHRPROC create_image;
   PFNEGLDESTROYIMAGEKHRPROC destroy_image;
   PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;
   GLuint program;
   GLint y_uniform;
   GLint uv_uniform;
   char renderer[128];
};

struct tmc_egl_surface {
   EGLImageKHR y_image;
   EGLImageKHR uv_image;
   GLuint y_texture;
   GLuint uv_texture;
};

static bool
has_extension(const char *extensions, const char *name)
{
   size_t length = strlen(name);
   const char *position = extensions;
   if (!extensions || !name[0] || strchr(name, ' '))
      return false;
   while ((position = strstr(position, name)) != NULL) {
      bool begins = position == extensions || position[-1] == ' ';
      bool ends = position[length] == '\0' || position[length] == ' ';
      if (begins && ends)
         return true;
      position += length;
   }
   return false;
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
      char log[1024] = {0};
      glGetShaderInfoLog(shader, sizeof(log), NULL, log);
      fprintf(stderr, "EGL consumer shader compile failed: %s\n", log);
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
      "varying vec2 texcoord;\n"
      "void main() {\n"
      "  gl_Position = vec4(position, 0.0, 1.0);\n"
      "  texcoord = position * 0.5 + 0.5;\n"
      "}\n";
   static const char fragment_source[] =
      "precision highp float;\n"
      "varying vec2 texcoord;\n"
      "uniform sampler2D y_plane;\n"
      "uniform sampler2D uv_plane;\n"
      "void main() {\n"
      "  float y = texture2D(y_plane, texcoord).r;\n"
      "  vec2 uv = texture2D(uv_plane, texcoord).rg;\n"
      "  gl_FragColor = vec4(y, uv, 1.0);\n"
      "}\n";
   GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
   GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
   if (!vertex || !fragment) {
      if (vertex)
         glDeleteShader(vertex);
      if (fragment)
         glDeleteShader(fragment);
      return 0;
   }
   GLuint program = glCreateProgram();
   glAttachShader(program, vertex);
   glAttachShader(program, fragment);
   glBindAttribLocation(program, 0, "position");
   glLinkProgram(program);
   glDeleteShader(vertex);
   glDeleteShader(fragment);
   GLint ok = GL_FALSE;
   glGetProgramiv(program, GL_LINK_STATUS, &ok);
   if (!ok) {
      char log[1024] = {0};
      glGetProgramInfoLog(program, sizeof(log), NULL, log);
      fprintf(stderr, "EGL consumer program link failed: %s\n", log);
      glDeleteProgram(program);
      return 0;
   }
   return program;
}

static bool
warm_render_target(void)
{
   uint8_t pixel[4] = {0};
   glViewport(0, 0, 1, 1);
   glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
   return glGetError() == GL_NO_ERROR && pixel[3] >= 254;
}

struct tmc_egl_consumer *
tmc_egl_consumer_create(void)
{
   static const EGLint config_attributes[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_NONE,
   };
   static const EGLint pbuffer_attributes[] = {
      EGL_WIDTH, 1,
      EGL_HEIGHT, 1,
      EGL_NONE,
   };
   static const EGLint context_attributes[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE,
   };
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)
         eglGetProcAddress("eglGetPlatformDisplayEXT");
   struct tmc_egl_consumer *consumer = calloc(1, sizeof(*consumer));
   if (!consumer || !get_platform_display)
      goto fail;
   consumer->display = EGL_NO_DISPLAY;
   consumer->pbuffer = EGL_NO_SURFACE;
   consumer->context = EGL_NO_CONTEXT;
   consumer->display = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                             EGL_DEFAULT_DISPLAY, NULL);
   EGLint major = 0, minor = 0;
   if (consumer->display == EGL_NO_DISPLAY ||
       !eglInitialize(consumer->display, &major, &minor))
      goto fail;
   const char *egl_extensions = eglQueryString(consumer->display,
                                               EGL_EXTENSIONS);
   if (!has_extension(egl_extensions, "EGL_EXT_image_dma_buf_import"))
      goto fail;
   consumer->create_image = (PFNEGLCREATEIMAGEKHRPROC)
      eglGetProcAddress("eglCreateImageKHR");
   consumer->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)
      eglGetProcAddress("eglDestroyImageKHR");
   consumer->image_target_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
      eglGetProcAddress("glEGLImageTargetTexture2DOES");
   if (!consumer->create_image || !consumer->destroy_image ||
       !consumer->image_target_texture || !eglBindAPI(EGL_OPENGL_ES_API))
      goto fail;
   EGLConfig config;
   EGLint config_count = 0;
   if (!eglChooseConfig(consumer->display, config_attributes, &config, 1,
                        &config_count) || config_count != 1)
      goto fail;
   consumer->pbuffer = eglCreatePbufferSurface(
      consumer->display, config, pbuffer_attributes);
   consumer->context = eglCreateContext(
      consumer->display, config, EGL_NO_CONTEXT, context_attributes);
   if (consumer->pbuffer == EGL_NO_SURFACE ||
       consumer->context == EGL_NO_CONTEXT ||
       !eglMakeCurrent(consumer->display, consumer->pbuffer,
                       consumer->pbuffer, consumer->context))
      goto fail;
   const char *gl_extensions = (const char *)glGetString(GL_EXTENSIONS);
   if (!has_extension(gl_extensions, "GL_OES_EGL_image"))
      goto fail;
   consumer->program = create_program();
   if (!consumer->program || !warm_render_target())
      goto fail;
   consumer->y_uniform = glGetUniformLocation(consumer->program, "y_plane");
   consumer->uv_uniform = glGetUniformLocation(consumer->program, "uv_plane");
   const char *renderer = (const char *)glGetString(GL_RENDERER);
   snprintf(consumer->renderer, sizeof(consumer->renderer), "%s",
            renderer ? renderer : "unknown");
   return consumer;

fail:
   tmc_egl_consumer_destroy(consumer);
   return NULL;
}

void
tmc_egl_consumer_destroy(struct tmc_egl_consumer *consumer)
{
   if (!consumer)
      return;
   if (consumer->display != EGL_NO_DISPLAY) {
      if (consumer->context != EGL_NO_CONTEXT && consumer->program)
         glDeleteProgram(consumer->program);
      eglMakeCurrent(consumer->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                     EGL_NO_CONTEXT);
      if (consumer->context != EGL_NO_CONTEXT)
         eglDestroyContext(consumer->display, consumer->context);
      if (consumer->pbuffer != EGL_NO_SURFACE)
         eglDestroySurface(consumer->display, consumer->pbuffer);
      eglTerminate(consumer->display);
   }
   free(consumer);
}

const char *
tmc_egl_consumer_renderer(const struct tmc_egl_consumer *consumer)
{
   return consumer ? consumer->renderer : NULL;
}

static EGLImageKHR
create_plane_image(struct tmc_egl_consumer *consumer, int dmabuf_fd,
                   uint32_t width, uint32_t height, uint32_t fourcc,
                   uint32_t offset, uint32_t pitch)
{
   if (!width || !height || width > INT_MAX || height > INT_MAX ||
       offset > INT_MAX || pitch > INT_MAX)
      return EGL_NO_IMAGE_KHR;
   const EGLint attributes[] = {
      EGL_WIDTH, (EGLint)width,
      EGL_HEIGHT, (EGLint)height,
      EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fourcc,
      EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
      EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)pitch,
      EGL_NONE,
   };
   return consumer->create_image(
      consumer->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL,
      attributes);
}

static bool
bind_plane_texture(struct tmc_egl_consumer *consumer, EGLImageKHR image,
                   GLuint *texture)
{
   glGenTextures(1, texture);
   glBindTexture(GL_TEXTURE_2D, *texture);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   consumer->image_target_texture(GL_TEXTURE_2D, (GLeglImageOES)image);
   return glGetError() == GL_NO_ERROR;
}

struct tmc_egl_surface *
tmc_egl_surface_create(struct tmc_egl_consumer *consumer, int dmabuf_fd,
                       uint32_t width, uint32_t height, uint32_t stride,
                       uint32_t slice_height)
{
   if (!consumer || dmabuf_fd < 0 || !width || !height || !stride ||
       !slice_height || (uint64_t)stride * slice_height > UINT32_MAX)
      return NULL;
   struct tmc_egl_surface *surface = calloc(1, sizeof(*surface));
   if (!surface)
      return NULL;
   surface->y_image = EGL_NO_IMAGE_KHR;
   surface->uv_image = EGL_NO_IMAGE_KHR;
   surface->y_image = create_plane_image(
      consumer, dmabuf_fd, width, height, DRM_FORMAT_R8, 0, stride);
   surface->uv_image = create_plane_image(
      consumer, dmabuf_fd, (width + 1) / 2, (height + 1) / 2,
      DRM_FORMAT_GR88, stride * slice_height, stride);
   if (surface->y_image == EGL_NO_IMAGE_KHR ||
       surface->uv_image == EGL_NO_IMAGE_KHR ||
       !bind_plane_texture(consumer, surface->y_image, &surface->y_texture) ||
       !bind_plane_texture(consumer, surface->uv_image,
                           &surface->uv_texture)) {
      tmc_egl_surface_destroy(consumer, surface);
      return NULL;
   }
   return surface;
}

void
tmc_egl_surface_destroy(struct tmc_egl_consumer *consumer,
                        struct tmc_egl_surface *surface)
{
   if (!consumer || !surface)
      return;
   if (surface->y_texture)
      glDeleteTextures(1, &surface->y_texture);
   if (surface->uv_texture)
      glDeleteTextures(1, &surface->uv_texture);
   if (surface->y_image != EGL_NO_IMAGE_KHR)
      consumer->destroy_image(consumer->display, surface->y_image);
   if (surface->uv_image != EGL_NO_IMAGE_KHR)
      consumer->destroy_image(consumer->display, surface->uv_image);
   free(surface);
}

bool
tmc_egl_surface_sample(struct tmc_egl_consumer *consumer,
                       const struct tmc_egl_surface *surface,
                       uint8_t pixel[4])
{
   static const GLfloat vertices[] = {
      -1.0f, -1.0f,
       3.0f, -1.0f,
      -1.0f,  3.0f,
   };
   if (!consumer || !surface || !pixel)
      return false;
   while (glGetError() != GL_NO_ERROR)
      ;
   glViewport(0, 0, 1, 1);
   glUseProgram(consumer->program);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, surface->y_texture);
   glUniform1i(consumer->y_uniform, 0);
   glActiveTexture(GL_TEXTURE1);
   glBindTexture(GL_TEXTURE_2D, surface->uv_texture);
   glUniform1i(consumer->uv_uniform, 1);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
   glEnableVertexAttribArray(0);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
   return glGetError() == GL_NO_ERROR;
}
