#define _GNU_SOURCE

#include "release-fence.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <dma-uapi/dma-buf.h>
#include <mali_base_kernel.h>
#include <mali_kbase_ioctl.h>

static bool
debug_enabled(void)
{
   const char *value = getenv("TENSOR_RELEASE_FENCE_DEBUG");
   return value && value[0] && strcmp(value, "0") != 0;
}

static void
drain_events(struct tmc_release_fence_context *context)
{
   struct base_jd_event_v2 events[16];
   while (read(context->mali_fd, events, sizeof(events)) > 0)
      ;
}

void
tmc_release_fence_context_reset(struct tmc_release_fence_context *context)
{
   memset(context, 0, sizeof(*context));
   context->mali_fd = -1;
   context->stream_fd = -1;
   context->tracking = MAP_FAILED;
   context->events = MAP_FAILED;
   context->next_atom = 1;
   for (unsigned i = 0; i < TMC_RELEASE_FENCE_SLOTS; i++)
      context->slots[i].fence_fd = -1;
}

bool
tmc_release_fence_context_init(struct tmc_release_fence_context *context)
{
   tmc_release_fence_context_reset(context);
   long page_size = sysconf(_SC_PAGESIZE);
   context->page_size = page_size > 0 ? (size_t)page_size : 4096;
   context->mali_fd = open("/dev/mali0", O_RDWR | O_CLOEXEC | O_NONBLOCK);
   if (context->mali_fd < 0)
      goto fail;

   struct kbase_ioctl_version_check version = {0};
   struct kbase_ioctl_set_flags flags = {.create_flags = 0};
   if (ioctl(context->mali_fd, KBASE_IOCTL_VERSION_CHECK, &version) < 0 ||
       version.major != 11 ||
       ioctl(context->mali_fd, KBASE_IOCTL_SET_FLAGS, &flags) < 0)
      goto fail;

   context->tracking =
      mmap(NULL, context->page_size, PROT_NONE, MAP_SHARED, context->mali_fd,
           BASE_MEM_MAP_TRACKING_HANDLE);
   if (context->tracking == MAP_FAILED)
      goto fail;

   union kbase_ioctl_mem_alloc allocation = {
      .in = {
         .va_pages = 1,
         .commit_pages = 1,
         .flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                  BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                  BASE_MEM_COHERENT_LOCAL | BASE_MEM_SAME_VA,
      },
   };
   if (ioctl(context->mali_fd, KBASE_IOCTL_MEM_ALLOC, &allocation) < 0)
      goto fail;
   context->events =
      mmap(NULL, context->page_size, PROT_READ | PROT_WRITE, MAP_SHARED,
           context->mali_fd, allocation.out.gpu_va);
   if (context->events == MAP_FAILED)
      goto fail;
   memset((void *)context->events, BASE_JD_SOFT_EVENT_RESET,
          TMC_RELEASE_FENCE_SLOTS);

   struct kbase_ioctl_stream_create stream = {0};
   memcpy(stream.name, "tensor-codec-release", 21);
   context->stream_fd =
      ioctl(context->mali_fd, KBASE_IOCTL_STREAM_CREATE, &stream);
   if (context->stream_fd < 0)
      goto fail;
   return true;

fail:
   tmc_release_fence_context_destroy(context);
   return false;
}

bool
tmc_release_fence_signal(struct tmc_release_fence *release_fence)
{
   if (!release_fence || !release_fence->context || release_fence->slot < 0)
      return true;
   struct tmc_release_fence_context *context = release_fence->context;
   unsigned slot = (unsigned)release_fence->slot;
   if (slot >= TMC_RELEASE_FENCE_SLOTS || !context->slots[slot].active)
      return false;

   struct kbase_ioctl_soft_event_update update = {
      .event = (uintptr_t)&context->events[slot],
      .new_status = BASE_JD_SOFT_EVENT_SET,
   };
   bool signaled =
      ioctl(context->mali_fd, KBASE_IOCTL_SOFT_EVENT_UPDATE, &update) == 0;
   struct pollfd item = {
      .fd = context->slots[slot].fence_fd,
      .events = POLLIN,
   };
   int poll_result;
   do {
      poll_result = poll(&item, 1, 1000);
   } while (poll_result < 0 && errno == EINTR);
   signaled = signaled && poll_result > 0 && (item.revents & POLLIN) != 0;
   if (debug_enabled())
      fprintf(stderr,
              "release-fence signal slot=%u update=%d poll=%d revents=0x%x\n",
              slot, signaled, poll_result, item.revents);

   close(context->slots[slot].fence_fd);
   context->slots[slot].fence_fd = -1;
   context->slots[slot].active = false;
   release_fence->context = NULL;
   release_fence->slot = -1;
   drain_events(context);
   return signaled;
}

bool
tmc_release_fence_arm(struct tmc_release_fence_context *context,
                      int dmabuf_fd,
                      struct tmc_release_fence *release_fence)
{
   if (!context || context->mali_fd < 0 || dmabuf_fd < 0 || !release_fence)
      return false;
   unsigned slot;
   for (slot = 0; slot < TMC_RELEASE_FENCE_SLOTS; slot++)
      if (!context->slots[slot].active)
         break;
   if (slot == TMC_RELEASE_FENCE_SLOTS)
      return false;

   struct kbase_ioctl_soft_event_update reset = {
      .event = (uintptr_t)&context->events[slot],
      .new_status = BASE_JD_SOFT_EVENT_RESET,
   };
   if (ioctl(context->mali_fd, KBASE_IOCTL_SOFT_EVENT_UPDATE, &reset) < 0)
      return false;

   if (context->next_atom > 253)
      context->next_atom = 1;
   uint8_t wait_atom = context->next_atom++;
   uint8_t trigger_atom = context->next_atom++;
   struct base_fence fence = {
      .basep = {.fd = -1, .stream_fd = context->stream_fd},
   };
   struct base_jd_atom_v2 atoms[2] = {0};
   atoms[0].jc = (uintptr_t)&context->events[slot];
   atoms[0].atom_number = wait_atom;
   atoms[0].core_req = BASE_JD_REQ_SOFT_EVENT_WAIT;
   atoms[1].jc = (uintptr_t)&fence;
   atoms[1].atom_number = trigger_atom;
   atoms[1].pre_dep[0].atom_id = wait_atom;
   atoms[1].pre_dep[0].dependency_type = BASE_JD_DEP_TYPE_DATA;
   atoms[1].core_req = BASE_JD_REQ_SOFT_FENCE_TRIGGER;
   struct kbase_ioctl_job_submit submit = {
      .addr = (uintptr_t)atoms,
      .nr_atoms = 2,
      .stride = sizeof(atoms[0]),
   };
   if (ioctl(context->mali_fd, KBASE_IOCTL_JOB_SUBMIT, &submit) < 0 ||
       fence.basep.fd < 0)
      return false;

   struct dma_buf_import_sync_file import = {
      .flags = DMA_BUF_SYNC_WRITE,
      .fd = fence.basep.fd,
   };
   context->slots[slot].active = true;
   context->slots[slot].fence_fd = fence.basep.fd;
   release_fence->context = context;
   release_fence->slot = (int)slot;
   if (ioctl(dmabuf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &import) == 0)
   {
      if (debug_enabled())
         fprintf(stderr, "release-fence arm slot=%u fd=%d\n", slot,
                 fence.basep.fd);
      return true;
   }

   tmc_release_fence_signal(release_fence);
   return false;
}

void
tmc_release_fence_context_destroy(struct tmc_release_fence_context *context)
{
   if (!context)
      return;
   for (unsigned i = 0; i < TMC_RELEASE_FENCE_SLOTS; i++) {
      if (!context->slots[i].active)
         continue;
      struct tmc_release_fence release_fence = {
         .context = context,
         .slot = (int)i,
      };
      tmc_release_fence_signal(&release_fence);
   }
   if (context->stream_fd >= 0)
      close(context->stream_fd);
   if (context->events != MAP_FAILED)
      munmap((void *)context->events, context->page_size);
   if (context->tracking != MAP_FAILED)
      munmap(context->tracking, context->page_size);
   if (context->mali_fd >= 0)
      close(context->mali_fd);
   tmc_release_fence_context_reset(context);
}
