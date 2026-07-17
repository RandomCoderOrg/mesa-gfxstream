#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/xcb.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define FOURCC_CODE(a, b, c, d) \
   ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
    ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_FORMAT_XRGB8888 FOURCC_CODE('X', 'R', '2', '4')

#define WIDTH 640
#define HEIGHT 480
#define STRIDE (WIDTH * 4)

#define STAGE(message) do { \
   fprintf(stderr, "[egl-dmabuf-dri3] %s\n", message); \
   fflush(stderr); \
} while (0)

static bool
has_extension(const char *extensions, const char *name)
{
   size_t length = strlen(name);
   const char *position = extensions;

   if (!extensions || !name[0] || strchr(name, ' '))
      return false;

   while ((position = strstr(position, name)) != NULL) {
      bool starts_word = position == extensions || position[-1] == ' ';
      bool ends_word = position[length] == '\0' || position[length] == ' ';

      if (starts_word && ends_word)
         return true;
      position += length;
   }
   return false;
}

static int
egl_fail(const char *operation)
{
   fprintf(stderr, "%s failed: EGL error 0x%04x\n", operation, eglGetError());
   return 1;
}

static int
xcb_fail(xcb_connection_t *connection, xcb_void_cookie_t cookie,
         const char *operation)
{
   xcb_generic_error_t *error = xcb_request_check(connection, cookie);

   if (!error)
      return 0;
   fprintf(stderr, "%s failed: X11 error %u, major %u, minor %u\n",
           operation, error->error_code, error->major_code, error->minor_code);
   free(error);
   return 1;
}

static int
allocate_dma_buf(size_t size)
{
   const char *heap_name = getenv("PAN_MALI_DMA_HEAP");
   char heap_path[128];
   struct dma_heap_allocation_data allocation = {
      .len = size,
      .fd_flags = O_RDWR | O_CLOEXEC,
      .heap_flags = 0,
   };
   int heap;
   int result;

   if (!heap_name || !heap_name[0])
      heap_name = "system";
   if (snprintf(heap_path, sizeof(heap_path), "/dev/dma_heap/%s", heap_name) >=
       (int)sizeof(heap_path)) {
      fprintf(stderr, "DMA heap name is too long\n");
      return -1;
   }
   fprintf(stderr, "[egl-dmabuf-dri3] DMA heap: %s\n", heap_path);
   heap = open(heap_path, O_RDONLY | O_CLOEXEC);
   if (heap < 0) {
      fprintf(stderr, "open(%s) failed: %s\n", heap_path, strerror(errno));
      return -1;
   }
   result = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &allocation);
   close(heap);
   if (result < 0) {
      fprintf(stderr, "DMA_HEAP_IOCTL_ALLOC failed: %s\n", strerror(errno));
      return -1;
   }
   return (int)allocation.fd;
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
      "void main() {\n"
      "  gl_Position = vec4(position, 0.0, 1.0);\n"
      "}\n";
   static const char fragment_source[] =
      "precision mediump float;\n"
      "void main() {\n"
      "  gl_FragColor = vec4(0.125, 0.875, 0.375, 1.0);\n"
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

static xcb_screen_t *
get_screen(xcb_connection_t *connection, int screen_number)
{
   const xcb_setup_t *setup = xcb_get_setup(connection);
   xcb_screen_iterator_t iterator = xcb_setup_roots_iterator(setup);

   while (screen_number-- > 0)
      xcb_screen_next(&iterator);
   return iterator.data;
}

static void
hold_for_capture(unsigned seconds)
{
   struct timespec remaining = {
      .tv_sec = seconds,
      .tv_nsec = 0,
   };

   while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR)
      ;
}

int
main(int argc, char **argv)
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
   static const EGLint surface_attributes[] = {
      EGL_WIDTH, 16,
      EGL_HEIGHT, 16,
      EGL_NONE,
   };
   static const EGLint context_attributes[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE,
   };
   static const GLfloat vertices[] = {
       0.0f,  0.80f,
      -0.80f, -0.75f,
       0.80f, -0.75f,
   };
   bool copy_mode = true;
   bool readback = false;
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
   PFNEGLCREATEIMAGEKHRPROC create_image;
   PFNEGLDESTROYIMAGEKHRPROC destroy_image;
   PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC image_renderbuffer_storage;
   EGLDisplay egl_display = EGL_NO_DISPLAY;
   EGLConfig config;
   EGLSurface surface = EGL_NO_SURFACE;
   EGLContext context = EGL_NO_CONTEXT;
   EGLImageKHR image = EGL_NO_IMAGE_KHR;
   EGLint major, minor, config_count;
   const char *egl_extensions;
   const char *gl_extensions;
   GLuint renderbuffer = 0;
   GLuint framebuffer = 0;
   GLuint program = 0;
   GLubyte pixel[4] = {0};
   int dma_buf = -1;
   xcb_connection_t *connection = NULL;
   xcb_screen_t *screen;
   xcb_window_t window = XCB_NONE;
   xcb_pixmap_t pixmap = XCB_NONE;
   xcb_dri3_query_version_reply_t *dri3_reply = NULL;
   xcb_present_query_version_reply_t *present_reply = NULL;
   int screen_number = 0;
   int status = 1;

   for (int i = 1; i < argc; ++i) {
      if (strcmp(argv[i], "--zero-copy") == 0)
         copy_mode = false;
      else if (strcmp(argv[i], "--readback") == 0)
         readback = true;
      else {
         fprintf(stderr, "usage: %s [--zero-copy] [--readback]\n", argv[0]);
         return 2;
      }
   }

   STAGE("create a surfaceless Panfork context");
   get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
      eglGetProcAddress("eglGetPlatformDisplayEXT");
   create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
   destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
   if (!get_platform_display || !create_image || !destroy_image) {
      fprintf(stderr, "required EGL image entrypoints are unavailable\n");
      goto cleanup;
   }
   egl_display = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                      EGL_DEFAULT_DISPLAY, NULL);
   if (egl_display == EGL_NO_DISPLAY || !eglInitialize(egl_display, &major, &minor)) {
      egl_fail("initialize surfaceless EGL");
      goto cleanup;
   }
   egl_extensions = eglQueryString(egl_display, EGL_EXTENSIONS);
   if (!has_extension(egl_extensions, "EGL_EXT_image_dma_buf_import")) {
      fprintf(stderr, "EGL_EXT_image_dma_buf_import is unavailable\nEGL_EXTENSIONS=%s\n",
              egl_extensions ? egl_extensions : "(null)");
      goto cleanup;
   }
   if (!eglBindAPI(EGL_OPENGL_ES_API) ||
       !eglChooseConfig(egl_display, config_attributes, &config, 1, &config_count) ||
       config_count != 1) {
      egl_fail("choose GLES2 config");
      goto cleanup;
   }
   surface = eglCreatePbufferSurface(egl_display, config, surface_attributes);
   context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, context_attributes);
   if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
       !eglMakeCurrent(egl_display, surface, surface, context)) {
      egl_fail("create current GLES2 context");
      goto cleanup;
   }
   gl_extensions = (const char *)glGetString(GL_EXTENSIONS);
   if (!has_extension(gl_extensions, "GL_OES_EGL_image")) {
      fprintf(stderr, "GL_OES_EGL_image is unavailable\nGL_EXTENSIONS=%s\n",
              gl_extensions ? gl_extensions : "(null)");
      goto cleanup;
   }
   image_renderbuffer_storage = (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)
      eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
   if (!image_renderbuffer_storage) {
      fprintf(stderr, "glEGLImageTargetRenderbufferStorageOES is unavailable\n");
      goto cleanup;
   }

   STAGE("allocate an Android DMA-heap buffer");
   dma_buf = allocate_dma_buf((size_t)STRIDE * HEIGHT);
   if (dma_buf < 0)
      goto cleanup;
   {
      const EGLint image_attributes[] = {
         EGL_WIDTH, WIDTH,
         EGL_HEIGHT, HEIGHT,
         EGL_LINUX_DRM_FOURCC_EXT, (EGLint)DRM_FORMAT_XRGB8888,
         EGL_DMA_BUF_PLANE0_FD_EXT, dma_buf,
         EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
         EGL_DMA_BUF_PLANE0_PITCH_EXT, STRIDE,
         EGL_NONE,
      };

      STAGE("import the DMA-BUF into Panfork as an EGLImage");
      image = create_image(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                           NULL, image_attributes);
   }
   if (image == EGL_NO_IMAGE_KHR) {
      egl_fail("eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)");
      goto cleanup;
   }

   STAGE("bind the EGLImage as a GLES2 framebuffer");
   glGenRenderbuffers(1, &renderbuffer);
   glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
   image_renderbuffer_storage(GL_RENDERBUFFER, (GLeglImageOES)image);
   glGenFramebuffers(1, &framebuffer);
   glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_RENDERBUFFER, renderbuffer);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      fprintf(stderr, "DMA-BUF framebuffer is incomplete: 0x%04x\n",
              glCheckFramebufferStatus(GL_FRAMEBUFFER));
      goto cleanup;
   }

   STAGE("render a triangle into the DMA-BUF and wait for Kbase");
   program = create_program();
   if (!program)
      goto cleanup;
   glViewport(0, 0, WIDTH, HEIGHT);
   glClearColor(0.03125f, 0.0625f, 0.125f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glUseProgram(program);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
   glEnableVertexAttribArray(0);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glFinish();
   if (glGetError() != GL_NO_ERROR) {
      fprintf(stderr, "GLES rendering failed\n");
      goto cleanup;
   }
   printf("EGL=%d.%d\n", major, minor);
   printf("GL_RENDERER=%s\n", glGetString(GL_RENDERER));
   if (readback) {
      STAGE("read back the imported image on the CPU");
      glReadPixels(WIDTH / 2, HEIGHT / 2, 1, 1,
                   GL_RGBA, GL_UNSIGNED_BYTE, pixel);
      printf("CENTER_PIXEL=%u,%u,%u,%u\n",
             pixel[0], pixel[1], pixel[2], pixel[3]);
      if (abs((int)pixel[0] - 32) > 2 || abs((int)pixel[1] - 223) > 2 ||
          abs((int)pixel[2] - 96) > 2 || pixel[3] != 255) {
         fprintf(stderr, "rendered center pixel does not match the triangle\n");
         goto cleanup;
      }
   } else {
      puts("CENTER_PIXEL=skipped (imported Kbase CPU cache sync is not safe yet)");
   }
   fflush(stdout);

   STAGE("connect to Termux:X11 and negotiate DRI3/Present");
   connection = xcb_connect(NULL, &screen_number);
   if (!connection || xcb_connection_has_error(connection)) {
      fprintf(stderr, "xcb_connect failed\n");
      goto cleanup;
   }
   screen = get_screen(connection, screen_number);
   dri3_reply = xcb_dri3_query_version_reply(
      connection, xcb_dri3_query_version(connection, 1, 2), NULL);
   present_reply = xcb_present_query_version_reply(
      connection, xcb_present_query_version(connection, 1, 2), NULL);
   if (!dri3_reply || !present_reply) {
      fprintf(stderr, "Termux:X11 did not negotiate DRI3 and Present\n");
      goto cleanup;
   }
   printf("DRI3=%u.%u PRESENT=%u.%u MODE=%s\n",
          dri3_reply->major_version, dri3_reply->minor_version,
          present_reply->major_version, present_reply->minor_version,
          copy_mode ? "copy" : "zero-copy");
   fflush(stdout);

   window = xcb_generate_id(connection);
   {
      uint32_t values[] = {
         screen->black_pixel,
         1,
         XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY,
      };
      xcb_void_cookie_t cookie = xcb_create_window_checked(
         connection, screen->root_depth, window, screen->root,
         40, 40, WIDTH, HEIGHT, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
         screen->root_visual,
         XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK,
         values);

      if (xcb_fail(connection, cookie, "xcb_create_window"))
         goto cleanup;
   }
   xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
                       XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                       strlen("Tensor G1 Panfork DMA-BUF DRI3"),
                       "Tensor G1 Panfork DMA-BUF DRI3");
   xcb_map_window(connection, window);
   xcb_flush(connection);
   {
      bool mapped = false;

      for (int i = 0; i < 200 && !mapped; ++i) {
         xcb_generic_event_t *event = xcb_poll_for_event(connection);

         if (event) {
            mapped = (event->response_type & 0x7f) == XCB_MAP_NOTIFY;
            free(event);
         } else {
            usleep(10000);
         }
      }
      if (!mapped) {
         fprintf(stderr, "window did not receive MapNotify\n");
         goto cleanup;
      }
   }

   STAGE("wrap the rendered DMA-BUF as a DRI3 pixmap");
   pixmap = xcb_generate_id(connection);
   {
      int x_fd = fcntl(dma_buf, F_DUPFD_CLOEXEC, 3);
      xcb_void_cookie_t cookie;

      if (x_fd < 0) {
         fprintf(stderr, "dup DMA-BUF for X11 failed: %s\n", strerror(errno));
         goto cleanup;
      }
      cookie = xcb_dri3_pixmap_from_buffers_checked(
         connection, pixmap, window, 1, WIDTH, HEIGHT,
         STRIDE, 0, 0, 0, 0, 0, 0, 0,
         24, 32, 0, &x_fd);
      if (xcb_fail(connection, cookie, "xcb_dri3_pixmap_from_buffers"))
         goto cleanup;
   }

   STAGE("present the DRI3 pixmap");
   {
      uint32_t options = XCB_PRESENT_OPTION_ASYNC |
                         (copy_mode ? XCB_PRESENT_OPTION_COPY : XCB_PRESENT_OPTION_NONE);
      xcb_void_cookie_t cookie = xcb_present_pixmap_checked(
         connection, window, pixmap, 1,
         XCB_NONE, XCB_NONE, 0, 0,
         XCB_NONE, XCB_NONE, XCB_NONE,
         options, 0, 0, 0, 0, NULL);

      if (xcb_fail(connection, cookie, "xcb_present_pixmap"))
         goto cleanup;
   }
   xcb_flush(connection);
   puts("Pan fork DMA-BUF to Termux:X11 DRI3 probe: PASS");
   fflush(stdout);
   hold_for_capture(15);
   status = 0;

cleanup:
   free(dri3_reply);
   free(present_reply);
   if (connection) {
      if (pixmap != XCB_NONE)
         xcb_free_pixmap(connection, pixmap);
      if (window != XCB_NONE)
         xcb_destroy_window(connection, window);
      xcb_flush(connection);
      xcb_disconnect(connection);
   }
   if (context != EGL_NO_CONTEXT) {
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      if (program)
         glDeleteProgram(program);
      if (framebuffer)
         glDeleteFramebuffers(1, &framebuffer);
      if (renderbuffer)
         glDeleteRenderbuffers(1, &renderbuffer);
   }
   if (image != EGL_NO_IMAGE_KHR)
      destroy_image(egl_display, image);
   if (egl_display != EGL_NO_DISPLAY) {
      eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (context != EGL_NO_CONTEXT)
         eglDestroyContext(egl_display, context);
      if (surface != EGL_NO_SURFACE)
         eglDestroySurface(egl_display, surface);
      eglTerminate(egl_display);
   }
   if (dma_buf >= 0)
      close(dma_buf);
   return status;
}
