#define _GNU_SOURCE

/*
 * Browser-free reproduction of the VA/Firefox decode-surface lifecycle.
 *
 * For each H.264 access unit this client registers a DMA-BUF destination,
 * submits the packet, and measures whether the matching shared-frame event is
 * received before the packet ACK.  That is the important ordering at the
 * vaEndPicture/export boundary, without starting Firefox or an X server.
 */

#include "bridge-protocol.h"
#ifdef TMC_EGL_CONSUMER
#include "egl-nv12-consumer.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_ACCESS_UNITS 4096
#define DEFAULT_SURFACE_POOL 8

/* Keep the tiny stable DMA-heap UAPI local so the probe also builds against
 * older Jammy linux-libc-dev packages. */
struct dma_heap_allocation_data {
   uint64_t len;
   uint32_t fd;
   uint32_t fd_flags;
   uint64_t heap_flags;
};

struct dma_buf_sync {
   uint64_t flags;
};

#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC \
   _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)
#define DMA_BUF_BASE 'b'
#define DMA_BUF_IOCTL_SYNC _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)
#define DMA_BUF_SYNC_READ (1ULL << 0)
#define DMA_BUF_SYNC_START (0ULL << 2)
#define DMA_BUF_SYNC_END (1ULL << 2)

struct surface_slot {
   int fd;
   size_t size;
#ifdef TMC_EGL_CONSUMER
   void *mapping;
   struct tmc_egl_surface *egl_surface;
#endif
};

#ifdef TMC_EGL_CONSUMER
static int
environment_enabled(const char *name)
{
   const char *value = getenv(name);
   return value && value[0] && strcmp(value, "0") != 0;
}

struct egl_sample_result {
   uint32_t frame_index;
   uint32_t ok;
   uint32_t unready_after_sample;
   uint8_t pixel[4];
   uint64_t elapsed_ns;
};

struct egl_worker_ready {
   uint32_t ok;
   char renderer[128];
};

struct egl_worker {
   pid_t pid;
   int task_fd;
   int result_fd;
   char renderer[128];
};
#endif

static uint64_t
monotonic_ns(void)
{
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
      return 0;
   return (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
}

static int
dmabuf_read_ready(int fd)
{
   struct pollfd item = {.fd = fd, .events = POLLIN};
   int result;
   do {
      result = poll(&item, 1, 0);
   } while (result < 0 && errno == EINTR);
   return result > 0 && (item.revents & POLLIN) != 0;
}

#ifdef TMC_EGL_CONSUMER
static int
dmabuf_cpu_sync(int fd, uint64_t flags)
{
   struct dma_buf_sync sync = {.flags = flags};
   int result;
   do {
      result = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
   } while (result < 0 && (errno == EINTR || errno == EAGAIN));
   return result == 0;
}
#endif

static int
transfer(int fd, void *data, size_t size, int writing)
{
   uint8_t *cursor = data;
   while (size) {
      ssize_t count = writing ? send(fd, cursor, size, MSG_NOSIGNAL)
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

#ifdef TMC_EGL_CONSUMER
static int
pipe_transfer(int fd, void *data, size_t size, int writing)
{
   uint8_t *cursor = data;
   while (size) {
      ssize_t count = writing ? write(fd, cursor, size)
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
#endif

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
send_surface(int socket_fd, int64_t pts_us, uint32_t stride,
             uint32_t slice_height, const struct surface_slot *surface)
{
   struct tmc_message message = {
      .magic = TMC_MAGIC,
      .version = TMC_VERSION,
      .type = TMC_SURFACE,
      .pts_us = pts_us,
      .arg0 = stride,
      .arg1 = slice_height,
      .arg2 = (uint32_t)surface->size,
   };
   struct iovec iov = {.iov_base = &message, .iov_len = sizeof(message)};
   char control[CMSG_SPACE(sizeof(int))] = {0};
   struct msghdr msg = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = control,
      .msg_controllen = sizeof(control),
   };
   struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
   cmsg->cmsg_level = SOL_SOCKET;
   cmsg->cmsg_type = SCM_RIGHTS;
   cmsg->cmsg_len = CMSG_LEN(sizeof(int));
   memcpy(CMSG_DATA(cmsg), &surface->fd, sizeof(surface->fd));
   ssize_t sent;
   do {
      sent = sendmsg(socket_fd, &msg, MSG_NOSIGNAL);
   } while (sent < 0 && errno == EINTR);
   return sent == (ssize_t)sizeof(message);
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
         *payload = NULL;
         return 0;
      }
      (*payload)[message->payload_size] = 0;
   }
   if (message->type == TMC_ERROR) {
      fprintf(stderr, "service: %s\n", *payload ? (char *)*payload : "error");
      free(*payload);
      *payload = NULL;
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
   if (fseek(file, 0, SEEK_END)) {
      fclose(file);
      return NULL;
   }
   long length = ftell(file);
   if (length < 0 || fseek(file, 0, SEEK_SET)) {
      fclose(file);
      return NULL;
   }
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

static int
allocate_surface(struct surface_slot *surface, size_t size)
{
   const char *heap_name = getenv("TENSOR_VA_DMA_HEAP");
   if (!heap_name || !heap_name[0])
      heap_name = "system";
   char path[128];
   if (snprintf(path, sizeof(path), "/dev/dma_heap/%s", heap_name) >=
       (int)sizeof(path))
      return 0;
   int heap = open(path, O_RDONLY | O_CLOEXEC);
   if (heap < 0)
      return 0;
   struct dma_heap_allocation_data allocation = {
      .len = size,
      .fd_flags = O_RDWR | O_CLOEXEC,
   };
   int result = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &allocation);
   close(heap);
   if (result < 0)
      return 0;
   surface->fd = (int)allocation.fd;
   surface->size = size;
   return 1;
}

static int
compare_u64(const void *left, const void *right)
{
   uint64_t a = *(const uint64_t *)left;
   uint64_t b = *(const uint64_t *)right;
   return (a > b) - (a < b);
}

static uint64_t
percentile(uint64_t *values, unsigned count, unsigned percent)
{
   if (!count)
      return 0;
   qsort(values, count, sizeof(*values), compare_u64);
   unsigned index = (percent * count + 99) / 100;
   if (index)
      index--;
   return values[index < count ? index : count - 1];
}

#ifdef TMC_EGL_CONSUMER
static int
sample_egl_surface(struct tmc_egl_consumer *consumer,
                   struct surface_slot *slot, unsigned frame_index,
                   uint64_t *sample_times, unsigned *sample_count,
                   uint8_t pixel[4],
                   unsigned *unready_after_sample)
{
   uint64_t started = monotonic_ns();
   if (!tmc_egl_surface_sample(consumer, slot->egl_surface, pixel)) {
      fprintf(stderr, "Panfrost sample failed at frame %u\n", frame_index);
      return 0;
   }
   sample_times[(*sample_count)++] = monotonic_ns() - started;
   *unready_after_sample += !dmabuf_read_ready(slot->fd);
   return 1;
}

static void
run_egl_worker(int task_fd, int result_fd, struct surface_slot *surfaces,
               unsigned pool_size, uint32_t width, uint32_t height,
               uint32_t stride, uint32_t slice_height)
{
   struct egl_worker_ready ready = {0};
   struct tmc_egl_consumer *consumer = tmc_egl_consumer_create();
   if (consumer) {
      ready.ok = 1;
      snprintf(ready.renderer, sizeof(ready.renderer), "%s",
               tmc_egl_consumer_renderer(consumer));
      for (unsigned i = 0; i < pool_size; i++) {
         surfaces[i].egl_surface = tmc_egl_surface_create(
            consumer, surfaces[i].fd, width, height, stride, slice_height);
         if (!surfaces[i].egl_surface) {
            ready.ok = 0;
            break;
         }
      }
   }
   if (!pipe_transfer(result_fd, &ready, sizeof(ready), 1) || !ready.ok)
      goto out;

   uint32_t frame_index;
   while (pipe_transfer(task_fd, &frame_index, sizeof(frame_index), 0)) {
      struct egl_sample_result result = {.frame_index = frame_index};
      if (frame_index >= pool_size)
         goto send_result;
      unsigned sample_count = 0;
      unsigned unready = 0;
      uint64_t elapsed = 0;
      result.ok = sample_egl_surface(
         consumer, &surfaces[frame_index], frame_index, &elapsed,
         &sample_count, result.pixel, &unready);
      result.unready_after_sample = unready;
      result.elapsed_ns = elapsed;
send_result:
      if (!pipe_transfer(result_fd, &result, sizeof(result), 1))
         break;
   }

out:
   if (consumer) {
      for (unsigned i = 0; i < pool_size; i++)
         tmc_egl_surface_destroy(consumer, surfaces[i].egl_surface);
      tmc_egl_consumer_destroy(consumer);
   }
   close(task_fd);
   close(result_fd);
}

static int
start_egl_worker(struct egl_worker *worker, struct surface_slot *surfaces,
                 unsigned pool_size, uint32_t width, uint32_t height,
                 uint32_t stride, uint32_t slice_height)
{
   int tasks[2] = {-1, -1};
   int results[2] = {-1, -1};
   if (pipe(tasks) || pipe(results))
      goto fail;
   pid_t pid = fork();
   if (pid < 0)
      goto fail;
   if (pid == 0) {
      close(tasks[1]);
      close(results[0]);
      run_egl_worker(tasks[0], results[1], surfaces, pool_size, width, height,
                     stride, slice_height);
      _exit(0);
   }
   close(tasks[0]);
   close(results[1]);
   worker->pid = pid;
   worker->task_fd = tasks[1];
   worker->result_fd = results[0];
   struct egl_worker_ready ready;
   if (!pipe_transfer(worker->result_fd, &ready, sizeof(ready), 0) ||
       !ready.ok)
      goto fail_worker;
   snprintf(worker->renderer, sizeof(worker->renderer), "%s",
            ready.renderer);
   return 1;

fail_worker:
   close(worker->task_fd);
   close(worker->result_fd);
   waitpid(worker->pid, NULL, 0);
   worker->pid = -1;
   worker->task_fd = -1;
   worker->result_fd = -1;
   return 0;
fail:
   if (tasks[0] >= 0)
      close(tasks[0]);
   if (tasks[1] >= 0)
      close(tasks[1]);
   if (results[0] >= 0)
      close(results[0]);
   if (results[1] >= 0)
      close(results[1]);
   return 0;
}

static void
stop_egl_worker(struct egl_worker *worker, int terminate)
{
   if (worker->pid < 0)
      return;
   if (worker->task_fd >= 0)
      close(worker->task_fd);
   if (terminate)
      kill(worker->pid, SIGTERM);
   waitpid(worker->pid, NULL, 0);
   if (worker->result_fd >= 0)
      close(worker->result_fd);
   worker->pid = -1;
   worker->task_fd = -1;
   worker->result_fd = -1;
}
#endif

int
main(int argc, char **argv)
{
   if (argc < 6 || argc > 8) {
      fprintf(stderr, "usage: %s socket annex-b.h264 width height fps "
                      "[surface-pool] [codec-component]\n", argv[0]);
      return 2;
   }
   uint32_t width = (uint32_t)strtoul(argv[3], NULL, 10);
   uint32_t height = (uint32_t)strtoul(argv[4], NULL, 10);
   uint32_t fps = (uint32_t)strtoul(argv[5], NULL, 10);
   unsigned pool_size = argc >= 7 ? (unsigned)strtoul(argv[6], NULL, 10)
                                  : DEFAULT_SURFACE_POOL;
   const char *component = argc == 8 ? argv[7] : "c2.exynos.h264.decoder";
   if (!width || width > UINT32_MAX - 63u || !height ||
       height > UINT32_MAX - 15u || !fps || !pool_size || pool_size > 64) {
      fprintf(stderr, "invalid dimensions, frame rate, or surface pool\n");
      return 2;
   }

   size_t input_size = 0;
   uint8_t *input = read_file(argv[2], &input_size);
   size_t offsets[MAX_ACCESS_UNITS];
   unsigned unit_count = input ? find_access_units(
      input, input_size, offsets, MAX_ACCESS_UNITS) : 0;
   if (!unit_count) {
      fprintf(stderr, "input has no Annex-B access-unit delimiters\n");
      free(input);
      return 1;
   }

   uint32_t stride = (width + 63u) & ~63u;
   uint32_t slice_height = (height + 15u) & ~15u;
   size_t pixels = (size_t)stride * slice_height;
   size_t allocation_size = pixels + pixels / 2;
   long page_size = sysconf(_SC_PAGESIZE);
   if (page_size <= 0)
      page_size = 4096;
   allocation_size = (allocation_size + (size_t)page_size - 1) &
                     ~((size_t)page_size - 1);
   struct surface_slot *surfaces = calloc(pool_size, sizeof(*surfaces));
   if (!surfaces) {
      free(input);
      return 1;
   }
#ifdef TMC_EGL_CONSUMER
   int egl_enabled = environment_enabled("TENSOR_EGL_CONSUMER");
   struct egl_worker egl_worker = {
      .pid = -1,
      .task_fd = -1,
      .result_fd = -1,
   };
#else
   int egl_enabled = 0;
#endif
   for (unsigned i = 0; i < pool_size; i++) {
      surfaces[i].fd = -1;
#ifdef TMC_EGL_CONSUMER
      surfaces[i].mapping = MAP_FAILED;
#endif
      if (!allocate_surface(&surfaces[i], allocation_size)) {
         perror("DMA-heap surface allocation");
         pool_size = i;
         goto fail;
      }
#ifdef TMC_EGL_CONSUMER
      if (egl_enabled) {
         surfaces[i].mapping = mmap(NULL, allocation_size, PROT_READ,
                                    MAP_SHARED, surfaces[i].fd, 0);
         if (surfaces[i].mapping == MAP_FAILED) {
            fprintf(stderr, "failed to map EGL surface %u\n", i);
            pool_size = i + 1;
            goto fail;
         }
      }
#endif
   }
#ifdef TMC_EGL_CONSUMER
   if (egl_enabled && pool_size < unit_count) {
      fprintf(stderr, "EGL worker needs one surface per access unit "
                      "(%u surfaces for this smoke test)\n", unit_count);
      goto fail;
   }
   if (egl_enabled &&
       !start_egl_worker(&egl_worker, surfaces, pool_size, width, height,
                         stride, slice_height)) {
      fprintf(stderr, "failed to start surfaceless Panfrost worker\n");
      goto fail;
   }
#endif

   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   struct sockaddr_un address = {.sun_family = AF_UNIX};
   if (fd < 0)
      goto fail;
   if (strlen(argv[1]) >= sizeof(address.sun_path)) {
      close(fd);
      goto fail;
   }
   strcpy(address.sun_path, argv[1]);
   if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
      perror(argv[1]);
      goto fail;
   }
   if (!send_message(fd, TMC_CONFIG, 0, width, height, fps, component,
                     (uint32_t)strlen(component) + 1))
      goto fail_connected;
   struct tmc_message message;
   uint8_t *payload = NULL;
   if (!receive_message(fd, &message, &payload) || message.type != TMC_READY ||
       !(message.arg3 & TMC_CAP_SHARED_SURFACE)) {
      free(payload);
      fprintf(stderr, "service does not advertise shared surfaces\n");
      goto fail_connected;
   }
   free(payload);
   payload = NULL;

   uint64_t *ack_ns = calloc(unit_count, sizeof(*ack_ns));
   if (!ack_ns)
      goto fail_connected;
   uint64_t *gpu_sample_ns = egl_enabled ?
      calloc(unit_count, sizeof(*gpu_sample_ns)) : NULL;
   if (egl_enabled && !gpu_sample_ns)
      goto fail_run;
   uint64_t run_started = monotonic_ns();
   unsigned frames = 0, shared_frames = 0, ready_at_ack = 0;
   unsigned frames_before_ack = 0, nonmatching_frames_before_ack = 0;
   unsigned first_output_after_inputs = 0, raw_frames = 0;
   unsigned fenced_at_ack = 0, unsafe_at_ack = 0;
   unsigned gpu_samples = 0, gpu_pixel_mismatches = 0;
   unsigned gpu_unready_after_sample = 0;
   for (unsigned i = 0; i < unit_count; i++) {
      int64_t pts = (int64_t)i * 1000000 / fps;
      size_t begin = offsets[i];
      size_t end = i + 1 < unit_count ? offsets[i + 1] : input_size;
      if (!send_surface(fd, pts, stride, slice_height,
                        &surfaces[i % pool_size]))
         goto fail_run;
      uint64_t packet_started = monotonic_ns();
      if (!send_message(fd, TMC_PACKET, pts, 0, 0, 0, input + begin,
                        (uint32_t)(end - begin)))
         goto fail_run;
      int matching_frame_before_ack = 0;
      do {
         if (!receive_message(fd, &message, &payload))
            goto fail_run;
         if (message.type == TMC_FRAME) {
            frames++;
            frames_before_ack++;
            if (!first_output_after_inputs)
               first_output_after_inputs = i + 1;
            if (message.flags & TMC_FRAME_FLAG_SHARED_SURFACE) {
               shared_frames++;
               if (message.pts_us == pts)
                  matching_frame_before_ack = 1;
               else
                  nonmatching_frames_before_ack++;
            } else {
               raw_frames++;
            }
         }
         free(payload);
         payload = NULL;
      } while (message.type != TMC_ACK);
      ack_ns[i] = monotonic_ns() - packet_started;
      ready_at_ack += matching_frame_before_ack;
      int dmabuf_ready = dmabuf_read_ready(surfaces[i % pool_size].fd);
      fenced_at_ack += !dmabuf_ready;
      unsafe_at_ack += !matching_frame_before_ack && dmabuf_ready;
#ifdef TMC_EGL_CONSUMER
      if (egl_enabled) {
         uint32_t task = i;
         if (!pipe_transfer(egl_worker.task_fd, &task, sizeof(task), 1))
            goto fail_run;
      }
#endif
   }

   int64_t eos_pts = (int64_t)unit_count * 1000000 / fps;
   if (!send_message(fd, TMC_INPUT_EOS, eos_pts, 0, 0, 0, NULL, 0))
      goto fail_run;
   do {
      if (!receive_message(fd, &message, &payload))
         goto fail_run;
      if (message.type == TMC_FRAME) {
         frames++;
         if (message.flags & TMC_FRAME_FLAG_SHARED_SURFACE)
            shared_frames++;
         else
            raw_frames++;
      }
      free(payload);
      payload = NULL;
   } while (message.type != TMC_OUTPUT_EOS);

   uint64_t elapsed_ns = monotonic_ns() - run_started;
#ifdef TMC_EGL_CONSUMER
   if (egl_enabled) {
      close(egl_worker.task_fd);
      egl_worker.task_fd = -1;
      while (gpu_samples < unit_count) {
         struct egl_sample_result result;
         if (!pipe_transfer(egl_worker.result_fd, &result, sizeof(result), 0) ||
             !result.ok || result.frame_index >= unit_count)
            goto fail_run;
         gpu_sample_ns[gpu_samples++] = result.elapsed_ns;
         gpu_unready_after_sample += result.unready_after_sample;
         struct surface_slot *slot = &surfaces[result.frame_index];
         if (!dmabuf_cpu_sync(slot->fd,
                              DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ))
            goto fail_run;
         uint32_t x = width / 2;
         uint32_t y = height / 2;
         const uint8_t *bytes = slot->mapping;
         uint8_t expected[4] = {
            bytes[(size_t)y * stride + x],
            bytes[(size_t)stride * slice_height +
                  (size_t)(y / 2) * stride + (x & ~1u)],
            bytes[(size_t)stride * slice_height +
                  (size_t)(y / 2) * stride + (x & ~1u) + 1],
            255,
         };
         for (unsigned channel = 0; channel < 4; channel++) {
            if (abs((int)result.pixel[channel] -
                    (int)expected[channel]) > 1) {
               gpu_pixel_mismatches++;
               fprintf(stderr,
                       "Panfrost final mismatch frame=%u got=%u,%u,%u,%u "
                       "expected=%u,%u,%u,%u\n",
                       result.frame_index, result.pixel[0], result.pixel[1],
                       result.pixel[2], result.pixel[3], expected[0],
                       expected[1], expected[2], expected[3]);
               break;
            }
         }
         if (!dmabuf_cpu_sync(slot->fd,
                              DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ))
            goto fail_run;
      }
      stop_egl_worker(&egl_worker, 0);
   }
#endif
   uint64_t ack_p50_ns = percentile(ack_ns, unit_count, 50);
   uint64_t ack_p95_ns = percentile(ack_ns, unit_count, 95);
   uint64_t gpu_p50_ns = percentile(gpu_sample_ns, gpu_samples, 50);
   uint64_t gpu_p95_ns = percentile(gpu_sample_ns, gpu_samples, 95);
   uint64_t ack_max_ns = ack_ns[unit_count - 1];
   for (unsigned i = 0; i < unit_count; i++)
      if (ack_ns[i] > ack_max_ns)
         ack_max_ns = ack_ns[i];
   double ready_percent = 100.0 * ready_at_ack / unit_count;
   double protected_percent =
      100.0 * (unit_count - unsafe_at_ack) / unit_count;
   double decoded_fps = elapsed_ns ? 1e9 * frames / elapsed_ns : 0.0;
   printf("surface-lifecycle: frames=%u/%u shared=%u raw=%u "
          "ready-at-ack=%u (%.2f%%) fenced-at-ack=%u "
          "protected-at-ack=%.2f%% unsafe-at-ack=%u "
          "nonmatching-before-ack=%u "
          "first-output-after-inputs=%u "
          "decoded-fps=%.2f ack-p50=%.3fms ack-p95=%.3fms\n",
          frames, unit_count, shared_frames, raw_frames, ready_at_ack,
          ready_percent, fenced_at_ack, protected_percent, unsafe_at_ack,
          nonmatching_frames_before_ack,
          first_output_after_inputs, decoded_fps,
          ack_p50_ns / 1e6, ack_p95_ns / 1e6);
#ifdef TMC_EGL_CONSUMER
   if (egl_enabled)
      printf("surface-lifecycle-egl: renderer=%s samples=%u mismatches=%u "
             "unready-after-sample=%u gpu-p50=%.3fms gpu-p95=%.3fms\n",
             egl_worker.renderer, gpu_samples,
             gpu_pixel_mismatches, gpu_unready_after_sample,
             gpu_p50_ns / 1e6, gpu_p95_ns / 1e6);
#endif
   printf("{\"schema\":\"tensor-perf-v1\",\"kind\":\"benchmark\","
          "\"name\":\"surface-lifecycle\",\"elapsed_ns\":%llu,"
          "\"access_units\":%u,\"frames\":%u,\"shared_frames\":%u,"
          "\"raw_frames\":%u,\"ready_at_ack_percent\":%.6f,"
          "\"fenced_at_ack\":%u,\"protected_at_ack_percent\":%.6f,"
          "\"unsafe_at_ack\":%u,"
          "\"frames_before_ack\":%u,"
          "\"nonmatching_frames_before_ack\":%u,"
          "\"first_output_after_inputs\":%u,\"decoded_fps\":%.6f,"
          "\"ack_p50_us\":%.3f,\"ack_p95_us\":%.3f,"
          "\"ack_max_us\":%.3f,\"gpu_samples\":%u,"
          "\"gpu_pixel_mismatches\":%u,"
          "\"gpu_unready_after_sample\":%u,"
          "\"gpu_sample_p50_us\":%.3f,\"gpu_sample_p95_us\":%.3f}\n",
          (unsigned long long)elapsed_ns, unit_count, frames, shared_frames,
          raw_frames, ready_percent, fenced_at_ack, protected_percent,
          unsafe_at_ack, frames_before_ack,
          nonmatching_frames_before_ack, first_output_after_inputs, decoded_fps,
          ack_p50_ns / 1e3, ack_p95_ns / 1e3, ack_max_ns / 1e3,
          gpu_samples, gpu_pixel_mismatches, gpu_unready_after_sample,
          gpu_p50_ns / 1e3, gpu_p95_ns / 1e3);
   int passed = frames == unit_count && shared_frames == unit_count &&
                gpu_pixel_mismatches == 0 && gpu_unready_after_sample == 0;
   free(gpu_sample_ns);
   free(ack_ns);
   close(fd);
   for (unsigned i = 0; i < pool_size; i++) {
#ifdef TMC_EGL_CONSUMER
      if (surfaces[i].mapping != MAP_FAILED)
         munmap(surfaces[i].mapping, surfaces[i].size);
#endif
      close(surfaces[i].fd);
   }
   free(surfaces);
   free(input);
   return passed ? 0 : 1;

fail_run:
   free(payload);
   free(gpu_sample_ns);
   free(ack_ns);
fail_connected:
   close(fd);
fail:
#ifdef TMC_EGL_CONSUMER
   stop_egl_worker(&egl_worker, 1);
#endif
   for (unsigned i = 0; i < pool_size; i++)
      if (surfaces[i].fd >= 0) {
#ifdef TMC_EGL_CONSUMER
         if (surfaces[i].mapping != MAP_FAILED)
            munmap(surfaces[i].mapping, surfaces[i].size);
#endif
         close(surfaces[i].fd);
      }
   free(surfaces);
   free(input);
   return 1;
}
