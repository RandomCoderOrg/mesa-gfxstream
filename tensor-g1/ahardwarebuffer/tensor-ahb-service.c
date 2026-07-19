/*
 * Native Android AHardwareBuffer broker for the glibc Panfrost driver.
 *
 * AHardwareBuffer objects cannot safely cross the Bionic/glibc boundary as
 * pointers.  This process owns them, gives Mesa a duplicate of their data
 * DMA-BUF, and later sends the complete gralloc handle to Termux:X11 through
 * its custom DRI3 socket-fd modifier.
 */

#include <android/hardware_buffer.h>

#include "tensor_ahb_protocol.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define TENSOR_AHB_MAX_BUFFERS 16
#define TENSOR_AHB_PADDING_ROWS 8

struct tensor_native_handle {
   int version;
   int num_fds;
   int num_ints;
   int data[];
};

typedef const struct tensor_native_handle *
(*get_native_handle_fn)(const AHardwareBuffer *buffer);

struct tensor_ahb_buffer {
   bool used;
   uint32_t id;
   AHardwareBuffer *buffer;
   uint32_t width;
   uint32_t height;
   uint32_t format;
   uint32_t stride;
   uint32_t data_size;
};

struct present_worker {
   AHardwareBuffer *buffer;
   int socket_fd;
};

static struct tensor_ahb_buffer buffers[TENSOR_AHB_MAX_BUFFERS];
static pthread_mutex_t buffers_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t next_id = 1;
static get_native_handle_fn get_native_handle;

static int
send_message(int socket_fd, const struct tensor_ahb_message *message,
             int sent_fd)
{
   struct iovec iov = {
      .iov_base = (void *)message,
      .iov_len = sizeof(*message),
   };
   char control[CMSG_SPACE(sizeof(int))] = {0};
   struct msghdr header = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
   };

   if (sent_fd >= 0) {
      header.msg_control = control;
      header.msg_controllen = sizeof(control);
      struct cmsghdr *cmsg = CMSG_FIRSTHDR(&header);
      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_type = SCM_RIGHTS;
      cmsg->cmsg_len = CMSG_LEN(sizeof(int));
      memcpy(CMSG_DATA(cmsg), &sent_fd, sizeof(sent_fd));
   }

   ssize_t result;
   do {
      result = sendmsg(socket_fd, &header, MSG_NOSIGNAL);
   } while (result < 0 && errno == EINTR);
   return result == (ssize_t)sizeof(*message) ? 0 : -1;
}

static int
receive_message(int socket_fd, struct tensor_ahb_message *message,
                int *received_fd)
{
   struct iovec iov = {
      .iov_base = message,
      .iov_len = sizeof(*message),
   };
   char control[CMSG_SPACE(sizeof(int))] = {0};
   struct msghdr header = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = control,
      .msg_controllen = sizeof(control),
   };

   *received_fd = -1;
   ssize_t result;
   do {
      result = recvmsg(socket_fd, &header, MSG_CMSG_CLOEXEC);
   } while (result < 0 && errno == EINTR);
   if (result != (ssize_t)sizeof(*message))
      return -1;

   for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&header); cmsg;
        cmsg = CMSG_NXTHDR(&header, cmsg)) {
      if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
          cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
         memcpy(received_fd, CMSG_DATA(cmsg), sizeof(*received_fd));
         break;
      }
   }
   return 0;
}

static struct tensor_ahb_buffer *
find_buffer(uint32_t id)
{
   for (unsigned i = 0; i < TENSOR_AHB_MAX_BUFFERS; i++)
      if (buffers[i].used && buffers[i].id == id)
         return &buffers[i];
   return NULL;
}

static void *
present_main(void *data)
{
   struct present_worker *worker = data;
   uint8_t request;
   ssize_t result;

   do {
      result = read(worker->socket_fd, &request, sizeof(request));
   } while (result < 0 && errno == EINTR);
   if (result == 1) {
      int status = AHardwareBuffer_sendHandleToUnixSocket(worker->buffer,
                                                           worker->socket_fd);
      if (status)
         fprintf(stderr, "tensor-ahb: handle send failed: %d\n", status);
   }

   close(worker->socket_fd);
   AHardwareBuffer_release(worker->buffer);
   free(worker);
   return NULL;
}

static int
handle_allocate(const struct tensor_ahb_message *request,
                struct tensor_ahb_message *response, int *response_fd)
{
   if (!request->width || !request->height || request->width > 8192 ||
       request->height > 8192 - TENSOR_AHB_PADDING_ROWS)
      return EINVAL;

   uint32_t format = request->format ? request->format : 5;
   pthread_mutex_lock(&buffers_mutex);
   struct tensor_ahb_buffer *slot = NULL;
   for (unsigned i = 0; i < TENSOR_AHB_MAX_BUFFERS; i++) {
      if (!buffers[i].used && buffers[i].buffer &&
          buffers[i].width == request->width &&
          buffers[i].height == request->height &&
          buffers[i].format == format) {
         slot = &buffers[i];
         break;
      }
   }
   if (!slot) {
      for (unsigned i = 0; i < TENSOR_AHB_MAX_BUFFERS; i++) {
         if (!buffers[i].used && !buffers[i].buffer) {
            slot = &buffers[i];
            break;
         }
      }
   }
   if (!slot) {
      pthread_mutex_unlock(&buffers_mutex);
      return ENOSPC;
   }

   bool recycled = slot->buffer != NULL;
   if (recycled) {
      slot->used = true;
      slot->id = next_id++;
      if (!next_id)
         next_id = 1;
   }
   pthread_mutex_unlock(&buffers_mutex);

   if (recycled) {
      const struct tensor_native_handle *handle =
         get_native_handle(slot->buffer);
      if (!handle || handle->num_fds < 1)
         return ENODEV;
      response->id = slot->id;
      response->width = slot->width;
      response->height = slot->height;
      response->format = slot->format;
      response->stride = slot->stride;
      response->data_size = slot->data_size;
      *response_fd = handle->data[0];
      fprintf(stderr,
              "tensor-ahb: recycled id=%u %ux%u format=%u stride=%u size=%u\n",
              response->id, response->width, response->height,
              response->format, response->stride, response->data_size);
      return 0;
   }

   AHardwareBuffer_Desc description = {
      .width = request->width,
      /* Panfrost's linear render-target layout includes several pages of
       * tail padding beyond the visible rows. Gralloc normally allocates
       * only one extra page, so keep hidden rows in the physical AHB while
       * reporting the caller's logical height through the broker protocol. */
      .height = request->height + TENSOR_AHB_PADDING_ROWS,
      .layers = 1,
      .format = format,
      .usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
               AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
               AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
               AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER,
   };
   AHardwareBuffer *buffer = NULL;
   int status = AHardwareBuffer_allocate(&description, &buffer);
   if (status || !buffer)
      return status ? status : ENOMEM;

   AHardwareBuffer_describe(buffer, &description);
   /* Gralloc allocations are lazy on Tensor.  Fault and initialise the
    * complete linear pixel range before Kbase imports the data DMA-BUF;
    * otherwise the first large render target can be terminated by Kbase. */
   void *pixels = NULL;
   status = AHardwareBuffer_lock(buffer,
                                 AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                                 -1, NULL, &pixels);
   if (status || !pixels) {
      AHardwareBuffer_release(buffer);
      return status ? status : EFAULT;
   }
   memset(pixels, 0, (size_t)description.stride * description.height * 4);
   status = AHardwareBuffer_unlock(buffer, NULL);
   if (status) {
      AHardwareBuffer_release(buffer);
      return status;
   }

   const struct tensor_native_handle *handle = get_native_handle(buffer);
   if (!handle || handle->num_fds < 1) {
      AHardwareBuffer_release(buffer);
      return ENODEV;
   }

   struct stat file_status;
   if (fstat(handle->data[0], &file_status) < 0 || file_status.st_size <= 0 ||
       (uint64_t)file_status.st_size > UINT32_MAX) {
      status = errno ? errno : EOVERFLOW;
      AHardwareBuffer_release(buffer);
      return status;
   }

   pthread_mutex_lock(&buffers_mutex);
   slot->used = true;
   slot->id = next_id++;
   if (!next_id)
      next_id = 1;
   slot->buffer = buffer;
   slot->width = description.width;
   slot->height = request->height;
   slot->format = description.format;
   slot->stride = description.stride;
   slot->data_size = (uint32_t)file_status.st_size;
   response->id = slot->id;
   pthread_mutex_unlock(&buffers_mutex);

   response->width = description.width;
   response->height = request->height;
   response->format = description.format;
   response->stride = description.stride;
   response->data_size = (uint32_t)file_status.st_size;
   *response_fd = handle->data[0];
   fprintf(stderr,
           "tensor-ahb: allocated id=%u logical=%ux%u physical=%ux%u "
           "format=%u stride=%u size=%u\n",
           response->id, response->width, response->height,
           description.width, description.height, response->format,
           response->stride, response->data_size);
   return 0;
}

static int
handle_present(const struct tensor_ahb_message *request, int received_fd)
{
   if (received_fd < 0)
      return EBADF;

   struct present_worker *worker = calloc(1, sizeof(*worker));
   if (!worker)
      return ENOMEM;

   pthread_mutex_lock(&buffers_mutex);
   struct tensor_ahb_buffer *entry = find_buffer(request->id);
   if (entry) {
      worker->buffer = entry->buffer;
      AHardwareBuffer_acquire(worker->buffer);
   }
   pthread_mutex_unlock(&buffers_mutex);
   if (!worker->buffer) {
      free(worker);
      return ENOENT;
   }

   worker->socket_fd = received_fd;
   pthread_t thread;
   int status = pthread_create(&thread, NULL, present_main, worker);
   if (status) {
      AHardwareBuffer_release(worker->buffer);
      free(worker);
      return status;
   }
   pthread_detach(thread);
   return 0;
}

static int
handle_release(const struct tensor_ahb_message *request)
{
   pthread_mutex_lock(&buffers_mutex);
   struct tensor_ahb_buffer *entry = find_buffer(request->id);
   if (entry) {
      entry->used = false;
      entry->id = 0;
   }
   pthread_mutex_unlock(&buffers_mutex);

   if (!entry)
      return ENOENT;
   fprintf(stderr, "tensor-ahb: pooled id=%u\n", request->id);
   return 0;
}

static void
serve_client(int client)
{
   struct tensor_ahb_message request;
   int received_fd = -1;
   if (receive_message(client, &request, &received_fd) < 0)
      return;

   struct tensor_ahb_message response = {
      .magic = TENSOR_AHB_MAGIC,
      .version = TENSOR_AHB_VERSION,
      .type = TENSOR_AHB_RESPONSE,
   };
   int response_fd = -1;
   if (request.magic != TENSOR_AHB_MAGIC ||
       request.version != TENSOR_AHB_VERSION) {
      response.status = EPROTO;
   } else {
      switch (request.type) {
      case TENSOR_AHB_ALLOCATE:
         response.status = handle_allocate(&request, &response, &response_fd);
         break;
      case TENSOR_AHB_PRESENT:
         response.status = handle_present(&request, received_fd);
         if (!response.status)
            received_fd = -1;
         break;
      case TENSOR_AHB_RELEASE:
         response.status = handle_release(&request);
         break;
      case TENSOR_AHB_PING:
         break;
      default:
         response.status = EINVAL;
         break;
      }
   }

   send_message(client, &response, response.status ? -1 : response_fd);
   if (received_fd >= 0)
      close(received_fd);
}

int
main(int argc, char **argv)
{
   const char *socket_path = argc > 1 ? argv[1] : TENSOR_AHB_SOCKET_DEFAULT;
   if (strlen(socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
      fprintf(stderr, "tensor-ahb: socket path is too long\n");
      return 1;
   }

   void *system_android = dlopen("/system/lib64/libandroid.so",
                                 RTLD_NOW | RTLD_LOCAL);
   if (!system_android) {
      fprintf(stderr, "tensor-ahb: cannot load system libandroid: %s\n",
              dlerror());
      return 1;
   }
   get_native_handle = (get_native_handle_fn)dlsym(
      system_android, "AHardwareBuffer_getNativeHandle");
   if (!get_native_handle) {
      fprintf(stderr, "tensor-ahb: getNativeHandle is unavailable: %s\n",
              dlerror());
      return 1;
   }

   signal(SIGPIPE, SIG_IGN);
   int server = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
   if (server < 0) {
      perror("tensor-ahb: socket");
      return 1;
   }

   struct sockaddr_un address = {.sun_family = AF_UNIX};
   strcpy(address.sun_path, socket_path);
   unlink(socket_path);
   if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0 ||
       listen(server, 16) < 0) {
      fprintf(stderr, "tensor-ahb: listen %s failed: %s\n", socket_path,
              strerror(errno));
      close(server);
      return 1;
   }
   chmod(socket_path, 0600);
   fprintf(stderr, "tensor-ahb: listening on %s\n", socket_path);

   for (;;) {
      int client;
      do {
         client = accept4(server, NULL, NULL, SOCK_CLOEXEC);
      } while (client < 0 && errno == EINTR);
      if (client < 0) {
         fprintf(stderr, "tensor-ahb: accept failed: %s\n", strerror(errno));
         break;
      }
      serve_client(client);
      close(client);
   }

   close(server);
   unlink(socket_path);
   return 1;
}
