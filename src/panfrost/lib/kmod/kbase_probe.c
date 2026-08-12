/* SPDX-License-Identifier: MIT */

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pan_kmod.h"

/* The standalone probe does not initialize Mesa's optional CPU tracer. */
uint64_t pan_trace_categories = 0;

int
main(void)
{
   int fd = open("/dev/mali0", O_RDWR | O_CLOEXEC);
   if (fd < 0) {
      perror("open(/dev/mali0)");
      return 1;
   }

   setenv("PAN_KMOD_BACKEND", "kbase", 1);
   struct pan_kmod_dev *dev =
      pan_kmod_dev_create(fd, PAN_KMOD_DEV_FLAG_OWNS_FD, NULL);
   if (!dev) {
      fprintf(stderr, "kbase device creation failed\n");
      close(fd);
      return 2;
   }

   printf("gpu_id=0x%" PRIx64 " shader_present=0x%" PRIx64
          " mmu_features=0x%x\n",
          dev->props.gpu_id, dev->props.shader_present,
          dev->props.mmu_features);

   struct pan_kmod_va_range va = pan_kmod_dev_query_user_va_range(dev);
   struct pan_kmod_vm *vm =
      pan_kmod_vm_create(dev, PAN_KMOD_VM_FLAG_AUTO_VA, va.start, va.size);
   if (!vm) {
      fprintf(stderr, "VM creation failed\n");
      pan_kmod_dev_destroy(dev);
      return 3;
   }

   struct pan_kmod_bo *bo =
      pan_kmod_bo_alloc(dev, vm, 4096, PAN_KMOD_BO_FLAG_WB_MMAP);
   if (!bo) {
      fprintf(stderr, "BO allocation failed\n");
      pan_kmod_vm_destroy(vm);
      pan_kmod_dev_destroy(dev);
      return 4;
   }

   struct pan_kmod_vm_op bind = {
      .type = PAN_KMOD_VM_OP_TYPE_MAP,
      .va = { .start = PAN_KMOD_VM_MAP_AUTO_VA, .size = bo->size },
      .map = { .bo = bo, .bo_offset = 0 },
   };
   if (pan_kmod_vm_bind(vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &bind, 1)) {
      fprintf(stderr, "VM bind failed\n");
      pan_kmod_bo_put(bo);
      pan_kmod_vm_destroy(vm);
      pan_kmod_dev_destroy(dev);
      return 5;
   }

   void *map = pan_kmod_bo_mmap(bo, PROT_READ | PROT_WRITE, MAP_SHARED, NULL);
   if (map == MAP_FAILED) {
      fprintf(stderr, "BO mmap failed\n");
      pan_kmod_bo_put(bo);
      pan_kmod_vm_destroy(vm);
      pan_kmod_dev_destroy(dev);
      return 6;
   }

   memset(map, 0xa5, bo->size);
   pan_kmod_queue_bo_map_sync(bo, 0, map, bo->size,
                              PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH);
   pan_kmod_flush_bo_map_syncs(dev);
   printf("bo_handle=%u gpu_va=0x%" PRIx64 " map=%p status=PASS\n",
          bo->handle, bind.va.start, map);

   munmap(map, bo->size);
   pan_kmod_bo_put(bo);
   pan_kmod_vm_destroy(vm);
   pan_kmod_dev_destroy(dev);
   return 0;
}
