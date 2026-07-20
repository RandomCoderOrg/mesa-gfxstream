#include <stdio.h>

#ifdef __ANDROID__

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <android/hardware_buffer.h>
#include <android/native_window.h>

#include <dlfcn.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef bool (*binder_set_max_threads_fn)(uint32_t);
typedef void (*binder_start_thread_pool_fn)(void);

static void *binder_ndk;

struct tensor_native_handle {
   int version;
   int num_fds;
   int num_ints;
   int data[];
};

typedef const struct tensor_native_handle *
(*get_native_handle_fn)(const AHardwareBuffer *buffer);

static uint64_t
monotonic_ns(void)
{
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
      return 0;
   return (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
}

static int
start_binder_thread_pool(void)
{
   binder_ndk = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
   if (!binder_ndk) {
      fprintf(stderr, "dlopen libbinder_ndk.so failed: %s\n", dlerror());
      return 0;
   }

   binder_set_max_threads_fn set_max_threads =
      (binder_set_max_threads_fn)dlsym(
         binder_ndk, "ABinderProcess_setThreadPoolMaxThreadCount");
   binder_start_thread_pool_fn start_thread_pool =
      (binder_start_thread_pool_fn)dlsym(
         binder_ndk, "ABinderProcess_startThreadPool");
   if (!set_max_threads || !start_thread_pool) {
      fprintf(stderr, "libbinder_ndk.so lacks Binder process functions: %s\n",
              dlerror());
      return 0;
   }

   if (!set_max_threads(4)) {
      fprintf(stderr, "failed to configure the Binder thread pool\n");
      return 0;
   }

   start_thread_pool();
   return 1;
}

static uint8_t *
read_file(const char *path, size_t *size_out)
{
   FILE *file = fopen(path, "rb");
   if (!file)
      return NULL;

   if (fseek(file, 0, SEEK_END) || ftell(file) < 0) {
      fclose(file);
      return NULL;
   }

   long length = ftell(file);
   rewind(file);

   uint8_t *data = malloc((size_t)length);
   if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
      free(data);
      fclose(file);
      return NULL;
   }

   fclose(file);
   *size_out = (size_t)length;
   return data;
}

static unsigned
find_access_units(const uint8_t *data, size_t size, size_t *offsets,
                  unsigned capacity)
{
   unsigned count = 0;
   for (size_t i = 0; i + 4 < size && count < capacity; ++i) {
      size_t header = 0;
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
         header = i + 3;
      else if (i + 5 < size && data[i] == 0 && data[i + 1] == 0 &&
               data[i + 2] == 0 && data[i + 3] == 1)
         header = i + 4;
      else
         continue;

      if ((data[header] & 0x1f) == 9) {
         offsets[count++] = i;
         i = header;
      }
   }
   return count;
}

static int
find_nal(const uint8_t *data, size_t size, unsigned wanted_type,
         size_t *offset_out, size_t *size_out)
{
   for (size_t i = 0; i + 4 < size; ++i) {
      size_t header = 0;
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
         header = i + 3;
      else if (i + 5 < size && data[i] == 0 && data[i + 1] == 0 &&
               data[i + 2] == 0 && data[i + 3] == 1)
         header = i + 4;
      else
         continue;

      if ((data[header] & 0x1f) != wanted_type) {
         i = header;
         continue;
      }

      size_t end = header + 1;
      while (end + 3 < size) {
         if (data[end] == 0 && data[end + 1] == 0 &&
             (data[end + 2] == 1 ||
              (end + 3 < size && data[end + 2] == 0 &&
               data[end + 3] == 1)))
            break;
         ++end;
      }
      *offset_out = i;
      *size_out = end - i;
      return 1;
   }
   return 0;
}

static int
acquire_surface_image(AImageReader *reader, unsigned *image_count, int quiet)
{
   for (unsigned attempt = 0; attempt < 100; attempt++) {
      AImage *image = NULL;
      int fence_fd = -1;
      media_status_t status =
         AImageReader_acquireNextImageAsync(reader, &image, &fence_fd);
      if (status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) {
         usleep(1000);
         continue;
      }
      if (status != AMEDIA_OK || !image) {
         fprintf(stderr, "acquire image failed: %d\n", status);
         return 0;
      }

      if (fence_fd >= 0) {
         struct pollfd fence = {.fd = fence_fd, .events = POLLIN};
         if (poll(&fence, 1, 1000) <= 0) {
            fprintf(stderr, "image acquire fence did not signal\n");
            close(fence_fd);
            AImage_delete(image);
            return 0;
         }
         close(fence_fd);
      }

      (*image_count)++;
      if (quiet) {
         AImage_delete(image);
         return 1;
      }

      AHardwareBuffer *buffer = NULL;
      AHardwareBuffer_Desc descriptor = {0};
      int32_t width = 0, height = 0, planes = 0;
      int64_t timestamp_ns = 0;
      AImage_getWidth(image, &width);
      AImage_getHeight(image, &height);
      AImage_getTimestamp(image, &timestamp_ns);
      AImage_getNumberOfPlanes(image, &planes);
      if (AImage_getHardwareBuffer(image, &buffer) == AMEDIA_OK && buffer)
         AHardwareBuffer_describe(buffer, &descriptor);

      printf("image: count=%u image=%dx%d planes=%d timestamp-ns=%lld "
             "ahb=%ux%u stride=%u layers=%u format=%u usage=0x%llx "
             "acquire-fence=%s\n",
             *image_count, width, height, planes, (long long)timestamp_ns,
             descriptor.width, descriptor.height, descriptor.stride,
             descriptor.layers, descriptor.format,
             (unsigned long long)descriptor.usage,
             fence_fd >= 0 ? "signaled" : "none");
      if (*image_count == 1 && buffer) {
         for (int32_t plane = 0; plane < planes; plane++) {
            uint8_t *plane_data = NULL;
            int data_length = 0, row_stride = 0, pixel_stride = 0;
            media_status_t data_status = AImage_getPlaneData(
               image, plane, &plane_data, &data_length);
            AImage_getPlaneRowStride(image, plane, &row_stride);
            AImage_getPlanePixelStride(image, plane, &pixel_stride);
            printf("plane: index=%d status=%d length=%d row-stride=%d "
                   "pixel-stride=%d first=%u\n",
                   plane, data_status, data_length, row_stride, pixel_stride,
                   data_status == AMEDIA_OK && plane_data && data_length
                      ? plane_data[0]
                      : 0);
         }
         void *system_android =
            dlopen("/system/lib64/libandroid.so", RTLD_NOW | RTLD_LOCAL);
         get_native_handle_fn get_native_handle =
            system_android
               ? (get_native_handle_fn)dlsym(
                    system_android, "AHardwareBuffer_getNativeHandle")
               : NULL;
         const struct tensor_native_handle *handle =
            get_native_handle ? get_native_handle(buffer) : NULL;
         if (handle) {
            printf("native-handle: fds=%d ints=%d", handle->num_fds,
                   handle->num_ints);
            for (int i = 0; i < handle->num_fds; i++) {
               struct stat status;
               long long size = fstat(handle->data[i], &status) == 0
                                   ? (long long)status.st_size
                                   : -1;
               printf(" fd%d=%d size%d=%lld", i, handle->data[i], i, size);
            }
            putchar('\n');
         }
         if (system_android)
            dlclose(system_android);
      }
      AImage_delete(image);
      return 1;
   }

   fprintf(stderr, "timed out waiting for an ImageReader buffer\n");
   return 0;
}

int
main(int argc, char **argv)
{
   if (argc != 2 && argc != 3 && argc != 5 && argc != 6) {
      fprintf(stderr,
              "usage: %s annex-b.h264 "
              "[codec-component [width height [frame-rate]]]\n",
              argv[0]);
      return 2;
   }

   if (!start_binder_thread_pool())
      return 1;

   int width = 128;
   int height = 72;
   int frame_rate = 30;
   if (argc >= 5) {
      width = atoi(argv[3]);
      height = atoi(argv[4]);
      if (width <= 0 || height <= 0) {
         fprintf(stderr, "width and height must be positive integers\n");
         return 2;
      }
   }
   if (argc == 6) {
      frame_rate = atoi(argv[5]);
      if (frame_rate <= 0) {
         fprintf(stderr, "frame rate must be a positive integer\n");
         return 2;
      }
   }

   size_t input_size = 0;
   uint8_t *input = read_file(argv[1], &input_size);
   if (!input) {
      perror(argv[1]);
      return 1;
   }

   AMediaCodec *codec = argc == 3 ? AMediaCodec_createCodecByName(argv[2])
                                  : AMediaCodec_createDecoderByType("video/avc");
   AMediaFormat *format = AMediaFormat_new();
   AImageReader *reader = NULL;
   ANativeWindow *window = NULL;
   int private_output = getenv("TENSOR_MEDIACODEC_PRIVATE") != NULL;
   int quiet = getenv("TENSOR_MEDIACODEC_QUIET") != NULL;
   int surface_output =
      private_output || getenv("TENSOR_MEDIACODEC_SURFACE") != NULL;
   media_status_t status = AMEDIA_OK;
   if (!codec || !format) {
      fprintf(stderr, "failed to create MediaCodec or MediaFormat\n");
      free(input);
      return 1;
   }

   char *name = NULL;
   AMediaCodec_getName(codec, &name);
   AMediaFormat_setString(format, "mime", "video/avc");
   AMediaFormat_setInt32(format, "width", width);
   AMediaFormat_setInt32(format, "height", height);
   AMediaFormat_setInt32(format, "frame-rate", frame_rate);
   AMediaFormat_setInt32(format, "low-latency", 1);
   AMediaFormat_setInt32(format, "color-format", 21);
   AMediaFormat_setInt32(format, "max-input-size", (int32_t)input_size);

   size_t sps_offset = 0, sps_size = 0;
   size_t pps_offset = 0, pps_size = 0;
   if (!getenv("TENSOR_MEDIACODEC_INBAND_CSD")) {
      if (find_nal(input, input_size, 7, &sps_offset, &sps_size))
         AMediaFormat_setBuffer(format, "csd-0", input + sps_offset, sps_size);
      if (find_nal(input, input_size, 8, &pps_offset, &pps_size))
         AMediaFormat_setBuffer(format, "csd-1", input + pps_offset, pps_size);
   }

   if (surface_output) {
      int32_t image_format = private_output ? AIMAGE_FORMAT_PRIVATE
                                            : AIMAGE_FORMAT_YUV_420_888;
      uint64_t usage = private_output
                          ? AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE
                          : AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
      status = AImageReader_newWithUsage(
         width, height, image_format, usage, 8, &reader);
      if (status != AMEDIA_OK || !reader ||
          AImageReader_getWindow(reader, &window) != AMEDIA_OK || !window) {
         fprintf(stderr, "failed to create MediaCodec ImageReader: %d\n",
                 status);
         goto fail;
      }
   }

   status = AMediaCodec_configure(codec, format, window, NULL, 0);
   if (status == AMEDIA_OK)
      status = AMediaCodec_start(codec);
   if (status != AMEDIA_OK) {
      fprintf(stderr, "codec=%s configure/start failed: %d\n",
              name ? name : "(unknown)", status);
      goto fail;
   }

   size_t access_units[4096];
   unsigned access_unit_count =
      find_access_units(input, input_size, access_units, 4096);
   if (!access_unit_count) {
      access_units[0] = 0;
      access_unit_count = 1;
   }

   unsigned next_frame = 0;
   int eos_queued = 0;
   unsigned decoded_buffers = 0;
   size_t decoded_bytes = 0;
   unsigned acquired_images = 0;
   int saw_eos = 0;

   unsigned maximum_attempts = access_unit_count * 3u + 100u;
   uint64_t run_started_ns = monotonic_ns();
   for (unsigned attempt = 0; attempt < maximum_attempts && !saw_eos;
        ++attempt) {
      if (!eos_queued) {
         ssize_t input_index = AMediaCodec_dequeueInputBuffer(codec, 10000);
         if (input_index >= 0) {
            if (next_frame < access_unit_count) {
               size_t begin = access_units[next_frame];
               size_t end = next_frame + 1 < access_unit_count
                               ? access_units[next_frame + 1]
                               : input_size;
               size_t access_unit_size = end - begin;
               size_t input_capacity = 0;
               uint8_t *input_buffer = AMediaCodec_getInputBuffer(
                  codec, (size_t)input_index, &input_capacity);
               if (!input_buffer || input_capacity < access_unit_size) {
                  fprintf(stderr,
                          "input buffer unavailable or too small: %zu < %zu\n",
                          input_capacity, access_unit_size);
                  goto fail_started;
               }

               memcpy(input_buffer, input + begin, access_unit_size);
               status = AMediaCodec_queueInputBuffer(
                  codec, (size_t)input_index, 0, access_unit_size,
                  (uint64_t)next_frame * 1000000 / (uint64_t)frame_rate, 0);
               if (status != AMEDIA_OK) {
                  fprintf(stderr, "queue input frame %u failed: %d\n",
                          next_frame, status);
                  goto fail_started;
               }
               ++next_frame;
            } else {
               status = AMediaCodec_queueInputBuffer(
                  codec, (size_t)input_index, 0, 0,
                  (uint64_t)next_frame * 1000000 / (uint64_t)frame_rate,
                  AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
               if (status != AMEDIA_OK) {
                  fprintf(stderr, "queue EOS failed: %d\n", status);
                  goto fail_started;
               }
               eos_queued = 1;
            }
         }
      }

      AMediaCodecBufferInfo info;
      ssize_t output_index =
         AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);

      if (output_index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
         AMediaFormat *output_format = AMediaCodec_getOutputFormat(codec);
         if (!quiet)
            printf("output-format: %s\n",
                   AMediaFormat_toString(output_format));
         AMediaFormat_delete(output_format);
      }
      if (output_index < 0)
         continue;

      size_t output_capacity = 0;
      uint8_t *output = AMediaCodec_getOutputBuffer(
         codec, (size_t)output_index, &output_capacity);
      if (info.size > 0 && (surface_output || output)) {
         ++decoded_buffers;
         decoded_bytes += (size_t)info.size;
      }
      saw_eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
      if (!quiet)
         printf("output: size=%d capacity=%zu flags=0x%x pts=%lld\n",
                info.size, output_capacity, info.flags,
                (long long)info.presentationTimeUs);
      AMediaCodec_releaseOutputBuffer(codec, (size_t)output_index,
                                      surface_output && info.size > 0);
      if (surface_output && info.size > 0 &&
          !acquire_surface_image(reader, &acquired_images, quiet))
         goto fail_started;
   }

   uint64_t elapsed_ns = monotonic_ns() - run_started_ns;
   double decoded_fps = elapsed_ns ? 1e9 * decoded_buffers / elapsed_ns : 0.0;

   printf("codec=%s input=%zu access-units=%u decoded-buffers=%u "
          "decoded-bytes=%zu images=%u eos=%d\n",
          name ? name : "(unknown)", input_size, access_unit_count,
          decoded_buffers, decoded_bytes, acquired_images, saw_eos);
   printf("{\"schema\":\"tensor-perf-v1\",\"kind\":\"benchmark\","
          "\"name\":\"mediacodec-decode\",\"elapsed_ns\":%llu,"
          "\"input_bytes\":%zu,\"access_units\":%u,"
          "\"decoded_frames\":%u,\"decoded_bytes\":%zu,"
          "\"acquired_images\":%u,\"decoded_fps\":%.6f,"
          "\"surface_output\":%s,\"private_output\":%s,\"eos\":%s}\n",
          (unsigned long long)elapsed_ns, input_size, access_unit_count,
          decoded_buffers, decoded_bytes, acquired_images, decoded_fps,
          surface_output ? "true" : "false",
          private_output ? "true" : "false", saw_eos ? "true" : "false");

   AMediaCodec_stop(codec);
   if (name)
      AMediaCodec_releaseName(codec, name);
   AMediaCodec_delete(codec);
   if (reader)
      AImageReader_delete(reader);
   AMediaFormat_delete(format);
   free(input);
   return decoded_buffers && saw_eos ? 0 : 1;

fail_started:
   AMediaCodec_stop(codec);
fail:
   if (name)
      AMediaCodec_releaseName(codec, name);
   AMediaCodec_delete(codec);
   if (reader)
      AImageReader_delete(reader);
   AMediaFormat_delete(format);
   free(input);
   return 1;
}

#else

int
main(void)
{
   fputs("mediacodec-decode must be built for Android\n", stderr);
   return 77;
}

#endif
