#include <stdio.h>

#ifdef __ANDROID__

#include "bridge-protocol.h"

#include <dlfcn.h>
#include <errno.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

typedef bool (*binder_set_max_threads_fn)(uint32_t);
typedef void (*binder_start_thread_pool_fn)(void);

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
send_output_format(int fd, AMediaCodec *codec)
{
   AMediaFormat *format = AMediaCodec_getOutputFormat(codec);
   int32_t width = 0, height = 0, stride = 0, slice_height = 0;
   AMediaFormat_getInt32(format, "width", &width);
   AMediaFormat_getInt32(format, "height", &height);
   AMediaFormat_getInt32(format, "stride", &stride);
   AMediaFormat_getInt32(format, "slice-height", &slice_height);
   printf("output-format: %s\n", AMediaFormat_toString(format));
   int ok = send_message(fd, TMC_FORMAT, 0, 0, (uint32_t)width,
                         (uint32_t)height, (uint32_t)stride,
                         (uint32_t)slice_height, NULL, 0);
   AMediaFormat_delete(format);
   return ok;
}

static int
drain_output(int fd, AMediaCodec *codec, int64_t first_timeout_us,
             int wait_for_eos)
{
   unsigned empty_polls = 0;
   for (;;) {
      AMediaCodecBufferInfo info;
      ssize_t index = AMediaCodec_dequeueOutputBuffer(
         codec, &info, empty_polls ? 100000 : first_timeout_us);
      if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
         if (!send_output_format(fd, codec))
            return 0;
         continue;
      }
      if (index < 0) {
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

      if (size && !send_message(fd, TMC_FRAME, info.presentationTimeUs,
                                info.flags, 0, 0, 0, 0,
                                buffer + info.offset, size)) {
         AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false);
         return 0;
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
            int64_t pts_us, uint32_t flags)
{
   for (;;) {
      ssize_t index = AMediaCodec_dequeueInputBuffer(codec, 100000);
      if (index < 0)
         continue;
      size_t capacity = 0;
      uint8_t *buffer = AMediaCodec_getInputBuffer(codec, (size_t)index,
                                                   &capacity);
      if (!buffer || capacity < size)
         return 0;
      if (size)
         memcpy(buffer, payload, size);
      return AMediaCodec_queueInputBuffer(codec, (size_t)index, 0, size,
                                          (uint64_t)pts_us, flags) == AMEDIA_OK;
   }
}

static int
serve_client(int fd)
{
   AMediaCodec *codec = NULL;
   AMediaFormat *format = NULL;
   int started = 0;
   int result = 0;

   for (;;) {
      struct tmc_message message;
      int read_status = read_all(fd, &message, sizeof(message));
      if (read_status <= 0)
         break;
      if (message.magic != TMC_MAGIC || message.version != TMC_VERSION ||
          message.payload_size > TMC_MAX_PAYLOAD) {
         send_error(fd, "invalid bridge message header");
         break;
      }

      uint8_t *payload = NULL;
      if (message.payload_size) {
         payload = malloc(message.payload_size + 1u);
         if (!payload || read_all(fd, payload, message.payload_size) <= 0) {
            free(payload);
            break;
         }
         payload[message.payload_size] = 0;
      }

      if (message.type == TMC_CONFIG) {
         if (codec || !payload || !message.arg0 || !message.arg1) {
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
         AMediaFormat_setInt32(format, "max-input-size", 4 * 1024 * 1024);
         media_status_t status =
            AMediaCodec_configure(codec, format, NULL, NULL, 0);
         if (status == AMEDIA_OK)
            status = AMediaCodec_start(codec);
         free(payload);
         if (status != AMEDIA_OK) {
            send_error(fd, "MediaCodec configure/start failed");
            break;
         }
         started = 1;
         if (!send_message(fd, TMC_READY, 0, 0, message.arg0, message.arg1,
                           message.arg2, 0, NULL, 0))
            break;
         continue;
      }

      if (!started) {
         free(payload);
         send_error(fd, "codec is not configured");
         break;
      }

      if (message.type == TMC_PACKET) {
         int ok = queue_input(codec, payload, message.payload_size,
                              message.pts_us, 0);
         free(payload);
         if (!ok || !drain_output(fd, codec, 0, 0) ||
             !send_message(fd, TMC_ACK, message.pts_us, 0, 0, 0, 0, 0,
                           NULL, 0))
            break;
         continue;
      }

      if (message.type == TMC_DRAIN) {
         free(payload);
         if (!drain_output(fd, codec, 100000, 0) ||
             !send_message(fd, TMC_ACK, message.pts_us, 0, 0, 0, 0, 0,
                           NULL, 0))
            break;
         continue;
      }

      free(payload);
      if (message.type == TMC_INPUT_EOS) {
         if (!queue_input(codec, NULL, 0, message.pts_us,
                          AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) ||
             !drain_output(fd, codec, 100000, 1))
            break;
         result = 1;
         break;
      }

      send_error(fd, "unknown bridge message type");
      break;
   }

   /* Wake a failed client before a vendor codec stop has a chance to block. */
   if (!result)
      shutdown(fd, SHUT_RDWR);
   if (started)
      AMediaCodec_stop(codec);
   if (codec)
      AMediaCodec_delete(codec);
   if (format)
      AMediaFormat_delete(format);
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
