#define _GNU_SOURCE

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <vulkan/vulkan.h>

typedef int (*tls_initialized_fn)(void);

struct worker_context {
    unsigned index;
    unsigned cycles;
    pthread_barrier_t *barrier;
    atomic_uint *failures;
    tls_initialized_fn tls_initialized;
};

static _Thread_local uint64_t host_tls_canary;

static void fail(struct worker_context *context, unsigned cycle,
                 const char *stage, int result)
{
    fprintf(stderr, "FAIL thread=%u cycle=%u stage=%s result=%d\n",
            context->index, cycle, stage, result);
    atomic_fetch_add_explicit(context->failures, 1, memory_order_relaxed);
}

static void *run_worker(void *opaque)
{
    struct worker_context *context = opaque;
    const uint64_t expected_canary =
        UINT64_C(0x5544524f49440000) | context->index;
    const VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "udroid-vulkan-thread-lifecycle",
        .apiVersion = VK_API_VERSION_1_0,
    };
    const VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };

    host_tls_canary = expected_canary;
    if (!context->tls_initialized()) {
        fail(context, 0, "android-tls-before-vulkan", 0);
        return NULL;
    }

    pthread_barrier_wait(context->barrier);
    for (unsigned cycle = 1; cycle <= context->cycles; ++cycle) {
        VkInstance instance = VK_NULL_HANDLE;
        VkResult result = vkCreateInstance(&create_info, NULL, &instance);
        if (result != VK_SUCCESS) {
            fail(context, cycle, "create-instance", result);
            return NULL;
        }

        uint32_t count = 0;
        result = vkEnumeratePhysicalDevices(instance, &count, NULL);
        if (result != VK_SUCCESS || count == 0) {
            fail(context, cycle, "enumerate-count", result);
            vkDestroyInstance(instance, NULL);
            return NULL;
        }

        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        count = 1;
        result = vkEnumeratePhysicalDevices(instance, &count, &physical_device);
        if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0) {
            fail(context, cycle, "enumerate-device", result);
            vkDestroyInstance(instance, NULL);
            return NULL;
        }

        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physical_device, &properties);
        vkDestroyInstance(instance, NULL);

        if (properties.deviceName[0] == '\0') {
            fail(context, cycle, "physical-device-properties", 0);
            return NULL;
        }

        if (host_tls_canary != expected_canary) {
            fail(context, cycle, "host-tls-canary", 0);
            return NULL;
        }
        if (!context->tls_initialized()) {
            fail(context, cycle, "android-tls-after-vulkan", 0);
            return NULL;
        }
    }

    return NULL;
}

static unsigned parse_count(const char *value, unsigned fallback,
                            unsigned maximum, const char *name)
{
    if (value == NULL)
        return fallback;

    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end == NULL || *end != '\0' || parsed == 0 || parsed > maximum) {
        fprintf(stderr, "invalid %s: %s\n", name, value);
        exit(64);
    }
    return (unsigned)parsed;
}

int main(int argc, char **argv)
{
    const unsigned thread_count =
        parse_count(argc > 1 ? argv[1] : NULL, 8, 64, "thread count");
    const unsigned cycles =
        parse_count(argc > 2 ? argv[2] : NULL, 25, 10000, "cycle count");
    tls_initialized_fn tls_initialized =
        (tls_initialized_fn)dlsym(RTLD_DEFAULT,
                                  "hybris_tls_current_thread_initialized");
    if (tls_initialized == NULL) {
        fprintf(stderr, "FAIL stage=tls-diagnostic-symbol error=%s\n", dlerror());
        return 2;
    }
    if (!tls_initialized()) {
        fprintf(stderr, "FAIL stage=main-thread-android-tls\n");
        return 3;
    }

    pthread_t *threads = calloc(thread_count, sizeof(*threads));
    struct worker_context *contexts = calloc(thread_count, sizeof(*contexts));
    pthread_barrier_t barrier;
    atomic_uint failures = 0;
    if (threads == NULL || contexts == NULL ||
        pthread_barrier_init(&barrier, NULL, thread_count) != 0) {
        fprintf(stderr, "FAIL stage=allocate-workers\n");
        free(contexts);
        free(threads);
        return 4;
    }

    unsigned created = 0;
    for (unsigned index = 0; index < thread_count; ++index) {
        contexts[index] = (struct worker_context){
            .index = index + 1,
            .cycles = cycles,
            .barrier = &barrier,
            .failures = &failures,
            .tls_initialized = tls_initialized,
        };
        int result = pthread_create(&threads[index], NULL, run_worker,
                                    &contexts[index]);
        if (result != 0) {
            fprintf(stderr, "FAIL stage=create-worker index=%u result=%d\n",
                    index + 1, result);
            /* Process exit releases workers waiting at the full-size barrier. */
            exit(4);
        }
        ++created;
    }
    for (unsigned index = 0; index < created; ++index)
        pthread_join(threads[index], NULL);

    pthread_barrier_destroy(&barrier);
    free(contexts);
    free(threads);

    const unsigned failure_count =
        atomic_load_explicit(&failures, memory_order_relaxed);
    if (failure_count != 0) {
        fprintf(stderr, "FAIL threads=%u cycles=%u failures=%u\n",
                thread_count, cycles, failure_count);
        return 1;
    }

    printf("PASS threads=%u cycles-per-thread=%u total-lifecycles=%u "
           "tls-isolated=true host-canary=true failures=0\n",
           thread_count, cycles, thread_count * cycles);
    return 0;
}
