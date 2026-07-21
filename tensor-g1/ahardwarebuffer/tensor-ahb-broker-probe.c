/*
 * Cheap lifecycle probe for the Tensor AHardwareBuffer broker.
 *
 * It deliberately abandons one Present handoff and verifies that the broker
 * closes its copy of the socket after the bounded wait instead of leaking a
 * detached worker forever.
 */

#include "tensor_ahb_protocol.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define PROBE_TIMEOUT_MS 7000

static int
connect_broker(const char *socket_path)
{
   int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
   if (fd < 0)
      return -1;

   struct sockaddr_un address = {.sun_family = AF_UNIX};
   if (strlen(socket_path) >= sizeof(address.sun_path)) {
      close(fd);
      errno = ENAMETOOLONG;
      return -1;
   }
   strcpy(address.sun_path, socket_path);
   if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
      close(fd);
      return -1;
   }
   return fd;
}

static int
send_request(const char *socket_path, struct tensor_ahb_message *message,
             int sent_fd, int *received_fd)
{
   int client = connect_broker(socket_path);
   if (client < 0)
      return -1;

   struct iovec iov = {.iov_base = message, .iov_len = sizeof(*message)};
   char control[CMSG_SPACE(sizeof(int))] = {0};
   struct msghdr header = {.msg_iov = &iov, .msg_iovlen = 1};
   if (sent_fd >= 0) {
      header.msg_control = control;
      header.msg_controllen = sizeof(control);
      struct cmsghdr *cmsg = CMSG_FIRSTHDR(&header);
      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_type = SCM_RIGHTS;
      cmsg->cmsg_len = CMSG_LEN(sizeof(int));
      memcpy(CMSG_DATA(cmsg), &sent_fd, sizeof(sent_fd));
   }
   if (sendmsg(client, &header, MSG_NOSIGNAL) != (ssize_t)sizeof(*message)) {
      close(client);
      return -1;
   }

   memset(control, 0, sizeof(control));
   header.msg_control = control;
   header.msg_controllen = sizeof(control);
   ssize_t result = recvmsg(client, &header, MSG_CMSG_CLOEXEC);
   close(client);
   if (result != (ssize_t)sizeof(*message) ||
       message->magic != TENSOR_AHB_MAGIC ||
       message->type != TENSOR_AHB_RESPONSE || message->status) {
      errno = message->status ? message->status : EPROTO;
      return -1;
   }

   if (received_fd) {
      *received_fd = -1;
      for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&header); cmsg;
           cmsg = CMSG_NXTHDR(&header, cmsg)) {
         if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            memcpy(received_fd, CMSG_DATA(cmsg), sizeof(*received_fd));
            break;
         }
      }
   }
   return 0;
}

static double
elapsed_ms(const struct timespec *start, const struct timespec *end)
{
   return (end->tv_sec - start->tv_sec) * 1000.0 +
          (end->tv_nsec - start->tv_nsec) / 1000000.0;
}

int
main(int argc, char **argv)
{
   const char *socket_path = argc > 1 ? argv[1] : TENSOR_AHB_SOCKET_DEFAULT;
   bool abandon = argc > 2 && !strcmp(argv[2], "--abandon");
   bool allocate_release = argc > 2 && !strcmp(argv[2], "--allocate-release");
   uint32_t width = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 64;
   uint32_t height = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) : 64;
   struct tensor_ahb_message message = {
      .magic = TENSOR_AHB_MAGIC,
      .version = TENSOR_AHB_VERSION,
      .type = TENSOR_AHB_ALLOCATE,
      .width = width,
      .height = height,
      .format = 5,
   };
   int dma_fd = -1;
   if (send_request(socket_path, &message, -1, &dma_fd) < 0 || dma_fd < 0) {
      fprintf(stderr, "allocate failed: %s\n", strerror(errno));
      return 1;
   }
   uint32_t id = message.id;
   close(dma_fd);

   if (abandon) {
      printf("abandoned-allocation: id=%u size=%ux%u\n", id, width, height);
      return 0;
   }

   if (allocate_release) {
      message = (struct tensor_ahb_message){
         .magic = TENSOR_AHB_MAGIC,
         .version = TENSOR_AHB_VERSION,
         .type = TENSOR_AHB_RELEASE,
         .id = id,
      };
      if (send_request(socket_path, &message, -1, NULL) < 0) {
         fprintf(stderr, "release failed: %s\n", strerror(errno));
         return 1;
      }
      printf("allocate-release: PASS id=%u size=%ux%u\n", id, width, height);
      return 0;
   }

   int present[2];
   if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, present) < 0) {
      perror("socketpair");
      return 1;
   }
   message = (struct tensor_ahb_message){
      .magic = TENSOR_AHB_MAGIC,
      .version = TENSOR_AHB_VERSION,
      .type = TENSOR_AHB_PRESENT,
      .id = id,
   };
   if (send_request(socket_path, &message, present[0], NULL) < 0) {
      fprintf(stderr, "present failed: %s\n", strerror(errno));
      return 1;
   }
   close(present[0]);

   struct timespec start, end;
   clock_gettime(CLOCK_MONOTONIC, &start);
   struct pollfd poll_fd = {.fd = present[1], .events = POLLIN};
   int poll_result;
   do {
      poll_result = poll(&poll_fd, 1, PROBE_TIMEOUT_MS);
   } while (poll_result < 0 && errno == EINTR);
   clock_gettime(CLOCK_MONOTONIC, &end);
   close(present[1]);

   message = (struct tensor_ahb_message){
      .magic = TENSOR_AHB_MAGIC,
      .version = TENSOR_AHB_VERSION,
      .type = TENSOR_AHB_RELEASE,
      .id = id,
   };
   int release_result = send_request(socket_path, &message, -1, NULL);

   double wait_ms = elapsed_ms(&start, &end);
   if (poll_result <= 0 || !(poll_fd.revents & POLLHUP) || release_result < 0) {
      fprintf(stderr,
              "timeout cleanup failed: poll=%d revents=0x%x wait_ms=%.1f "
              "release=%d\n",
              poll_result, poll_fd.revents, wait_ms, release_result);
      return 2;
   }

   printf("present-timeout-cleanup: PASS id=%u wait_ms=%.1f revents=0x%x\n",
          id, wait_ms, poll_fd.revents);
   return 0;
}
