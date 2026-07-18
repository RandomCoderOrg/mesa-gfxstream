#include "bridge-protocol.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int
transfer(int fd, void *data, size_t size, int writing)
{
   uint8_t *cursor = data;
   while (size) {
      ssize_t count =
         writing ? send(fd, cursor, size, MSG_NOSIGNAL)
                 : read(fd, cursor, size);
      if (count == 0)
         return 0;
      if (count < 0) {
         if (errno == EINTR)
            continue;
         return 0;
      }
      cursor += count;
      size -= (size_t)count;
   }
   return 1;
}

static int
send_message(int fd, uint16_t type, int64_t pts_us, uint32_t arg0,
             uint32_t arg1, uint32_t arg2, const void *payload,
             uint32_t payload_size)
{
   struct tmc_message message = {
      .magic = TMC_MAGIC,
      .version = TMC_VERSION,
      .type = type,
      .payload_size = payload_size,
      .pts_us = pts_us,
      .arg0 = arg0,
      .arg1 = arg1,
      .arg2 = arg2,
   };
   return transfer(fd, &message, sizeof(message), 1) &&
          (!payload_size || transfer(fd, (void *)payload, payload_size, 1));
}

static int
receive_message(int fd, struct tmc_message *message, uint8_t **payload)
{
   *payload = NULL;
   if (!transfer(fd, message, sizeof(*message), 0) ||
       message->magic != TMC_MAGIC || message->version != TMC_VERSION ||
       message->payload_size > TMC_MAX_PAYLOAD)
      return 0;
   if (message->payload_size) {
      *payload = malloc(message->payload_size + 1u);
      if (!*payload || !transfer(fd, *payload, message->payload_size, 0)) {
         free(*payload);
         return 0;
      }
      (*payload)[message->payload_size] = 0;
   }
   if (message->type == TMC_ERROR) {
      fprintf(stderr, "service: %s\n", *payload ? (char *)*payload : "error");
      free(*payload);
      return 0;
   }
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
      if (header && (data[header] & 0x1f) == 9) {
         offsets[count++] = i;
         i = header;
      }
   }
   return count;
}

int
main(int argc, char **argv)
{
   if (argc != 6 && argc != 7) {
      fprintf(stderr, "usage: %s socket annex-b.h264 width height fps "
                      "[codec-component]\n", argv[0]);
      return 2;
   }
   const char *component = argc == 7 ? argv[6] : "c2.exynos.h264.decoder";
   uint32_t width = (uint32_t)strtoul(argv[3], NULL, 10);
   uint32_t height = (uint32_t)strtoul(argv[4], NULL, 10);
   uint32_t fps = (uint32_t)strtoul(argv[5], NULL, 10);

   size_t input_size = 0;
   uint8_t *input = read_file(argv[2], &input_size);
   if (!input || !width || !height || !fps) {
      fprintf(stderr, "invalid input or video dimensions\n");
      return 1;
   }

   size_t offsets[4096];
   unsigned unit_count = find_access_units(input, input_size, offsets, 4096);
   if (!unit_count) {
      fprintf(stderr, "no Annex-B access-unit delimiters found\n");
      free(input);
      return 1;
   }

   if (strlen(argv[1]) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
      fprintf(stderr, "socket path is too long\n");
      free(input);
      return 1;
   }

   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   struct sockaddr_un address = {.sun_family = AF_UNIX};
   strcpy(address.sun_path, argv[1]);
   if (fd < 0 ||
       connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
      perror(argv[1]);
      free(input);
      return 1;
   }

   if (!send_message(fd, TMC_CONFIG, 0, width, height, fps, component,
                     (uint32_t)strlen(component) + 1))
      return 1;
   struct tmc_message message;
   uint8_t *payload = NULL;
   if (!receive_message(fd, &message, &payload) || message.type != TMC_READY)
      return 1;
   free(payload);

   unsigned frames = 0;
   size_t bytes = 0;
   for (unsigned i = 0; i < unit_count; ++i) {
      size_t begin = offsets[i];
      size_t end = i + 1 < unit_count ? offsets[i + 1] : input_size;
      int64_t pts = (int64_t)i * 1000000 / fps;
      if (!send_message(fd, TMC_PACKET, pts, 0, 0, 0, input + begin,
                        (uint32_t)(end - begin)))
         return 1;
      do {
         if (!receive_message(fd, &message, &payload))
            return 1;
         if (message.type == TMC_FORMAT)
            printf("format=%ux%u stride=%u slice-height=%u\n", message.arg0,
                   message.arg1, message.arg2, message.arg3);
         if (message.type == TMC_FRAME) {
            frames++;
            bytes += message.payload_size;
         }
         free(payload);
      } while (message.type != TMC_ACK);
   }

   int64_t eos_pts = (int64_t)unit_count * 1000000 / fps;
   if (!send_message(fd, TMC_INPUT_EOS, eos_pts, 0, 0, 0, NULL, 0))
      return 1;
   do {
      if (!receive_message(fd, &message, &payload))
         return 1;
      if (message.type == TMC_FORMAT)
         printf("format=%ux%u stride=%u slice-height=%u\n", message.arg0,
                message.arg1, message.arg2, message.arg3);
      if (message.type == TMC_FRAME) {
         frames++;
         bytes += message.payload_size;
      }
      free(payload);
   } while (message.type != TMC_OUTPUT_EOS);

   printf("access-units=%u decoded-frames=%u decoded-bytes=%zu eos=1\n",
          unit_count, frames, bytes);
   close(fd);
   free(input);
   return frames == unit_count ? 0 : 1;
}
