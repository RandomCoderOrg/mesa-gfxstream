#include <stdio.h>

#ifdef __ANDROID__

#include "bridge-protocol.h"
#include "perf-metrics.h"
#include "release-fence.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

typedef bool (*binder_set_max_threads_fn)(uint32_t);
typedef void (*binder_start_thread_pool_fn)(void);

#define TMC_MAX_SHARED_SURFACES 64

struct tmc_shared_surface {
   bool used;
   int64_t pts_us;
   int fd;
   uint8_t *data;
   size_t data_size;
   uint32_t stride;
   uint32_t slice_height;
   bool cpu_sync_active;
   struct tmc_release_fence release_fence;
};

static int
read_all(int fd, void *data, size_t size)
{
   uint8_t *cursor = data;
   while (size) {
      ssize_t got = read(fd, cursor, size);
      if (got == 0)
         return 0;
      if (got < 0) {
         if (errno == EINTR)
            continue;
         return -1;
      }
      cursor += got;
      size -= (size_t)got;
   }
   return 1;
}

static int
remap_latest_enabled(void)
{
   const char *value = getenv("TENSOR_MEDIACODEC_REMAP_LATEST");
   return value && value[0] && strcmp(value, "0") != 0;
}

static int
release_fence_enabled(void)
{
   const char *value = getenv("TENSOR_MEDIACODEC_RELEASE_FENCE");
   return value && value[0] && strcmp(value, "0") != 0;
}

static int
receive_header(int fd, struct tmc_message *message, int *received_fd)
{
   struct iovec iov = {
      .iov_base = message,
      .iov_len = sizeof(*message),
   };
   char control[CMSG_SPACE(sizeof(int))] = {0};
   struct msghdr msg = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = control,
      .msg_controllen = sizeof(control),
   };

   *received_fd = -1;
   ssize_t got;
   do {
      got = recvmsg(fd, &msg, MSG_WAITALL);
   } while (got < 0 && errno == EINTR);
   if (got == 0)
      return 0;
   if (got != (ssize_t)sizeof(*message))
      return -1;

   for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg;
        cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
          cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
         memcpy(received_fd, CMSG_DATA(cmsg), sizeof(*received_fd));
         break;
      }
   }
   return 1;
}

static int
dma_buf_sync(int fd, uint64_t flags)
{
   struct dma_buf_sync sync = {.flags = flags};
   int result;
   do {
      result = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
   } while (result < 0 && errno == EINTR);
   return result == 0;
}

static void
release_shared_surface(struct tmc_shared_surface *surface,
                       struct tmc_perf *perf)
{
   if (surface->cpu_sync_active) {
      if (!dma_buf_sync(surface->fd,
                        DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE))
         fprintf(stderr, "failed to end DMA-BUF CPU write pts=%lld\n",
                 (long long)surface->pts_us);
      surface->cpu_sync_active = false;
   }
   bool release_fenced = surface->release_fence.context != NULL;
   uint64_t timer = release_fenced ? tmc_perf_begin(perf) : 0;
   bool signaled = tmc_release_fence_signal(&surface->release_fence);
   if (release_fenced)
      tmc_perf_record(perf, TMC_PERF_RELEASE_FENCE_SIGNAL, timer, 0);
   if (!signaled)
      fprintf(stderr, "failed to signal DMA-BUF release fence pts=%lld\n",
              (long long)surface->pts_us);
   if (surface->data && surface->data_size)
      munmap(surface->data, surface->data_size);
   if (surface->fd >= 0)
      close(surface->fd);
   memset(surface, 0, sizeof(*surface));
   surface->fd = -1;
   surface->release_fence.slot = -1;
}

static void
release_shared_surfaces(struct tmc_shared_surface *surfaces,
                        struct tmc_perf *perf)
{
   for (unsigned i = 0; i < TMC_MAX_SHARED_SURFACES; i++)
      if (surfaces[i].used)
         release_shared_surface(&surfaces[i], perf);
}

static struct tmc_shared_surface *
find_shared_surface(struct tmc_shared_surface *surfaces, int64_t pts_us)
{
   for (unsigned i = 0; i < TMC_MAX_SHARED_SURFACES; i++)
      if (surfaces[i].used && surfaces[i].pts_us == pts_us)
         return &surfaces[i];
   return NULL;
}

static int
register_shared_surface(struct tmc_shared_surface *surfaces,
                        const struct tmc_message *message, int received_fd,
                        struct tmc_release_fence_context *fence_context,
                        struct tmc_perf *perf)
{
   if (received_fd < 0 || !message->arg0 || !message->arg1 || !message->arg2)
      return 0;

   /*
    * Firefox exports each VA surface before Exynos returns the matching
    * display-order output. In the experimental remap mode only the newest
    * registered destination can be useful: older placeholders have already
    * been handed to the compositor. Drop their service-side references so a
    * delayed output can be copied into the current surface synchronously.
    */
   if (remap_latest_enabled())
      release_shared_surfaces(surfaces, perf);

   struct tmc_shared_surface *surface =
      find_shared_surface(surfaces, message->pts_us);
   if (surface)
      release_shared_surface(surface, perf);
   else {
      for (unsigned i = 0; i < TMC_MAX_SHARED_SURFACES; i++) {
         if (!surfaces[i].used) {
            surface = &surfaces[i];
            break;
         }
      }
   }
   if (!surface)
      return 0;

   void *mapping = mmap(NULL, message->arg2, PROT_READ | PROT_WRITE,
                        MAP_SHARED, received_fd, 0);
   if (mapping == MAP_FAILED)
      return 0;

   memset(surface, 0, sizeof(*surface));
   surface->used = true;
   surface->pts_us = message->pts_us;
   surface->fd = received_fd;
   surface->data = mapping;
   surface->data_size = message->arg2;
   surface->stride = message->arg0;
   surface->slice_height = message->arg1;
   surface->release_fence.slot = -1;
   if (fence_context) {
      uint64_t timer = tmc_perf_begin(perf);
      bool sync_started = dma_buf_sync(
         received_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
      tmc_perf_record(perf, TMC_PERF_DMABUF_SYNC_START, timer, 0);
      if (!sync_started) {
         release_shared_surface(surface, perf);
         return 0;
      }
      surface->cpu_sync_active = true;
      timer = tmc_perf_begin(perf);
      bool armed = tmc_release_fence_arm(fence_context, received_fd,
                                         &surface->release_fence);
      tmc_perf_record(perf, TMC_PERF_RELEASE_FENCE_ARM, timer, 0);
      if (!armed) {
         release_shared_surface(surface, perf);
         return 0;
      }
   }
   return 1;
}

static int
copy_to_shared_surface(struct tmc_shared_surface *surface,
                       const uint8_t *source, size_t source_size,
                       uint32_t source_stride, uint32_t source_slice_height,
                       struct tmc_perf *perf)
{
   if (!source || !source_stride || !source_slice_height ||
       (size_t)source_stride > SIZE_MAX / source_slice_height ||
       (size_t)surface->stride > SIZE_MAX / surface->slice_height)
      return 0;

   size_t source_y_size = (size_t)source_stride * source_slice_height;
   size_t destination_y_size =
      (size_t)surface->stride * surface->slice_height;
   if (source_y_size > source_size ||
       destination_y_size > surface->data_size ||
       destination_y_size > SIZE_MAX - destination_y_size / 2 ||
       destination_y_size + destination_y_size / 2 > surface->data_size)
      return 0;

   bool release_fenced = surface->release_fence.context != NULL;
   uint64_t timer = 0;
   int sync_started = surface->cpu_sync_active;
   if (!release_fenced) {
      timer = tmc_perf_begin(perf);
      sync_started =
         dma_buf_sync(surface->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
      tmc_perf_record(perf, TMC_PERF_DMABUF_SYNC_START, timer, 0);
   }
   if (!sync_started)
      return 0;

   unsigned luma_rows = source_slice_height < surface->slice_height
                           ? source_slice_height
                           : surface->slice_height;
   size_t row_bytes = source_stride < surface->stride
                         ? source_stride
                         : surface->stride;
   size_t source_uv_size = source_size - source_y_size;
   unsigned chroma_rows = source_slice_height / 2;
   if (chroma_rows > surface->slice_height / 2)
      chroma_rows = surface->slice_height / 2;

   uint64_t cleared_bytes = 0;
   timer = tmc_perf_begin(perf);
   if (row_bytes < surface->stride) {
      size_t tail_bytes = surface->stride - row_bytes;
      for (unsigned row = 0; row < luma_rows; row++)
         memset(surface->data + (size_t)row * surface->stride + row_bytes,
                16, tail_bytes);
      cleared_bytes += (uint64_t)luma_rows * tail_bytes;
   }
   if (luma_rows < surface->slice_height) {
      size_t missing_bytes =
         (size_t)(surface->slice_height - luma_rows) * surface->stride;
      memset(surface->data + (size_t)luma_rows * surface->stride, 16,
             missing_bytes);
      cleared_bytes += missing_bytes;
   }
   for (unsigned row = 0; row < chroma_rows; row++) {
      size_t source_offset = (size_t)row * source_stride;
      size_t available = source_offset < source_uv_size
                            ? source_uv_size - source_offset
                            : 0;
      size_t copy_bytes = row_bytes < available ? row_bytes : available;
      if (copy_bytes < surface->stride) {
         size_t tail_bytes = surface->stride - copy_bytes;
         memset(surface->data + destination_y_size +
                   (size_t)row * surface->stride + copy_bytes,
                128, tail_bytes);
         cleared_bytes += tail_bytes;
      }
   }
   unsigned destination_chroma_rows = surface->slice_height / 2;
   if (chroma_rows < destination_chroma_rows) {
      size_t missing_bytes =
         (size_t)(destination_chroma_rows - chroma_rows) * surface->stride;
      memset(surface->data + destination_y_size +
                (size_t)chroma_rows * surface->stride,
             128, missing_bytes);
      cleared_bytes += missing_bytes;
   }
   tmc_perf_record(perf, TMC_PERF_SURFACE_CLEAR, timer, cleared_bytes);

   timer = tmc_perf_begin(perf);
   for (unsigned row = 0; row < luma_rows; row++)
      memcpy(surface->data + (size_t)row * surface->stride,
             source + (size_t)row * source_stride, row_bytes);
   tmc_perf_record(perf, TMC_PERF_SURFACE_COPY_Y, timer,
                   (uint64_t)luma_rows * row_bytes);

   uint64_t copied_uv_bytes = 0;
   timer = tmc_perf_begin(perf);
   for (unsigned row = 0; row < chroma_rows; row++) {
      size_t source_offset = (size_t)row * source_stride;
      if (source_offset >= source_uv_size)
         break;
      size_t available = source_uv_size - source_offset;
      size_t copy_bytes = row_bytes < available ? row_bytes : available;
      memcpy(surface->data + destination_y_size +
                (size_t)row * surface->stride,
             source + source_y_size + source_offset, copy_bytes);
      copied_uv_bytes += copy_bytes;
   }
   tmc_perf_record(perf, TMC_PERF_SURFACE_COPY_UV, timer, copied_uv_bytes);
   timer = tmc_perf_begin(perf);
   int sync_ended =
      dma_buf_sync(surface->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
   if (sync_ended)
      surface->cpu_sync_active = false;
   tmc_perf_record(perf, TMC_PERF_DMABUF_SYNC_END, timer, 0);
   return sync_ended;
}

static int
write_all(int fd, const void *data, size_t size)
{
   const uint8_t *cursor = data;
   while (size) {
      ssize_t sent = send(fd, cursor, size, MSG_NOSIGNAL);
      if (sent == 0)
         return 0;
      if (sent < 0) {
         if (errno == EINTR)
            continue;
         return 0;
      }
      cursor += sent;
      size -= (size_t)sent;
   }
   return 1;
}

static int
send_message(int fd, uint16_t type, int64_t pts_us, uint32_t flags,
             uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3,
             const void *payload, uint32_t payload_size)
{
   struct tmc_message message = {
      .magic = TMC_MAGIC,
      .version = TMC_VERSION,
      .type = type,
      .payload_size = payload_size,
      .flags = flags,
      .pts_us = pts_us,
      .arg0 = arg0,
      .arg1 = arg1,
      .arg2 = arg2,
      .arg3 = arg3,
   };
   return write_all(fd, &message, sizeof(message)) &&
          (!payload_size || write_all(fd, payload, payload_size));
}

static int
send_error(int fd, const char *message)
{
   uint32_t size = (uint32_t)strlen(message) + 1;
   send_message(fd, TMC_ERROR, 0, 0, 0, 0, 0, 0, message, size);
   fprintf(stderr, "%s\n", message);
   return 0;
}

static int
start_binder_thread_pool(void)
{
   void *library = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
   if (!library)
      return 0;
   binder_set_max_threads_fn set_max_threads =
      (binder_set_max_threads_fn)dlsym(
         library, "ABinderProcess_setThreadPoolMaxThreadCount");
   binder_start_thread_pool_fn start_thread_pool =
      (binder_start_thread_pool_fn)dlsym(
         library, "ABinderProcess_startThreadPool");
   if (!set_max_threads || !start_thread_pool || !set_max_threads(4))
      return 0;
   start_thread_pool();
   return 1;
}

static int
send_output_format(int fd, AMediaCodec *codec, uint32_t *output_stride,
                   uint32_t *output_slice_height)
{
   AMediaFormat *format = AMediaCodec_getOutputFormat(codec);
   int32_t width = 0, height = 0, stride = 0, slice_height = 0;
   AMediaFormat_getInt32(format, "width", &width);
   AMediaFormat_getInt32(format, "height", &height);
   AMediaFormat_getInt32(format, "stride", &stride);
   AMediaFormat_getInt32(format, "slice-height", &slice_height);
   *output_stride = stride > 0 ? (uint32_t)stride : (uint32_t)width;
   *output_slice_height =
      slice_height > 0 ? (uint32_t)slice_height : (uint32_t)height;
   printf("output-format: %s\n", AMediaFormat_toString(format));
   int ok = send_message(fd, TMC_FORMAT, 0, 0, (uint32_t)width,
                         (uint32_t)height, (uint32_t)stride,
                         (uint32_t)slice_height, NULL, 0);
   AMediaFormat_delete(format);
   return ok;
}

static int
drain_output(int fd, AMediaCodec *codec, int64_t first_timeout_us,
             int wait_for_eos, struct tmc_shared_surface *surfaces,
             uint32_t *output_stride, uint32_t *output_slice_height,
             struct tmc_perf *perf)
{
   unsigned empty_polls = 0;
   for (;;) {
      AMediaCodecBufferInfo info;
      uint64_t timer = tmc_perf_begin(perf);
      ssize_t index = AMediaCodec_dequeueOutputBuffer(
         codec, &info, empty_polls ? 100000 : first_timeout_us);
      tmc_perf_record(perf, TMC_PERF_OUTPUT_DEQUEUE, timer, 0);
      if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
         if (!send_output_format(fd, codec, output_stride,
                                 output_slice_height))
            return 0;
         continue;
      }
      if (index < 0) {
         perf->output_empty_polls++;
         if (!wait_for_eos || ++empty_polls >= 100)
            return !wait_for_eos;
         continue;
      }

      empty_polls = 0;
      size_t capacity = 0;
      uint8_t *buffer = AMediaCodec_getOutputBuffer(codec, (size_t)index,
                                                    &capacity);
      uint32_t size = info.size > 0 ? (uint32_t)info.size : 0;
      if (size && (!buffer || (size_t)info.offset + size > capacity)) {
         AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false);
         return send_error(fd, "invalid MediaCodec output buffer");
      }

      if (size) {
         struct tmc_shared_surface *surface = NULL;
         if (remap_latest_enabled()) {
            for (unsigned i = 0; i < TMC_MAX_SHARED_SURFACES; i++) {
               if (surfaces[i].used) {
                  surface = &surfaces[i];
                  break;
               }
            }
         } else {
            surface = find_shared_surface(surfaces, info.presentationTimeUs);
         }
         if (surface) {
            int64_t delivered_pts = surface->pts_us;
            if (remap_latest_enabled())
               perf->pts_remaps++;
            if (remap_latest_enabled())
               fprintf(stderr, "remap output pts=%lld -> surface pts=%lld\n",
                       (long long)info.presentationTimeUs,
                       (long long)delivered_pts);
            int copied = copy_to_shared_surface(
               surface, buffer + info.offset, size, *output_stride,
               *output_slice_height, perf);
            release_shared_surface(surface, perf);
            timer = tmc_perf_begin(perf);
            int sent = copied &&
               send_message(fd, TMC_FRAME, delivered_pts,
                            info.flags | TMC_FRAME_FLAG_SHARED_SURFACE,
                            0, 0, 0, 0, NULL, 0);
            tmc_perf_record(perf, TMC_PERF_FRAME_SEND, timer, 0);
            if (!sent) {
               AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false);
               return 0;
            }
            perf->shared_frames++;
         } else {
            timer = tmc_perf_begin(perf);
            int sent = send_message(fd, TMC_FRAME, info.presentationTimeUs,
                                    info.flags, 0, 0, 0, 0,
                                    buffer + info.offset, size);
            tmc_perf_record(perf, TMC_PERF_FRAME_SEND, timer, size);
            if (!sent) {
               AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false);
               return 0;
            }
         }
         perf->output_frames++;
         perf->output_bytes += size;
      }
      bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
      AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false);
      if (eos)
         return send_message(fd, TMC_OUTPUT_EOS, info.presentationTimeUs,
                             info.flags, 0, 0, 0, 0, NULL, 0);

      first_timeout_us = 0;
   }
}

static int
queue_input(AMediaCodec *codec, const void *payload, uint32_t size,
            int64_t pts_us, uint32_t flags, struct tmc_perf *perf)
{
   for (;;) {
      uint64_t timer = tmc_perf_begin(perf);
      ssize_t index = AMediaCodec_dequeueInputBuffer(codec, 100000);
      tmc_perf_record(perf, TMC_PERF_INPUT_DEQUEUE, timer, 0);
      if (index < 0)
         continue;
      size_t capacity = 0;
      uint8_t *buffer = AMediaCodec_getInputBuffer(codec, (size_t)index,
                                                   &capacity);
      if (!buffer || capacity < size)
         return 0;
      if (size) {
         timer = tmc_perf_begin(perf);
         memcpy(buffer, payload, size);
         tmc_perf_record(perf, TMC_PERF_INPUT_COPY, timer, size);
      }
      timer = tmc_perf_begin(perf);
      int queued = AMediaCodec_queueInputBuffer(codec, (size_t)index, 0, size,
                                                (uint64_t)pts_us, flags) == AMEDIA_OK;
      tmc_perf_record(perf, TMC_PERF_INPUT_QUEUE, timer, size);
      return queued;
   }
}

static int
serve_client(int fd)
{
   struct tmc_perf perf;
   tmc_perf_init(&perf);
   AMediaCodec *codec = NULL;
   AMediaFormat *format = NULL;
   int started = 0;
   int result = 0;
   uint32_t output_stride = 0;
   uint32_t output_slice_height = 0;
   struct tmc_shared_surface surfaces[TMC_MAX_SHARED_SURFACES] = {0};
   for (unsigned i = 0; i < TMC_MAX_SHARED_SURFACES; i++) {
      surfaces[i].fd = -1;
      surfaces[i].release_fence.slot = -1;
   }
   struct tmc_release_fence_context fence_context;
   tmc_release_fence_context_reset(&fence_context);
   int use_release_fences = release_fence_enabled();
   if (use_release_fences && remap_latest_enabled()) {
      send_error(fd, "release fences cannot be combined with remap-latest");
      goto finish;
   }
   if (use_release_fences &&
       !tmc_release_fence_context_init(&fence_context)) {
      send_error(fd, "failed to initialize Kbase release fences");
      goto finish;
   }

   for (;;) {
      struct tmc_message message;
      int received_fd = -1;
      uint64_t timer = tmc_perf_begin(&perf);
      int read_status = receive_header(fd, &message, &received_fd);
      tmc_perf_record(&perf, TMC_PERF_RECEIVE_HEADER, timer,
                      read_status > 0 ? sizeof(message) : 0);
      if (read_status <= 0)
         break;
      if (message.magic != TMC_MAGIC || message.version != TMC_VERSION ||
          message.payload_size > TMC_MAX_PAYLOAD) {
         if (received_fd >= 0)
            close(received_fd);
         send_error(fd, "invalid bridge message header");
         break;
      }

      uint8_t *payload = NULL;
      if (message.payload_size) {
         payload = malloc(message.payload_size + 1u);
         timer = tmc_perf_begin(&perf);
         int payload_read = payload &&
            read_all(fd, payload, message.payload_size) > 0;
         tmc_perf_record(&perf, TMC_PERF_RECEIVE_PAYLOAD, timer,
                         payload_read ? message.payload_size : 0);
         if (!payload_read) {
            if (received_fd >= 0)
               close(received_fd);
            free(payload);
            break;
         }
         payload[message.payload_size] = 0;
      }

      if (message.type == TMC_CONFIG) {
         if (codec || !payload || !message.arg0 || !message.arg1) {
            if (received_fd >= 0)
               close(received_fd);
            free(payload);
            send_error(fd, "invalid or duplicate codec configuration");
            break;
         }
         codec = AMediaCodec_createCodecByName((const char *)payload);
         format = AMediaFormat_new();
         if (!codec || !format) {
            free(payload);
            send_error(fd, "failed to create MediaCodec");
            break;
         }
         AMediaFormat_setString(format, "mime", "video/avc");
         AMediaFormat_setInt32(format, "width", (int32_t)message.arg0);
         AMediaFormat_setInt32(format, "height", (int32_t)message.arg1);
         AMediaFormat_setInt32(format, "frame-rate", (int32_t)message.arg2);
         AMediaFormat_setInt32(format, "color-format", 21);
         AMediaFormat_setInt32(format, "low-latency", 1);
         /* Exynos Codec2 does not advertise Android's generic low-latency
          * feature, but its H.264 decoder exposes the RTC vendor control used
          * by low-latency Android streaming clients. */
         AMediaFormat_setInt32(
            format, "vendor.rtc-ext-dec-low-latency.enable", 1);
         AMediaFormat_setInt32(format, "max-input-size", 4 * 1024 * 1024);
         output_stride = message.arg0;
         output_slice_height = message.arg1;
         timer = tmc_perf_begin(&perf);
         media_status_t status =
            AMediaCodec_configure(codec, format, NULL, NULL, 0);
         if (status == AMEDIA_OK)
            status = AMediaCodec_start(codec);
         tmc_perf_record(&perf, TMC_PERF_CODEC_CONFIGURE_START, timer, 0);
         free(payload);
         if (received_fd >= 0)
            close(received_fd);
         if (status != AMEDIA_OK) {
            send_error(fd, "MediaCodec configure/start failed");
            break;
         }
         started = 1;
         if (!send_message(fd, TMC_READY, 0, 0, message.arg0, message.arg1,
                           message.arg2, TMC_CAP_SHARED_SURFACE, NULL, 0))
            break;
         continue;
      }

      if (!started) {
         if (received_fd >= 0)
            close(received_fd);
         free(payload);
         send_error(fd, "codec is not configured");
         break;
      }

      if (message.type == TMC_SURFACE) {
         free(payload);
         timer = tmc_perf_begin(&perf);
         int registered =
            register_shared_surface(surfaces, &message, received_fd,
                                    use_release_fences ? &fence_context : NULL,
                                    &perf);
         tmc_perf_record(&perf, TMC_PERF_SURFACE_REGISTER, timer,
                         registered ? message.arg2 : 0);
         if (!registered) {
            if (received_fd >= 0)
               close(received_fd);
            send_error(fd, "failed to register shared decode surface");
            break;
         }
         perf.surface_registrations++;
         continue;
      }

      if (received_fd >= 0)
         close(received_fd);

      if (message.type == TMC_CLOSE) {
         free(payload);
         result = 1;
         break;
      }

      if (message.type == TMC_PACKET) {
         perf.input_packets++;
         perf.input_bytes += message.payload_size;
         int ok = queue_input(codec, payload, message.payload_size,
                              message.pts_us, 0, &perf);
         free(payload);
         /* In remap mode, give a low-delay decoder a short chance to finish
          * the just-queued frame before acknowledging vaEndPicture. This
          * keeps Firefox from exporting the destination before the CPU write
          * completes. Streams that need future access units still time out
          * quickly and retain the existing deferred-export behavior. */
         int64_t output_wait_us = remap_latest_enabled() ? 10000 : 0;
         if (!ok || !drain_output(fd, codec, output_wait_us, 0, surfaces,
                                  &output_stride, &output_slice_height,
                                  &perf))
            break;
         timer = tmc_perf_begin(&perf);
         int acked = send_message(fd, TMC_ACK, message.pts_us, 0, 0, 0, 0, 0,
                                  NULL, 0);
         tmc_perf_record(&perf, TMC_PERF_ACK_SEND, timer, 0);
         if (!acked)
            break;
         continue;
      }

      if (message.type == TMC_DRAIN) {
         free(payload);
         if (!drain_output(fd, codec, 100000, 0, surfaces,
                           &output_stride, &output_slice_height, &perf) ||
             !send_message(fd, TMC_ACK, message.pts_us, 0, 0, 0, 0, 0,
                           NULL, 0))
            break;
         continue;
      }

      free(payload);
      if (message.type == TMC_INPUT_EOS) {
         if (!queue_input(codec, NULL, 0, message.pts_us,
                          AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM, &perf) ||
             !drain_output(fd, codec, 100000, 1, surfaces,
                           &output_stride, &output_slice_height, &perf))
            break;
         result = 1;
         break;
      }

      send_error(fd, "unknown bridge message type");
      break;
   }

finish:
   /* Wake a failed client before a vendor codec stop has a chance to block. */
   if (!result)
      shutdown(fd, SHUT_RDWR);
   if (started) {
      uint64_t timer = tmc_perf_begin(&perf);
      AMediaCodec_stop(codec);
      tmc_perf_record(&perf, TMC_PERF_CODEC_STOP, timer, 0);
   }
   if (codec)
      AMediaCodec_delete(codec);
   if (format)
      AMediaFormat_delete(format);
   release_shared_surfaces(surfaces, &perf);
   tmc_release_fence_context_destroy(&fence_context);
   tmc_perf_finish(&perf, result != 0);
   return result;
}

int
main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IOLBF, 0);
   const char *path = argc > 1 ? argv[1] : "/tmp/tensor-mediacodec.sock";
   int once = argc > 2 && strcmp(argv[2], "--once") == 0;
   if (!start_binder_thread_pool()) {
      fprintf(stderr, "failed to start NDK Binder thread pool\n");
      return 1;
   }

   int server = socket(AF_UNIX, SOCK_STREAM, 0);
   struct sockaddr_un address = {.sun_family = AF_UNIX};
   if (server < 0 || strlen(path) >= sizeof(address.sun_path)) {
      perror("socket");
      return 1;
   }
   strcpy(address.sun_path, path);
   unlink(path);
   if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0 ||
       chmod(path, 0600) < 0 || listen(server, 1) < 0) {
      perror(path);
      close(server);
      unlink(path);
      return 1;
   }

   signal(SIGPIPE, SIG_IGN);
   printf("listening on %s%s\n", path, once ? " (one client)" : "");
   fflush(stdout);
   int result = 0;
   do {
      int client = accept(server, NULL, NULL);
      if (client < 0) {
         if (errno == EINTR)
            continue;
         perror("accept");
         break;
      }
      result = serve_client(client);
      close(client);
      printf("client disconnected: %s\n", result ? "complete" : "error");
      fflush(stdout);
   } while (!once);
   close(server);
   unlink(path);
   return result ? 0 : 1;
}

#else

int
main(void)
{
   fputs("mediacodec-service must be built for Android\n", stderr);
   return 77;
}

#endif
