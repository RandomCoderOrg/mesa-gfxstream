/*
 * Rootless AHardwareBuffer capability probe for Termux/Android.
 *
 * This intentionally uses dlsym for AHardwareBuffer_getNativeHandle: the
 * function is exported by Android's libandroid but is not part of the public
 * NDK link stub shipped by Termux.
 */

#include <android/hardware_buffer.h>

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

struct tensor_native_handle {
   int version;
   int num_fds;
   int num_ints;
   int data[];
};

typedef const struct tensor_native_handle *
(*get_native_handle_fn)(const AHardwareBuffer *buffer);

static bool
describe_native_handle(get_native_handle_fn get_handle,
                       const AHardwareBuffer *buffer, const char *label)
{
   const struct tensor_native_handle *handle = get_handle(buffer);
   if (!handle) {
      fprintf(stderr, "%s: no native handle\n", label);
      return false;
   }

   printf("%s: native-handle version=%d fds=%d ints=%d\n", label,
          handle->version, handle->num_fds, handle->num_ints);
   for (int i = 0; i < handle->num_fds; i++) {
      struct stat status;
      if (fstat(handle->data[i], &status) < 0) {
         fprintf(stderr, "%s: fstat fd %d failed: %s\n", label,
                 handle->data[i], strerror(errno));
         return false;
      }
      printf("%s: fd[%d]=%d dev=%ju ino=%ju size=%jd\n", label, i,
             handle->data[i], (uintmax_t)status.st_dev,
             (uintmax_t)status.st_ino, (intmax_t)status.st_size);
   }
   return handle->num_fds > 0;
}

int
main(int argc, char **argv)
{
   uint32_t width = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 256;
   uint32_t height = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 256;
   void *system_android = dlopen("/system/lib64/libandroid.so",
                                 RTLD_NOW | RTLD_LOCAL);
   if (!system_android) {
      fprintf(stderr, "dlopen system libandroid: %s\n", dlerror());
      return 1;
   }
   get_native_handle_fn get_handle =
      (get_native_handle_fn)dlsym(system_android,
                                  "AHardwareBuffer_getNativeHandle");
   if (!get_handle) {
      fprintf(stderr, "AHardwareBuffer_getNativeHandle: %s\n", dlerror());
      return 1;
   }

   const uint32_t formats[] = {
      AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
      AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM,
      5, /* HAL_PIXEL_FORMAT_BGRA_8888 */
   };
   const uint64_t usage =
      AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
      AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
      AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
      AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER;

   for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
      AHardwareBuffer_Desc requested = {
         .width = width,
         .height = height,
         .layers = 1,
         .format = formats[i],
         .usage = usage,
      };
      AHardwareBuffer *buffer = NULL;
      int result = AHardwareBuffer_allocate(&requested, &buffer);
      printf("format=%u allocate=%d\n", formats[i], result);
      if (result || !buffer)
         continue;

      AHardwareBuffer_Desc actual;
      AHardwareBuffer_describe(buffer, &actual);
      printf("format=%u actual=%ux%u layers=%u stride=%u usage=0x%" PRIx64
             "\n", formats[i], actual.width, actual.height, actual.layers,
             actual.stride, actual.usage);
      if (!describe_native_handle(get_handle, buffer, "allocated")) {
         AHardwareBuffer_release(buffer);
         return 2;
      }

      void *mapping = NULL;
      result = AHardwareBuffer_lock(buffer,
                                    AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                                    -1, NULL, &mapping);
      printf("format=%u lock=%d address=%p\n", formats[i], result, mapping);
      if (!result && mapping) {
         memset(mapping, 0x5a, (size_t)actual.stride * actual.height * 4);
         AHardwareBuffer_unlock(buffer, NULL);

         const struct tensor_native_handle *handle = get_handle(buffer);
         struct stat status;
         if (handle && handle->num_fds > 0 &&
             !fstat(handle->data[0], &status)) {
            uint8_t *direct = mmap(NULL, (size_t)status.st_size, PROT_READ,
                                   MAP_SHARED, handle->data[0], 0);
            if (direct != MAP_FAILED) {
               size_t first = 0;
               while (first < (size_t)status.st_size &&
                      direct[first] != 0x5a)
                  first++;
               printf("format=%u first-written-byte=%zu expected-pixels=%zu\n",
                      formats[i], first,
                      (size_t)actual.stride * actual.height * 4);
               munmap(direct, (size_t)status.st_size);
            }
         }
      }

      int sockets[2];
      if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) < 0) {
         perror("socketpair");
         AHardwareBuffer_release(buffer);
         return 3;
      }
      result = AHardwareBuffer_sendHandleToUnixSocket(buffer, sockets[0]);
      printf("format=%u send=%d\n", formats[i], result);
      AHardwareBuffer *received = NULL;
      if (!result)
         result = AHardwareBuffer_recvHandleFromUnixSocket(sockets[1],
                                                            &received);
      printf("format=%u receive=%d buffer=%p\n", formats[i], result,
             (void *)received);
      if (result || !received ||
          !describe_native_handle(get_handle, received, "received")) {
         close(sockets[0]);
         close(sockets[1]);
         if (received)
            AHardwareBuffer_release(received);
         AHardwareBuffer_release(buffer);
         return 4;
      }

      close(sockets[0]);
      close(sockets[1]);
      AHardwareBuffer_release(received);
      AHardwareBuffer_release(buffer);
   }

   return 0;
}
