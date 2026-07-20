#define _GNU_SOURCE

#include <fcntl.h>
#include <gbm.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

int
main(void)
{
   int fd = open("/dev/mali0", O_RDWR | O_CLOEXEC | O_NONBLOCK);
   if (fd < 0) {
      perror("open /dev/mali0");
      return 1;
   }

   struct gbm_device *device = gbm_create_device(fd);
   if (!device) {
      fprintf(stderr, "gbm_create_device failed\n");
      close(fd);
      return 1;
   }

   printf("GBM_BACKEND=%s\n", gbm_device_get_backend_name(device));
   printf("GBM_DEVICE_FD=%d\n", gbm_device_get_fd(device));

   struct gbm_bo *bo = gbm_bo_create(device, 64, 64, GBM_FORMAT_XRGB8888,
                                     GBM_BO_USE_RENDERING |
                                     GBM_BO_USE_LINEAR);
   if (!bo) {
      fprintf(stderr, "gbm_bo_create failed\n");
      gbm_device_destroy(device);
      close(fd);
      return 1;
   }

   printf("GBM_BO=%ux%u STRIDE=%u\n", gbm_bo_get_width(bo),
          gbm_bo_get_height(bo), gbm_bo_get_stride(bo));

   gbm_bo_destroy(bo);
   gbm_device_destroy(device);
   close(fd);
   return 0;
}
