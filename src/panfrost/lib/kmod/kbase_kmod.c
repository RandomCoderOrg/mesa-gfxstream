/*
 * Copyright © 2026
 * SPDX-License-Identifier: MIT
 *
 * Adapter between Mesa's current pan_kmod interface and Arm's Android Kbase
 * Job Manager ABI.  This is deliberately a small winsys boundary: command
 * generation remains entirely upstream Panfrost.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "drm-uapi/panfrost_drm.h"
#include "pan_base.h"
#include "mali_kbase_gpuprops.h"

#include "pan_kmod_backend.h"
#include "kbase_kmod.h"

const struct pan_kmod_ops kbase_kmod_ops;

bool
pan_kmod_kbase_fd_matches(int fd)
{
   const char *path = getenv("PAN_MALI_DEV");
   struct stat candidate;
   struct stat opened;

   if (!path || !path[0])
      path = "/dev/mali0";

   return fd >= 0 && fstat(fd, &opened) == 0 && stat(path, &candidate) == 0 &&
          S_ISCHR(opened.st_mode) && S_ISCHR(candidate.st_mode) &&
          opened.st_rdev == candidate.st_rdev;
}

struct kbase_kmod_dev {
   struct pan_kmod_dev base;
   struct kbase_ kbase;
};

struct kbase_kmod_vm {
   struct pan_kmod_vm base;
};

struct kbase_kmod_bo {
   struct pan_kmod_bo base;
   struct base_ptr ptr;
   bool needs_free_ioctl;
};

static inline struct kbase_kmod_dev *
to_kbase_dev(struct pan_kmod_dev *dev)
{
   return container_of(dev, struct kbase_kmod_dev, base);
}

static inline struct kbase_kmod_bo *
to_kbase_bo(struct pan_kmod_bo *bo)
{
   return container_of(bo, struct kbase_kmod_bo, base);
}

static uint64_t
kbase_query(struct kbase_kmod_dev *dev, unsigned prop, uint64_t fallback)
{
   uint64_t value = 0;
   return dev->kbase.get_mali_gpuprop(&dev->kbase, prop, &value)
             ? value
             : fallback;
}

static void
kbase_query_props(struct kbase_kmod_dev *dev)
{
   struct pan_kmod_dev_props *props = &dev->base.props;

   memset(props, 0, sizeof(*props));
   props->gpu_id = kbase_query(dev, KBASE_GPUPROP_RAW_GPU_ID, 0);
   props->shader_present =
      kbase_query(dev, KBASE_GPUPROP_RAW_SHADER_PRESENT, 1);
   props->tiler_features =
      kbase_query(dev, KBASE_GPUPROP_RAW_TILER_FEATURES, 0x809);
   props->mem_features =
      kbase_query(dev, KBASE_GPUPROP_RAW_MEM_FEATURES, 0);
   props->mmu_features =
      kbase_query(dev, KBASE_GPUPROP_RAW_MMU_FEATURES, 48);
   props->l2_features =
      kbase_query(dev, KBASE_GPUPROP_RAW_L2_FEATURES, 0);

   props->texture_features[0] =
      kbase_query(dev, KBASE_GPUPROP_RAW_TEXTURE_FEATURES_0, 0);
   props->texture_features[1] =
      kbase_query(dev, KBASE_GPUPROP_RAW_TEXTURE_FEATURES_1, 0);
   props->texture_features[2] =
      kbase_query(dev, KBASE_GPUPROP_RAW_TEXTURE_FEATURES_2, 0);
   props->texture_features[3] =
      kbase_query(dev, KBASE_GPUPROP_RAW_TEXTURE_FEATURES_3, 0);

   props->max_threads_per_core =
      kbase_query(dev, KBASE_GPUPROP_RAW_THREAD_MAX_THREADS, 512);
   props->max_threads_per_wg =
      kbase_query(dev, KBASE_GPUPROP_RAW_THREAD_MAX_WORKGROUP_SIZE,
                  props->max_threads_per_core);

   uint64_t thread_features =
      kbase_query(dev, KBASE_GPUPROP_RAW_THREAD_FEATURES, 0);
   props->max_tasks_per_core = MAX2(thread_features >> 24, 1);
   props->num_registers_per_core = thread_features & 0xffff;
   if (!props->num_registers_per_core)
      props->num_registers_per_core = props->max_threads_per_core * 32;

   props->max_tls_instance_per_core =
      kbase_query(dev, KBASE_GPUPROP_RAW_THREAD_TLS_ALLOC,
                  props->max_threads_per_core);
   if (!props->max_tls_instance_per_core)
      props->max_tls_instance_per_core = props->max_threads_per_core;

   /* Keep AFBC disabled during the initial port. The old Tensor branch did
    * the same after compressed display targets caused Kbase faults. */
   props->afbc_features = 1;
   props->allowed_group_priorities_mask =
      PAN_KMOD_GROUP_ALLOW_PRIORITY_MEDIUM;
   props->supported_bo_flags = PAN_KMOD_BO_FLAG_EXECUTABLE |
                               PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT |
                               PAN_KMOD_BO_FLAG_NO_MMAP |
                               PAN_KMOD_BO_FLAG_WB_MMAP;
   props->pgsize_bitmap = PAN_PGSIZE_4K;
}

static struct pan_kmod_dev *
kbase_kmod_dev_create(int fd, uint32_t flags,
                      const struct pan_kmod_driver *drv_info,
                      const struct pan_kmod_allocator *allocator)
{
   struct kbase_kmod_dev *dev = pan_kmod_alloc(allocator, sizeof(*dev));
   if (!dev)
      return NULL;

   if (!kbase_open(&dev->kbase, fd, 4,
                   getenv("PAN_KBASE_VERBOSE") != NULL)) {
      pan_kmod_free(allocator, dev);
      return NULL;
   }

   pan_kmod_dev_init(&dev->base, fd, flags, drv_info, &kbase_kmod_ops,
                     allocator);
   kbase_query_props(dev);
   return &dev->base;
}

static void
kbase_kmod_dev_destroy(struct pan_kmod_dev *base)
{
   struct kbase_kmod_dev *dev = to_kbase_dev(base);

   dev->kbase.close(&dev->kbase);
   base->flags &= ~PAN_KMOD_DEV_FLAG_OWNS_FD;
   pan_kmod_dev_cleanup(base);
   pan_kmod_free(base->allocator, dev);
}

static struct pan_kmod_bo *
kbase_kmod_bo_alloc(struct pan_kmod_dev *base,
                    struct pan_kmod_vm *exclusive_vm, uint64_t size,
                    uint32_t flags)
{
   struct kbase_kmod_dev *dev = to_kbase_dev(base);
   struct kbase_kmod_bo *bo = pan_kmod_dev_alloc(base, sizeof(*bo));
   unsigned pan_flags = 0;

   if (!bo)
      return NULL;

   if (flags & PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT)
      pan_flags |= PANFROST_BO_HEAP;
   if (!(flags & PAN_KMOD_BO_FLAG_EXECUTABLE))
      pan_flags |= PANFROST_BO_NOEXEC;
   if (flags & PAN_KMOD_BO_FLAG_WB_MMAP)
      pan_flags |= MALI_BO_CACHED_CPU;

   bo->ptr = dev->kbase.alloc(&dev->kbase, size, pan_flags, 0);
   if (!bo->ptr.gpu) {
      pan_kmod_dev_free(base, bo);
      return NULL;
   }

   int handle = kbase_alloc_gem_handle(&dev->kbase, bo->ptr.gpu, -1);
   if (handle < 0) {
      dev->kbase.free(&dev->kbase, bo->ptr.gpu);
      pan_kmod_dev_free(base, bo);
      return NULL;
   }

   bo->needs_free_ioctl = (uintptr_t)bo->ptr.cpu != bo->ptr.gpu;
   pan_kmod_bo_init(&bo->base, base, exclusive_vm, size,
                    flags | PAN_KMOD_BO_FLAG_PERSISTENT_MAP, handle);
   return &bo->base;
}

static void
kbase_kmod_bo_free(struct pan_kmod_bo *base)
{
   struct kbase_kmod_dev *dev = to_kbase_dev(base->dev);
   struct kbase_kmod_bo *bo = to_kbase_bo(base);

   pan_kmod_bo_cleanup(base);
   if (bo->needs_free_ioctl)
      dev->kbase.free(&dev->kbase, bo->ptr.gpu);
   kbase_free_gem_handle(&dev->kbase, base->handle);
   pan_kmod_dev_free(base->dev, bo);
}

static struct pan_kmod_bo *
kbase_kmod_bo_import_dmabuf(struct pan_kmod_dev *base, int fd, uint64_t size)
{
   struct kbase_kmod_dev *dev = to_kbase_dev(base);
   int handle = dev->kbase.import_dmabuf(&dev->kbase, fd);
   if (handle < 0)
      return NULL;

   simple_mtx_lock(&base->handle_to_bo.lock);
   struct pan_kmod_bo **slot =
      util_sparse_array_get(&base->handle_to_bo.array, handle);
   if (!slot) {
      simple_mtx_unlock(&base->handle_to_bo.lock);
      kbase_free_gem_handle(&dev->kbase, handle);
      return NULL;
   }

   if (*slot) {
      struct pan_kmod_bo *found = *slot;
      p_atomic_inc(&found->refcnt);
      simple_mtx_unlock(&base->handle_to_bo.lock);
      return found;
   }

   struct kbase_kmod_bo *bo = pan_kmod_dev_alloc(base, sizeof(*bo));
   if (!bo) {
      simple_mtx_unlock(&base->handle_to_bo.lock);
      kbase_free_gem_handle(&dev->kbase, handle);
      return NULL;
   }

   kbase_handle imported = kbase_gem_handle_get(&dev->kbase, handle);
   bo->ptr.gpu = imported.va;
   if (sizeof(void *) > 4 || imported.va < (1ull << 32)) {
      bo->ptr.cpu = (void *)(uintptr_t)imported.va;
   } else {
      bo->ptr.cpu = dev->kbase.mmap_import(&dev->kbase, imported.va, size);
      bo->needs_free_ioctl = true;
   }

   pan_kmod_bo_init(&bo->base, base, NULL, size,
                    PAN_KMOD_BO_FLAG_IMPORTED |
                       PAN_KMOD_BO_FLAG_PERSISTENT_MAP,
                    handle);
   *slot = &bo->base;
   simple_mtx_unlock(&base->handle_to_bo.lock);
   return &bo->base;
}

static int
kbase_kmod_bo_export_dmabuf(struct pan_kmod_bo *base)
{
   struct kbase_kmod_dev *dev = to_kbase_dev(base->dev);
   kbase_handle handle = kbase_gem_handle_get(&dev->kbase, base->handle);
   return handle.fd >= 0 ? os_dupfd_cloexec(handle.fd) : -1;
}

static struct pan_kmod_bo *
kbase_kmod_bo_import_unreachable(struct pan_kmod_dev *dev, uint32_t handle,
                                 uint64_t size)
{
   mesa_loge("Kbase imports must use the DMA-BUF hook");
   return NULL;
}

static off_t
kbase_kmod_bo_get_mmap_offset(struct pan_kmod_bo *bo)
{
   return -1;
}

static void *
kbase_kmod_bo_mmap(struct pan_kmod_bo *base, int prot, int flags,
                   void *host_addr)
{
   return to_kbase_bo(base)->ptr.cpu ?: MAP_FAILED;
}

static int
kbase_kmod_flush_bo_map_syncs(struct pan_kmod_dev *base)
{
   struct kbase_kmod_dev *dev = to_kbase_dev(base);

   util_dynarray_foreach(&base->pending_bo_syncs.array,
                         struct pan_kmod_deferred_bo_sync, sync) {
      struct kbase_kmod_bo *bo = to_kbase_bo(sync->bo);
      if (!bo->ptr.cpu)
         continue;

      dev->kbase.mem_sync(&dev->kbase, bo->ptr.gpu + sync->start,
                          (uint8_t *)bo->ptr.cpu + sync->start, sync->size,
                          sync->type ==
                             PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE);
   }

   return 0;
}

static bool
kbase_kmod_bo_wait(struct pan_kmod_bo *base, int64_t timeout_ns,
                   bool for_read_only_access)
{
   struct kbase_kmod_dev *dev = to_kbase_dev(base->dev);
   bool trace = getenv("PAN_KBASE_TRACE_SYNC") != NULL;
   if (trace) {
      kbase_handle handle = kbase_gem_handle_get(&dev->kbase, base->handle);
      fprintf(stderr, "kbase-sync: wait begin handle=%u use=%u timeout=%" PRId64
                      " readers=%u\n",
              base->handle, handle.use_count, timeout_ns,
              !for_read_only_access);
   }

   int ret = kbase_wait_bo(&dev->kbase, base->handle, timeout_ns,
                           !for_read_only_access);

   if (trace) {
      kbase_handle handle = kbase_gem_handle_get(&dev->kbase, base->handle);
      fprintf(stderr, "kbase-sync: wait end handle=%u use=%u ret=%d errno=%d\n",
              base->handle, handle.use_count, ret, ret ? errno : 0);
   }

   return ret == 0;
}

static struct pan_kmod_vm *
kbase_kmod_vm_create(struct pan_kmod_dev *dev, uint32_t flags,
                     uint64_t va_start, uint64_t va_range)
{
   struct kbase_kmod_vm *vm = pan_kmod_dev_alloc(dev, sizeof(*vm));
   if (!vm)
      return NULL;

   pan_kmod_vm_init(&vm->base, dev, 0, flags);
   return &vm->base;
}

static void
kbase_kmod_vm_destroy(struct pan_kmod_vm *base)
{
   struct kbase_kmod_vm *vm = container_of(base, struct kbase_kmod_vm, base);
   pan_kmod_vm_cleanup(base);
   pan_kmod_dev_free(base->dev, vm);
}

static int
kbase_kmod_vm_bind(struct pan_kmod_vm *vm, enum pan_kmod_vm_op_mode mode,
                   struct pan_kmod_vm_op *ops, uint32_t op_count)
{
   /* Kbase assigns the VA as part of BO allocation and releases it when the
    * BO is destroyed.  UNMAP is therefore bookkeeping-only here, just like
    * the upstream panfrost_kmod backend.  Accept the deferred-idle mode used
    * by Gallium for safe BO teardown; JM submission is serialized by the
    * Kbase adapter before the allocation is actually released. */
   if (mode != PAN_KMOD_VM_OP_MODE_IMMEDIATE &&
       mode != PAN_KMOD_VM_OP_MODE_DEFER_TO_NEXT_IDLE_POINT)
      return -1;

   for (uint32_t i = 0; i < op_count; i++) {
      if (pan_kmod_vm_op_check(vm, mode, &ops[i]))
         return -1;

      if (ops[i].type == PAN_KMOD_VM_OP_TYPE_MAP) {
         struct kbase_kmod_bo *bo = to_kbase_bo(ops[i].map.bo);
         uint64_t va = bo->ptr.gpu + ops[i].map.bo_offset;
         if (ops[i].va.start != PAN_KMOD_VM_MAP_AUTO_VA &&
             ops[i].va.start != va)
            return -1;
         ops[i].va.start = va;
      }
   }

   return 0;
}

static uint64_t
kbase_kmod_query_timestamp(const struct pan_kmod_dev *dev)
{
   return 0;
}

static struct pan_kmod_perf_session *
kbase_kmod_perf_create(struct pan_kmod_dev *dev)
{
   return NULL;
}

static int kbase_kmod_perf_enable(struct pan_kmod_perf_session *s,
                                  const struct pan_kmod_perf_config *c) { return -1; }
static int kbase_kmod_perf_disable(struct pan_kmod_perf_session *s) { return -1; }
static void kbase_kmod_perf_dump(struct pan_kmod_perf_session *s,
                                 struct mali_perf_dump_info *i) { }
static void kbase_kmod_perf_destroy(struct pan_kmod_perf_session *s) { }

const struct pan_kmod_ops kbase_kmod_ops = {
   .dev_create = kbase_kmod_dev_create,
   .dev_destroy = kbase_kmod_dev_destroy,
   .bo_alloc = kbase_kmod_bo_alloc,
   .bo_free = kbase_kmod_bo_free,
   .bo_import = kbase_kmod_bo_import_unreachable,
   .bo_import_dmabuf = kbase_kmod_bo_import_dmabuf,
   .bo_export_dmabuf = kbase_kmod_bo_export_dmabuf,
   .bo_get_mmap_offset = kbase_kmod_bo_get_mmap_offset,
   .bo_mmap = kbase_kmod_bo_mmap,
   .flush_bo_map_syncs = kbase_kmod_flush_bo_map_syncs,
   .bo_wait = kbase_kmod_bo_wait,
   .vm_create = kbase_kmod_vm_create,
   .vm_destroy = kbase_kmod_vm_destroy,
   .vm_bind = kbase_kmod_vm_bind,
   .query_timestamp = kbase_kmod_query_timestamp,
   .perf_create = kbase_kmod_perf_create,
   .perf_enable = kbase_kmod_perf_enable,
   .perf_disable = kbase_kmod_perf_disable,
   .perf_dump = kbase_kmod_perf_dump,
   .perf_destroy = kbase_kmod_perf_destroy,
};

bool
pan_kmod_dev_is_kbase(const struct pan_kmod_dev *dev)
{
   return dev && dev->ops == &kbase_kmod_ops;
}

struct kbase_syncobj *
pan_kmod_kbase_syncobj_create(struct pan_kmod_dev *base)
{
   struct kbase_kmod_dev *dev = to_kbase_dev(base);
   return dev->kbase.syncobj_create(&dev->kbase);
}

void
pan_kmod_kbase_syncobj_destroy(struct pan_kmod_dev *base,
                               struct kbase_syncobj *sync)
{
   struct kbase_kmod_dev *dev = to_kbase_dev(base);
   dev->kbase.syncobj_destroy(&dev->kbase, sync);
}

struct kbase_syncobj *
pan_kmod_kbase_syncobj_dup(struct pan_kmod_dev *base,
                           struct kbase_syncobj *sync)
{
   struct kbase_kmod_dev *dev = to_kbase_dev(base);
   return dev->kbase.syncobj_dup(&dev->kbase, sync);
}

bool
pan_kmod_kbase_syncobj_wait(struct pan_kmod_dev *base,
                            struct kbase_syncobj *sync,
                            int64_t timeout_ns)
{
   struct kbase_kmod_dev *dev = to_kbase_dev(base);
   return dev->kbase.syncobj_wait(&dev->kbase, sync, timeout_ns);
}

int
pan_kmod_kbase_jm_submit(struct pan_kmod_dev *base, uint64_t jc,
                         uint32_t requirements,
                         struct kbase_syncobj *out_sync,
                         int32_t *bo_handles, uint32_t bo_handle_count)
{
   struct kbase_kmod_dev *dev = to_kbase_dev(base);

   dev->kbase.handle_events(&dev->kbase);
   int atom = dev->kbase.submit(&dev->kbase, jc, requirements, out_sync,
                                bo_handles, bo_handle_count);
   if (getenv("PAN_KBASE_TRACE_SYNC")) {
      fprintf(stderr, "kbase-sync: submit atom=%d req=0x%x handles=",
              atom, requirements);
      for (uint32_t i = 0; i < bo_handle_count; i++) {
         kbase_handle handle =
            kbase_gem_handle_get(&dev->kbase, bo_handles[i]);
         fprintf(stderr, "%s%d:%u", i ? "," : "", bo_handles[i],
                 handle.use_count);
      }
      fputc('\n', stderr);
   }
   if (atom < 0) {
      errno = EINVAL;
      return -1;
   }

   return 0;
}
