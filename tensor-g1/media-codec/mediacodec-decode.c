#include <stdio.h>

#ifdef __ANDROID__

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef bool (*binder_set_max_threads_fn)(uint32_t);
typedef void (*binder_start_thread_pool_fn)(void);

static void *binder_ndk;

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

   media_status_t status = AMediaCodec_configure(codec, format, NULL, NULL, 0);
   if (status == AMEDIA_OK)
      status = AMediaCodec_start(codec);
   if (status != AMEDIA_OK) {
      fprintf(stderr, "codec=%s configure/start failed: %d\n",
              name ? name : "(unknown)", status);
      goto fail;
   }

   size_t access_units[256];
   unsigned access_unit_count =
      find_access_units(input, input_size, access_units, 256);
   if (!access_unit_count) {
      access_units[0] = 0;
      access_unit_count = 1;
   }

   unsigned next_frame = 0;
   int eos_queued = 0;
   unsigned decoded_buffers = 0;
   size_t decoded_bytes = 0;
   int saw_eos = 0;

   for (unsigned attempt = 0; attempt < 500 && !saw_eos; ++attempt) {
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
         printf("output-format: %s\n", AMediaFormat_toString(output_format));
         AMediaFormat_delete(output_format);
      }
      if (output_index < 0)
         continue;

      size_t output_capacity = 0;
      uint8_t *output = AMediaCodec_getOutputBuffer(
         codec, (size_t)output_index, &output_capacity);
      if (info.size > 0 && output) {
         ++decoded_buffers;
         decoded_bytes += (size_t)info.size;
      }
      saw_eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
      printf("output: size=%d capacity=%zu flags=0x%x pts=%lld\n", info.size,
             output_capacity, info.flags, (long long)info.presentationTimeUs);
      AMediaCodec_releaseOutputBuffer(codec, (size_t)output_index, false);
   }

   printf("codec=%s input=%zu access-units=%u decoded-buffers=%u "
          "decoded-bytes=%zu eos=%d\n",
          name ? name : "(unknown)", input_size, access_unit_count,
          decoded_buffers, decoded_bytes, saw_eos);

   AMediaCodec_stop(codec);
   if (name)
      AMediaCodec_releaseName(codec, name);
   AMediaCodec_delete(codec);
   AMediaFormat_delete(format);
   free(input);
   return decoded_buffers && saw_eos ? 0 : 1;

fail_started:
   AMediaCodec_stop(codec);
fail:
   if (name)
      AMediaCodec_releaseName(codec, name);
   AMediaCodec_delete(codec);
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
