#define _GNU_SOURCE

/*
 * Prove that an unprivileged Termux process can bridge a userspace-controlled
 * Kbase sync_file into a DMA-BUF reservation object.  This is the primitive
 * needed to replace Firefox's unsafe deferred-export/remap workaround: import
 * an unsignalled write fence before export, fill the surface later, then
 * signal the fence so implicit-sync GPU readers can safely sample it.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <dma-uapi/dma-buf.h>
#include <mali_base_kernel.h>
#include <mali_kbase_ioctl.h>

static int
poll_ready(int fd, short events, int timeout_ms)
{
   struct pollfd item = {.fd = fd, .events = events};
   int result;
   do {
      result = poll(&item, 1, timeout_ms);
   } while (result < 0 && errno == EINTR);
   return result > 0 && (item.revents & events) != 0;
}

static int
allocate_dmabuf(size_t size)
{
   int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
   if (heap < 0)
      return -1;
   struct dma_heap_allocation_data allocation = {
      .len = size,
      .fd_flags = O_RDWR | O_CLOEXEC,
   };
   int result = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &allocation);
   close(heap);
   return result == 0 ? (int)allocation.fd : -1;
}

int
main(void)
{
   long page_size = sysconf(_SC_PAGESIZE);
   if (page_size <= 0)
      page_size = 4096;

   int mali = open("/dev/mali0", O_RDWR | O_CLOEXEC);
   if (mali < 0) {
      perror("/dev/mali0");
      return 1;
   }

   struct kbase_ioctl_version_check version = {0};
   if (ioctl(mali, KBASE_IOCTL_VERSION_CHECK, &version) < 0) {
      perror("KBASE_IOCTL_VERSION_CHECK");
      close(mali);
      return 1;
   }
   struct kbase_ioctl_set_flags flags = {.create_flags = 0};
   if (ioctl(mali, KBASE_IOCTL_SET_FLAGS, &flags) < 0) {
      perror("KBASE_IOCTL_SET_FLAGS");
      close(mali);
      return 1;
   }

   void *tracking = mmap(NULL, (size_t)page_size, PROT_NONE, MAP_SHARED, mali,
                         BASE_MEM_MAP_TRACKING_HANDLE);
   if (tracking == MAP_FAILED) {
      perror("mmap(BASE_MEM_MAP_TRACKING_HANDLE)");
      close(mali);
      return 1;
   }

   union kbase_ioctl_mem_alloc allocation = {
      .in = {
         .va_pages = 1,
         .commit_pages = 1,
         .flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                  BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                  BASE_MEM_COHERENT_LOCAL | BASE_MEM_SAME_VA,
      },
   };
   if (ioctl(mali, KBASE_IOCTL_MEM_ALLOC, &allocation) < 0) {
      perror("KBASE_IOCTL_MEM_ALLOC");
      munmap(tracking, (size_t)page_size);
      close(mali);
      return 1;
   }
   void *event = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, mali, allocation.out.gpu_va);
   if (event == MAP_FAILED) {
      perror("mmap(soft event)");
      munmap(tracking, (size_t)page_size);
      close(mali);
      return 1;
   }
   *(volatile uint8_t *)event = BASE_JD_SOFT_EVENT_RESET;

   struct kbase_ioctl_stream_create stream = {0};
   snprintf(stream.name, sizeof(stream.name), "tensor-release");
   int stream_fd = ioctl(mali, KBASE_IOCTL_STREAM_CREATE, &stream);
   if (stream_fd < 0) {
      perror("KBASE_IOCTL_STREAM_CREATE");
      munmap(event, (size_t)page_size);
      munmap(tracking, (size_t)page_size);
      close(mali);
      return 1;
   }

   struct base_fence fence = {
      .basep = {.fd = -1, .stream_fd = stream_fd},
   };
   struct base_jd_atom_v2 atoms[2] = {0};
   atoms[0].jc = (uintptr_t)event;
   atoms[0].atom_number = 1;
   atoms[0].core_req = BASE_JD_REQ_SOFT_EVENT_WAIT;
   atoms[1].jc = (uintptr_t)&fence;
   atoms[1].atom_number = 2;
   atoms[1].pre_dep[0].atom_id = 1;
   atoms[1].pre_dep[0].dependency_type = BASE_JD_DEP_TYPE_DATA;
   atoms[1].core_req = BASE_JD_REQ_SOFT_FENCE_TRIGGER;
   struct kbase_ioctl_job_submit submit = {
      .addr = (uintptr_t)atoms,
      .nr_atoms = 2,
      .stride = sizeof(atoms[0]),
   };
   if (ioctl(mali, KBASE_IOCTL_JOB_SUBMIT, &submit) < 0 ||
       fence.basep.fd < 0) {
      perror("KBASE_IOCTL_JOB_SUBMIT(fence)");
      close(stream_fd);
      munmap(event, (size_t)page_size);
      munmap(tracking, (size_t)page_size);
      close(mali);
      return 1;
   }

   struct kbase_ioctl_fence_validate validate = {.fd = fence.basep.fd};
   int fence_valid = ioctl(mali, KBASE_IOCTL_FENCE_VALIDATE, &validate) == 0;
   int fence_ready_before = poll_ready(fence.basep.fd, POLLIN, 0);

   int dmabuf = allocate_dmabuf((size_t)page_size);
   struct dma_buf_import_sync_file import = {
      .flags = DMA_BUF_SYNC_WRITE,
      .fd = fence.basep.fd,
   };
   int import_ok = dmabuf >= 0 &&
                   ioctl(dmabuf, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &import) == 0;
   int dmabuf_ready_before =
      import_ok ? poll_ready(dmabuf, POLLIN, 0) : -1;

   void *dmabuf_mapping =
      dmabuf >= 0 ? mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, dmabuf, 0) : MAP_FAILED;
   struct dma_buf_sync cpu_sync = {
      .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE,
   };
   int cpu_sync_start_ok = dmabuf_mapping != MAP_FAILED &&
                           ioctl(dmabuf, DMA_BUF_IOCTL_SYNC, &cpu_sync) == 0;
   if (cpu_sync_start_ok)
      *(volatile uint8_t *)dmabuf_mapping = 0x5a;
   cpu_sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
   int cpu_sync_end_ok = cpu_sync_start_ok &&
                         ioctl(dmabuf, DMA_BUF_IOCTL_SYNC, &cpu_sync) == 0;

   struct kbase_ioctl_soft_event_update update = {
      .event = (uintptr_t)event,
      .new_status = BASE_JD_SOFT_EVENT_SET,
   };
   int signal_ok = ioctl(mali, KBASE_IOCTL_SOFT_EVENT_UPDATE, &update) == 0;
   int fence_ready_after = poll_ready(fence.basep.fd, POLLIN, 1000);
   int dmabuf_ready_after =
      import_ok ? poll_ready(dmabuf, POLLIN, 1000) : -1;

   printf("Kbase=%u.%u fence-valid=%d fence-before=%d import=%d "
          "dmabuf-before=%d cpu-sync=%d/%d signal=%d fence-after=%d "
          "dmabuf-after=%d\n",
          version.major, version.minor, fence_valid, fence_ready_before,
          import_ok, dmabuf_ready_before, cpu_sync_start_ok, cpu_sync_end_ok,
          signal_ok, fence_ready_after, dmabuf_ready_after);
   printf("{\"schema\":\"tensor-perf-v1\",\"kind\":\"probe\","
          "\"name\":\"dmabuf-release-fence\",\"kbase_major\":%u,"
          "\"kbase_minor\":%u,\"fence_valid\":%s,"
          "\"fence_ready_before\":%s,\"import_sync_file\":%s,"
          "\"dmabuf_ready_before\":%s,\"cpu_sync_start\":%s,"
          "\"cpu_sync_end\":%s,\"signal_ok\":%s,"
          "\"fence_ready_after\":%s,\"dmabuf_ready_after\":%s}\n",
          version.major, version.minor, fence_valid ? "true" : "false",
          fence_ready_before ? "true" : "false",
          import_ok ? "true" : "false",
          dmabuf_ready_before == 1 ? "true" : "false",
          cpu_sync_start_ok ? "true" : "false",
          cpu_sync_end_ok ? "true" : "false",
          signal_ok ? "true" : "false",
          fence_ready_after ? "true" : "false",
          dmabuf_ready_after == 1 ? "true" : "false");

   if (dmabuf_mapping != MAP_FAILED)
      munmap(dmabuf_mapping, (size_t)page_size);
   if (dmabuf >= 0)
      close(dmabuf);
   close(fence.basep.fd);
   close(stream_fd);
   munmap(event, (size_t)page_size);
   munmap(tracking, (size_t)page_size);
   close(mali);

   return fence_valid && !fence_ready_before && import_ok &&
          !dmabuf_ready_before && cpu_sync_start_ok && cpu_sync_end_ok &&
          signal_ok && fence_ready_after && dmabuf_ready_after ? 0 : 1;
}
