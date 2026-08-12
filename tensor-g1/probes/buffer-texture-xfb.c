/*
 * SPDX-License-Identifier: MIT
 *
 * Differential Mali probe for the buffer-texture + transform-feedback path
 * used by Blender's legacy particle hair renderer.  Keep this source usable
 * with both Mesa/Panfrost and Android's proprietary GLES implementation.
 */

#define _POSIX_C_SOURCE 200809L
#define GL_GLEXT_PROTOTYPES 1

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#endif

#ifndef GL_TEXTURE_BUFFER_EXT
#define GL_TEXTURE_BUFFER_EXT 0x8C2A
#endif

#ifndef GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT_EXT
#define GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT_EXT 0x919F
#endif

typedef void(GL_APIENTRYP tex_buffer_ext_fn)(GLenum target,
                                             GLenum internalformat,
                                             GLuint buffer);
typedef void(GL_APIENTRYP tex_buffer_range_ext_fn)(GLenum target,
                                                   GLenum internalformat,
                                                   GLuint buffer,
                                                   GLintptr offset,
                                                   GLsizeiptr size);

struct probe_case {
   const char *name;
   GLenum internal_format;
   const void *input;
   size_t input_size;
   const void *expected;
   size_t output_size;
   bool integer_output;
   bool use_range;
   size_t range_offset;
   size_t range_size;
   bool orphan_after_binding;
};

static uint64_t
monotonic_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t
fnv1a64(const void *data, size_t size)
{
   const uint8_t *bytes = data;
   uint64_t hash = UINT64_C(14695981039346656037);

   for (size_t i = 0; i < size; ++i) {
      hash ^= bytes[i];
      hash *= UINT64_C(1099511628211);
   }

   return hash;
}

static bool
case_enabled(const char *name)
{
   const char *filter = getenv("TENSOR_PROBE_CASE");
   return !filter || !filter[0] || strcmp(filter, name) == 0;
}

static bool
has_extension(const char *wanted)
{
   GLint count = 0;
   glGetIntegerv(GL_NUM_EXTENSIONS, &count);

   for (GLint i = 0; i < count; ++i) {
      const char *extension = (const char *)glGetStringi(GL_EXTENSIONS, i);
      if (extension && strcmp(extension, wanted) == 0)
         return true;
   }

   return false;
}

static GLuint
compile_shader(GLenum type, const char *source)
{
   GLuint shader = glCreateShader(type);
   glShaderSource(shader, 1, &source, NULL);
   glCompileShader(shader);

   GLint ok = GL_FALSE;
   glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
   if (ok)
      return shader;

   GLint length = 0;
   glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
   char *log = calloc((size_t)length + 1, 1);
   if (log) {
      glGetShaderInfoLog(shader, length, NULL, log);
      fprintf(stderr, "shader compile failed: %s\n", log);
      free(log);
   }
   glDeleteShader(shader);
   return 0;
}

static GLuint
create_program(bool integer_output)
{
   static const char float_vertex[] =
      "#version 310 es\n"
      "#extension GL_EXT_texture_buffer : require\n"
      "precision highp float;\n"
      "uniform highp samplerBuffer source_buffer;\n"
      "out highp vec4 captured;\n"
      "void main() {\n"
      "  captured = texelFetch(source_buffer, gl_VertexID);\n"
      "  gl_Position = vec4(0.0);\n"
      "}\n";
   static const char integer_vertex[] =
      "#version 310 es\n"
      "#extension GL_EXT_texture_buffer : require\n"
      "precision highp float;\n"
      "precision highp int;\n"
      "uniform highp usamplerBuffer source_buffer;\n"
      "flat out highp uint captured;\n"
      "void main() {\n"
      "  captured = texelFetch(source_buffer, gl_VertexID).x;\n"
      "  gl_Position = vec4(0.0);\n"
      "}\n";
   static const char fragment[] =
      "#version 310 es\n"
      "precision highp float;\n"
      "layout(location = 0) out vec4 color;\n"
      "void main() { color = vec4(0.0); }\n";

   GLuint vertex = compile_shader(GL_VERTEX_SHADER,
                                  integer_output ? integer_vertex
                                                 : float_vertex);
   GLuint pixel = compile_shader(GL_FRAGMENT_SHADER, fragment);
   if (!vertex || !pixel) {
      glDeleteShader(vertex);
      glDeleteShader(pixel);
      return 0;
   }

   GLuint program = glCreateProgram();
   glAttachShader(program, vertex);
   glAttachShader(program, pixel);
   const char *varying = "captured";
   glTransformFeedbackVaryings(program, 1, &varying,
                               GL_INTERLEAVED_ATTRIBS);
   glLinkProgram(program);
   glDeleteShader(vertex);
   glDeleteShader(pixel);

   GLint ok = GL_FALSE;
   glGetProgramiv(program, GL_LINK_STATUS, &ok);
   if (ok)
      return program;

   GLint length = 0;
   glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
   char *log = calloc((size_t)length + 1, 1);
   if (log) {
      glGetProgramInfoLog(program, length, NULL, log);
      fprintf(stderr, "program link failed: %s\n", log);
      free(log);
   }
   glDeleteProgram(program);
   return 0;
}

static GLuint
create_interleaved_program(void)
{
   static const char vertex_source[] =
      "#version 310 es\n"
      "#extension GL_EXT_texture_buffer : require\n"
      "precision highp float;\n"
      "precision highp int;\n"
      "uniform highp usamplerBuffer source_buffer;\n"
      "flat out highp uvec2 captured_pair;\n"
      "flat out highp uint captured_tail;\n"
      "void main() {\n"
      "  highp uint value = texelFetch(source_buffer, gl_VertexID).x;\n"
      "  captured_pair = uvec2(value, value ^ 0x55aa55aau);\n"
      "  captured_tail = value + 17u;\n"
      "  gl_Position = vec4(0.0);\n"
      "}\n";
   static const char fragment_source[] =
      "#version 310 es\n"
      "precision highp float;\n"
      "layout(location = 0) out vec4 color;\n"
      "void main() { color = vec4(0.0); }\n";

   GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
   GLuint pixel = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
   if (!vertex || !pixel) {
      glDeleteShader(vertex);
      glDeleteShader(pixel);
      return 0;
   }

   GLuint program = glCreateProgram();
   glAttachShader(program, vertex);
   glAttachShader(program, pixel);
   const char *varyings[] = {"captured_pair", "captured_tail"};
   glTransformFeedbackVaryings(program, 2, varyings, GL_INTERLEAVED_ATTRIBS);
   glLinkProgram(program);
   glDeleteShader(vertex);
   glDeleteShader(pixel);

   GLint ok = GL_FALSE;
   glGetProgramiv(program, GL_LINK_STATUS, &ok);
   if (ok)
      return program;

   GLint length = 0;
   glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
   char *log = calloc((size_t)length + 1, 1);
   if (log) {
      glGetProgramInfoLog(program, length, NULL, log);
      fprintf(stderr, "interleaved program link failed: %s\n", log);
      free(log);
   }
   glDeleteProgram(program);
   return 0;
}

static bool
run_case(const struct probe_case *test, GLuint program,
         tex_buffer_ext_fn tex_buffer,
         tex_buffer_range_ext_fn tex_buffer_range,
         const char *renderer,
         unsigned iteration)
{
   GLuint source_bo = 0;
   GLuint output_bo = 0;
   GLuint texture = 0;
   bool pass = false;

   glGenBuffers(1, &source_bo);
   glBindBuffer(GL_TEXTURE_BUFFER_EXT, source_bo);
   glBufferData(GL_TEXTURE_BUFFER_EXT, (GLsizeiptr)test->input_size,
                test->input, GL_STATIC_DRAW);

   glGenTextures(1, &texture);
   glBindTexture(GL_TEXTURE_BUFFER_EXT, texture);
   if (test->use_range) {
      tex_buffer_range(GL_TEXTURE_BUFFER_EXT, test->internal_format,
                       source_bo, (GLintptr)test->range_offset,
                       (GLsizeiptr)test->range_size);
   } else {
      tex_buffer(GL_TEXTURE_BUFFER_EXT, test->internal_format, source_bo);
   }

   if (test->orphan_after_binding) {
      glBindBuffer(GL_TEXTURE_BUFFER_EXT, source_bo);
      glBufferData(GL_TEXTURE_BUFFER_EXT, (GLsizeiptr)test->input_size,
                   NULL, GL_STREAM_DRAW);
      glBufferSubData(GL_TEXTURE_BUFFER_EXT, 0,
                      (GLsizeiptr)test->input_size, test->input);
   }

   glGenBuffers(1, &output_bo);
   glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, output_bo);
   glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER,
                (GLsizeiptr)test->output_size, NULL, GL_STREAM_READ);
   glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, output_bo);

   glUseProgram(program);
   glUniform1i(glGetUniformLocation(program, "source_buffer"), 0);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_BUFFER_EXT, texture);

   while (glGetError() != GL_NO_ERROR)
      ;

   uint64_t begin_ns = monotonic_ns();
   glEnable(GL_RASTERIZER_DISCARD);
   glBeginTransformFeedback(GL_POINTS);
   glDrawArrays(GL_POINTS, 0, 4);
   glEndTransformFeedback();
   glDisable(GL_RASTERIZER_DISCARD);
   glFinish();
   uint64_t finish_ns = monotonic_ns();

   GLenum draw_error = glGetError();
   glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, output_bo);
   void *actual = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0,
                                   (GLsizeiptr)test->output_size,
                                   GL_MAP_READ_BIT);

   uint64_t expected_hash = fnv1a64(test->expected, test->output_size);
   uint64_t actual_hash = actual ? fnv1a64(actual, test->output_size) : 0;
   pass = draw_error == GL_NO_ERROR && actual &&
          memcmp(actual, test->expected, test->output_size) == 0;

   printf("{\"probe\":\"buffer-texture-xfb\","
          "\"case\":\"%s\",\"iteration\":%u,\"renderer\":\"%s\","
          "\"status\":\"%s\",\"gl_error\":%u,"
          "\"expected_hash\":\"%016" PRIx64 "\","
          "\"actual_hash\":\"%016" PRIx64 "\","
          "\"gpu_wait_us\":%.3f}\n",
          test->name, iteration, renderer, pass ? "PASS" : "FAIL", draw_error,
          expected_hash, actual_hash,
          (double)(finish_ns - begin_ns) / 1000.0);

   if (actual)
      glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);

   glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
   glDeleteBuffers(1, &output_bo);
   glDeleteTextures(1, &texture);
   glDeleteBuffers(1, &source_bo);
   return pass;
}

static EGLDisplay
get_display(void)
{
   const char *platform = getenv("TENSOR_PROBE_PLATFORM");
   if (platform && strcmp(platform, "android") == 0)
      return eglGetDisplay(EGL_DEFAULT_DISPLAY);

   PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
         "eglGetPlatformDisplayEXT");
   if (!get_platform_display)
      return EGL_NO_DISPLAY;

   return get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                               EGL_DEFAULT_DISPLAY, NULL);
}

int
main(void)
{
   static const float rgba32f_input[16] = {
      1.0f, -2.0f, 3.5f, 4.25f,
      -5.0f, 6.5f, 7.0f, -8.25f,
      9.0f, 10.5f, -11.0f, 12.25f,
      13.0f, -14.5f, 15.0f, 16.25f,
   };
   static const uint32_t r32ui_input[4] = {
      UINT32_C(0), UINT32_C(1), UINT32_C(0x12345678), UINT32_MAX,
   };
   static const uint16_t r16ui_input[4] = {
      UINT16_C(0), UINT16_C(1), UINT16_C(0x1234), UINT16_MAX,
   };
   static const uint32_t r16ui_expected[4] = {
      UINT32_C(0), UINT32_C(1), UINT32_C(0x1234), UINT16_MAX,
   };
   static const struct probe_case tests[] = {
      {
         .name = "rgba32f",
         .internal_format = GL_RGBA32F,
         .input = rgba32f_input,
         .input_size = sizeof(rgba32f_input),
         .expected = rgba32f_input,
         .output_size = sizeof(rgba32f_input),
         .integer_output = false,
      },
      {
         .name = "r32ui",
         .internal_format = GL_R32UI,
         .input = r32ui_input,
         .input_size = sizeof(r32ui_input),
         .expected = r32ui_input,
         .output_size = sizeof(r32ui_input),
         .integer_output = true,
      },
      {
         .name = "r16ui",
         .internal_format = GL_R16UI,
         .input = r16ui_input,
         .input_size = sizeof(r16ui_input),
         .expected = r16ui_expected,
         .output_size = sizeof(r16ui_expected),
         .integer_output = true,
      },
   };

   uint64_t init_begin_ns = monotonic_ns();
   EGLDisplay display = get_display();
   EGLint egl_major = 0;
   EGLint egl_minor = 0;
   if (display == EGL_NO_DISPLAY ||
       !eglInitialize(display, &egl_major, &egl_minor)) {
      fprintf(stderr, "eglInitialize failed: 0x%x\n", eglGetError());
      return 2;
   }

   static const EGLint config_attributes[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_NONE,
   };
   static const EGLint pbuffer_attributes[] = {
      EGL_WIDTH, 1,
      EGL_HEIGHT, 1,
      EGL_NONE,
   };
   static const EGLint context_attributes[] = {
      EGL_CONTEXT_CLIENT_VERSION, 3,
      EGL_NONE,
   };

   EGLConfig config = NULL;
   EGLint config_count = 0;
   if (!eglBindAPI(EGL_OPENGL_ES_API) ||
       !eglChooseConfig(display, config_attributes, &config, 1,
                        &config_count) || config_count != 1) {
      fprintf(stderr, "EGL config selection failed: 0x%x\n", eglGetError());
      eglTerminate(display);
      return 2;
   }

   EGLSurface surface = eglCreatePbufferSurface(display, config,
                                                 pbuffer_attributes);
   EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
                                         context_attributes);
   if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
       !eglMakeCurrent(display, surface, surface, context)) {
      fprintf(stderr, "EGL context creation failed: 0x%x\n", eglGetError());
      eglTerminate(display);
      return 2;
   }

   const char *renderer = (const char *)glGetString(GL_RENDERER);
   const char *version = (const char *)glGetString(GL_VERSION);
   if (!renderer)
      renderer = "unknown";
   if (!version)
      version = "unknown";

   printf("{\"probe\":\"environment\",\"renderer\":\"%s\","
          "\"gl_version\":\"%s\",\"egl_version\":\"%d.%d\","
          "\"init_us\":%.3f}\n",
          renderer, version, egl_major, egl_minor,
          (double)(monotonic_ns() - init_begin_ns) / 1000.0);

   if (!has_extension("GL_EXT_texture_buffer")) {
      fprintf(stderr, "GL_EXT_texture_buffer is unavailable\n");
      return 3;
   }

   tex_buffer_ext_fn tex_buffer =
      (tex_buffer_ext_fn)eglGetProcAddress("glTexBufferEXT");
   tex_buffer_range_ext_fn tex_buffer_range =
      (tex_buffer_range_ext_fn)eglGetProcAddress("glTexBufferRangeEXT");
   if (!tex_buffer || !tex_buffer_range) {
      fprintf(stderr, "GL_EXT_texture_buffer entry points are unavailable\n");
      return 3;
   }

   GLint range_alignment = 0;
   glGetIntegerv(GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT_EXT, &range_alignment);
   if (range_alignment <= 0) {
      fprintf(stderr, "invalid texture-buffer range alignment: %d\n",
              range_alignment);
      return 3;
   }
   printf("{\"probe\":\"capability\","
          "\"texture_buffer_offset_alignment\":%d}\n",
          range_alignment);

   const uint32_t range_values[4] = {
      UINT32_C(0x01020304), UINT32_C(0x11223344),
      UINT32_C(0x89abcdef), UINT32_C(0xfedcba98),
   };
   size_t range_input_size = (size_t)range_alignment + sizeof(range_values);
   uint8_t *range_input = calloc(range_input_size, 1);
   if (!range_input)
      return 4;
   memcpy(range_input + range_alignment, range_values, sizeof(range_values));

   const struct probe_case range_test = {
      .name = "r32ui_range",
      .internal_format = GL_R32UI,
      .input = range_input,
      .input_size = range_input_size,
      .expected = range_values,
      .output_size = sizeof(range_values),
      .integer_output = true,
      .use_range = true,
      .range_offset = (size_t)range_alignment,
      .range_size = sizeof(range_values),
   };
   const struct probe_case range_orphan_test = {
      .name = "r32ui_range_orphan_after_bind",
      .internal_format = GL_R32UI,
      .input = range_input,
      .input_size = range_input_size,
      .expected = range_values,
      .output_size = sizeof(range_values),
      .integer_output = true,
      .use_range = true,
      .range_offset = (size_t)range_alignment,
      .range_size = sizeof(range_values),
      .orphan_after_binding = true,
   };

   static const uint32_t interleaved_input[4] = {
      UINT32_C(3), UINT32_C(7), UINT32_C(11), UINT32_C(19),
   };
   static const uint32_t interleaved_expected[12] = {
      UINT32_C(3), UINT32_C(3) ^ UINT32_C(0x55aa55aa), UINT32_C(20),
      UINT32_C(7), UINT32_C(7) ^ UINT32_C(0x55aa55aa), UINT32_C(24),
      UINT32_C(11), UINT32_C(11) ^ UINT32_C(0x55aa55aa), UINT32_C(28),
      UINT32_C(19), UINT32_C(19) ^ UINT32_C(0x55aa55aa), UINT32_C(36),
   };
   const struct probe_case interleaved_test = {
      .name = "r32ui_xfb_interleaved_stride12",
      .internal_format = GL_R32UI,
      .input = interleaved_input,
      .input_size = sizeof(interleaved_input),
      .expected = interleaved_expected,
      .output_size = sizeof(interleaved_expected),
      .integer_output = true,
   };

   GLuint float_program = create_program(false);
   GLuint integer_program = create_program(true);
   GLuint interleaved_program = create_interleaved_program();
   if (!float_program || !integer_program || !interleaved_program)
      return 4;

   unsigned repeat = 1;
   const char *repeat_option = getenv("TENSOR_PROBE_REPEAT");
   if (repeat_option) {
      unsigned long parsed = strtoul(repeat_option, NULL, 10);
      if (parsed > 0 && parsed <= 10000)
         repeat = (unsigned)parsed;
   }

   unsigned failures = 0;
   for (unsigned iteration = 1; iteration <= repeat; ++iteration) {
      for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
         if (!case_enabled(tests[i].name))
            continue;
         GLuint program = tests[i].integer_output ? integer_program
                                                   : float_program;
         if (!run_case(&tests[i], program, tex_buffer, tex_buffer_range,
                       renderer, iteration))
            ++failures;
      }
      if (case_enabled(range_test.name) &&
          !run_case(&range_test, integer_program, tex_buffer,
                    tex_buffer_range, renderer, iteration))
         ++failures;
      if (case_enabled(range_orphan_test.name) &&
          !run_case(&range_orphan_test, integer_program, tex_buffer,
                    tex_buffer_range, renderer, iteration))
         ++failures;
      if (case_enabled(interleaved_test.name) &&
          !run_case(&interleaved_test, interleaved_program, tex_buffer,
                    tex_buffer_range, renderer, iteration))
         ++failures;
   }

   free(range_input);
   glDeleteProgram(interleaved_program);
   glDeleteProgram(integer_program);
   glDeleteProgram(float_program);
   eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(display, context);
   eglDestroySurface(display, surface);
   eglTerminate(display);
   return failures ? 1 : 0;
}
