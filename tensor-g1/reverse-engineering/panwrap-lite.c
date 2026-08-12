/* SPDX-License-Identifier: MIT */
/*
 * Minimal process-local Kbase ioctl recorder for native Android/Bionic tests.
 *
 * This intentionally starts smaller than panwrap: it records the stable ioctl
 * boundary without trying to guess pointer graphs or descriptor layouts.  The
 * resulting JSONL is the input to one-variable differential experiments.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PANWRAP_MAX_BYTES
#define PANWRAP_MAX_BYTES 256u
#endif

#ifdef __BIONIC__
typedef int ioctl_request_t;
#else
typedef unsigned long ioctl_request_t;
#endif

static int log_fd = -1;
static __thread bool in_hook;

static long
thread_id(void)
{
   return syscall(SYS_gettid);
}

static uint64_t
monotonic_ns(void)
{
   struct timespec ts = {0};
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static bool
copy_from_self(void *destination, const void *source, size_t size)
{
#ifdef SYS_process_vm_readv
   struct iovec local = {.iov_base = destination, .iov_len = size};
   struct iovec remote = {.iov_base = (void *)source, .iov_len = size};
   return syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) ==
          (ssize_t)size;
#else
   memcpy(destination, source, size);
   return true;
#endif
}

static bool
is_mali_fd(int fd)
{
   char link_name[64];
   char target[256];
   int n = snprintf(link_name, sizeof(link_name), "/proc/self/fd/%d", fd);
   if (n <= 0 || (size_t)n >= sizeof(link_name))
      return false;

   ssize_t len = readlink(link_name, target, sizeof(target) - 1);
   if (len < 0)
      return false;

   target[len] = '\0';
   return strcmp(target, "/dev/mali0") == 0;
}

static const char *
kbase_request_name(ioctl_request_t request)
{
   unsigned long request_u = (unsigned int)request;
   if (_IOC_TYPE(request_u) != 0x80)
      return "unknown";

   switch (_IOC_NR(request_u)) {
   case 0: return "VERSION_CHECK";
   case 1: return "SET_FLAGS";
   case 2: return "JOB_SUBMIT";
   case 3: return "GET_GPUPROPS";
   case 4: return "POST_TERM";
   case 5: return "MEM_ALLOC";
   case 6: return "MEM_QUERY";
   case 7: return "MEM_FREE";
   case 8: return "HWCNT_READER_SETUP";
   case 12: return "DISJOINT_QUERY";
   case 13: return "GET_DDK_VERSION";
   case 14: return "MEM_JIT_INIT";
   case 15: return "MEM_SYNC";
   case 16: return "MEM_FIND_CPU_OFFSET";
   case 17: return "GET_CONTEXT_ID";
   case 18: return "TLSTREAM_ACQUIRE";
   case 19: return "TLSTREAM_FLUSH";
   case 20: return "MEM_COMMIT";
   case 21: return "MEM_ALIAS";
   case 22: return "MEM_IMPORT";
   case 23: return "MEM_FLAGS_CHANGE";
   case 24: return "STREAM_CREATE";
   case 25: return "FENCE_VALIDATE";
   case 27: return "MEM_PROFILE_ADD";
   case 28: return "SOFT_EVENT_UPDATE";
   case 29: return "STICKY_RESOURCE_MAP";
   case 30: return "STICKY_RESOURCE_UNMAP";
   case 31: return "MEM_FIND_GPU_START_AND_OFFSET";
   case 33: return "CINSTR_GWT_START";
   case 34: return "CINSTR_GWT_STOP";
   case 35: return "CINSTR_GWT_DUMP";
   case 38: return "MEM_EXEC_INIT";
   case 50: return "GET_CPU_GPU_TIMEINFO";
   case 51: return "KINSTR_JM_FD";
   case 52: return "VERSION_CHECK_RESERVED";
   case 54: return "CONTEXT_PRIORITY_CHECK";
   case 55: return "SET_LIMITED_CORE_COUNT";
   default: return "unknown";
   }
}

static void
bytes_to_hex(const void *data, size_t size, char *hex, size_t hex_size)
{
   static const char digits[] = "0123456789abcdef";
   const uint8_t *bytes = data;
   size_t max_bytes = hex_size > 0 ? (hex_size - 1) / 2 : 0;
   if (size > max_bytes)
      size = max_bytes;

   for (size_t i = 0; i < size; ++i) {
      hex[i * 2] = digits[bytes[i] >> 4];
      hex[i * 2 + 1] = digits[bytes[i] & 0xf];
   }
   if (hex_size > 0)
      hex[size * 2] = '\0';
}

static void
emit_record(ioctl_request_t request, int result, int saved_errno,
            uint64_t begin_ns, uint64_t end_ns, const char *before,
            const char *after, size_t captured_size)
{
   if (log_fd < 0)
      return;

   unsigned long request_u = (unsigned int)request;
   char record[2048];
   int length = snprintf(
      record, sizeof(record),
      "{\"event\":\"ioctl\",\"pid\":%ld,\"tid\":%ld,"
      "\"begin_ns\":%llu,\"duration_ns\":%llu,"
      "\"request\":\"0x%lx\",\"type\":%u,\"nr\":%u,"
      "\"dir\":%u,\"size\":%u,\"name\":\"%s\","
      "\"captured_size\":%zu,\"before\":\"%s\",\"after\":\"%s\","
      "\"result\":%d,\"errno\":%d}\n",
      (long)getpid(), thread_id(), (unsigned long long)begin_ns,
      (unsigned long long)(end_ns - begin_ns), request_u,
      (unsigned)_IOC_TYPE(request_u), (unsigned)_IOC_NR(request_u),
      (unsigned)_IOC_DIR(request_u), (unsigned)_IOC_SIZE(request_u),
      kbase_request_name(request), captured_size, before, after, result,
      saved_errno);

   if (length > 0) {
      size_t write_size = (size_t)length;
      if (write_size >= sizeof(record))
         write_size = sizeof(record) - 1;
      (void)write(log_fd, record, write_size);
   }
}

static void
emit_job_atoms(const void *ioctl_arg)
{
   struct job_submit {
      uint64_t addr;
      uint32_t nr_atoms;
      uint32_t stride;
   } submit;

   if (log_fd < 0 || !ioctl_arg)
      return;

   if (!copy_from_self(&submit, ioctl_arg, sizeof(submit)))
      return;
   if (!submit.addr || !submit.nr_atoms || !submit.stride)
      return;

   size_t size = (size_t)submit.nr_atoms * submit.stride;
   if (size > 4096)
      size = 4096;

   uint8_t atoms[4096];
   if (!copy_from_self(atoms, (const void *)(uintptr_t)submit.addr, size))
      return;
   char hex[sizeof(atoms) * 2 + 1];
   bytes_to_hex(atoms, size, hex, sizeof(hex));

   char record[9000];
   int length = snprintf(
      record, sizeof(record),
      "{\"event\":\"job_submit_atoms\",\"pid\":%ld,\"tid\":%ld,"
      "\"addr\":\"0x%llx\",\"nr_atoms\":%u,\"stride\":%u,"
      "\"captured_size\":%zu,\"atoms\":\"%s\"}\n",
      (long)getpid(), thread_id(), (unsigned long long)submit.addr,
      submit.nr_atoms, submit.stride, size, hex);
   if (length > 0) {
      size_t write_size = (size_t)length;
      if (write_size >= sizeof(record))
         write_size = sizeof(record) - 1;
      (void)write(log_fd, record, write_size);
   }
}

__attribute__((constructor)) static void
panwrap_lite_init(void)
{
   const char *path = getenv("PANWRAP_LOG");
   if (!path || !path[0])
      path = "/data/local/tmp/panwrap-lite.jsonl";

   log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
}

__attribute__((destructor)) static void
panwrap_lite_fini(void)
{
   if (log_fd >= 0)
      close(log_fd);
}

int
ioctl(int fd, ioctl_request_t request, ...)
{
   unsigned long request_u = (unsigned int)request;
   void *arg = NULL;
   if (_IOC_SIZE(request_u) > 0) {
      va_list ap;
      va_start(ap, request);
      arg = va_arg(ap, void *);
      va_end(ap);
   }

   if (log_fd < 0 || in_hook || !is_mali_fd(fd))
      return (int)syscall(SYS_ioctl, fd, (unsigned int)request, arg);

   in_hook = true;

   size_t size = _IOC_SIZE(request_u);
   if (size > PANWRAP_MAX_BYTES)
      size = PANWRAP_MAX_BYTES;

   uint8_t before_bytes[PANWRAP_MAX_BYTES] = {0};
   uint8_t after_bytes[PANWRAP_MAX_BYTES] = {0};
   if (arg && size)
      (void)copy_from_self(before_bytes, arg, size);

   uint64_t begin_ns = monotonic_ns();
   int result = (int)syscall(SYS_ioctl, fd, (unsigned int)request, arg);
   int call_errno = errno;
   int logged_errno = result < 0 ? call_errno : 0;
   uint64_t end_ns = monotonic_ns();

   if (arg && size)
      (void)copy_from_self(after_bytes, arg, size);

   char before_hex[PANWRAP_MAX_BYTES * 2 + 1];
   char after_hex[PANWRAP_MAX_BYTES * 2 + 1];
   bytes_to_hex(before_bytes, size, before_hex, sizeof(before_hex));
   bytes_to_hex(after_bytes, size, after_hex, sizeof(after_hex));
   emit_record(request, result, logged_errno, begin_ns, end_ns, before_hex,
               after_hex, size);
   if (_IOC_TYPE(request_u) == 0x80 && _IOC_NR(request_u) == 2)
      emit_job_atoms(arg);

   in_hook = false;
   errno = call_errno;
   return result;
}
