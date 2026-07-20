#define _GNU_SOURCE

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define FOURCC_CODE(a, b, c, d) \
   ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
    ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_FORMAT_XRGB8888 FOURCC_CODE('X', 'R', '2', '4')
#define WIDTH 64
#define HEIGHT 64
#define STRIDE (WIDTH * 4)
#define BUFFER_SIZE ((size_t)STRIDE * HEIGHT)

enum import_mode {
   IMPORT_PERSISTENT,
   IMPORT_EACH_FRAME,
   IMPORT_FRESH_BUFFER,
};

static uint64_t
monotonic_ns(void)
{
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return (uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec;
}

static int
dma_sync(int fd, uint64_t flags)
{
   struct dma_buf_sync sync = { .flags = flags };
   if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0)
      return 0;
   fprintf(stderr, "DMA_BUF_IOCTL_SYNC flags=0x%llx failed: %s\n",
           (unsigned long long)flags, strerror(errno));
   return -1;
}

static int
allocate_dma_buf(void)
{
   struct dma_heap_allocation_data allocation = {
      .len = BUFFER_SIZE,
      .fd_flags = O_RDWR | O_CLOEXEC,
   };
   int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
   if (heap < 0) {
      perror("open /dev/dma_heap/system");
      return -1;
   }
   int result = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &allocation);
   close(heap);
   if (result < 0) {
      perror("DMA_HEAP_IOCTL_ALLOC");
      return -1;
   }
   return allocation.fd;
}

static bool
fill_dma_buf(int fd, uint32_t color)
{
   uint32_t *mapping = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
   if (mapping == MAP_FAILED) {
      perror("mmap DMA-BUF");
      return false;
   }
   if (dma_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) < 0) {
      munmap(mapping, BUFFER_SIZE);
      return false;
   }
   for (size_t i = 0; i < BUFFER_SIZE / sizeof(*mapping); ++i)
      mapping[i] = color;
   bool ok = dma_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) == 0;
   munmap(mapping, BUFFER_SIZE);
   return ok;
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
      char log[1024];
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
      "attribute vec2 position; varying vec2 uv;"
      "void main(){ uv=(position+1.0)*0.5; gl_Position=vec4(position,0,1); }";
   static const char fragment_source[] =
      "precision mediump float; varying vec2 uv; uniform sampler2D image;"
      "void main(){ gl_FragColor=texture2D(image,uv); }";
   GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
   GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
   GLuint program = glCreateProgram();
   GLint ok = GL_FALSE;
   if (!vertex || !fragment)
      return 0;
   glAttachShader(program, vertex);
   glAttachShader(program, fragment);
   glBindAttribLocation(program, 0, "position");
   glLinkProgram(program);
   glGetProgramiv(program, GL_LINK_STATUS, &ok);
   glDeleteShader(vertex);
   glDeleteShader(fragment);
   if (!ok) {
      glDeleteProgram(program);
      return 0;
   }
   return program;
}

static EGLImageKHR
import_image(EGLDisplay display, PFNEGLCREATEIMAGEKHRPROC create_image, int fd)
{
   const EGLint attributes[] = {
      EGL_WIDTH, WIDTH,
      EGL_HEIGHT, HEIGHT,
      EGL_LINUX_DRM_FOURCC_EXT, (EGLint)DRM_FORMAT_XRGB8888,
      EGL_DMA_BUF_PLANE0_FD_EXT, fd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
      EGL_DMA_BUF_PLANE0_PITCH_EXT, STRIDE,
      EGL_NONE,
   };
   return create_image(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                       NULL, attributes);
}

int
main(int argc, char **argv)
{
   enum import_mode mode = IMPORT_PERSISTENT;
   unsigned iterations = 6;
   if (argc > 1 && strcmp(argv[1], "--reimport") == 0)
      mode = IMPORT_EACH_FRAME;
   else if (argc > 1 && strcmp(argv[1], "--fresh") == 0)
      mode = IMPORT_FRESH_BUFFER;
   else if (argc > 1 && strcmp(argv[1], "--persistent") != 0) {
      fprintf(stderr, "usage: %s [--persistent|--reimport|--fresh] [frames]\n",
              argv[0]);
      return 2;
   }
   if (argc > 2)
      iterations = strtoul(argv[2], NULL, 10);
   if (!iterations || iterations > 10000)
      return 2;

   static const EGLint config_attributes[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
      EGL_NONE,
   };
   static const EGLint surface_attributes[] = {
      EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE,
   };
   static const EGLint context_attributes[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE,
   };
   static const GLfloat vertices[] = {
      -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f,
   };
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)
         eglGetProcAddress("eglGetPlatformDisplayEXT");
   PFNEGLCREATEIMAGEKHRPROC create_image = (PFNEGLCREATEIMAGEKHRPROC)
      eglGetProcAddress("eglCreateImageKHR");
   PFNEGLDESTROYIMAGEKHRPROC destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)
      eglGetProcAddress("eglDestroyImageKHR");
   PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture =
      (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
         eglGetProcAddress("glEGLImageTargetTexture2DOES");
   if (!get_platform_display || !create_image || !destroy_image ||
       !image_target_texture)
      return 1;

   EGLDisplay display = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                              EGL_DEFAULT_DISPLAY, NULL);
   EGLint major = 0, minor = 0, config_count = 0;
   EGLConfig config;
   if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) ||
       !eglBindAPI(EGL_OPENGL_ES_API) ||
       !eglChooseConfig(display, config_attributes, &config, 1, &config_count) ||
       config_count != 1)
      return 1;
   EGLSurface surface = eglCreatePbufferSurface(display, config,
                                                surface_attributes);
   EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
                                         context_attributes);
   if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
       !eglMakeCurrent(display, surface, surface, context))
      return 1;

   GLuint program = create_program();
   if (!program)
      return 1;
   glUseProgram(program);
   glUniform1i(glGetUniformLocation(program, "image"), 0);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
   glEnableVertexAttribArray(0);
   glViewport(0, 0, 1, 1);

   int fd = -1;
   EGLImageKHR image = EGL_NO_IMAGE_KHR;
   GLuint texture = 0;
   uint64_t total_fill = 0, total_import = 0, total_sample = 0;
   bool passed = true;
   for (unsigned frame = 0; frame < iterations; ++frame) {
      const uint32_t color = frame & 1 ? 0x0020c060 : 0x00e08020;
      const GLubyte expected[3] = {
         (GLubyte)(color >> 16), (GLubyte)(color >> 8), (GLubyte)color,
      };
      uint64_t started = monotonic_ns();
      if (mode == IMPORT_FRESH_BUFFER || fd < 0) {
         if (fd >= 0)
            close(fd);
         fd = allocate_dma_buf();
      }
      if (fd < 0 || !fill_dma_buf(fd, color))
         return 1;
      total_fill += monotonic_ns() - started;

      started = monotonic_ns();
      if (mode != IMPORT_PERSISTENT || image == EGL_NO_IMAGE_KHR) {
         if (texture)
            glDeleteTextures(1, &texture);
         if (image != EGL_NO_IMAGE_KHR)
            destroy_image(display, image);
         image = import_image(display, create_image, fd);
         glGenTextures(1, &texture);
         glBindTexture(GL_TEXTURE_2D, texture);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
         image_target_texture(GL_TEXTURE_2D, (GLeglImageOES)image);
      } else {
         glBindTexture(GL_TEXTURE_2D, texture);
      }
      if (image == EGL_NO_IMAGE_KHR || glGetError() != GL_NO_ERROR)
         return 1;
      total_import += monotonic_ns() - started;

      GLubyte pixel[4] = {0};
      started = monotonic_ns();
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glFinish();
      glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
      total_sample += monotonic_ns() - started;
      bool frame_ok = pixel[0] == expected[0] && pixel[1] == expected[1] &&
                      pixel[2] == expected[2] && pixel[3] == 255;
      printf("FRAME=%u EXPECTED=%u,%u,%u,255 ACTUAL=%u,%u,%u,%u %s\n",
             frame, expected[0], expected[1], expected[2],
             pixel[0], pixel[1], pixel[2], pixel[3],
             frame_ok ? "PASS" : "FAIL");
      if (!frame_ok) {
         passed = false;
         break;
      }
   }

   const char *mode_name = mode == IMPORT_PERSISTENT ? "persistent" :
                           mode == IMPORT_EACH_FRAME ? "reimport" : "fresh";
   printf("MODE=%s GL_RENDERER=%s RESULT=%s\n", mode_name,
          glGetString(GL_RENDERER), passed ? "PASS" : "FAIL");
   printf("{\"schema\":\"tensor-perf-v1\",\"kind\":\"coherency\","
          "\"mode\":\"%s\",\"fill_total_ns\":%llu,"
          "\"import_total_ns\":%llu,\"sample_total_ns\":%llu}\n",
          mode_name, (unsigned long long)total_fill,
          (unsigned long long)total_import,
          (unsigned long long)total_sample);

   if (texture)
      glDeleteTextures(1, &texture);
   if (image != EGL_NO_IMAGE_KHR)
      destroy_image(display, image);
   if (fd >= 0)
      close(fd);
   glDeleteProgram(program);
   eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(display, context);
   eglDestroySurface(display, surface);
   eglTerminate(display);
   return passed ? 0 : 1;
}
