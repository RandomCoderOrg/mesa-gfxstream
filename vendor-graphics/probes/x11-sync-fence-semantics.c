#define _GNU_SOURCE
#define VK_USE_PLATFORM_XCB_KHR

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#include <xcb/dri3.h>
#include <xcb/sync.h>
#include <xcb/xcb.h>

static xcb_screen_t *get_screen(xcb_connection_t *connection, int number)
{
    xcb_screen_iterator_t iterator =
        xcb_setup_roots_iterator(xcb_get_setup(connection));
    while (iterator.rem && number-- > 0)
        xcb_screen_next(&iterator);
    return iterator.data;
}

static int create_triggered_xshmfence(void)
{
    int fd = memfd_create("udroid-xshmfence-control", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, sizeof(uint32_t)) != 0)
        return -1;

    uint32_t *value = mmap(NULL, sizeof(uint32_t), PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
    if (value == MAP_FAILED) {
        close(fd);
        return -1;
    }
    __atomic_store_n(value, 1, __ATOMIC_RELEASE);
    munmap(value, sizeof(uint32_t));
    return fd;
}

static int import_fence(xcb_connection_t *connection, xcb_drawable_t drawable,
                        int fd, uint8_t initially_triggered,
                        uint8_t *error_code)
{
    uint32_t fence = xcb_generate_id(connection);
    xcb_void_cookie_t cookie = xcb_dri3_fence_from_fd_checked(
        connection, drawable, fence, initially_triggered, fd);
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    if (error) {
        *error_code = error->error_code;
        free(error);
        return 0;
    }

    cookie = xcb_sync_destroy_fence_checked(connection, fence);
    error = xcb_request_check(connection, cookie);
    if (error) {
        *error_code = error->error_code;
        free(error);
        return 0;
    }
    *error_code = 0;
    return 1;
}

static int create_vulkan_sync_file(int *out_fd, const char **device_name,
                                   uint32_t *api_version)
{
    VkApplicationInfo application = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "udroid-x11-sync-fence-semantics",
        .apiVersion = VK_API_VERSION_1_0,
    };
    VkInstanceCreateInfo instance_create = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_create, NULL, &instance) != VK_SUCCESS)
        return 0;

    uint32_t physical_count = 0;
    if (vkEnumeratePhysicalDevices(instance, &physical_count, NULL) != VK_SUCCESS ||
        physical_count == 0) {
        vkDestroyInstance(instance, NULL);
        return 0;
    }
    VkPhysicalDevice *physical_devices =
        calloc(physical_count, sizeof(*physical_devices));
    if (!physical_devices ||
        vkEnumeratePhysicalDevices(instance, &physical_count,
                                   physical_devices) != VK_SUCCESS) {
        free(physical_devices);
        vkDestroyInstance(instance, NULL);
        return 0;
    }
    VkPhysicalDevice physical = physical_devices[0];
    free(physical_devices);

    static VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    *device_name = properties.deviceName;
    *api_version = properties.apiVersion;

    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, NULL);
    VkQueueFamilyProperties *queue_properties =
        calloc(queue_count, sizeof(*queue_properties));
    if (!queue_properties) {
        vkDestroyInstance(instance, NULL);
        return 0;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count,
                                             queue_properties);
    uint32_t queue_family = UINT32_MAX;
    for (uint32_t index = 0; index < queue_count; ++index) {
        if (queue_properties[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queue_family = index;
            break;
        }
    }
    free(queue_properties);
    if (queue_family == UINT32_MAX) {
        vkDestroyInstance(instance, NULL);
        return 0;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_create = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char *device_extensions[] = {
        VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
    };
    VkDeviceCreateInfo device_create = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_create,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = device_extensions,
    };
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(physical, &device_create, NULL, &device) != VK_SUCCESS) {
        vkDestroyInstance(instance, NULL);
        return 0;
    }

    VkExportFenceCreateInfo export_create = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkFenceCreateInfo fence_create = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = &export_create,
    };
    VkFence fence = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    int ok = vkCreateFence(device, &fence_create, NULL, &fence) == VK_SUCCESS &&
             vkQueueSubmit(queue, 0, NULL, fence) == VK_SUCCESS;

    PFN_vkGetFenceFdKHR get_fence_fd =
        (PFN_vkGetFenceFdKHR)vkGetDeviceProcAddr(device, "vkGetFenceFdKHR");
    if (!get_fence_fd)
        ok = 0;
    if (ok) {
        VkFenceGetFdInfoKHR get_info = {
            .sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,
            .fence = fence,
            .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
        };
        ok = get_fence_fd(device, &get_info, out_fd) == VK_SUCCESS;
    }

    if (fence != VK_NULL_HANDLE)
        vkDestroyFence(device, fence, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return ok;
}

static void timeout_handler(int signal_number)
{
    (void)signal_number;
    static const char message[] = "FAIL stage=timeout\n";
    ssize_t written = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)written;
    _exit(124);
}

int main(int argc, char **argv)
{
    int run_xshm = 1;
    int run_sync_file = 1;
    if (argc == 2 && strcmp(argv[1], "--xshm-only") == 0)
        run_sync_file = 0;
    else if (argc == 2 && strcmp(argv[1], "--sync-file-only") == 0)
        run_xshm = 0;
    else if (argc != 1) {
        fprintf(stderr, "usage: %s [--xshm-only|--sync-file-only]\n", argv[0]);
        return 2;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGALRM, timeout_handler);
    alarm(8);

    int screen_number = 0;
    xcb_connection_t *connection = xcb_connect(NULL, &screen_number);
    if (xcb_connection_has_error(connection)) {
        fprintf(stderr, "FAIL stage=xcb-connect\n");
        return 1;
    }
    xcb_screen_t *screen = get_screen(connection, screen_number);
    if (!screen) {
        fprintf(stderr, "FAIL stage=xcb-screen\n");
        return 1;
    }

    int xshm_accepted = 0;
    uint8_t xshm_error = 0;
    if (run_xshm) {
        fprintf(stderr, "PROBE stage=create-xshmfence\n");
        int xshm_fd = create_triggered_xshmfence();
        if (xshm_fd < 0) {
            fprintf(stderr, "FAIL stage=create-xshmfence errno=%d\n", errno);
            return 1;
        }
        fprintf(stderr, "PROBE stage=import-xshmfence\n");
        xshm_accepted = import_fence(connection, screen->root, xshm_fd, 1,
                                     &xshm_error);
        close(xshm_fd);
        fprintf(stderr, "PROBE stage=xshmfence-result accepted=%d error=%u\n",
                xshm_accepted, xshm_error);
    }

    int sync_fd = -1;
    const char *device_name = "unknown";
    uint32_t api_version = 0;
    int sync_exported = 0;
    uint8_t sync_error = 0;
    int sync_accepted = 0;
    if (run_sync_file) {
        fprintf(stderr, "PROBE stage=export-vulkan-sync-file\n");
        sync_exported = create_vulkan_sync_file(&sync_fd, &device_name,
                                                &api_version);
        fprintf(stderr, "PROBE stage=vulkan-sync-file-result exported=%d\n",
                sync_exported);
        if (sync_exported) {
            fprintf(stderr, "PROBE stage=import-vulkan-sync-file\n");
            sync_accepted = import_fence(connection, screen->root, sync_fd, 0,
                                         &sync_error);
            close(sync_fd);
            fprintf(stderr,
                    "PROBE stage=sync-file-result accepted=%d error=%u\n",
                    sync_accepted, sync_error);
        }
    }

    printf("{\"xshmfence_accepted\":%s,\"xshmfence_error\":%u,"
           "\"vulkan_sync_fd_exported\":%s,\"sync_file_accepted\":%s,"
           "\"sync_file_error\":%u,\"device\":\"%s\","
           "\"api\":\"%u.%u.%u\"}\n",
           xshm_accepted ? "true" : "false", xshm_error,
           sync_exported ? "true" : "false",
           sync_accepted ? "true" : "false", sync_error, device_name,
           VK_VERSION_MAJOR(api_version), VK_VERSION_MINOR(api_version),
           VK_VERSION_PATCH(api_version));

    xcb_disconnect(connection);
    alarm(0);
    return (!run_xshm || xshm_accepted) &&
           (!run_sync_file || sync_exported) ? 0 : 1;
}
