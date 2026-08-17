/* SPDX-License-Identifier: MIT */

#ifndef _Nullable
#define _Nullable
#endif
#ifndef _Nonnull
#define _Nonnull
#endif
#ifndef __INTRODUCED_IN
#define __INTRODUCED_IN(api_level)
#endif

#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <errno.h>
#include <hybris/common/dlfcn.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef int (*allocate_fn)(const AHardwareBuffer_Desc *, AHardwareBuffer **);
typedef void (*release_fn)(AHardwareBuffer *);
typedef void (*describe_fn)(const AHardwareBuffer *, AHardwareBuffer_Desc *);
typedef int (*lock_fn)(AHardwareBuffer *, uint64_t, int32_t, const ARect *, void **);
typedef int (*unlock_fn)(AHardwareBuffer *, int32_t *);
typedef int (*send_fn)(const AHardwareBuffer *, int);
typedef int (*recv_fn)(int, AHardwareBuffer **);

struct ahb_api {
    void *handle;
    allocate_fn allocate;
    release_fn release;
    describe_fn describe;
    lock_fn lock;
    unlock_fn unlock;
    send_fn send;
    recv_fn recv;
    int ready;
};

static struct ahb_api api;
static pthread_once_t api_once = PTHREAD_ONCE_INIT;
static int trace_enabled;

static void trace(const char *stage, const char *detail)
{
    if (trace_enabled) {
        fprintf(stderr, "ahb-wrapper: stage=%s detail=%s\n", stage, detail);
        fflush(stderr);
    }
}

static void *resolve(const char *name)
{
    trace("resolve_begin", name);
    void *symbol = hybris_dlsym(api.handle, name);
    if (symbol == NULL)
        fprintf(stderr, "ahb-wrapper: missing %s: %s\n", name, hybris_dlerror());
    trace("resolve_end", name);
    return symbol;
}

static void initialize_api(void)
{
    const char *library = getenv("HYBRIS_AHB_LIBRARY");
    trace_enabled = getenv("UDROID_AHB_TRACE") != NULL;
    if (library == NULL || library[0] == '\0') {
        /* libandroid pulls framework dependencies that are unrelated to AHB
         * and unsafe in some vendor processes. libnativewindow is the narrow
         * Android provider used by the qualified Exynos and Tensor routes. */
        library = "libnativewindow.so";
    }
    trace("dlopen_begin", library);
    api.handle = hybris_dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (api.handle == NULL) {
        fprintf(stderr, "ahb-wrapper: could not load %s: %s\n",
                library, hybris_dlerror());
        return;
    }
    trace("dlopen_end", library);

    api.allocate = (allocate_fn)resolve("AHardwareBuffer_allocate");
    api.release = (release_fn)resolve("AHardwareBuffer_release");
    api.describe = (describe_fn)resolve("AHardwareBuffer_describe");
    api.lock = (lock_fn)resolve("AHardwareBuffer_lock");
    api.unlock = (unlock_fn)resolve("AHardwareBuffer_unlock");
    api.send = (send_fn)resolve("AHardwareBuffer_sendHandleToUnixSocket");
    api.recv = (recv_fn)resolve("AHardwareBuffer_recvHandleFromUnixSocket");
    api.ready = api.allocate != NULL && api.release != NULL &&
                api.describe != NULL && api.lock != NULL &&
                api.unlock != NULL && api.send != NULL && api.recv != NULL;
    trace("initialize_end", api.ready ? "ready" : "incomplete");
}

static int ensure_api(void)
{
    int result = pthread_once(&api_once, initialize_api);
    if (result != 0)
        return -result;
    return api.ready ? 0 : -ENOSYS;
}

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *desc,
                             AHardwareBuffer **out_buffer)
{
    int result = ensure_api();
    if (result != 0)
        return result;
    if (trace_enabled) {
        fprintf(stderr,
                "ahb-wrapper: stage=allocate_call wrapper=%p target=%p "
                "width=%u height=%u usage=0x%" PRIx64 "\n",
                (void *)AHardwareBuffer_allocate, (void *)api.allocate,
                desc->width, desc->height, desc->usage);
        fflush(stderr);
    }
    if (api.allocate == AHardwareBuffer_allocate) {
        fprintf(stderr,
                "ahb-wrapper: refusing recursive AHardwareBuffer_allocate target\n");
        return -ELOOP;
    }
    result = api.allocate(desc, out_buffer);
    trace("allocate_return", result == 0 ? "success" : "failure");
    return result;
}

void AHardwareBuffer_release(AHardwareBuffer *buffer)
{
    if (buffer != NULL && ensure_api() == 0) {
        trace("release_call", "begin");
        api.release(buffer);
        trace("release_return", "success");
    }
}

void AHardwareBuffer_describe(const AHardwareBuffer *buffer,
                              AHardwareBuffer_Desc *out_desc)
{
    if (buffer != NULL && out_desc != NULL && ensure_api() == 0) {
        if (trace_enabled) {
            fprintf(stderr,
                    "ahb-wrapper: stage=describe_call wrapper=%p target=%p "
                    "buffer=%p\n",
                    (void *)AHardwareBuffer_describe, (void *)api.describe,
                    (const void *)buffer);
            fflush(stderr);
        }
        if (api.describe == AHardwareBuffer_describe) {
            fprintf(stderr,
                    "ahb-wrapper: refusing recursive AHardwareBuffer_describe target\n");
            return;
        }
        api.describe(buffer, out_desc);
        trace("describe_return", "success");
    }
}

int AHardwareBuffer_lock(AHardwareBuffer *buffer, uint64_t usage, int32_t fence,
                         const ARect *rect, void **out_virtual_address)
{
    int result = ensure_api();
    if (result != 0)
        return result;
    if (trace_enabled) {
        fprintf(stderr,
                "ahb-wrapper: stage=lock_call target=%p buffer=%p usage=0x%"
                PRIx64 "\n",
                (void *)api.lock, (void *)buffer, usage);
        fflush(stderr);
    }
    result = api.lock(buffer, usage, fence, rect, out_virtual_address);
    trace("lock_return", result == 0 ? "success" : "failure");
    return result;
}

int AHardwareBuffer_unlock(AHardwareBuffer *buffer, int32_t *fence)
{
    int result = ensure_api();
    if (result != 0)
        return result;
    trace("unlock_call", "begin");
    result = api.unlock(buffer, fence);
    trace("unlock_return", result == 0 ? "success" : "failure");
    return result;
}

int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer *buffer,
                                            int socket_fd)
{
    int result = ensure_api();
    if (result != 0)
        return result;
    trace("send_call", "begin");
    result = api.send(buffer, socket_fd);
    trace("send_return", result == 0 ? "success" : "failure");
    return result;
}

int AHardwareBuffer_recvHandleFromUnixSocket(int socket_fd,
                                             AHardwareBuffer **out_buffer)
{
    int result = ensure_api();
    if (result != 0)
        return result;
    trace("recv_call", "begin");
    result = api.recv(socket_fd, out_buffer);
    trace("recv_return", result == 0 ? "success" : "failure");
    return result;
}
